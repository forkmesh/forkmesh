package com.forkmesh.nativeapp.feature.localrepos

import android.content.ContentResolver
import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import com.forkmesh.nativeapp.git.worktree.FileSystemEntityType
import com.forkmesh.nativeapp.localrepos.LocalImportDirectory
import com.forkmesh.nativeapp.localrepos.LocalImportEntry
import java.io.File
import java.io.FileNotFoundException
import java.io.InputStream

internal class DocumentTreeImportDirectory(
    private val resolver: ContentResolver,
    private val treeUri: Uri,
    private val documentId: String = DocumentsContract.getTreeDocumentId(treeUri),
) : LocalImportDirectory {
    override fun isReadableDirectory(): Boolean = try {
        resolver.query(
            DocumentsContract.buildDocumentUriUsingTree(treeUri, documentId),
            arrayOf(DocumentsContract.Document.COLUMN_MIME_TYPE),
            null,
            null,
            null,
        )?.use { cursor -> cursor.moveToFirst() && cursor.getString(0) == DocumentsContract.Document.MIME_TYPE_DIR } == true
    } catch (_: Exception) {
        false
    }

    override fun list(): List<LocalImportEntry> {
        val children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, documentId)
        val columns = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
        )
        val out = ArrayList<LocalImportEntry>()
        resolver.query(children, columns, null, null, null)?.use { cursor ->
            while (cursor.moveToNext()) {
                val id = cursor.getString(0) ?: continue
                val name = cursor.getString(1) ?: continue
                val mime = cursor.getString(2) ?: ""
                val size = if (cursor.isNull(3)) -1L else cursor.getLong(3)
                out.add(Entry(id, name, mime, size))
            }
        } ?: throw FileNotFoundException("The selected folder could not be read.")
        return out
    }

    private inner class Entry(
        private val id: String,
        override val name: String,
        mime: String,
        size: Long,
    ) : LocalImportEntry {
        override val type: FileSystemEntityType =
            if (mime == DocumentsContract.Document.MIME_TYPE_DIR) FileSystemEntityType.directory else FileSystemEntityType.file

        override val size: Long = if (type == FileSystemEntityType.file) size else -1

        override fun directory(): LocalImportDirectory = DocumentTreeImportDirectory(resolver, treeUri, id)

        override fun open(): InputStream =
            resolver.openInputStream(DocumentsContract.buildDocumentUriUsingTree(treeUri, id))
                ?: throw FileNotFoundException(name)
    }
}

internal object ImportSources {
    fun treeName(context: Context, treeUri: Uri): String {
        val documentId = DocumentsContract.getTreeDocumentId(treeUri)
        val uri = DocumentsContract.buildDocumentUriUsingTree(treeUri, documentId)
        val name = try {
            context.contentResolver.query(uri, arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null)
                ?.use { cursor -> if (cursor.moveToFirst()) cursor.getString(0) else null }
        } catch (_: Exception) {
            null
        }
        return name ?: documentId.substringAfterLast(':').substringAfterLast('/').ifEmpty { "imported" }
    }

    fun displayName(context: Context, uri: Uri): String = try {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
            ?.use { cursor -> if (cursor.moveToFirst()) cursor.getString(0) else null }
    } catch (_: Exception) {
        null
    } ?: uri.lastPathSegment?.substringAfterLast('/') ?: "imported.zip"

    fun copyToCache(context: Context, uri: Uri): File {
        val directory = File(context.cacheDir, "zip-import").apply { mkdirs() }
        val target = File.createTempFile("import-", ".zip", directory)
        context.contentResolver.openInputStream(uri)?.use { input ->
            target.outputStream().use { output -> input.copyTo(output) }
        } ?: throw FileNotFoundException("The selected archive could not be read.")
        return target
    }
}
