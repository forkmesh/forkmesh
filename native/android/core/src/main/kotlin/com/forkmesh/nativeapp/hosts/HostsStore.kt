package com.forkmesh.nativeapp.hosts

import com.forkmesh.nativeapp.control.ControlErrorText
import com.forkmesh.nativeapp.control.ControlEvent
import com.forkmesh.nativeapp.control.DesktopReach
import com.forkmesh.nativeapp.control.DesktopTerminalInfo
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.json.str
import java.util.Locale
import java.util.concurrent.CopyOnWriteArrayList

class HostsStore(
    val channel: HostsChannel,
    private val executor: HostsExecutor = HostsExecutor.Threads,
    private val now: () -> Long = System::currentTimeMillis,
) {
    private val lock = Object()
    private val listeners = CopyOnWriteArrayList<() -> Unit>()
    private var refreshing = false
    private var again = false
    private var tap: Any? = channel.addEventTap(::onEvent)

    @Volatile
    var snapshot: HostsSnapshot? = null
        private set

    @Volatile
    var loading: Boolean = false
        private set

    @Volatile
    var error: String = ""
        private set

    @Volatile
    var outcomes: List<HostActionOutcome> = emptyList()
        private set

    @Volatile
    var running: Set<String> = emptySet()
        private set

    @Volatile
    var revision: Int = 0
        private set

    val reach: DesktopReach get() = channel.reach

    fun addListener(listener: () -> Unit) = listeners.add(listener)

    fun removeListener(listener: () -> Unit) = listeners.remove(listener)

    private fun changed() {
        revision++
        for (listener in listeners) runCatching { listener() }
    }

    private fun onEvent(event: ControlEvent) {
        when (event.event) {
            "hosts.changed" -> executor.run { refresh() }
            "hosts.action" -> {
                val outcome = HostActionOutcome.fromJson(event.data, now())
                synchronized(lock) {
                    running = running - key(outcome.action, outcome.host)
                    outcomes = (listOf(outcome) + outcomes).take(20)
                }
                changed()
            }
        }
    }

    fun refresh() {
        synchronized(lock) {
            if (refreshing) {
                again = true
                return
            }
            refreshing = true
            loading = true
        }
        changed()
        try {
            do {
                synchronized(lock) { again = false }
                try {
                    snapshot = HostsSnapshot.fromJson(channel.request("hosts.list", emptyMap(), 20_000))
                    error = ""
                } catch (failure: Exception) {
                    error = ControlErrorText.describe(failure)
                }
            } while (synchronized(lock) { again })
        } finally {
            synchronized(lock) {
                refreshing = false
                loading = false
            }
            changed()
        }
    }

    fun refreshAsync() = executor.run { refresh() }

    fun detail(host: String): HostRow = HostRow.fromJson(channel.request("hosts.detail", mapOf("host" to host), 20_000))

    fun createOptions(): HostCreateOptions = HostCreateOptions.fromJson(channel.request("hosts.options", emptyMap(), 30_000))

    fun prepare(action: HostAction, host: String): HostConfirmation {
        val result = channel.request("hosts.prepare", mapOf("action" to action.wire, "host" to host), 20_000)
        return HostConfirmation.fromJson(result) ?: throw LocalControlException("bad_answer")
    }

    fun run(confirmation: HostConfirmation, typedName: String = "", plan: String = "", region: String = ""): String {
        if (confirmation.action.opensTerminal) throw LocalControlException("unsupported_action")
        if (!confirmation.accepts(typedName)) throw LocalControlException("name_mismatch")
        val params = linkedMapOf<String, Any?>(
            "action" to confirmation.action.wire,
            "host" to confirmation.host,
            "token" to confirmation.token,
        )
        if (confirmation.needsTypedName) params["confirmName"] = typedName
        if (plan.isNotEmpty()) params["plan"] = plan
        if (region.isNotEmpty()) params["region"] = region
        val result = channel.request("hosts.run", params, 30_000)
        val host = result.str("host", confirmation.host)
        synchronized(lock) { running = running + key(confirmation.action.wire, host) }
        changed()
        refresh()
        return result.str("message")
    }

    fun openTerminal(confirmation: HostConfirmation, cols: Int = 80, rows: Int = 24): String {
        if (!confirmation.action.opensTerminal) throw LocalControlException("unsupported_action")
        val result = channel.request(
            "hosts.run",
            mapOf(
                "action" to confirmation.action.wire,
                "host" to confirmation.host,
                "token" to confirmation.token,
                "cols" to cols,
                "rows" to rows,
            ),
            30_000,
        )
        return result.str("terminal").ifEmpty { throw LocalControlException("terminal_failed") }
    }

    fun openTerminals(host: String): List<DesktopTerminalInfo> {
        val key = host.lowercase(Locale.ROOT)
        return DesktopTerminalInfo.list(channel.request("terminal.list", emptyMap(), 20_000))
            .filter { it.provider == "ssh" && it.host.lowercase(Locale.ROOT) == key }
    }

    fun isRunning(action: HostAction, host: String): Boolean = key(action.wire, host) in running

    fun isRunningAny(host: String): Boolean {
        val suffix = ":" + host.lowercase(Locale.ROOT)
        return running.any { it.endsWith(suffix) }
    }

    fun isInstalling(): Boolean = running.any { it.startsWith("install:") }

    fun host(named: String): HostRow? = snapshot?.hosts?.firstOrNull { it.id == named.lowercase(Locale.ROOT) }

    fun outcomesFor(host: String): List<HostActionOutcome> =
        outcomes.filter { it.host.lowercase(Locale.ROOT) == host.lowercase(Locale.ROOT) }

    fun dispose() {
        tap?.let { channel.removeEventTap(it) }
        tap = null
        listeners.clear()
    }

    private fun key(action: String, host: String): String = "$action:${host.lowercase(Locale.ROOT)}"
}
