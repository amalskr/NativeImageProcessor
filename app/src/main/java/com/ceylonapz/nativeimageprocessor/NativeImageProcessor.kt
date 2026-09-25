package com.ceylonapz.nativeimageprocessor

class NativeImageProcessor {

    companion object {
        init {
            System.loadLibrary("native-image-processor")
        }
    }

    external fun sayHello()
}