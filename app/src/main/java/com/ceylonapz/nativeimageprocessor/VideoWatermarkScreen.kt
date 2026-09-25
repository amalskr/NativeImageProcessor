package com.ceylonapz.nativeimageprocessor

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.media.MediaMetadataRetriever
import android.net.Uri
import android.widget.MediaController
import android.widget.VideoView
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalInspectionMode
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

private const val WATERMARK_TEXT = "CeylonAPZ"

@Composable
fun VideoWatermarkScreen(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    // Native lib can't load in the IDE preview, so skip it there.
    val isPreview = LocalInspectionMode.current
    val processor = remember { if (isPreview) null else NativeImageProcessor() }
    val scope = rememberCoroutineScope()

    var videoUri by remember { mutableStateOf<Uri?>(null) }
    var outputFile by remember { mutableStateOf<File?>(null) }
    var isProcessing by remember { mutableStateOf(false) }
    var progress by remember { mutableFloatStateOf(0f) }
    var status by remember { mutableStateOf<String?>(null) }

    val pickVideo = rememberLauncherForActivityResult(
        ActivityResultContracts.PickVisualMedia()
    ) { uri ->
        if (uri != null) {
            videoUri = uri
            outputFile = null
            status = null
        }
    }

    val saveVideo = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("video/mp4")
    ) { dest ->
        val source = outputFile
        if (dest != null && source != null) {
            scope.launch {
                status = try {
                    withContext(Dispatchers.IO) {
                        context.contentResolver.openOutputStream(dest)!!.use { out ->
                            source.inputStream().use { it.copyTo(out) }
                        }
                    }
                    "Saved"
                } catch (e: Exception) {
                    "Save failed: ${e.message}"
                }
            }
        }
    }

    Column(
        modifier = modifier.padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .weight(1f),
            contentAlignment = Alignment.Center
        ) {
            val result = outputFile
            when {
                result != null -> key(result) {
                    AndroidView(
                        factory = { ctx ->
                            VideoView(ctx).apply {
                                setMediaController(MediaController(ctx).also { it.setAnchorView(this) })
                                setVideoPath(result.absolutePath)
                                setOnPreparedListener { it.isLooping = true; start() }
                            }
                        },
                        onRelease = { it.stopPlayback() },
                        // Without a size, VideoView measures 0x0 in Compose and never shows.
                        // It keeps the video's aspect ratio within these bounds.
                        modifier = Modifier.fillMaxSize()
                    )
                }
                videoUri != null -> Text("Video selected. Tap \"Add Watermark\".")
                else -> Text("No video selected")
            }
        }

        if (isProcessing) {
            LinearProgressIndicator(progress = { progress }, modifier = Modifier.fillMaxWidth())
            Text("Processing… ${(progress * 100).roundToInt()}%")
        }
        status?.let { Text(it) }

        Button(
            onClick = {
                pickVideo.launch(
                    PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.VideoOnly)
                )
            },
            enabled = !isProcessing,
            modifier = Modifier.fillMaxWidth()
        ) {
            Text("Pick Video")
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Button(
                onClick = {
                    val uri = videoUri ?: return@Button
                    val native = processor ?: return@Button
                    scope.launch {
                        isProcessing = true
                        progress = 0f
                        status = null
                        outputFile = null
                        val output = File(context.cacheDir, "watermarked.mp4")
                        val error = withContext(Dispatchers.Default) {
                            watermarkVideo(context, native, uri, output) { progress = it }
                        }
                        if (error == null) {
                            outputFile = output
                            status = "Done"
                        } else {
                            status = "Failed: $error"
                        }
                        isProcessing = false
                    }
                },
                enabled = videoUri != null && !isProcessing,
                modifier = Modifier.weight(1f)
            ) {
                Text("Add Watermark")
            }

            OutlinedButton(
                onClick = { saveVideo.launch("watermarked_${System.currentTimeMillis()}.mp4") },
                enabled = outputFile != null && !isProcessing,
                modifier = Modifier.weight(1f)
            ) {
                Text("Save")
            }
        }
    }
}

/** Runs the native watermarker. Returns null on success, otherwise an error message. */
private fun watermarkVideo(
    context: Context,
    processor: NativeImageProcessor,
    uri: Uri,
    output: File,
    onProgress: (Float) -> Unit
): String? {
    val (displayWidth, displayHeight) = readDisplaySize(context, uri)
        ?: return "Cannot read video size"

    // Size the text relative to the video so it looks the same at any resolution.
    val shortSide = min(displayWidth, displayHeight)
    val watermark = createWatermarkBitmap(WATERMARK_TEXT, textSizePx = shortSide * 0.06f)
    val margin = (shortSide * 0.03f).roundToInt()

    output.delete()
    return try {
        // Don't chain with `?:` here: addVideoWatermark returns null on success.
        val pfd = context.contentResolver.openFileDescriptor(uri, "r")
            ?: return "Cannot open video"
        pfd.use {
            // FFmpeg's file protocol can open the content Uri through the fd's /proc path.
            processor.addVideoWatermark(
                inputPath = "/proc/self/fd/${it.fd}",
                outputPath = output.absolutePath,
                watermark = watermark,
                marginPx = margin,
                listener = onProgress
            )
        }
    } catch (e: Exception) {
        "Cannot open video: ${e.message}"
    } finally {
        watermark.recycle()
    }
}

/** Width and height as displayed, i.e. after applying the rotation tag. */
private fun readDisplaySize(context: Context, uri: Uri): Pair<Int, Int>? {
    val retriever = MediaMetadataRetriever()
    return try {
        retriever.setDataSource(context, uri)
        val width = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH)
            ?.toIntOrNull() ?: return null
        val height = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT)
            ?.toIntOrNull() ?: return null
        val rotation = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION)
            ?.toIntOrNull() ?: 0
        if (rotation % 180 != 0) height to width else width to height
    } catch (e: Exception) {
        null
    } finally {
        retriever.release()
    }
}

/** White bold text with a soft shadow so it stays readable on light and dark footage. */
private fun createWatermarkBitmap(text: String, textSizePx: Float): Bitmap {
    val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        textSize = max(12f, textSizePx)
        typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
        setShadowLayer(textSize * 0.08f, 0f, textSize * 0.04f, Color.argb(160, 0, 0, 0))
    }
    val pad = (paint.textSize * 0.15f).roundToInt()
    val metrics = paint.fontMetricsInt
    val width = paint.measureText(text).roundToInt() + pad * 2
    val height = metrics.descent - metrics.ascent + pad * 2

    val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
    Canvas(bitmap).drawText(text, pad.toFloat(), (pad - metrics.ascent).toFloat(), paint)
    return bitmap
}
