#include <jni.h>
#include <android/log.h>

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

