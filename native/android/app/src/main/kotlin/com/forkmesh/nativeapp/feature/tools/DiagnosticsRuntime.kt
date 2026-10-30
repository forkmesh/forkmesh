package com.forkmesh.nativeapp.feature.tools

import android.app.Activity
import android.app.Application
import android.content.pm.ApplicationInfo
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.Process
import android.os.SystemClock
import android.util.Log
import android.view.FrameMetrics
import android.view.Window
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.tools.AppBuild
import com.forkmesh.nativeapp.tools.AppLog
import com.forkmesh.nativeapp.tools.AppTelemetry
import com.forkmesh.nativeapp.tools.DebugPrint
import com.forkmesh.nativeapp.tools.HeartbeatScheduler
import com.forkmesh.nativeapp.tools.LogcatLines
import com.forkmesh.nativeapp.tools.PerformanceMonitorService
import com.forkmesh.nativeapp.tools.StallLogFile
import java.util.Locale
import java.util.WeakHashMap
import kotlin.concurrent.thread
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

internal object DiagnosticsRuntime {
    const val LOG_TAG = "ForkMeshPrint"

    @Volatile
    var build: AppBuild = AppBuild.UNKNOWN
        private set

    @Volatile
    var debuggable: Boolean = false
        private set

    private val visible = MutableStateFlow(true)
    private var installed = false
    private val registered = ArrayList<() -> Unit>()
    private var sampler: Job? = null

    fun install(app: App) {
        if (installed) {
            registered.forEach { it() }
            return
        }
        installed = true
        debuggable = (app.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0
        build = androidBuild(app, debuggable)

        DebugPrint.sink = { message -> if (message != null) Log.i(LOG_TAG, message) }
        val appLog = Services.put(AppLog())
        registered += { Services.put(appLog) }
        appLog.captureProcessLogs()
        LogcatCapture(appLog).start()

        val monitor = Services.put(
            PerformanceMonitorService(
                stallLog = StallLogFile(directory = { app.filesDir }, build = build),
                scheduler = MainLooperScheduler,
            ),
        )
        val telemetry = Services.put(
            AppTelemetry(log = appLog, monitor = monitor, relayFrames = { ToolsFeature.relayFrames() }),
        )
        registered += { Services.put(monitor) }
        registered += { Services.put(telemetry) }
        monitor.onHeartbeatLag = telemetry::recordLoopLag
        monitor.screenName = { NavigationFeature.screenTitle() }
        observeHttp(monitor, telemetry)

        app.registerActivityLifecycleCallbacks(Visibility(FrameMetricsWatcher(monitor, telemetry), monitor))
        AppLifecycle.scope.launch {
            visible.collect { shown ->
                if (shown) {
                    monitor.startHeartbeat()
                    startSampler(telemetry)
                } else {
                    monitor.stopHeartbeat()
                    stopSampler()
                }
            }
        }
    }

    private fun startSampler(telemetry: AppTelemetry) {
        if (sampler != null) return
        sampler = AppLifecycle.scope.launch {
            val interval = telemetry.sampleIntervalMs
            var next = SystemClock.uptimeMillis() + interval
            while (isActive) {
                delay(maxOf(0L, next - SystemClock.uptimeMillis()))
                telemetry.sampleNow()
                next += interval
                val now = SystemClock.uptimeMillis()
                if (next <= now) next = now + interval
            }
        }
    }

    private fun stopSampler() {
        sampler?.cancel()
        sampler = null
    }

    private fun observeHttp(monitor: PerformanceMonitorService, telemetry: AppTelemetry) {
        val previous = HttpClient.observer
        HttpClient.observer = HttpClient.Observer { method, url, status, bytesOut, bytesIn, elapsedMs, error ->
            previous?.onExchange(method, url, status, bytesOut, bytesIn, elapsedMs, error)
            telemetry.recordRequestFinished(method, url, if (error != null) null else status, elapsedMs, error, bytesOut, bytesIn)
            monitor.recordExchange(method, url, status, elapsedMs * 1000, error)
        }
    }

    @Suppress("DEPRECATION")
    private fun androidBuild(app: App, debuggable: Boolean): AppBuild {
        val info = try {
            app.packageManager.getPackageInfo(app.packageName, 0)
        } catch (_: Exception) {
            null
        }
        val code = when {
            info == null -> 0L
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.P -> info.longVersionCode
            else -> info.versionCode.toLong()
        }
        return AppBuild(
            version = AppBuild.version(info?.versionName ?: "", code),
            platform = "android",
            osVersion = AppBuild.androidOsVersion(Build.VERSION.RELEASE ?: "", Build.VERSION.SDK_INT),
            mode = AppBuild.mode(debuggable),
        )
    }

    private class Visibility(
        private val frames: FrameMetricsWatcher,
        private val monitor: PerformanceMonitorService,
    ) : Application.ActivityLifecycleCallbacks {
        private var started = 0

        override fun onActivityStarted(activity: Activity) {
            started++
            frames.attach(activity)
            if (debuggable) DevPerformanceOverlay.attach(activity, monitor)
            visible.value = true
        }

        override fun onActivityStopped(activity: Activity) {
            frames.detach(activity)
            started = maxOf(0, started - 1)
            if (started == 0 && !activity.isChangingConfigurations) visible.value = false
        }

        override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) {}

        override fun onActivityResumed(activity: Activity) {}

        override fun onActivityPaused(activity: Activity) {}

        override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) {}

        override fun onActivityDestroyed(activity: Activity) {}
    }

    private object MainLooperScheduler : HeartbeatScheduler {
        private val handler = Handler(Looper.getMainLooper())

        override fun schedule(delayMs: Long, action: () -> Unit): () -> Unit {
            val runnable = Runnable { action() }
            handler.postDelayed(runnable, delayMs)
            return { handler.removeCallbacks(runnable) }
        }
    }

    private class FrameMetricsWatcher(
        private val monitor: PerformanceMonitorService,
        private val telemetry: AppTelemetry,
    ) {
        private val handler: Handler by lazy {
            Handler(HandlerThread("forkmesh-frame-metrics").apply { start() }.looper)
        }
        private val listeners = WeakHashMap<Activity, Window.OnFrameMetricsAvailableListener>()

        fun attach(activity: Activity) {
            if (listeners.containsKey(activity)) return
            val listener = Window.OnFrameMetricsAvailableListener { _, metrics, dropped ->
                val buildNanos = metrics.getMetric(FrameMetrics.INPUT_HANDLING_DURATION) +
                    metrics.getMetric(FrameMetrics.ANIMATION_DURATION) +
                    metrics.getMetric(FrameMetrics.LAYOUT_MEASURE_DURATION) +
                    metrics.getMetric(FrameMetrics.DRAW_DURATION)
                val rasterNanos = metrics.getMetric(FrameMetrics.SYNC_DURATION) +
                    metrics.getMetric(FrameMetrics.COMMAND_ISSUE_DURATION) +
                    metrics.getMetric(FrameMetrics.SWAP_BUFFERS_DURATION)
                val totalNanos = metrics.getMetric(FrameMetrics.TOTAL_DURATION)
                repeat(dropped) { telemetry.recordFrame(0) }
                telemetry.recordFrame(totalNanos / 1000)
                monitor.recordFrameDelay(
                    buildMicros = buildNanos / 1000,
                    rasterMicros = rasterNanos / 1000,
                    vsyncMicros = metrics.getMetric(FrameMetrics.UNKNOWN_DELAY_DURATION) / 1000,
                    totalMicros = totalNanos / 1000,
                )
            }
            try {
                activity.window.addOnFrameMetricsAvailableListener(listener, handler)
                listeners[activity] = listener
            } catch (_: RuntimeException) {
            }
        }

        fun detach(activity: Activity) {
            val listener = listeners.remove(activity) ?: return
            try {
                activity.window.removeOnFrameMetricsAvailableListener(listener)
            } catch (_: RuntimeException) {
            }
        }
    }

    private class LogcatCapture(private val log: AppLog) {
        fun start() {
            thread(name = "forkmesh-logcat", isDaemon = true) { run() }
        }

        private fun run() {
            var since = System.currentTimeMillis()
            var quickExits = 0
            while (quickExits < 3) {
                val launched = SystemClock.elapsedRealtime()
                var process: java.lang.Process? = null
                try {
                    process = ProcessBuilder(
                        "logcat", "-v", "tag", "--pid=${Process.myPid()}",
                        "-T", String.format(Locale.ROOT, "%d.%03d", since / 1000, since % 1000),
                        "*:I",
                    ).redirectErrorStream(true).start()
                    process.inputStream.bufferedReader().useLines { lines ->
                        for (line in lines) {
                            since = System.currentTimeMillis()
                            LogcatLines.message(line, LOG_TAG)?.let(log::addLine)
                        }
                    }
                } catch (_: Exception) {
                } finally {
                    process?.destroy()
                }
                quickExits = if (SystemClock.elapsedRealtime() - launched > 60_000) 0 else quickExits + 1
                try {
                    Thread.sleep(5_000)
                } catch (_: InterruptedException) {
                    return
                }
            }
        }
    }
}
