package com.forkmesh.nativeapp.platform

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.ImageDecoder
import android.net.Uri
import android.os.Build
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import com.forkmesh.nativeapp.settings.Wallpaper
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

object WallpaperStore {
    private const val TAG = "wallpaper"
    private const val QUALITY = 88

    var loadedName: String by mutableStateOf("")
        private set

    var loaded: ImageBitmap? by mutableStateOf(null)
        private set

    private fun directory(context: Context): File = File(context.applicationContext.filesDir, Wallpaper.DIRECTORY)

    private fun file(context: Context, name: String): File? {
        val clean = Wallpaper.cleanName(name)
        return if (clean.isEmpty()) null else File(directory(context), clean)
    }

    fun forName(name: String): ImageBitmap? = if (name.isNotEmpty() && name == loadedName) loaded else null

    suspend fun load(context: Context, name: String) {
        if (name.isEmpty()) {
            loadedName = ""
            loaded = null
            return
        }
        if (name == loadedName && loaded != null) return
        val source = file(context, name) ?: return
        val bitmap = withContext(Dispatchers.IO) {
            runCatching { BitmapFactory.decodeFile(source.path) }.getOrNull()
        }
        if (bitmap == null) {
            Log.w(TAG, "the background image $name could not be read")
            return
        }
        loadedName = name
        loaded = bitmap.asImageBitmap()
    }

    suspend fun save(context: Context, uri: Uri): String? = withContext(Dispatchers.IO) {
        val app = context.applicationContext
        val bitmap = runCatching { decode(app, uri) }.onFailure { Log.w(TAG, "could not read the picked image", it) }.getOrNull()
            ?: return@withContext null
        val name = Wallpaper.fileName(System.currentTimeMillis())
        val target = File(directory(app), name)
        val written = runCatching {
            directory(app).mkdirs()
            val partial = File(directory(app), "$name.part")
            partial.outputStream().use { bitmap.compress(Bitmap.CompressFormat.JPEG, QUALITY, it) }
            partial.renameTo(target)
        }.onFailure { Log.w(TAG, "could not save the background image", it) }.getOrDefault(false)
        if (!written) return@withContext null
        withContext(Dispatchers.Main) {
            loadedName = name
            loaded = bitmap.asImageBitmap()
        }
        name
    }

    fun delete(context: Context, name: String) {
        file(context, name)?.let { runCatching { it.delete() } }
    }

    private fun decode(context: Context, uri: Uri): Bitmap? {
        val metrics = context.resources.displayMetrics
        val screenWidth = minOf(metrics.widthPixels, metrics.heightPixels)
        val screenHeight = maxOf(metrics.widthPixels, metrics.heightPixels)
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            decodeWithDecoder(context, uri, screenWidth, screenHeight)
        } else {
            decodeWithFactory(context, uri, screenWidth, screenHeight)
        }
    }

    @androidx.annotation.RequiresApi(Build.VERSION_CODES.P)
    private fun decodeWithDecoder(context: Context, uri: Uri, screenWidth: Int, screenHeight: Int): Bitmap? =
        ImageDecoder.decodeBitmap(ImageDecoder.createSource(context.contentResolver, uri)) { decoder, info, _ ->
            decoder.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
            val size = info.size
            val (width, height) = coverFor(size.width, size.height, screenWidth, screenHeight)
            decoder.setTargetSize(width, height)
        }

    private fun decodeWithFactory(context: Context, uri: Uri, screenWidth: Int, screenHeight: Int): Bitmap? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        context.contentResolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, bounds) }
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null
        val options = BitmapFactory.Options().apply {
            inSampleSize = Wallpaper.sampleSize(bounds.outWidth, bounds.outHeight, screenWidth, screenHeight)
        }
        val decoded = context.contentResolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, options) } ?: return null
        val (width, height) = coverFor(decoded.width, decoded.height, screenWidth, screenHeight)
        return if (width == decoded.width && height == decoded.height) decoded else Bitmap.createScaledBitmap(decoded, width, height, true)
    }

    private fun coverFor(width: Int, height: Int, screenWidth: Int, screenHeight: Int): Pair<Int, Int> =
        Wallpaper.coverSize(width, height, screenWidth, screenHeight)
}
