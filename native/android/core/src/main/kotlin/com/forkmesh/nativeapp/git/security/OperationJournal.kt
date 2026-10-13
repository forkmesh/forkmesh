package com.forkmesh.nativeapp.git.security

import com.forkmesh.nativeapp.git.bytes.latin1Decode
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.worktree.FileSystemEntityType
import com.forkmesh.nativeapp.git.worktree.createDirectories
import com.forkmesh.nativeapp.git.worktree.createFileExclusive
import com.forkmesh.nativeapp.git.worktree.directoryExists
import com.forkmesh.nativeapp.git.worktree.entityType
import com.forkmesh.nativeapp.git.worktree.fileExists
import com.forkmesh.nativeapp.git.worktree.listEntries
import com.forkmesh.nativeapp.git.worktree.nowMicros
import com.forkmesh.nativeapp.git.worktree.writeStringFlushed
import java.io.File
import java.io.FileInputStream

class GitOperationJournal private constructor(private val file: File) {
    fun complete() {
        if (fileExists(file)) file.delete()
    }

    companion object {
        private val operationPattern = Regex("[a-z][a-z-]{0,63}")
        private val entryPattern = Regex("forkmesh-journal-v1\noperation=[a-z][a-z-]{0,63}\nstate=prepared\n")

        fun begin(gitDirectory: File, operation: String): GitOperationJournal {
            if (!operationPattern.matches(operation)) {
                throw IllegalArgumentException("Invalid argument (operation): $operation")
            }
            val directory = File(gitDirectory, "forkmesh-journal")
            createDirectories(directory)
            for (attempt in 0 until 32) {
                val file = File(directory, "${nowMicros()}-$attempt.journal")
                if (!createFileExclusive(file)) continue
                writeStringFlushed(file, "forkmesh-journal-v1\noperation=$operation\nstate=prepared\n")
                return GitOperationJournal(file)
            }
            throw GitException(GitErrorCode.refLocked, "could not allocate operation journal")
        }

        fun recover(gitDirectory: File) {
            val directory = File(gitDirectory, "forkmesh-journal")
            if (!directoryExists(directory)) return
            for (entity in listEntries(directory)) {
                if (entityType(entity) != FileSystemEntityType.file || !entity.path.endsWith(".journal")) {
                    throw GitException(GitErrorCode.operationInterrupted, "operation journal directory is malformed")
                }
                val bytes = readAtMost(entity, 1025)
                if (bytes.size > 1024 || !isValid(bytes)) {
                    throw GitException(GitErrorCode.operationInterrupted, "operation journal entry is malformed")
                }
                entity.delete()
            }
        }

        private fun isValid(bytes: ByteArray): Boolean = entryPattern.matches(latin1Decode(bytes))

        private fun readAtMost(file: File, limit: Int): ByteArray = FileInputStream(file).use { input ->
            val buffer = ByteArray(limit)
            var total = 0
            while (total < limit) {
                val read = input.read(buffer, total, limit - total)
                if (read == -1) break
                total += read
            }
            buffer.copyOf(total)
        }
    }
}
