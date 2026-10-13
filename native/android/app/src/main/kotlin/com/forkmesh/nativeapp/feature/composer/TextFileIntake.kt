package com.forkmesh.nativeapp.feature.composer

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import com.forkmesh.nativeapp.agents.PromptAttachmentException
import com.forkmesh.nativeapp.agents.PromptTextFile
import com.forkmesh.nativeapp.agents.PromptTextFiles
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

object TextFileIntake {
    suspend fun read(context: Context, uri: Uri): PromptTextFile = withContext(Dispatchers.IO) {
        var name = uri.lastPathSegment?.substringAfterLast('/').orEmpty()
        var size = -1L
        runCatching {
            context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) {
                    val nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    val sizeIndex = cursor.getColumnIndex(OpenableColumns.SIZE)
                    if (nameIndex >= 0 && !cursor.isNull(nameIndex)) name = cursor.getString(nameIndex)
                    if (sizeIndex >= 0 && !cursor.isNull(sizeIndex)) size = cursor.getLong(sizeIndex)
                }
            }
        }
        if (size > PromptTextFiles.MAX_BYTES) throw PromptTextFiles.tooLarge(name)
        val bytes = try {
            context.contentResolver.openInputStream(uri)?.use { input ->
                val out = java.io.ByteArrayOutputStream()
                val buffer = ByteArray(8192)
                while (out.size() <= PromptTextFiles.MAX_BYTES) {
                    val read = input.read(buffer)
                    if (read < 0) break
                    out.write(buffer, 0, read)
                }
                out.toByteArray()
            }
        } catch (error: java.io.IOException) {
            null
        } catch (error: SecurityException) {
            null
        } ?: throw PromptTextFiles.unreadable(name)
        PromptTextFiles.read(name, bytes)
    }

    fun message(error: Exception): String =
        (error as? PromptAttachmentException)?.message ?: error.message ?: "That file could not be attached."
}
