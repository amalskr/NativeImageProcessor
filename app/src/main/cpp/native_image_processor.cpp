#include <jni.h>
#include <android/log.h>
#include <android/bitmap.h>

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

