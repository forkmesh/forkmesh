package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.worktree.FileSystemEntityType
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.InputStream
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.attribute.BasicFileAttributes

interface LocalImportDirectory {
    fun isReadableDirectory(): Boolean

    fun list(): List<LocalImportEntry>
}

interface LocalImportEntry {
    val name: String

    val type: FileSystemEntityType

    val size: Long

    fun directory(): LocalImportDirectory

    fun open(): InputStream
}

internal fun readBounded(entry: LocalImportEntry, limit: Int): ByteArray = entry.open().use { input ->
    val out = ByteArrayOutputStream(if (entry.size in 0..limit.toLong()) entry.size.toInt() else 8192)
    val buffer = ByteArray(64 * 1024)
    var total = 0L
    while (total <= limit) {
        val read = input.read(buffer, 0, minOf(buffer.size.toLong(), limit + 1 - total).toInt())
        if (read < 0) break
        out.write(buffer, 0, read)
        total += read
    }
    out.toByteArray()
}

class FileImportDirectory(val directory: File) : LocalImportDirectory {
    override fun isReadableDirectory(): Boolean = typeOf(directory) == FileSystemEntityType.directory

    override fun list(): List<LocalImportEntry> = Files.newDirectoryStream(directory.toPath()).use { stream ->
        stream.map { path -> FileEntry(path.toFile()) }
    }

    private class FileEntry(private val file: File) : LocalImportEntry {
        override val name: String = file.name
        override val type: FileSystemEntityType = typeOf(file)
        override val size: Long
            get() = if (type == FileSystemEntityType.file) file.length() else -1

        override fun directory(): LocalImportDirectory = FileImportDirectory(file)

        override fun open(): InputStream = Files.newInputStream(file.toPath(), LinkOption.NOFOLLOW_LINKS)
    }

    private companion object {
        fun typeOf(file: File): FileSystemEntityType {
            val attributes = try {
                Files.readAttributes(file.toPath(), BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS)
            } catch (error: Exception) {
                return FileSystemEntityType.notFound
            }
            return when {
                attributes.isSymbolicLink -> FileSystemEntityType.link
                attributes.isDirectory -> FileSystemEntityType.directory
                attributes.isRegularFile -> FileSystemEntityType.file
                else -> FileSystemEntityType.other
            }
        }
    }
}
