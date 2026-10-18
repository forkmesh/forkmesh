package com.forkmesh.nativeapp.files

import java.io.Closeable
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.nio.file.Files

data class FileItem(
    val name: String,
    val path: String,
    val isFolder: Boolean,
    val size: Long,
    val modified: Long,
    val hidden: Boolean,
) {
    val kind: FileKind get() = if (isFolder) FileKind.Folder else FilesModel.kind(name)
}

class FileProblem(message: String) : Exception(message)

interface FileSource : Closeable {
    val title: String

    fun list(path: String): List<FileItem>

    fun stat(path: String): FileItem

    fun readTo(path: String, out: OutputStream, limit: Long = Long.MAX_VALUE, progress: (Long, Long) -> Boolean = { _, _ -> true }): Long

    fun writeFrom(path: String, input: InputStream, length: Long, progress: (Long, Long) -> Boolean = { _, _ -> true }): Long

    fun mkdir(path: String)

    fun delete(path: String)

    fun rename(path: String, newPath: String)

    fun location(path: String): String

    override fun close() {}
}

class LocalFileSource(private val root: File, override val title: String) : FileSource {
    fun file(path: String): File {
        val parts = FilesModel.parts(path)
        if (parts.any { it == ".." || it == "." }) throw FileProblem("That path is outside this location.")
        return parts.fold(root) { folder, part -> File(folder, part) }
    }

    private fun item(file: File, path: String) = FileItem(file.name, path, file.isDirectory, if (file.isDirectory) 0L else file.length(), file.lastModified(), file.isHidden || file.name.startsWith("."))

    override fun list(path: String): List<FileItem> {
        val folder = file(path)
        if (!folder.isDirectory) throw FileProblem("That file or folder no longer exists.")
        val children = folder.listFiles() ?: throw FileProblem("Access denied.")
        return children.map { item(it, FilesModel.join(path, it.name)) }
    }

    override fun stat(path: String): FileItem {
        val target = file(path)
        if (!target.exists()) throw FileProblem("That file or folder no longer exists.")
        return item(target, path)
    }

    override fun readTo(path: String, out: OutputStream, limit: Long, progress: (Long, Long) -> Boolean): Long {
        val target = file(path)
        if (!target.isFile) throw FileProblem("That file or folder no longer exists.")
        val total = target.length()
        if (total > limit) throw FileProblem(SmbStatus.message(SmbStatus.TOO_LARGE))
        return target.inputStream().use { FilesModel.pump(it, out, total, progress) }
    }

    override fun writeFrom(path: String, input: InputStream, length: Long, progress: (Long, Long) -> Boolean): Long {
        val target = file(path)
        return target.outputStream().use { FilesModel.pump(input, it, length, progress) }
    }

    override fun mkdir(path: String) {
        val target = file(path)
        if (target.exists()) throw FileProblem(SmbStatus.message(SmbStatus.OBJECT_NAME_COLLISION))
        if (!target.mkdirs()) throw FileProblem("Access denied.")
    }

    override fun delete(path: String) {
        if (FilesModel.parts(path).isEmpty()) throw FileProblem("Access denied.")
        val target = file(path)
        if (!target.exists()) throw FileProblem("That file or folder no longer exists.")
        if (target.isDirectory && !Files.isSymbolicLink(target.toPath())) {
            for (child in list(path)) delete(child.path)
        }
        if (!target.delete()) throw FileProblem(if (target.isDirectory) SmbStatus.message(SmbStatus.DIRECTORY_NOT_EMPTY) else "Access denied.")
    }

    override fun rename(path: String, newPath: String) {
        val from = file(path)
        val to = file(newPath)
        if (to.exists()) throw FileProblem(SmbStatus.message(SmbStatus.OBJECT_NAME_COLLISION))
        if (!from.renameTo(to)) throw FileProblem("Access denied.")
    }

    override fun location(path: String): String = file(path).absolutePath
}

class SmbFileSource(private val client: SmbClient, override val title: String) : FileSource {
    private fun item(entry: SmbEntry, path: String) = FileItem(entry.name, path, entry.isFolder, entry.size, entry.modified, entry.hidden)

    override fun list(path: String): List<FileItem> = client.run { share -> share.list(path).map { item(it, FilesModel.join(path, it.name)) } }

    override fun stat(path: String): FileItem = client.run { item(it.stat(path), path) }

    override fun readTo(path: String, out: OutputStream, limit: Long, progress: (Long, Long) -> Boolean): Long =
        client.run { it.readTo(path, out, limit, progress) }

    override fun writeFrom(path: String, input: InputStream, length: Long, progress: (Long, Long) -> Boolean): Long =
        client.run { it.writeFrom(path, input, length, progress) }

    override fun mkdir(path: String) = client.run { it.mkdir(path) }

    override fun delete(path: String) = client.run { it.delete(path) }

    override fun rename(path: String, newPath: String) = client.run { it.rename(path, newPath) }

    override fun location(path: String): String = FilesModel.smbUrl(client.host, client.share, path)

    override fun close() = client.close()
}

object FileTransfer {
    fun copy(
        from: FileSource,
        path: String,
        to: FileSource,
        folder: String,
        name: String,
        temp: () -> File,
        progress: (String) -> Boolean = { true },
    ) {
        if (from === to && FilesModel.isInside(folder, path)) throw FileProblem("A folder cannot be copied into itself.")
        val item = from.stat(path)
        val target = FilesModel.join(folder, name)
        if (!progress(item.name)) throw FileProblem(SmbStatus.message(SmbStatus.CANCELLED))
        if (item.isFolder) {
            to.mkdir(target)
            for (child in from.list(path)) copy(from, child.path, to, target, child.name, temp, progress)
            return
        }
        val scratch = temp()
        try {
            scratch.outputStream().use { from.readTo(path, it) }
            scratch.inputStream().use { to.writeFrom(target, it, scratch.length()) }
        } finally {
            scratch.delete()
        }
    }

    fun move(
        from: FileSource,
        path: String,
        to: FileSource,
        folder: String,
        name: String,
        temp: () -> File,
        progress: (String) -> Boolean = { true },
    ) {
        if (from === to) {
            if (FilesModel.isInside(folder, path)) throw FileProblem("A folder cannot be moved into itself.")
            from.rename(path, FilesModel.join(folder, name))
            return
        }
        copy(from, path, to, folder, name, temp, progress)
        from.delete(path)
    }
}
