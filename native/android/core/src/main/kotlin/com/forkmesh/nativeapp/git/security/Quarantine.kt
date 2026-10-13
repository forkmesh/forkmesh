package com.forkmesh.nativeapp.git.security

import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.worktree.createDirectories
import com.forkmesh.nativeapp.git.worktree.createDirectory
import com.forkmesh.nativeapp.git.worktree.deleteRecursive
import com.forkmesh.nativeapp.git.worktree.directoryExists
import com.forkmesh.nativeapp.git.worktree.fileExists
import com.forkmesh.nativeapp.git.worktree.listEntries
import com.forkmesh.nativeapp.git.worktree.nowMicros
import com.forkmesh.nativeapp.git.worktree.writeBytesFlushed
import java.io.File
import java.security.SecureRandom

class GitObjectQuarantine private constructor(
    val directory: File,
    private val cancellation: GitCancellationToken?,
    private val maximumStagedBytes: Int,
) {
    fun stage(name: String, bytes: ByteArray): File {
        if (!entryName.matches(name)) {
            throw GitException(GitErrorCode.quarantineRejected, "quarantine entry name is invalid")
        }
        try {
            cancellation?.throwIfCancelled()
            if (bytes.size > maximumStagedBytes) {
                throw GitException(GitErrorCode.resourceLimitExceeded, "quarantine entry exceeds the operation limit")
            }
            val destination = File(directory, name)
            if (fileExists(destination)) {
                throw GitException(GitErrorCode.quarantineRejected, "quarantine entry already exists")
            }
            writeBytesFlushed(destination, bytes)
            cancellation?.throwIfCancelled()
            return destination
        } catch (error: Throwable) {
            reject()
            throw error
        }
    }

    fun reject() {
        if (directoryExists(directory)) deleteRecursive(directory)
    }

    companion object {
        private val entryName = Regex("[A-Za-z0-9][A-Za-z0-9._-]{0,127}")
        private val random = SecureRandom()

        fun begin(
            gitDirectory: File,
            cancellation: GitCancellationToken? = null,
            limits: GitResourceLimits? = null,
        ): GitObjectQuarantine {
            val root = File(gitDirectory, "forkmesh-quarantine")
            createDirectories(root)
            for (attempt in 0 until 32) {
                val suffix = (random.nextInt().toLong() and 0xffffffffL).toString(16)
                val directory = File(root, "${nowMicros()}-$suffix-$attempt")
                if (directoryExists(directory)) continue
                createDirectory(directory)
                return GitObjectQuarantine(
                    directory,
                    cancellation,
                    limits?.maxInflatedBytes ?: GitResourceLimits.defaultMaxInflatedBytes,
                )
            }
            throw GitException(GitErrorCode.quarantineRejected, "could not allocate operation quarantine")
        }

        fun recover(gitDirectory: File) {
            val root = File(gitDirectory, "forkmesh-quarantine")
            if (!directoryExists(root)) return
            for (entity in listEntries(root)) {
                deleteRecursive(entity)
            }
        }
    }
}
