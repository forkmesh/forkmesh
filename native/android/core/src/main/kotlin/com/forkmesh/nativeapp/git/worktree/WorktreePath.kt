package com.forkmesh.nativeapp.git.worktree

import com.forkmesh.nativeapp.git.bytes.utf8Encode
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.nio.file.Paths

class GitWorktreePath(val value: String) {
    val bytes: ByteArray = utf8Encode(value)
    val components: List<String> = value.split("/")

    init {
        if (value.isEmpty() ||
            value.startsWith("/") ||
            value.startsWith("\\") ||
            bytes.size > 4096 ||
            value.contains("\\") ||
            value.any { char -> char.code == 0 || char.code < 0x20 || char.code == 0x7f }
        ) {
            throw GitException(GitErrorCode.pathEscapesWorktree, "path is not portable")
        }
        for (component in components) {
            if (component.isEmpty() ||
                component == "." ||
                component == ".." ||
                utf8Encode(component).size > 255 ||
                component.endsWith(".") ||
                component.endsWith(" ") ||
                component.contains(":") ||
                isReservedDeviceName(component)
            ) {
                throw GitException(GitErrorCode.pathEscapesWorktree, "path is not portable")
            }
        }
    }

    override fun equals(other: Any?): Boolean = other is GitWorktreePath && other.value == value

    override fun hashCode(): Int = value.hashCode()

    override fun toString(): String = value
}

class GitWorktree(val root: File) {
    fun writeFile(path: GitWorktreePath, bytes: ByteArray, flush: Boolean = true) {
        val file = safeFile(path, createParents = true)
        writeBytes(file, bytes, flush)
    }

    fun removeEmptyParents(path: GitWorktreePath) {
        var components = path.components.dropLast(1)
        while (components.isNotEmpty()) {
            val directory = File(root, components.joinToString(File.separator))
            if (entityType(directory) != FileSystemEntityType.directory) return
            try {
                Files.delete(directory.toPath())
            } catch (error: IOException) {
                return
            }
            components = components.dropLast(1)
        }
    }

    fun readFile(path: GitWorktreePath): ByteArray {
        val file = safeFile(path, createParents = false)
        if (!fileExists(file)) {
            throw GitException(GitErrorCode.invalidRepository, "worktree file is missing")
        }
        return file.readBytes()
    }

    fun readSymlink(path: GitWorktreePath): String {
        val file = safeFile(path, createParents = false, allowFinalSymlink = true)
        if (entityType(file) != FileSystemEntityType.link) {
            throw GitException(GitErrorCode.invalidRepository, "worktree path is not a symlink")
        }
        val target = Files.readSymbolicLink(file.toPath()).toString()
        validateSymlinkTarget(target)
        return target
    }

    fun deleteFile(path: GitWorktreePath) {
        val file = safeFile(path, createParents = false)
        if (fileExists(file)) Files.delete(file.toPath())
    }

    fun deleteEntry(path: GitWorktreePath) {
        val file = safeFile(path, createParents = false, allowFinalSymlink = true)
        val type = entityType(file)
        if (type == FileSystemEntityType.file || type == FileSystemEntityType.link) Files.delete(file.toPath())
    }

    fun writeSymlink(path: GitWorktreePath, target: String) {
        validateSymlinkTarget(target)
        val file = safeFile(path, createParents = true, allowFinalSymlink = true)
        val type = entityType(file)
        if (type == FileSystemEntityType.directory) {
            throw GitException(GitErrorCode.pathEscapesWorktree, "cannot replace a directory with a symlink")
        }
        if (type != FileSystemEntityType.notFound) Files.delete(file.toPath())
        Files.createSymbolicLink(file.toPath(), Paths.get(target))
    }

    fun typeOf(path: GitWorktreePath): FileSystemEntityType {
        val file = safeFile(path, createParents = false, allowFinalSymlink = true)
        return entityType(file)
    }

    private fun safeFile(path: GitWorktreePath, createParents: Boolean, allowFinalSymlink: Boolean = false): File {
        if (entityType(root) == FileSystemEntityType.link) {
            throw GitException(GitErrorCode.symlinkEscape, "worktree root must not be a symlink")
        }
        var current = root
        for (component in path.components.subList(0, path.components.size - 1)) {
            val next = File(current, component)
            var type = entityType(next)
            if (type == FileSystemEntityType.link) {
                throw GitException(GitErrorCode.symlinkEscape, "worktree parent is a symlink")
            }
            if (type == FileSystemEntityType.notFound && createParents) {
                createDirectory(next)
                type = entityType(next)
            }
            if (type != FileSystemEntityType.directory) {
                throw GitException(GitErrorCode.pathEscapesWorktree, "worktree parent is not a directory")
            }
            current = next
        }
        val file = File(current, path.components.last())
        if (!allowFinalSymlink && entityType(file) == FileSystemEntityType.link) {
            throw GitException(GitErrorCode.symlinkEscape, "worktree destination is a symlink")
        }
        return file
    }
}

private val reservedPortName = Regex("(COM|LPT)[1-9]")

private fun isReservedDeviceName(component: String): Boolean {
    val stem = component.split(".").first().uppercase()
    return stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || reservedPortName.matches(stem)
}

private fun validateSymlinkTarget(target: String) {
    if (target.isEmpty() ||
        target.startsWith("/") ||
        target.startsWith("\\") ||
        target.contains("\\") ||
        target.split("/").any { part -> part.isEmpty() || part == "." || part == ".." }
    ) {
        throw GitException(GitErrorCode.symlinkEscape, "symlink target escapes worktree")
    }
}
