package com.forkmesh.nativeapp.git.index

import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.security.GitResourceLimits
import com.forkmesh.nativeapp.git.worktree.createFileExclusive
import com.forkmesh.nativeapp.git.worktree.fileExists
import com.forkmesh.nativeapp.git.worktree.renameFile
import com.forkmesh.nativeapp.git.worktree.writeBytesFlushed
import java.io.File

class GitIndexStore(val gitDirectory: File, val limits: GitResourceLimits) {
    val file: File get() = File(gitDirectory, "index")

    fun read(): GitIndexV2 {
        if (!fileExists(file)) return GitIndexV2(emptyList())
        return GitIndexV2.parse(file.readBytes(), limits)
    }

    fun write(index: GitIndexV2) {
        val destination = file
        val lock = File("${destination.path}.lock")
        if (!createFileExclusive(lock)) {
            throw GitException(GitErrorCode.refLocked, "index lock already exists")
        }
        try {
            writeBytesFlushed(lock, index.encode())
            renameFile(lock, destination)
        } finally {
            if (fileExists(lock)) lock.delete()
        }
    }
}
