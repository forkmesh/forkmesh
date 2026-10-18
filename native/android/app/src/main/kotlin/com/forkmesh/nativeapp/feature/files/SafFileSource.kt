package com.forkmesh.nativeapp.feature.files

import android.content.ContentResolver
import android.net.Uri
import android.provider.DocumentsContract
import android.webkit.MimeTypeMap
import com.forkmesh.nativeapp.files.FileItem
import com.forkmesh.nativeapp.files.FileProblem
import com.forkmesh.nativeapp.files.FilesModel
import com.forkmesh.nativeapp.files.SmbStatus
import java.io.InputStream
import java.io.OutputStream

class SafFileSource(private val resolver: ContentResolver, private val tree: Uri, override val title: String) : com.forkmesh.nativeapp.files.FileSource {
    private val rootId: String = DocumentsContract.getTreeDocumentId(tree)
    private val ids = mutableMapOf("" to rootId)

    private data class Row(val id: String, val name: String, val mime: String, val size: Long, val modified: Long)

    private fun uri(id: String): Uri = DocumentsContract.buildDocumentUriUsingTree(tree, id)

    private fun children(id: String): List<Row> {
        val columns = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
            DocumentsContract.Document.COLUMN_LAST_MODIFIED,
        )
        val cursor = try {
            resolver.query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, id), columns, null, null, null)
        } catch (_: Exception) {
            null
        } ?: throw FileProblem("Access denied. Add the folder again to give ForkMesh access.")
        return cursor.use {
            buildList {
                while (it.moveToNext()) {
                    add(Row(it.getString(0), it.getString(1) ?: "", it.getString(2) ?: "", if (it.isNull(3)) 0L else it.getLong(3), if (it.isNull(4)) 0L else it.getLong(4)))
                }
            }
        }
    }

    @Synchronized
    private fun id(path: String): String {
        val key = FilesModel.join("", path)
        ids[key]?.let { return it }
        val parentId = id(FilesModel.parent(key))
        val name = FilesModel.name(key)
        val row = children(parentId).firstOrNull { it.name == name } ?: throw FileProblem(SmbStatus.message(SmbStatus.OBJECT_NAME_NOT_FOUND))
        ids[key] = row.id
        return row.id
    }

    private fun idOrNull(path: String): String? = try {
        id(path)
    } catch (_: FileProblem) {
        null
    }

    @Synchronized
    private fun forget(path: String) {
        val key = FilesModel.join("", path)
        ids.keys.filter { it.isNotEmpty() && FilesModel.isInside(it, key) }.forEach { ids.remove(it) }
    }

    private fun item(row: Row, path: String): FileItem {
        val folder = row.mime == DocumentsContract.Document.MIME_TYPE_DIR
        return FileItem(row.name, path, folder, if (folder) 0L else row.size, row.modified, row.name.startsWith("."))
    }

    override fun list(path: String): List<FileItem> {
        val rows = children(id(path))
        synchronized(this) { rows.forEach { ids[FilesModel.join(path, it.name)] = it.id } }
        return rows.map { item(it, FilesModel.join(path, it.name)) }
    }

    override fun stat(path: String): FileItem {
        if (FilesModel.parts(path).isEmpty()) return FileItem(title, "", true, 0L, 0L, false)
        val parent = FilesModel.parent(path)
        val row = children(id(parent)).firstOrNull { it.name == FilesModel.name(path) } ?: throw FileProblem(SmbStatus.message(SmbStatus.OBJECT_NAME_NOT_FOUND))
        return item(row, FilesModel.join("", path))
    }

    override fun readTo(path: String, out: OutputStream, limit: Long, progress: (Long, Long) -> Boolean): Long {
        val entry = stat(path)
        if (entry.size > limit) throw FileProblem(SmbStatus.message(SmbStatus.TOO_LARGE))
        val input = resolver.openInputStream(uri(id(path))) ?: throw FileProblem("Access denied.")
        return input.use { FilesModel.pump(it, out, entry.size, progress) }
    }

    override fun writeFrom(path: String, input: InputStream, length: Long, progress: (Long, Long) -> Boolean): Long {
        val existing = idOrNull(path)
        val target = if (existing != null) {
            uri(existing)
        } else {
            val name = FilesModel.name(path)
            val mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(FilesModel.extension(name)) ?: "application/octet-stream"
            val created = DocumentsContract.createDocument(resolver, uri(id(FilesModel.parent(path))), mime, name) ?: throw FileProblem("Access denied.")
            synchronized(this) { ids[FilesModel.join("", path)] = DocumentsContract.getDocumentId(created) }
            created
        }
        val output = resolver.openOutputStream(target, "wt") ?: throw FileProblem("Access denied.")
        return output.use { FilesModel.pump(input, it, length, progress) }
    }

    override fun mkdir(path: String) {
        if (idOrNull(path) != null) throw FileProblem(SmbStatus.message(SmbStatus.OBJECT_NAME_COLLISION))
        val created = DocumentsContract.createDocument(resolver, uri(id(FilesModel.parent(path))), DocumentsContract.Document.MIME_TYPE_DIR, FilesModel.name(path))
            ?: throw FileProblem("Access denied.")
        synchronized(this) { ids[FilesModel.join("", path)] = DocumentsContract.getDocumentId(created) }
    }

    override fun delete(path: String) {
        if (FilesModel.parts(path).isEmpty()) throw FileProblem("Access denied.")
        val ok = try {
            DocumentsContract.deleteDocument(resolver, uri(id(path)))
        } catch (_: Exception) {
            false
        }
        if (!ok) throw FileProblem("Access denied.")
        forget(path)
    }

    override fun rename(path: String, newPath: String) {
        if (idOrNull(newPath) != null) throw FileProblem(SmbStatus.message(SmbStatus.OBJECT_NAME_COLLISION))
        var current = uri(id(path))
        try {
            if (FilesModel.parent(path) != FilesModel.parent(newPath)) {
                current = DocumentsContract.moveDocument(resolver, current, uri(id(FilesModel.parent(path))), uri(id(FilesModel.parent(newPath))))
                    ?: throw FileProblem("This folder cannot move files. Copy them instead.")
            }
            if (FilesModel.name(path) != FilesModel.name(newPath)) {
                DocumentsContract.renameDocument(resolver, current, FilesModel.name(newPath)) ?: throw FileProblem("Access denied.")
            }
        } catch (problem: FileProblem) {
            throw problem
        } catch (_: Exception) {
            throw FileProblem("This folder cannot move or rename that. Copy it instead.")
        } finally {
            forget(path)
            forget(newPath)
        }
    }

    override fun location(path: String): String = listOf(title).plus(FilesModel.parts(path)).joinToString("/")
}
