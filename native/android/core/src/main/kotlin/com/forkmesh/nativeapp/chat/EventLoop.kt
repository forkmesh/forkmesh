package com.forkmesh.nativeapp.chat

import java.util.concurrent.Callable
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.ExecutionException
import java.util.concurrent.Executors
import java.util.concurrent.Future
import java.util.concurrent.FutureTask
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.ScheduledThreadPoolExecutor
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

interface LoopTimer {
    val isActive: Boolean

    fun cancel()
}

class EventLoop(private val name: String, val virtualTime: Boolean = false) {
    private val executor = ScheduledThreadPoolExecutor(1) { runnable ->
        Thread(runnable, name).apply { isDaemon = true }
    }.apply {
        removeOnCancelPolicy = true
        executeExistingDelayedTasksAfterShutdownPolicy = false
        continueExistingPeriodicTasksAfterShutdownPolicy = false
    }

    @Volatile
    private var loopThread: Thread? = null
    private val pending = AtomicInteger()

    val failures = CopyOnWriteArrayList<Throwable>()

    @Volatile
    var isShutdown = false
        private set

    val isOnLoop: Boolean get() = Thread.currentThread() === loopThread

    fun post(block: () -> Unit) {
        pending.incrementAndGet()
        try {
            executor.execute { runTask(block) }
        } catch (_: RejectedExecutionException) {
            pending.decrementAndGet()
        }
    }

    fun <T> call(block: () -> T): T {
        if (isOnLoop || isShutdown) return block()
        val task = FutureTask(Callable { block() })
        pending.incrementAndGet()
        try {
            executor.execute {
                loopThread = Thread.currentThread()
                try {
                    task.run()
                } finally {
                    pending.decrementAndGet()
                }
            }
        } catch (_: RejectedExecutionException) {
            pending.decrementAndGet()
            return block()
        }
        try {
            return task.get()
        } catch (wrapped: ExecutionException) {
            throw wrapped.cause ?: wrapped
        }
    }

    fun io(block: () -> Unit) {
        pending.incrementAndGet()
        try {
            WORKERS.execute {
                try {
                    block()
                } catch (failure: Throwable) {
                    failures.add(failure)
                } finally {
                    pending.decrementAndGet()
                }
            }
        } catch (_: RejectedExecutionException) {
            pending.decrementAndGet()
        }
    }

    fun timer(delayMs: Long, block: () -> Unit): LoopTimer = schedule(delayMs, 0L, block)

    fun periodic(periodMs: Long, block: () -> Unit): LoopTimer = schedule(periodMs, periodMs, block)

    private fun schedule(delayMs: Long, periodMs: Long, block: () -> Unit): LoopTimer {
        val handle = Handle(periodMs, block)
        if (virtualTime) {
            synchronized(virtualTasks) {
                virtualTasks.add(VirtualTask(virtualNow + delayMs.coerceAtLeast(0), sequence.incrementAndGet(), handle))
            }
            return handle
        }
        try {
            handle.future = if (periodMs > 0) {
                executor.scheduleAtFixedRate({ handle.fire() }, delayMs, periodMs, TimeUnit.MILLISECONDS)
            } else {
                executor.schedule({ handle.fire() }, delayMs.coerceAtLeast(0), TimeUnit.MILLISECONDS)
            }
        } catch (_: RejectedExecutionException) {
            handle.cancel()
        }
        return handle
    }

    private inner class Handle(val periodMs: Long, val block: () -> Unit) : LoopTimer {
        @Volatile
        var cancelled = false

        @Volatile
        var fired = false

        @Volatile
        var future: Future<*>? = null

        override val isActive: Boolean get() = !cancelled && (periodMs > 0 || !fired)

        override fun cancel() {
            cancelled = true
            future?.cancel(false)
        }

        fun fire() {
            if (cancelled || (periodMs == 0L && fired)) return
            if (periodMs == 0L) fired = true
            pending.incrementAndGet()
            runTask(block)
        }
    }

    private fun runTask(block: () -> Unit) {
        loopThread = Thread.currentThread()
        try {
            block()
        } catch (failure: Throwable) {
            failures.add(failure)
        } finally {
            pending.decrementAndGet()
        }
    }

    private class VirtualTask(val dueAt: Long, val sequence: Long, val handle: EventLoop.Handle)

    private val virtualTasks = ArrayList<VirtualTask>()
    private val sequence = AtomicLong()

    @Volatile
    private var virtualNow = 0L

    val virtualElapsedMs: Long get() = virtualNow

    fun advance(ms: Long) {
        check(virtualTime) { "advance() needs a virtual-time loop" }
        val target = virtualNow + ms
        while (true) {
            awaitIdle()
            val next = synchronized(virtualTasks) {
                virtualTasks.removeAll { !it.handle.isActive }
                val due = virtualTasks.filter { it.dueAt <= target }.minWithOrNull(compareBy({ it.dueAt }, { it.sequence }))
                if (due != null) {
                    virtualTasks.remove(due)
                    if (due.handle.periodMs > 0) {
                        virtualTasks.add(VirtualTask(due.dueAt + due.handle.periodMs, sequence.incrementAndGet(), due.handle))
                    }
                }
                due
            } ?: break
            virtualNow = next.dueAt
            post { next.handle.fire() }
        }
        virtualNow = target
        awaitIdle()
    }

    fun awaitIdle(timeoutMs: Long = 10_000): Boolean {
        val deadline = System.currentTimeMillis() + timeoutMs
        var quietSince = 0L
        while (System.currentTimeMillis() < deadline) {
            if (pending.get() == 0) {
                if (quietSince != 0L && System.currentTimeMillis() - quietSince >= 4) return true
                if (quietSince == 0L) quietSince = System.currentTimeMillis()
            } else {
                quietSince = 0L
            }
            Thread.sleep(1)
        }
        return pending.get() == 0
    }

    fun shutdown() {
        isShutdown = true
        synchronized(virtualTasks) { virtualTasks.clear() }
        executor.shutdown()
    }

    inner class SerialQueue(laneName: String) {
        private val lane = ThreadPoolExecutor(
            1, 1, 30, TimeUnit.SECONDS, java.util.concurrent.LinkedBlockingQueue(),
        ) { runnable -> Thread(runnable, laneName).apply { isDaemon = true } }.apply { allowCoreThreadTimeOut(true) }

        fun <T> submit(block: () -> T): Future<T> {
            pending.incrementAndGet()
            val future = CompletableFuture<T>()
            try {
                lane.execute {
                    try {
                        future.complete(block())
                    } catch (failure: Throwable) {
                        future.completeExceptionally(failure)
                    } finally {
                        pending.decrementAndGet()
                    }
                }
            } catch (rejected: RejectedExecutionException) {
                pending.decrementAndGet()
                future.completeExceptionally(rejected)
            }
            return future
        }

        fun execute(block: () -> Unit) {
            submit {
                try {
                    block()
                } catch (failure: Throwable) {
                    failures.add(failure)
                }
            }
        }

        fun shutdown() {
            lane.shutdown()
        }
    }

    companion object {
        private val WORKERS = Executors.newCachedThreadPool { runnable ->
            Thread(runnable, "EventLoop-io").apply { isDaemon = true }
        }

        fun <T> done(value: T): Future<T> = CompletableFuture.completedFuture(value)
    }
}

internal fun dartErrorText(error: Throwable): String {
    if (error is com.forkmesh.nativeapp.api.DartException) return error.toString()
    val message = error.message
    return if (message.isNullOrEmpty()) error.javaClass.simpleName else "${error.javaClass.simpleName}: $message"
}
