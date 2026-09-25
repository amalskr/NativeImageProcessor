// Video watermarking with FFmpeg.
//
// Pipeline: demux (mov/mp4) -> decode video -> convert to NV12 -> alpha-blend watermark
//           -> encode H.264 (h264_mediacodec, i.e. the device's hardware encoder) -> mux mp4.
// Audio (and other A/V streams) are copied through without re-encoding.
//
// The watermark is an RGBA Bitmap rendered by Kotlin in *display* orientation. Phone videos
// are often stored rotated with a display-matrix tag, so the overlay is rotated into the
// coded frame's orientation and the tag is copied to the output.

#include <jni.h>
#include <android/bitmap.h>
#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/jni.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

#define LOG_TAG "VideoWatermark"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Routes FFmpeg's log output (stderr by default, which Android discards) to logcat.
static void ffmpegLogToLogcat(void* avcl, int level, const char* fmt, va_list args) {
    if (level > av_log_get_level()) return;
    int prio = level <= AV_LOG_ERROR ? ANDROID_LOG_ERROR
             : level <= AV_LOG_WARNING ? ANDROID_LOG_WARN
             : level <= AV_LOG_INFO ? ANDROID_LOG_INFO : ANDROID_LOG_DEBUG;
    char line[1024];
    int printPrefix = 1;
    av_log_format_line(avcl, level, fmt, args, line, sizeof(line), &printPrefix);
    __android_log_write(prio, "FFmpeg", line);
}

// FFmpeg's MediaCodec wrapper calls into Java, so it needs the JavaVM.
extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    av_jni_set_java_vm(vm, nullptr);
    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(ffmpegLogToLogcat);
    return JNI_VERSION_1_6;
}

namespace {

std::string avError(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

// ---------------------------------------------------------------------------------------------
// Watermark overlay, pre-converted to YUV + alpha in coded-frame space.
// ---------------------------------------------------------------------------------------------

struct Rgba {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;  // premultiplied, bytes R,G,B,A (0xAABBGGRR)
};

struct Overlay {
    int x = 0, y = 0;          // top-left in coded frame (even)
    int width = 0, height = 0; // even
    std::vector<uint8_t> lumaY, lumaA;             // width * height
    std::vector<uint8_t> chromaU, chromaV, chromaA; // (width/2) * (height/2)
};

// Maps a coded-frame pixel to display space for a clockwise display rotation.
inline void codedToDisplay(int rotation, int W, int H, int cx, int cy, int& dx, int& dy) {
    switch (rotation) {
        case 90:  dx = H - 1 - cy; dy = cx;         break;
        case 180: dx = W - 1 - cx; dy = H - 1 - cy; break;
        case 270: dx = cy;         dy = W - 1 - cx; break;
        default:  dx = cx;         dy = cy;         break;
    }
}

// Inverse of codedToDisplay.
inline void displayToCoded(int rotation, int W, int H, int dx, int dy, int& cx, int& cy) {
    switch (rotation) {
        case 90:  cx = dy;         cy = H - 1 - dx; break;
        case 180: cx = W - 1 - dx; cy = H - 1 - dy; break;
        case 270: cx = W - 1 - dy; cy = dx;         break;
        default:  cx = dx;         cy = dy;         break;
    }
}

inline uint8_t clampByte(float v) {
    return static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
}

// Limited-range RGB -> YUV using BT.709 or BT.601 coefficients.
void rgbToYuv(bool bt709, float r, float g, float b, uint8_t& y, uint8_t& u, uint8_t& v) {
    if (bt709) {
        y = clampByte(16 + (46.559f * r + 156.629f * g + 15.812f * b) / 255.0f);
        u = clampByte(128 + (-25.664f * r - 86.336f * g + 112.0f * b) / 255.0f);
        v = clampByte(128 + (112.0f * r - 101.730f * g - 10.270f * b) / 255.0f);
    } else {
        y = clampByte(16 + (65.481f * r + 128.553f * g + 24.966f * b) / 255.0f);
        u = clampByte(128 + (-37.797f * r - 74.203f * g + 112.0f * b) / 255.0f);
        v = clampByte(128 + (112.0f * r - 93.786f * g - 18.214f * b) / 255.0f);
    }
}

// Builds the overlay so the watermark lands at the bottom-right of the *displayed* video.
bool buildOverlay(const Rgba& wm, int W, int H, int rotation, int margin, bool bt709,
                  Overlay& out) {
    const bool swapped = rotation == 90 || rotation == 270;
    const int displayW = swapped ? H : W;
    const int displayH = swapped ? W : H;

    // Watermark rectangle in display space, clipped to the frame.
    const int dx0 = std::max(0, displayW - margin - wm.width);
    const int dy0 = std::max(0, displayH - margin - wm.height);
    const int dx1 = std::min(displayW, dx0 + wm.width) - 1;
    const int dy1 = std::min(displayH, dy0 + wm.height) - 1;
    if (dx1 < dx0 || dy1 < dy0) return false;

    // Same rectangle in coded space, expanded to even bounds for 4:2:0 chroma.
    int ax, ay, bx, by;
    displayToCoded(rotation, W, H, dx0, dy0, ax, ay);
    displayToCoded(rotation, W, H, dx1, dy1, bx, by);
    int cx0 = std::min(ax, bx) & ~1;
    int cy0 = std::min(ay, by) & ~1;
    int cx1 = std::min(std::max(ax, bx) | 1, (W & ~1) - 1);
    int cy1 = std::min(std::max(ay, by) | 1, (H & ~1) - 1);

    out.x = cx0;
    out.y = cy0;
    out.width = cx1 - cx0 + 1;
    out.height = cy1 - cy0 + 1;
    if (out.width <= 0 || out.height <= 0) return false;

    const size_t lumaSize = static_cast<size_t>(out.width) * out.height;
    std::vector<uint8_t> u(lumaSize), v(lumaSize);
    out.lumaY.assign(lumaSize, 0);
    out.lumaA.assign(lumaSize, 0);

    for (int y = 0; y < out.height; y++) {
        for (int x = 0; x < out.width; x++) {
            int dx, dy;
            codedToDisplay(rotation, W, H, cx0 + x, cy0 + y, dx, dy);
            const int wx = dx - dx0, wy = dy - dy0;
            if (wx < 0 || wy < 0 || wx >= wm.width || wy >= wm.height) continue;

            const uint32_t p = wm.pixels[static_cast<size_t>(wy) * wm.width + wx];
            const uint32_t a = p >> 24;
            if (a == 0) continue;

            // Un-premultiply.
            const float scale = 255.0f / a;
            const float r = (p & 0xFF) * scale;
            const float g = ((p >> 8) & 0xFF) * scale;
            const float b = ((p >> 16) & 0xFF) * scale;

            const size_t i = static_cast<size_t>(y) * out.width + x;
            rgbToYuv(bt709, r, g, b, out.lumaY[i], u[i], v[i]);
            out.lumaA[i] = static_cast<uint8_t>(a);
        }
    }

    // Chroma: alpha-weighted average of each 2x2 block.
    const int cw = out.width / 2, ch = out.height / 2;
    out.chromaU.assign(static_cast<size_t>(cw) * ch, 128);
    out.chromaV.assign(static_cast<size_t>(cw) * ch, 128);
    out.chromaA.assign(static_cast<size_t>(cw) * ch, 0);
    for (int y = 0; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
            uint32_t aSum = 0, uSum = 0, vSum = 0;
            for (int k = 0; k < 4; k++) {
                const size_t i = static_cast<size_t>(2 * y + (k >> 1)) * out.width + 2 * x + (k & 1);
                aSum += out.lumaA[i];
                uSum += u[i] * out.lumaA[i];
                vSum += v[i] * out.lumaA[i];
            }
            if (aSum == 0) continue;
            const size_t c = static_cast<size_t>(y) * cw + x;
            out.chromaU[c] = static_cast<uint8_t>(uSum / aSum);
            out.chromaV[c] = static_cast<uint8_t>(vSum / aSum);
            out.chromaA[c] = static_cast<uint8_t>(aSum / 4);
        }
    }
    return true;
}

inline uint8_t blend(uint8_t dst, uint8_t src, uint8_t alpha) {
    return static_cast<uint8_t>((dst * (255 - alpha) + src * alpha + 127) / 255);
}

// Blends the overlay into an NV12 frame (Y plane + interleaved UV plane).
void blendNv12(AVFrame* frame, const Overlay& ov) {
    for (int y = 0; y < ov.height; y++) {
        uint8_t* row = frame->data[0] + static_cast<ptrdiff_t>(ov.y + y) * frame->linesize[0] + ov.x;
        const size_t base = static_cast<size_t>(y) * ov.width;
        for (int x = 0; x < ov.width; x++) {
            const uint8_t a = ov.lumaA[base + x];
            if (a) row[x] = blend(row[x], ov.lumaY[base + x], a);
        }
    }
    const int cw = ov.width / 2, ch = ov.height / 2;
    for (int y = 0; y < ch; y++) {
        uint8_t* row = frame->data[1] + static_cast<ptrdiff_t>(ov.y / 2 + y) * frame->linesize[1] + ov.x;
        const size_t base = static_cast<size_t>(y) * cw;
        for (int x = 0; x < cw; x++) {
            const uint8_t a = ov.chromaA[base + x];
            if (!a) continue;
            row[2 * x] = blend(row[2 * x], ov.chromaU[base + x], a);
            row[2 * x + 1] = blend(row[2 * x + 1], ov.chromaV[base + x], a);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Transcoder
// ---------------------------------------------------------------------------------------------

class ProgressReporter {
public:
    ProgressReporter(JNIEnv* env, jobject listener) : env_(env), listener_(listener) {
        if (listener_) {
            jclass cls = env_->GetObjectClass(listener_);
            method_ = env_->GetMethodID(cls, "onProgress", "(F)V");
            env_->DeleteLocalRef(cls);
        }
    }

    // Returns false if the Java callback threw.
    bool report(float progress) {
        if (!method_) return true;
        progress = std::clamp(progress, 0.0f, 1.0f);
        if (progress - last_ < 0.01f && progress < 1.0f) return true;
        last_ = progress;
        env_->CallVoidMethod(listener_, method_, progress);
        return !env_->ExceptionCheck();
    }

private:
    JNIEnv* env_;
    jobject listener_;
    jmethodID method_ = nullptr;
    float last_ = -1.0f;
};

class Watermarker {
public:
    ~Watermarker() {
        av_packet_free(&packet_);
        av_packet_free(&encPacket_);
        av_frame_free(&decFrame_);
        av_frame_free(&encFrame_);
        sws_freeContext(sws_);
        avcodec_free_context(&decoder_);
        avcodec_free_context(&encoder_);
        avformat_close_input(&input_);
        if (output_) {
            if (!(output_->oformat->flags & AVFMT_NOFILE)) avio_closep(&output_->pb);
            avformat_free_context(output_);
        }
    }

    // Returns an empty string on success, otherwise an error message.
    std::string run(const char* inputPath, const char* outputPath, const Rgba& watermark,
                    int margin, ProgressReporter& progress) {
        std::string err;
        if (!(err = openInput(inputPath)).empty()) return err;
        if (!(err = openDecoder()).empty()) return err;
        if (!(err = openOutput(outputPath)).empty()) return err;
        if (!(err = openEncoder()).empty()) return err;
        if (!(err = addStreams()).empty()) return err;

        bt709_ = decoder_->colorspace == AVCOL_SPC_BT709 ||
                 (decoder_->colorspace == AVCOL_SPC_UNSPECIFIED && decoder_->height >= 720);
        if (!buildOverlay(watermark, decoder_->width, decoder_->height, rotation_, margin, bt709_,
                          overlay_)) {
            return "Watermark does not fit in the video";
        }

        int ret = avio_open(&output_->pb, outputPath, AVIO_FLAG_WRITE);
        if (ret < 0) return "Cannot open output: " + avError(ret);
        ret = avformat_write_header(output_, nullptr);
        if (ret < 0) return "Cannot write header: " + avError(ret);

        const double duration = input_->duration > 0
                                ? static_cast<double>(input_->duration) / AV_TIME_BASE : 0.0;

        while ((ret = av_read_frame(input_, packet_)) >= 0) {
            const int index = packet_->stream_index;
            if (index == videoIndex_) {
                if (duration > 0 && packet_->pts != AV_NOPTS_VALUE) {
                    const double t = packet_->pts * av_q2d(input_->streams[index]->time_base);
                    if (!progress.report(static_cast<float>(t / duration))) {
                        return "Cancelled by progress listener";
                    }
                }
                ret = avcodec_send_packet(decoder_, packet_);
                av_packet_unref(packet_);
                if (ret < 0 && ret != AVERROR_INVALIDDATA) return "Decode failed: " + avError(ret);
                if (!(err = drainDecoder()).empty()) return err;
            } else if (streamMap_[index] >= 0) {
                AVStream* in = input_->streams[index];
                AVStream* out = output_->streams[streamMap_[index]];
                av_packet_rescale_ts(packet_, in->time_base, out->time_base);
                packet_->stream_index = out->index;
                packet_->pos = -1;
                ret = av_interleaved_write_frame(output_, packet_);
                if (ret < 0) return "Write failed: " + avError(ret);
            } else {
                av_packet_unref(packet_);
            }
        }
        if (ret != AVERROR_EOF) return "Read failed: " + avError(ret);

        // Flush decoder, then encoder.
        avcodec_send_packet(decoder_, nullptr);
        if (!(err = drainDecoder()).empty()) return err;
        avcodec_send_frame(encoder_, nullptr);
        if (!(err = drainEncoder(true)).empty()) return err;

        ret = av_write_trailer(output_);
        if (ret < 0) return "Cannot finalize output: " + avError(ret);

        progress.report(1.0f);
        LOGI("Watermarked %d frames", frameCount_);
        return {};
    }

private:
    std::string openInput(const char* path) {
        int ret = avformat_open_input(&input_, path, nullptr, nullptr);
        if (ret < 0) return "Cannot open input: " + avError(ret);
        ret = avformat_find_stream_info(input_, nullptr);
        if (ret < 0) return "Cannot read stream info: " + avError(ret);
        videoIndex_ = av_find_best_stream(input_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoIndex_ < 0) return "No video stream found";

        // Clockwise display rotation from the stream's display matrix (if any).
        const AVCodecParameters* par = input_->streams[videoIndex_]->codecpar;
        const AVPacketSideData* sd = av_packet_side_data_get(
                par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
        if (sd && sd->size >= 9 * sizeof(int32_t)) {
            const double ccw = av_display_rotation_get(reinterpret_cast<const int32_t*>(sd->data));
            if (!std::isnan(ccw)) {
                int cw = static_cast<int>(std::lround(-ccw)) % 360;
                if (cw < 0) cw += 360;
                rotation_ = (cw + 45) / 90 * 90 % 360;  // snap to 0/90/180/270
            }
        }
        return {};
    }

    std::string openDecoder() {
        const AVStream* st = input_->streams[videoIndex_];
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec) {
            return std::string("Unsupported video codec: ") + avcodec_get_name(st->codecpar->codec_id);
        }
        decoder_ = avcodec_alloc_context3(codec);
        if (!decoder_) return "Out of memory";
        int ret = avcodec_parameters_to_context(decoder_, st->codecpar);
        if (ret < 0) return "Decoder setup failed: " + avError(ret);
        decoder_->pkt_timebase = st->time_base;
        decoder_->thread_count = 0;  // auto
        ret = avcodec_open2(decoder_, codec, nullptr);
        if (ret < 0) return "Cannot open decoder: " + avError(ret);

        packet_ = av_packet_alloc();
        encPacket_ = av_packet_alloc();
        decFrame_ = av_frame_alloc();
        encFrame_ = av_frame_alloc();
        if (!packet_ || !encPacket_ || !decFrame_ || !encFrame_) return "Out of memory";
        return {};
    }

    std::string openOutput(const char* path) {
        int ret = avformat_alloc_output_context2(&output_, nullptr, "mp4", path);
        if (ret < 0 || !output_) return "Cannot create output: " + avError(ret);
        return {};
    }

    // Frame rate to tell the encoder. Phone cameras record variable frame rate, where
    // av_guess_frame_rate() returns the timebase-derived r_frame_rate (e.g. 120 for a ~30 fps
    // clip) - far above what hardware encoders accept at 1080p. Prefer the real average.
    AVRational encoderFrameRate() const {
        const AVStream* in = input_->streams[videoIndex_];
        AVRational fps = in->avg_frame_rate;
        if (fps.num <= 0 || fps.den <= 0) {
            fps = av_guess_frame_rate(input_, const_cast<AVStream*>(in), nullptr);
        }
        if (fps.num <= 0 || fps.den <= 0) return {30, 1};
        // It's only a rate-control hint (timing comes from pts), so round and cap it.
        const int rounded = static_cast<int>(std::lround(av_q2d(fps)));
        return {std::clamp(rounded, 1, 60), 1};
    }

    std::string openEncoder() {
        const AVCodec* codec = avcodec_find_encoder_by_name("h264_mediacodec");
        if (!codec) return "h264_mediacodec encoder not available";

        AVRational fps = encoderFrameRate();
        int ret = openEncoderAt(codec, fps);
        if (ret < 0 && fps.num > 30) {
            // Some encoders can't do e.g. 1080p60; fall back to 30 fps.
            LOGI("Encoder rejected %d fps, retrying at 30", fps.num);
            ret = openEncoderAt(codec, {30, 1});
        }
        if (ret < 0) return "Cannot open H.264 encoder: " + avError(ret);

        encFrame_->format = encoder_->pix_fmt;
        encFrame_->width = encoder_->width;
        encFrame_->height = encoder_->height;
        ret = av_frame_get_buffer(encFrame_, 0);
        if (ret < 0) return "Cannot allocate frame: " + avError(ret);
        return {};
    }

    int openEncoderAt(const AVCodec* codec, AVRational fps) {
        avcodec_free_context(&encoder_);
        encoder_ = avcodec_alloc_context3(codec);
        if (!encoder_) return AVERROR(ENOMEM);

        const AVStream* in = input_->streams[videoIndex_];
        encoder_->width = decoder_->width;
        encoder_->height = decoder_->height;
        encoder_->pix_fmt = AV_PIX_FMT_NV12;  // widely supported by MediaCodec encoders
        encoder_->time_base = in->time_base;
        encoder_->framerate = fps;
        encoder_->sample_aspect_ratio = decoder_->sample_aspect_ratio;
        encoder_->gop_size = std::max(1, fps.num * 2 / fps.den);  // ~2 s
        encoder_->color_range = AVCOL_RANGE_MPEG;
        encoder_->color_primaries = decoder_->color_primaries;
        encoder_->color_trc = decoder_->color_trc;
        encoder_->colorspace = decoder_->colorspace;

        // Keep the source bitrate if known, otherwise ~0.15 bits per pixel per frame.
        int64_t bitrate = in->codecpar->bit_rate;
        if (bitrate <= 0) {
            bitrate = static_cast<int64_t>(encoder_->width * encoder_->height * av_q2d(fps) * 0.15);
        }
        encoder_->bit_rate = std::clamp<int64_t>(bitrate, 1'000'000, 40'000'000);

        // Deliberately NOT setting AV_CODEC_FLAG_GLOBAL_HEADER: h264_mediacodec implements it by
        // encoding a dummy frame + EOS and then flush()ing, after which some Codec2 encoders
        // (e.g. c2.android.avc.encoder) accept input but never emit output again. Without it,
        // SPS/PPS arrive in the first packet and the mp4 muxer builds avcC from that.

        LOGI("Encoder: %dx%d, %d/%d fps, %lld bps, rotation %d, source %s %s",
             encoder_->width, encoder_->height, fps.num, fps.den,
             static_cast<long long>(encoder_->bit_rate), rotation_,
             avcodec_get_name(decoder_->codec_id), av_get_pix_fmt_name(decoder_->pix_fmt));

        return avcodec_open2(encoder_, codec, nullptr);
    }

    std::string addStreams() {
        streamMap_.assign(input_->nb_streams, -1);

        for (unsigned i = 0; i < input_->nb_streams; i++) {
            AVStream* in = input_->streams[i];
            const AVMediaType type = in->codecpar->codec_type;

            if (static_cast<int>(i) == videoIndex_) {
                AVStream* out = avformat_new_stream(output_, nullptr);
                if (!out) return "Out of memory";
                int ret = avcodec_parameters_from_context(out->codecpar, encoder_);
                if (ret < 0) return "Stream setup failed: " + avError(ret);
                out->time_base = encoder_->time_base;
                out->avg_frame_rate = encoder_->framerate;

                // Keep the rotation tag so the output displays like the input.
                const AVPacketSideData* sd = av_packet_side_data_get(
                        in->codecpar->coded_side_data, in->codecpar->nb_coded_side_data,
                        AV_PKT_DATA_DISPLAYMATRIX);
                if (sd) {
                    AVPacketSideData* copy = av_packet_side_data_new(
                            &out->codecpar->coded_side_data, &out->codecpar->nb_coded_side_data,
                            AV_PKT_DATA_DISPLAYMATRIX, sd->size, 0);
                    if (copy) memcpy(copy->data, sd->data, sd->size);
                }
                videoOutIndex_ = out->index;
                streamMap_[i] = out->index;
            } else if (type == AVMEDIA_TYPE_AUDIO) {
                AVStream* out = avformat_new_stream(output_, nullptr);
                if (!out) return "Out of memory";
                int ret = avcodec_parameters_copy(out->codecpar, in->codecpar);
                if (ret < 0) return "Stream setup failed: " + avError(ret);
                out->codecpar->codec_tag = 0;
                out->time_base = in->time_base;
                streamMap_[i] = out->index;
            }
        }
        return {};
    }

    std::string drainDecoder() {
        while (true) {
            int ret = avcodec_receive_frame(decoder_, decFrame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return {};
            if (ret < 0) return "Decode failed: " + avError(ret);

            std::string err = processFrame();
            av_frame_unref(decFrame_);
            if (!err.empty()) return err;
        }
    }

    std::string processFrame() {
        // Encoder may still reference the previous buffer.
        int ret = av_frame_make_writable(encFrame_);
        if (ret < 0) return "Cannot allocate frame: " + avError(ret);

        std::string err = ensureScaler();
        if (!err.empty()) return err;
        sws_scale(sws_, decFrame_->data, decFrame_->linesize, 0, decFrame_->height,
                  encFrame_->data, encFrame_->linesize);

        blendNv12(encFrame_, overlay_);

        int64_t pts = decFrame_->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) {
            // No timestamp from the stream: derive one from the frame number.
            pts = av_rescale_q(frameCount_, av_inv_q(encoder_->framerate), encoder_->time_base);
        }
        encFrame_->pts = pts;

        // MediaCodec has a small input queue: EAGAIN means "collect output first, then retry".
        const int64_t deadline = av_gettime_relative() + 10 * AV_TIME_BASE;
        while ((ret = avcodec_send_frame(encoder_, encFrame_)) == AVERROR(EAGAIN)) {
            std::string err = drainEncoder();
            if (!err.empty()) return err;
            if (av_gettime_relative() > deadline) return "Encoder stalled";
            av_usleep(2000);
        }
        if (ret < 0) return "Encode failed: " + avError(ret);
        frameCount_++;
        return drainEncoder();
    }

    // When flushing, MediaCodec may still be producing output, so EAGAIN means "not yet"
    // rather than "done"; keep polling until EOF (with a safety timeout).
    // (Re)creates the NV12 converter only when the input format/size changes. Deprecated
    // full-range formats (yuvj420p, typical for phone cameras) are mapped to their plain
    // equivalent with an explicit range; passing them directly makes sws_getCachedContext
    // rebuild the context on every frame.
    std::string ensureScaler() {
        auto format = static_cast<AVPixelFormat>(decFrame_->format);
        bool fullRange = decFrame_->color_range == AVCOL_RANGE_JPEG;
        switch (format) {
            case AV_PIX_FMT_YUVJ420P: format = AV_PIX_FMT_YUV420P; fullRange = true; break;
            case AV_PIX_FMT_YUVJ422P: format = AV_PIX_FMT_YUV422P; fullRange = true; break;
            case AV_PIX_FMT_YUVJ444P: format = AV_PIX_FMT_YUV444P; fullRange = true; break;
            default: break;
        }
        if (sws_ && format == swsFormat_ && fullRange == swsFullRange_ &&
            decFrame_->width == swsWidth_ && decFrame_->height == swsHeight_) {
            return {};
        }

        sws_freeContext(sws_);
        sws_ = sws_getContext(decFrame_->width, decFrame_->height, format,
                              encFrame_->width, encFrame_->height, AV_PIX_FMT_NV12,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) return "Unsupported pixel format";

        // Same matrix in and out; only convert full -> limited range when needed.
        const int* coeffs = sws_getCoefficients(bt709_ ? SWS_CS_ITU709 : SWS_CS_ITU601);
        sws_setColorspaceDetails(sws_, coeffs, fullRange ? 1 : 0, coeffs, 0, 0, 1 << 16, 1 << 16);

        swsFormat_ = format;
        swsFullRange_ = fullRange;
        swsWidth_ = decFrame_->width;
        swsHeight_ = decFrame_->height;
        return {};
    }

    std::string drainEncoder(bool flushing = false) {
        const int64_t deadline = av_gettime_relative() + 10 * AV_TIME_BASE;
        while (true) {
            int ret = avcodec_receive_packet(encoder_, encPacket_);
            if (ret == AVERROR_EOF) return {};
            if (ret == AVERROR(EAGAIN)) {
                if (!flushing) return {};
                if (av_gettime_relative() > deadline) return "Encoder flush timed out";
                av_usleep(5000);
                continue;
            }
            if (ret < 0) return "Encode failed: " + avError(ret);

            AVStream* out = output_->streams[videoOutIndex_];
            av_packet_rescale_ts(encPacket_, encoder_->time_base, out->time_base);
            encPacket_->stream_index = videoOutIndex_;
            ret = av_interleaved_write_frame(output_, encPacket_);
            if (ret < 0) return "Write failed: " + avError(ret);
        }
    }

    AVFormatContext* input_ = nullptr;
    AVFormatContext* output_ = nullptr;
    AVCodecContext* decoder_ = nullptr;
    AVCodecContext* encoder_ = nullptr;
    SwsContext* sws_ = nullptr;
    AVPixelFormat swsFormat_ = AV_PIX_FMT_NONE;
    bool swsFullRange_ = false;
    int swsWidth_ = 0, swsHeight_ = 0;
    bool bt709_ = false;
    AVPacket* packet_ = nullptr;
    AVPacket* encPacket_ = nullptr;
    AVFrame* decFrame_ = nullptr;
    AVFrame* encFrame_ = nullptr;

    int videoIndex_ = -1;
    int videoOutIndex_ = -1;
    int rotation_ = 0;
    int frameCount_ = 0;
    std::vector<int> streamMap_;
    Overlay overlay_;
};

bool readBitmap(JNIEnv* env, jobject bitmap, Rgba& out) {
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) < 0 ||
        info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 || info.width == 0 || info.height == 0) {
        return false;
    }
    void* pixels = nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) < 0) return false;

    out.width = static_cast<int>(info.width);
    out.height = static_cast<int>(info.height);
    out.pixels.resize(static_cast<size_t>(out.width) * out.height);
    const auto* base = static_cast<const uint8_t*>(pixels);
    for (int y = 0; y < out.height; y++) {
        const auto* row = reinterpret_cast<const uint32_t*>(base + y * info.stride);
        std::copy(row, row + out.width, out.pixels.begin() + static_cast<size_t>(y) * out.width);
    }
    AndroidBitmap_unlockPixels(env, bitmap);
    return true;
}

}  // namespace

// Returns null on success, or an error message.
extern "C"
JNIEXPORT jstring JNICALL
Java_com_ceylonapz_nativeimageprocessor_NativeImageProcessor_addVideoWatermark(
        JNIEnv* env, jobject thiz,
        jstring inputPath, jstring outputPath,
        jobject watermark, jint margin, jobject listener) {

    Rgba wm;
    if (!readBitmap(env, watermark, wm)) {
        return env->NewStringUTF("Watermark must be an ARGB_8888 bitmap");
    }

    const char* in = env->GetStringUTFChars(inputPath, nullptr);
    const char* out = env->GetStringUTFChars(outputPath, nullptr);

    ProgressReporter progress(env, listener);
    std::string error;
    {
        Watermarker watermarker;
        error = watermarker.run(in, out, wm, std::max(0, static_cast<int>(margin)), progress);
    }

    env->ReleaseStringUTFChars(inputPath, in);
    env->ReleaseStringUTFChars(outputPath, out);

    if (error.empty()) return nullptr;
    LOGE("%s", error.c_str());
    // If the listener threw, let that exception propagate to Kotlin.
    if (env->ExceptionCheck()) return nullptr;
    return env->NewStringUTF(error.c_str());
}
