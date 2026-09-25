package com.ceylonapz.nativeimageprocessor

import android.graphics.Bitmap

class NativeImageProcessor {

    companion object {
        init {
            System.loadLibrary("native-image-processor")
        }
    }

    external fun sayHello()
    /** Converts [bitmap] to grayscale in place. Returns null if it isn't ARGB_8888. */
    external fun toGrayscale(bitmap: Bitmap): Bitmap?
}