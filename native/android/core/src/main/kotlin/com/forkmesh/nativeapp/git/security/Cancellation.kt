package com.forkmesh.nativeapp.git.security

import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException

class GitCancellationToken {
    @Volatile
    private var cancelled = false

    val isCancelled: Boolean get() = cancelled

    fun cancel() {
        cancelled = true
    }

    fun throwIfCancelled() {
        if (cancelled) {
            throw GitException(GitErrorCode.cancelled, "operation was cancelled")
        }
    }
}
