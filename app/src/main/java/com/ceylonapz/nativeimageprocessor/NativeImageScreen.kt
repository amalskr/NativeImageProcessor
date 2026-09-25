package com.ceylonapz.nativeimageprocessor

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.ImageDecoder
import android.net.Uri
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalInspectionMode
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.ceylonapz.nativeimageprocessor.ui.theme.NativeImageProcessorTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

@Composable
fun NativeImageScreen(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    // Native lib can't load in the IDE preview, so skip it there.
    val isPreview = LocalInspectionMode.current
    val processor = remember { if (isPreview) null else NativeImageProcessor() }

    var selectedUri by remember { mutableStateOf<Uri?>(null) }
    var bitmap by remember { mutableStateOf<Bitmap?>(null) }
    var isLoading by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }

    // System photo picker: no storage permission required.
    val pickImage = rememberLauncherForActivityResult(
        ActivityResultContracts.PickVisualMedia()
    ) { uri ->
        if (uri != null) selectedUri = uri
    }

    LaunchedEffect(selectedUri) {
        val uri = selectedUri ?: return@LaunchedEffect
        isLoading = true
        error = null
        bitmap = try {
            withContext(Dispatchers.IO) { loadBitmap(context, uri) }
        } catch (e: Exception) {
            error = "Failed to load image: ${e.message}"
            null
        }
        isLoading = false
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
            val current = bitmap
            when {
                isLoading -> CircularProgressIndicator()
                current != null -> Image(
                    bitmap = current.asImageBitmap(),
                    contentDescription = "Selected image",
                    contentScale = ContentScale.Fit,
                    modifier = Modifier.fillMaxWidth()
                )
                error != null -> Text(error!!)
                else -> Text("No image selected")
            }
        }

        Button(
            onClick = {
                pickImage.launch(
                    PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)
                )
            },
            modifier = Modifier.fillMaxWidth()
        ) {
            Text("Pick Image")
        }

        Button(
            onClick = { processor?.sayHello() },
            modifier = Modifier.fillMaxWidth()
        ) {
            Text("Call C++")
        }
    }
}

private const val MAX_IMAGE_DIMENSION = 2048

/**
 * Decodes [uri] into a mutable ARGB_8888 bitmap (the format native code expects),
 * downsampled so neither side exceeds [MAX_IMAGE_DIMENSION].
 */
private fun loadBitmap(context: Context, uri: Uri): Bitmap {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
        val source = ImageDecoder.createSource(context.contentResolver, uri)
        return ImageDecoder.decodeBitmap(source) { decoder, info, _ ->
            decoder.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
            decoder.isMutableRequired = true
            val largest = maxOf(info.size.width, info.size.height)
            var sample = 1
            while (largest / sample > MAX_IMAGE_DIMENSION) sample *= 2
            decoder.setTargetSampleSize(sample)
        }
    }

    val resolver = context.contentResolver
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    resolver.openInputStream(uri).use { BitmapFactory.decodeStream(it, null, bounds) }

    val largest = maxOf(bounds.outWidth, bounds.outHeight)
    var sample = 1
    while (largest / sample > MAX_IMAGE_DIMENSION) sample *= 2

    val options = BitmapFactory.Options().apply {
        inSampleSize = sample
        inPreferredConfig = Bitmap.Config.ARGB_8888
        inMutable = true
    }
    return resolver.openInputStream(uri).use { BitmapFactory.decodeStream(it, null, options) }
        ?: error("Unable to decode image")
}

@Preview(showBackground = true)
@Composable
fun NativeImageScreenPreview() {
    NativeImageProcessorTheme {
        NativeImageScreen()
    }
}
