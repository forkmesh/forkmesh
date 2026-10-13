package com.forkmesh.nativeapp.git.worktree

import com.forkmesh.nativeapp.git.bytes.utf8Decode
import com.forkmesh.nativeapp.git.bytes.utf8Encode
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.FileAlreadyExistsException
import java.nio.file.FileVisitResult
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.NoSuchFileException
import java.nio.file.Path
import java.nio.file.SimpleFileVisitor
import java.nio.file.StandardCopyOption
import java.nio.file.attribute.BasicFileAttributes

@Suppress("EnumEntryName")
enum class FileSystemEntityType { file, directory, link, notFound, other }

internal fun entityType(file: File, followLinks: Boolean = false): FileSystemEntityType {
    val attributes = try {
        if (followLinks) {
            Files.readAttributes(file.toPath(), BasicFileAttributes::class.java)
        } else {
            Files.readAttributes(file.toPath(), BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS)
        }
    } catch (error: NoSuchFileException) {
        return FileSystemEntityType.notFound
    } catch (error: IOException) {
        return FileSystemEntityType.notFound
    }
    return when {
        attributes.isSymbolicLink -> FileSystemEntityType.link
        attributes.isDirectory -> FileSystemEntityType.directory
        attributes.isRegularFile -> FileSystemEntityType.file
        else -> FileSystemEntityType.other
    }
}

internal fun fileExists(file: File): Boolean = entityType(file, followLinks = true) == FileSystemEntityType.file

internal fun directoryExists(file: File): Boolean =
    entityType(file, followLinks = true) == FileSystemEntityType.directory

internal fun createDirectories(directory: File) {
    Files.createDirectories(directory.toPath())
}

internal fun createDirectory(directory: File) {
    try {
        Files.createDirectory(directory.toPath())
    } catch (error: FileAlreadyExistsException) {
        if (!directoryExists(directory)) throw error
    }
}

internal fun createFileExclusive(file: File): Boolean = try {
    Files.createFile(file.toPath())
    true
} catch (error: IOException) {
    false
}

internal fun writeBytesFlushed(file: File, bytes: ByteArray) = writeBytes(file, bytes, flush = true)

internal fun writeBytes(file: File, bytes: ByteArray, flush: Boolean) {
    FileOutputStream(file).use { stream ->
        stream.write(bytes)
        stream.flush()
        if (flush) stream.fd.sync()
    }
}

internal fun writeStringFlushed(file: File, text: String) =
    writeBytesFlushed(file, utf8Encode(text))

internal fun readString(file: File): String {
    val bytes = file.readBytes()
    return utf8Decode(bytes)
}

internal fun renameFile(from: File, to: File) {
    try {
        Files.move(from.toPath(), to.toPath(), StandardCopyOption.ATOMIC_MOVE)
    } catch (error: AtomicMoveNotSupportedException) {
        Files.move(from.toPath(), to.toPath(), StandardCopyOption.REPLACE_EXISTING)
    }
}

internal fun deleteRecursive(entity: File) {
    val root = entity.toPath()
    val attributes = try {
        Files.readAttributes(root, BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS)
    } catch (error: NoSuchFileException) {
        return
    }
    if (!attributes.isDirectory) {
        Files.deleteIfExists(root)
        return
    }
    Files.walkFileTree(
        root,
        object : SimpleFileVisitor<Path>() {
            override fun visitFile(file: Path, attrs: BasicFileAttributes): FileVisitResult {
                Files.deleteIfExists(file)
                return FileVisitResult.CONTINUE
            }

            override fun postVisitDirectory(dir: Path, exc: IOException?): FileVisitResult {
                if (exc != null) throw exc
                Files.deleteIfExists(dir)
                return FileVisitResult.CONTINUE
            }
        },
    )
}

internal fun listEntries(directory: File): List<File> {
    val names = directory.list() ?: throw IOException("Directory listing failed: ${directory.path}")
    return names.map { File(directory, it) }
}

internal fun walkEntries(root: File, visit: (File, FileSystemEntityType) -> Unit) {
    Files.walkFileTree(
        root.toPath(),
        object : SimpleFileVisitor<Path>() {
            override fun preVisitDirectory(dir: Path, attrs: BasicFileAttributes): FileVisitResult {
                if (dir != root.toPath()) visit(dir.toFile(), FileSystemEntityType.directory)
                return FileVisitResult.CONTINUE
            }

            override fun visitFile(file: Path, attrs: BasicFileAttributes): FileVisitResult {
                val type = when {
                    attrs.isSymbolicLink -> FileSystemEntityType.link
                    attrs.isDirectory -> FileSystemEntityType.directory
                    attrs.isRegularFile -> FileSystemEntityType.file
                    else -> FileSystemEntityType.other
                }
                visit(file.toFile(), type)
                return FileVisitResult.CONTINUE
            }
        },
    )
}

internal fun nowMicros(): Long = System.currentTimeMillis() * 1000 + (System.nanoTime() / 1000) % 1000
