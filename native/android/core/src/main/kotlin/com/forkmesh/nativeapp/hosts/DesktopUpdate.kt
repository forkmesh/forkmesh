package com.forkmesh.nativeapp.hosts

import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.control.DesktopControlFamilyName
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings

data class DesktopUpdateState(
    val run: Int = 0,
    val running: Boolean = false,
    val kind: String = "",
    val percent: Int = 0,
    val phase: String = "",
    val activity: String = "",
    val status: String = "",
    val error: String = "",
    val relaunching: Boolean = false,
) {
    val headline: String
        get() = when {
            activity.isNotEmpty() -> activity
            phase.isNotEmpty() -> "$percent% · $phase"
            else -> status
        }

    companion object {
        fun fromJson(json: Map<String, Any?>) = DesktopUpdateState(
            run = json.int("run"),
            running = json.bool("running"),
            kind = json.str("kind"),
            percent = json.int("percent").coerceIn(0, 100),
            phase = json.str("phase"),
            activity = json.str("activity"),
            status = json.str("status"),
            error = json.str("error"),
            relaunching = json.bool("relaunching"),
        )
    }
}

enum class DesktopUpdateStage { Idle, Running, Restarting, Back, Failed }

class DesktopUpdateStore(
    private val channel: ControlCalling,
    private val executor: HostsExecutor = HostsExecutor.Threads,
    private val now: () -> Long = System::currentTimeMillis,
    private val sleep: (Long) -> Unit = { Thread.sleep(it) },
) : RemoteStoreBase(channel, DesktopControlFamilyName.Update) {
    private var closeTap: Any? = null
    private var startedAtMs = 0L
    private var restartFrom = 0L
    private var tracking = false
    private var waiting = false

    @Volatile
    var state: DesktopUpdateState = DesktopUpdateState()
        private set

    @Volatile
    var lines: List<String> = emptyList()
        private set

    @Volatile
    var stage: DesktopUpdateStage = DesktopUpdateStage.Idle
        private set

    @Volatile
    var version: String = ""
        private set

    @Volatile
    var previousVersion: String = ""
        private set

    @Volatile
    var error: String = ""
        private set

    @Volatile
    var unsupported: Boolean = false
        private set

    @Volatile
    var loaded: Boolean = false
        private set

    val busy: Boolean get() = stage == DesktopUpdateStage.Running || stage == DesktopUpdateStage.Restarting

    init {
        startTap()
        closeTap = channel.addEventTap { event -> if (event.event == "__closed") onConnectionLost() }
    }

    override fun onEvent(suffix: String, data: Map<String, Any?>) {
        if (suffix == "progress") onProgress(data)
    }

    fun refresh() {
        try {
            apply(family.call("status"))
        } catch (failure: Exception) {
            if (synchronized(lock) { tracking }) {
                onConnectionLost()
                return
            }
            unsupported = isUnsupportedControl(failure)
            error = RemoteErrorText.describe(failure)
            changed()
        }
    }

    fun refreshAsync() = executor.run { refresh() }

    fun prepare(): RemoteConfirmation = family.prepare(mapOf("action" to ACTION))

    fun prepareComputerRestart(): RemoteConfirmation =
        family.prepare(mapOf("action" to COMPUTER_RESTART_ACTION))

    fun run(confirmation: RemoteConfirmation): String {
        val started = family.run(confirmation)
        synchronized(lock) {
            lines = emptyList()
            state = DesktopUpdateState(running = true, kind = "update")
            error = ""
            beginTracking()
            stage = DesktopUpdateStage.Running
        }
        changed()
        return started.message
    }

    fun restartComputer(confirmation: RemoteConfirmation): String {
        val started = family.run(confirmation)
        synchronized(lock) {
            lines = listOf("==> Restarting computer", "\$ loginctl reboot")
            state = DesktopUpdateState(
                running = true,
                kind = COMPUTER_RESTART_ACTION,
                status = "Restarting computer…",
                relaunching = true,
            )
            error = ""
            beginTracking()
            stage = DesktopUpdateStage.Restarting
        }
        changed()
        return started.message
    }

    fun unwatch() {
        if (synchronized(lock) { tracking }) return
        runCatching { family.call("unwatch", emptyMap(), 10_000) }
    }

    override fun stop() {
        closeTap?.let(channel::removeEventTap)
        closeTap = null
        super.stop()
    }

    internal fun apply(json: Map<String, Any?>) {
        synchronized(lock) {
            val started = json.long("startedAtMs")
            val restarted = tracking && restartFrom != 0L && started != 0L && started != restartFrom
            version = json.str("version")
            startedAtMs = started
            loaded = true
            unsupported = false
            error = ""
            val next = DesktopUpdateState.fromJson(json)
            lines = json.strings("lines").takeLast(MAX_LINES)
            state = next
            if (restarted && !next.running) {
                tracking = false
                stage = DesktopUpdateStage.Back
            } else {
                settleStage()
            }
        }
        changed()
    }

    private fun onProgress(data: Map<String, Any?>) {
        val next = DesktopUpdateState.fromJson(data)
        synchronized(lock) {
            var out = if (next.run != state.run) emptyList() else lines
            val batch = data.strings("lines")
            if (batch.isNotEmpty()) {
                val skipped = data.long("skipped")
                if (skipped > 0) out = out + "… $skipped earlier line${if (skipped == 1L) "" else "s"} not shown"
                out = (out + batch).takeLast(MAX_LINES)
            }
            lines = out
            state = next
            settleStage()
        }
        changed()
        if (next.relaunching) onConnectionLost()
    }

    private fun beginTracking() {
        if (!tracking) {
            previousVersion = version
            restartFrom = startedAtMs
        }
        tracking = true
    }

    private fun settleStage() {
        stage = when {
            state.running -> {
                beginTracking()
                if (state.relaunching || state.kind == COMPUTER_RESTART_ACTION) DesktopUpdateStage.Restarting else DesktopUpdateStage.Running
            }
            state.error.isNotEmpty() -> {
                tracking = false
                DesktopUpdateStage.Failed
            }
            stage == DesktopUpdateStage.Back -> DesktopUpdateStage.Back
            else -> {
                tracking = false
                DesktopUpdateStage.Idle
            }
        }
    }

    fun onConnectionLost() {
        synchronized(lock) {
            if (!tracking || waiting) return
            waiting = true
            stage = DesktopUpdateStage.Restarting
        }
        changed()
        executor.run {
            try {
                waitForRestart()
            } finally {
                synchronized(lock) { waiting = false }
            }
        }
    }

    private fun waitForRestart() {
        val deadline = now() + RESTART_WAIT_MS
        while (synchronized(lock) { tracking } && now() < deadline) {
            sleep(RESTART_POLL_MS)
            val reply = runCatching { family.call("status", emptyMap(), 15_000) }.getOrNull() ?: continue
            val started = reply.long("startedAtMs")
            val sameProcess = synchronized(lock) { restartFrom != 0L && started == restartFrom }
            if (sameProcess && reply.bool("running")) continue
            apply(reply)
            if (!sameProcess || stage != DesktopUpdateStage.Restarting) return
        }
        val gaveUp = synchronized(lock) {
            if (!tracking) return@synchronized false
            tracking = false
            stage = DesktopUpdateStage.Failed
            error = "The desktop has not come back after the update. Check it on the desktop."
            true
        }
        if (gaveUp) changed()
    }

    companion object {
        const val ACTION = "updateRestart"
        const val COMPUTER_RESTART_ACTION = "computerRestart"
        const val MAX_LINES = 400
        const val RESTART_WAIT_MS = 20 * 60 * 1000L
        const val RESTART_POLL_MS = 3000L
    }
}
