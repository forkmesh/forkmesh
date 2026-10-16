package com.forkmesh.nativeapp.feature.facechanger

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.media.ExifInterface
import android.net.Uri
import android.util.Base64
import java.io.ByteArrayOutputStream
import java.io.File

object CallBackgroundStore {
    private const val FILE = "call-background.jpg"
    private const val PREFIX = "data:image/jpeg;base64,"
    const val SIDE = 1920

    fun load(context: Context): String = loadFile(context, FILE)

    fun save(context: Context, image: String) = saveFile(context, FILE, image)

    fun loadFile(context: Context, name: String): String {
        val bytes = runCatching { File(context.filesDir, name).readBytes() }.getOrNull() ?: return ""
        return if (bytes.isEmpty()) "" else PREFIX + Base64.encodeToString(bytes, Base64.NO_WRAP)
    }

    fun saveFile(context: Context, name: String, image: String) {
        if (!image.startsWith(PREFIX)) return
        val bytes = runCatching { Base64.decode(image.removePrefix(PREFIX), Base64.DEFAULT) }.getOrNull() ?: return
        runCatching { File(context.filesDir, name).writeBytes(bytes) }
    }

    fun fromUri(context: Context, uri: Uri, side: Int = SIDE): String? = runCatching {
        val resolver = context.contentResolver
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        resolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, bounds) }
        val longest = maxOf(bounds.outWidth, bounds.outHeight)
        if (longest <= 0) return null
        var sample = 1
        while (longest / (sample * 2) >= side) sample *= 2
        val decoded = resolver.openInputStream(uri)?.use {
            BitmapFactory.decodeStream(it, null, BitmapFactory.Options().apply { inSampleSize = sample })
        } ?: return null
        val orientation = resolver.openInputStream(uri)?.use {
            ExifInterface(it).getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL)
        } ?: ExifInterface.ORIENTATION_NORMAL
        val rotation = when (orientation) {
            ExifInterface.ORIENTATION_ROTATE_90 -> 90
            ExifInterface.ORIENTATION_ROTATE_180 -> 180
            ExifInterface.ORIENTATION_ROTATE_270 -> 270
            else -> 0
        }
        val scale = minOf(1f, side.toFloat() / maxOf(decoded.width, decoded.height))
        val matrix = Matrix().apply {
            postScale(scale, scale)
            postRotate(rotation.toFloat())
        }
        val fitted = Bitmap.createBitmap(decoded, 0, 0, decoded.width, decoded.height, matrix, true)
        val out = ByteArrayOutputStream()
        fitted.compress(Bitmap.CompressFormat.JPEG, 86, out)
        PREFIX + Base64.encodeToString(out.toByteArray(), Base64.NO_WRAP)
    }.getOrNull()

    fun thumbnail(text: String): Bitmap? {
        val comma = text.indexOf(',')
        if (comma < 0) return null
        val bytes = runCatching { Base64.decode(text.substring(comma + 1), Base64.DEFAULT) }.getOrNull() ?: return null
        return BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
    }
}

object CallFaceStore {
    private const val FILE = "call-face.jpg"
    const val SIDE = 1024

    fun load(context: Context): String = CallBackgroundStore.loadFile(context, FILE)

    fun save(context: Context, image: String) = CallBackgroundStore.saveFile(context, FILE, image)

    fun fromUri(context: Context, uri: Uri): String? = CallBackgroundStore.fromUri(context, uri, SIDE)
}
