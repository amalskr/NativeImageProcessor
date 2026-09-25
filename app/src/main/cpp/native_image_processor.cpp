#include <jni.h>
#include <android/log.h>
#include <android/bitmap.h>

#include <algorithm>
#include <new>
#include <vector>

#define LOG_TAG "NativeImageProcessor"

extern "C"
JNIEXPORT void JNICALL
Java_com_ceylonapz_nativeimageprocessor_NativeImageProcessor_sayHello(
        JNIEnv* env,
        jobject thiz) {

    __android_log_print(
            ANDROID_LOG_INFO,
            LOG_TAG,
            "Hello from C++!"
    );
}

// RGBA
// ↓
//R G B
// ↓
//Grayscale calculation
// ↓
//R = G = B

extern "C"
JNIEXPORT jobject JNICALL
Java_com_ceylonapz_nativeimageprocessor_NativeImageProcessor_toGrayscale(JNIEnv *env, jobject thiz,
                                                                         jobject bitmap) {
    AndroidBitmapInfo info;

    if (AndroidBitmap_getInfo(env, bitmap, &info) < 0) {
        return nullptr;
    }

    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
        return nullptr;
    }

    void* pixels = nullptr;

    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) < 0) {
        return nullptr;
    }

    // Rows may be padded, so step by info.stride (bytes), not width.
    // RGBA_8888 is stored as bytes R,G,B,A -> little-endian uint32 is 0xAABBGGRR.
    auto* base = static_cast<uint8_t*>(pixels);

    for (uint32_t y = 0; y < info.height; y++) {

        auto* row = reinterpret_cast<uint32_t*>(base + y * info.stride);

        for (uint32_t x = 0; x < info.width; x++) {

            uint32_t pixel = row[x];

            uint32_t r = pixel & 0xFF;
            uint32_t g = (pixel >> 8) & 0xFF;
            uint32_t b = (pixel >> 16) & 0xFF;

            // Integer version of 0.299R + 0.587G + 0.114B
            uint32_t gray = (77 * r + 150 * g + 29 * b) >> 8;

            row[x] = (pixel & 0xFF000000) |
                     (gray << 16) |
                     (gray << 8) |
                     gray;
        }
    }

    AndroidBitmap_unlockPixels(env, bitmap);

    return bitmap;
}


// Creates a new ARGB_8888 Bitmap via Bitmap.createBitmap(width, height, config).
// Returns nullptr with a pending Java exception (e.g. OutOfMemoryError) on failure.
static jobject createArgb8888Bitmap(JNIEnv* env, jint width, jint height) {
    jclass bitmapClass = env->FindClass("android/graphics/Bitmap");
    jclass configClass = env->FindClass("android/graphics/Bitmap$Config");
    if (bitmapClass == nullptr || configClass == nullptr) {
        return nullptr;
    }

    jfieldID argb8888Field = env->GetStaticFieldID(
            configClass, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
    jobject argb8888 = env->GetStaticObjectField(configClass, argb8888Field);

    jmethodID createBitmap = env->GetStaticMethodID(
            bitmapClass,
            "createBitmap",
            "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");

    jobject newBitmap = env->CallStaticObjectMethod(
            bitmapClass, createBitmap, width, height, argb8888);

    env->DeleteLocalRef(argb8888);
    env->DeleteLocalRef(configClass);
    env->DeleteLocalRef(bitmapClass);

    if (env->ExceptionCheck()) {
        return nullptr;
    }
    return newBitmap;
}

// Bilinear resize. The source is only read; a new Bitmap is returned.
extern "C"
JNIEXPORT jobject JNICALL
Java_com_ceylonapz_nativeimageprocessor_NativeImageProcessor_resize(JNIEnv *env, jobject thiz,
                                                                    jobject bitmap,
                                                                    jint newWidth,
                                                                    jint newHeight) {
    if (newWidth <= 0 || newHeight <= 0) {
        return nullptr;
    }

    AndroidBitmapInfo srcInfo;
    if (AndroidBitmap_getInfo(env, bitmap, &srcInfo) < 0 ||
        srcInfo.format != ANDROID_BITMAP_FORMAT_RGBA_8888 ||
        srcInfo.width == 0 || srcInfo.height == 0) {
        return nullptr;
    }

    jobject dstBitmap = createArgb8888Bitmap(env, newWidth, newHeight);
    if (dstBitmap == nullptr) {
        return nullptr;
    }

    AndroidBitmapInfo dstInfo;
    if (AndroidBitmap_getInfo(env, dstBitmap, &dstInfo) < 0) {
        env->DeleteLocalRef(dstBitmap);
        return nullptr;
    }

    void* srcPixels = nullptr;
    void* dstPixels = nullptr;

    if (AndroidBitmap_lockPixels(env, bitmap, &srcPixels) < 0) {
        env->DeleteLocalRef(dstBitmap);
        return nullptr;
    }
    if (AndroidBitmap_lockPixels(env, dstBitmap, &dstPixels) < 0) {
        AndroidBitmap_unlockPixels(env, bitmap);
        env->DeleteLocalRef(dstBitmap);
        return nullptr;
    }

    const auto* srcBase = static_cast<const uint8_t*>(srcPixels);
    auto* dstBase = static_cast<uint8_t*>(dstPixels);

    const float scaleX = static_cast<float>(srcInfo.width) / dstInfo.width;
    const float scaleY = static_cast<float>(srcInfo.height) / dstInfo.height;
    const int maxX = static_cast<int>(srcInfo.width) - 1;
    const int maxY = static_cast<int>(srcInfo.height) - 1;

    for (uint32_t y = 0; y < dstInfo.height; y++) {

        // Map destination pixel centre to source space.
        float sy = (y + 0.5f) * scaleY - 0.5f;
        if (sy < 0) sy = 0;
        int y0 = static_cast<int>(sy);
        int y1 = y0 < maxY ? y0 + 1 : maxY;
        float fy = sy - y0;

        const auto* row0 = reinterpret_cast<const uint32_t*>(srcBase + y0 * srcInfo.stride);
        const auto* row1 = reinterpret_cast<const uint32_t*>(srcBase + y1 * srcInfo.stride);
        auto* dstRow = reinterpret_cast<uint32_t*>(dstBase + y * dstInfo.stride);

        for (uint32_t x = 0; x < dstInfo.width; x++) {

            float sx = (x + 0.5f) * scaleX - 0.5f;
            if (sx < 0) sx = 0;
            int x0 = static_cast<int>(sx);
            int x1 = x0 < maxX ? x0 + 1 : maxX;
            float fx = sx - x0;

            uint32_t p00 = row0[x0], p10 = row0[x1];
            uint32_t p01 = row1[x0], p11 = row1[x1];

            // Interpolate each 8-bit channel (R, G, B, A) independently.
            // Pixels are premultiplied, which is the correct space to blend in.
            uint32_t out = 0;
            for (int shift = 0; shift < 32; shift += 8) {
                float c00 = (p00 >> shift) & 0xFF;
                float c10 = (p10 >> shift) & 0xFF;
                float c01 = (p01 >> shift) & 0xFF;
                float c11 = (p11 >> shift) & 0xFF;

                float top = c00 + (c10 - c00) * fx;
                float bottom = c01 + (c11 - c01) * fx;
                auto value = static_cast<uint32_t>(top + (bottom - top) * fy + 0.5f);

                out |= (value > 255 ? 255 : value) << shift;
            }
            dstRow[x] = out;
        }
    }

    AndroidBitmap_unlockPixels(env, dstBitmap);
    AndroidBitmap_unlockPixels(env, bitmap);

    return dstBitmap;
}

// One box-blur pass along a line of `count` pixels spaced `step` apart.
// Uses a sliding window, so cost is O(count) regardless of radius. Edges are clamped.
static void boxBlurLine(const uint32_t* src, uint32_t* dst, int count, int step, int radius) {
    const int window = 2 * radius + 1;
    const int last = count - 1;
    uint32_t sum[4] = {0, 0, 0, 0};

    // Prime the window centred on index 0.
    for (int i = -radius; i <= radius; i++) {
        uint32_t p = src[std::clamp(i, 0, last) * step];
        for (int c = 0; c < 4; c++) sum[c] += (p >> (c * 8)) & 0xFF;
    }

    for (int i = 0; i < count; i++) {
        uint32_t out = 0;
        for (int c = 0; c < 4; c++) {
            out |= ((sum[c] + window / 2) / window) << (c * 8);
        }
        dst[i * step] = out;

        // Slide: drop the pixel leaving on the left, add the one entering on the right.
        uint32_t leaving = src[std::clamp(i - radius, 0, last) * step];
        uint32_t entering = src[std::clamp(i + radius + 1, 0, last) * step];
        for (int c = 0; c < 4; c++) {
            sum[c] += ((entering >> (c * 8)) & 0xFF);
            sum[c] -= ((leaving >> (c * 8)) & 0xFF);
        }
    }
}

// Blur with the given radius. The source is only read; a new Bitmap is returned.
// Three box-blur passes approximate a Gaussian blur.
extern "C"
JNIEXPORT jobject JNICALL
Java_com_ceylonapz_nativeimageprocessor_NativeImageProcessor_blur(JNIEnv *env, jobject thiz,
                                                                  jobject bitmap,
                                                                  jint radius) {
    if (radius < 1) {
        return nullptr;
    }

    AndroidBitmapInfo srcInfo;
    if (AndroidBitmap_getInfo(env, bitmap, &srcInfo) < 0 ||
        srcInfo.format != ANDROID_BITMAP_FORMAT_RGBA_8888 ||
        srcInfo.width == 0 || srcInfo.height == 0) {
        return nullptr;
    }

    const int width = static_cast<int>(srcInfo.width);
    const int height = static_cast<int>(srcInfo.height);

    // Two packed working buffers (no row padding) for ping-ponging between passes.
    std::vector<uint32_t> a, b;
    try {
        a.resize(static_cast<size_t>(width) * height);
        b.resize(a.size());
    } catch (const std::bad_alloc&) {
        return nullptr;
    }

    void* srcPixels = nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &srcPixels) < 0) {
        return nullptr;
    }
    const auto* srcBase = static_cast<const uint8_t*>(srcPixels);
    for (int y = 0; y < height; y++) {
        const auto* row = reinterpret_cast<const uint32_t*>(srcBase + y * srcInfo.stride);
        std::copy(row, row + width, a.begin() + static_cast<size_t>(y) * width);
    }
    AndroidBitmap_unlockPixels(env, bitmap);

    // Pixels are premultiplied, which is the correct space to average in.
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < height; y++) {
            size_t offset = static_cast<size_t>(y) * width;
            boxBlurLine(a.data() + offset, b.data() + offset, width, 1, radius);
        }
        for (int x = 0; x < width; x++) {
            boxBlurLine(b.data() + x, a.data() + x, height, width, radius);
        }
    }

    jobject dstBitmap = createArgb8888Bitmap(env, width, height);
    if (dstBitmap == nullptr) {
        return nullptr;
    }

    AndroidBitmapInfo dstInfo;
    void* dstPixels = nullptr;
    if (AndroidBitmap_getInfo(env, dstBitmap, &dstInfo) < 0 ||
        AndroidBitmap_lockPixels(env, dstBitmap, &dstPixels) < 0) {
        env->DeleteLocalRef(dstBitmap);
        return nullptr;
    }
    auto* dstBase = static_cast<uint8_t*>(dstPixels);
    for (int y = 0; y < height; y++) {
        auto* row = reinterpret_cast<uint32_t*>(dstBase + y * dstInfo.stride);
        std::copy_n(a.begin() + static_cast<size_t>(y) * width, width, row);
    }
    AndroidBitmap_unlockPixels(env, dstBitmap);

    return dstBitmap;
}
