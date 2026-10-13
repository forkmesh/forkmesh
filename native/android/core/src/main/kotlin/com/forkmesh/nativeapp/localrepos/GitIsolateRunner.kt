package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.security.GitCancellationToken
import java.util.concurrent.atomic.AtomicReference
import java.util.concurrent.locks.ReentrantLock

class GitWorkerException(override val message: String, val detail: String = "") : RuntimeException(message) {
    override fun toString(): String = if (detail.isEmpty()) message else "$message ($detail)"
}

fun interface GitDurationListener {
    fun onDuration(name: String, elapsedMillis: Long, failed: Boolean)
}

class GitWorkContext internal constructor(
    val cancellation: GitCancellationToken,
    private val sink: (Any) -> Unit,
) {
    fun progress(message: Any) = sink(message)
}

open class GitIsolateRunner(val onDuration: GitDurationListener? = null) {
    private val gate = ReentrantLock(true)
    private val active = AtomicReference<ActiveRun?>(null)

    @Volatile
    private var disposed = false

    private class ActiveRun(val token: GitCancellationToken) {
        @Volatile
        var cancelRequested = false

        @Volatile
        var done = false
    }

    private sealed class Outcome {
        class Ok(val value: Any?) : Outcome()

        class Err(val error: Throwable) : Outcome()
    }

    open val isBusy: Boolean get() = active.get() != null

    open fun <T> run(name: String, body: (GitWorkContext) -> T): T = runWithProgress(name, body, null)

    open fun <T> runWithProgress(
        name: String,
        body: (GitWorkContext) -> T,
        onProgress: ((Any) -> Unit)?,
    ): T {
        if (disposed) throw GitException(GitErrorCode.cancelled, "git worker has been disposed")
        try {
            gate.lockInterruptibly()
        } catch (interrupted: InterruptedException) {
            Thread.currentThread().interrupt()
            throw GitException(GitErrorCode.cancelled, "operation was cancelled")
        }
        val started = System.nanoTime()
        var failed = false
        try {
            if (disposed) throw GitException(GitErrorCode.cancelled, "git worker has been disposed")
            val run = ActiveRun(GitCancellationToken())
            active.set(run)
            try {
                return execute(name, run, body, onProgress)
            } finally {
                run.done = true
                active.compareAndSet(run, null)
            }
        } catch (error: Throwable) {
            failed = true
            throw error
        } finally {
            gate.unlock()
            onDuration?.onDuration(name, (System.nanoTime() - started) / 1_000_000, failed)
        }
    }

    @Suppress("UNCHECKED_CAST")
    private fun <T> execute(
        name: String,
        run: ActiveRun,
        body: (GitWorkContext) -> T,
        onProgress: ((Any) -> Unit)?,
    ): T {
        val context = GitWorkContext(run.token) { message ->
            if (!run.done && !run.cancelRequested) onProgress?.invoke(message)
        }
        val outcome = AtomicReference<Outcome?>(null)
        val worker = Thread({
            outcome.set(
                try {
                    Outcome.Ok(body(context))
                } catch (error: Throwable) {
                    Outcome.Err(error)
                },
            )
        }, "forkmesh-git/$name")
        worker.isDaemon = true
        worker.start()
        var interrupted = false
        while (true) {
            try {
                worker.join()
                break
            } catch (error: InterruptedException) {
                interrupted = true
                cancelRun(run)
            }
        }
        run.done = true
        if (interrupted) Thread.currentThread().interrupt()
        return when (val result = outcome.get()) {
            is Outcome.Ok -> {
                if (interrupted) throw GitException(GitErrorCode.cancelled, "operation was cancelled")
                result.value as T
            }
            is Outcome.Err -> {
                if (run.cancelRequested) throw GitException(GitErrorCode.cancelled, "operation was cancelled")
                throw translate(result.error)
            }
            null -> throw GitException(GitErrorCode.cancelled, "operation was cancelled")
        }
    }

    private fun translate(error: Throwable): Throwable = when (error) {
        is GitException -> error
        is GitWorkerException -> error
        is GitWorkspaceException -> error
        else -> GitWorkerException(
            error.toString(),
            error.stackTrace.take(6).joinToString("\n") { "#  $it" },
        )
    }

    private fun cancelRun(run: ActiveRun) {
        run.cancelRequested = true
        run.token.cancel()
    }

    open fun cancel() {
        active.get()?.let(::cancelRun)
    }

    open fun dispose() {
        disposed = true
        cancel()
    }
}
