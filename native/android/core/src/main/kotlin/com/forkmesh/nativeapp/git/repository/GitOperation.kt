package com.forkmesh.nativeapp.git.repository

import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.security.GitCancellationToken
import java.util.concurrent.atomic.AtomicInteger

@Suppress("EnumEntryName")
enum class GitProgressPhase {
    connecting,
    listingRefs,
    negotiating,
    receivingPack,
    indexingPack,
    updatingRefs,
    checkingOut,
    preparingPack,
    sendingPack,
    awaitingReport,
    completed,
}

class GitProgress(
    val phase: GitProgressPhase,
    val bytes: Long = 0,
    val objects: Int = 0,
    val refs: Int = 0,
    val total: Int = 0,
    val remoteHost: String? = null,
) {
    override fun toString(): String =
        "GitProgress(${phase.name}, bytes: $bytes, objects: $objects, refs: $refs, total: $total)"
}

fun interface GitProgressListener {
    fun onProgress(progress: GitProgress)
}

sealed class GitResult<T>

class GitSuccess<T>(val value: T) : GitResult<T>()

class GitFailure<T>(val code: GitErrorCode, val message: String) : GitResult<T>() {
    fun toException(): GitException = GitException(code, message)
}

class GitOperation<T> private constructor(
    val id: Int,
    val cancellation: GitCancellationToken,
    val result: GitResult<T>,
) {
    val value: T
        get() = when (val outcome = result) {
            is GitSuccess -> outcome.value
            is GitFailure -> throw outcome.toException()
        }

    fun cancel() = cancellation.cancel()

    companion object {
        private val nextId = AtomicInteger(0)

        fun <T> start(
            cancellation: GitCancellationToken? = null,
            progress: GitProgressListener? = null,
            body: (GitCancellationToken, GitProgressSink) -> T,
        ): GitOperation<T> {
            val token = cancellation ?: GitCancellationToken()
            val sink = GitProgressSink(progress)
            val id = nextId.incrementAndGet()
            val result: GitResult<T> = try {
                val value = body(token, sink)
                sink.add(GitProgress(GitProgressPhase.completed))
                GitSuccess(value)
            } catch (error: GitException) {
                GitFailure(error.code, error.message)
            } catch (error: Throwable) {
                GitFailure(GitErrorCode.operationInterrupted, "unexpected failure: ${error.javaClass.simpleName}")
            } finally {
                sink.close()
            }
            return GitOperation(id, token, result)
        }
    }
}

class GitProgressSink internal constructor(private val listener: GitProgressListener?) {
    private var emitted = 0

    @Volatile
    private var closed = false

    fun add(progress: GitProgress) {
        if (closed || emitted >= maxEvents) return
        emitted += 1
        val target = listener ?: return
        try {
            target.onProgress(progress)
        } catch (error: Exception) {
        }
    }

    internal fun close() {
        closed = true
    }

    private companion object {
        const val maxEvents = 100000
    }
}
