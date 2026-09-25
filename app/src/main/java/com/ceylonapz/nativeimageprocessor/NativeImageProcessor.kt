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

    /**
     * Returns a new [width] x [height] ARGB_8888 bitmap scaled from [bitmap] (bilinear).
     * The source is not modified. Returns null on invalid input or unsupported format.
     */
    external fun resize(bitmap: Bitmap, width: Int, height: Int): Bitmap?

    /**
     * Returns a new blurred copy of [bitmap] (three box-blur passes, approximating a Gaussian).
     * The source is not modified. Returns null if [radius] < 1 or the format is unsupported.
     */
    external fun blur(bitmap: Bitmap, radius: Int): Bitmap?

    /**
     * Re-encodes the video at [inputPath] to an H.264 MP4 at [outputPath] with [watermark]
     * blended into the bottom-right corner, [marginPx] from the edges (FFmpeg + MediaCodec).
     * Audio is copied unchanged. Blocks until done; call off the main thread.
     *
     * @return null on success, otherwise an error message.
     */
    external fun addVideoWatermark(
        inputPath: String,
        outputPath: String,
        watermark: Bitmap,
        marginPx: Int,
        listener: ProgressListener?
    ): String?
}

/** Receives progress in 0..1 from native code, on the calling thread. */
fun interface ProgressListener {
    fun onProgress(progress: Float)
}