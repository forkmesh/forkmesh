package com.forkmesh.nativeapp.feature.composer

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ImageDecoder
import android.graphics.Matrix
import android.net.Uri
import android.os.Build
import android.provider.OpenableColumns
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.agents.PromptAttachmentException
import com.forkmesh.nativeapp.agents.PromptAttachmentLimits
import com.forkmesh.nativeapp.composer.PhotoPrep
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

object PhotoIntake {
    private const val MAX_SOURCE_BYTES = 64 * 1024 * 1024

    suspend fun fromUri(context: Context, uri: Uri): PromptAttachment = withContext(Dispatchers.IO) {
        val name = displayName(context, uri)
        val raw = context.contentResolver.openInputStream(uri)?.use { input ->
            val out = ByteArrayOutputStream()
            val buffer = ByteArray(64 * 1024)
            var total = 0
            while (true) {
                val read = input.read(buffer)
                if (read < 0) break
                total += read
                if (total > MAX_SOURCE_BYTES) throw PromptAttachmentException("That photo is too large to attach.")
                out.write(buffer, 0, read)
            }
            out.toByteArray()
        } ?: throw PromptAttachmentException("That photo could not be opened.")
        withContext(Dispatchers.Default) { prepare(raw, name) }
    }

    fun prepare(raw: ByteArray, name: String): PromptAttachment {
        val bitmap = decode(raw)
            ?: throw PromptAttachmentException("That file is not a photo this app can read. Try a JPEG or PNG.")
        val flat = flatten(bitmap)
        if (flat !== bitmap) bitmap.recycle()
        val out = ByteArrayOutputStream()
        flat.compress(Bitmap.CompressFormat.JPEG, PromptAttachmentLimits.JPEG_QUALITY, out)
        val width = flat.width
        val height = flat.height
        flat.recycle()
        return PhotoPrep.attachment(out.toByteArray(), name, width, height)
    }

    private fun decode(raw: ByteArray): Bitmap? {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            return try {
                ImageDecoder.decodeBitmap(ImageDecoder.createSource(ByteBuffer.wrap(raw))) { decoder, info, _ ->
                    val size = PhotoPrep.scaledSize(info.size.width, info.size.height)
                    decoder.setTargetSize(size.width, size.height)
                    decoder.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
                    decoder.isMutableRequired = false
                }
            } catch (_: Exception) {
                decodeLegacy(raw)
            }
        }
        return decodeLegacy(raw)
    }

    private fun decodeLegacy(raw: ByteArray): Bitmap? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(raw, 0, raw.size, bounds)
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null
        val options = BitmapFactory.Options().apply { inSampleSize = PhotoPrep.sampleSize(bounds.outWidth, bounds.outHeight) }
        val sampled = BitmapFactory.decodeByteArray(raw, 0, raw.size, options) ?: return null
        val fix = PhotoPrep.orientationFix(PhotoPrep.exifOrientation(raw))
        val upright = if (fix.rotateDegrees == 0 && !fix.mirror) {
            sampled
        } else {
            val matrix = Matrix()
            if (fix.mirror) matrix.postScale(-1f, 1f)
            if (fix.rotateDegrees != 0) matrix.postRotate(fix.rotateDegrees.toFloat())
            Bitmap.createBitmap(sampled, 0, 0, sampled.width, sampled.height, matrix, true).also {
                if (it !== sampled) sampled.recycle()
            }
        }
        val size = PhotoPrep.scaledSize(upright.width, upright.height)
        if (size.width == upright.width && size.height == upright.height) return upright
        return Bitmap.createScaledBitmap(upright, size.width, size.height, true).also {
            if (it !== upright) upright.recycle()
        }
    }

    private fun flatten(bitmap: Bitmap): Bitmap {
        if (!bitmap.hasAlpha() && bitmap.config == Bitmap.Config.ARGB_8888) return bitmap
        val flat = Bitmap.createBitmap(bitmap.width, bitmap.height, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(flat)
        canvas.drawColor(Color.WHITE)
        canvas.drawBitmap(bitmap, 0f, 0f, null)
        return flat
    }

    private fun displayName(context: Context, uri: Uri): String = try {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0) ?: "" else ""
        } ?: ""
    } catch (_: Exception) {
        ""
    }
}
