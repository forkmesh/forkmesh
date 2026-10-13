package com.forkmesh.nativeapp.feature.agentwatch

import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import com.forkmesh.nativeapp.agentwatch.AgentWatchClock
import com.forkmesh.nativeapp.agentwatch.AgentWatchLoader
import com.forkmesh.nativeapp.agentwatch.AgentWatchTimer
import java.util.concurrent.Executors
import kotlin.math.max

internal class AndroidWatchClock : AgentWatchClock {
    private val origin = SystemClock.elapsedRealtimeNanos()
    private val handler = Handler(Looper.getMainLooper())

    override val now: Double get() = (SystemClock.elapsedRealtimeNanos() - origin) / 1e9

    override fun after(seconds: Double, action: () -> Unit): AgentWatchTimer {
        var cancelled = false
        val runnable = Runnable { if (!cancelled) action() }
        handler.postDelayed(runnable, (max(0.0, seconds) * 1000).toLong())
        return AgentWatchTimer {
            cancelled = true
            handler.removeCallbacks(runnable)
        }
    }
}

private val watchLoads = Executors.newFixedThreadPool(3) { Thread(it, "AgentWatchLoad").apply { isDaemon = true } }

internal fun backgroundWatchLoader(): AgentWatchLoader {
    val main = Handler(Looper.getMainLooper())
    return AgentWatchLoader { target, done ->
        watchLoads.execute {
            val result = runCatching { target.load() }
            main.post { done(result.getOrNull(), result.exceptionOrNull()) }
        }
    }
}
