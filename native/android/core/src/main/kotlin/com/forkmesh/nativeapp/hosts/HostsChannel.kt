package com.forkmesh.nativeapp.hosts

import com.forkmesh.nativeapp.control.ControlEvent
import com.forkmesh.nativeapp.control.DesktopControlHub
import com.forkmesh.nativeapp.control.DesktopReach

interface HostsChannel {
    val reach: DesktopReach

    fun request(method: String, params: Map<String, Any?> = emptyMap(), timeoutMs: Long = 20_000): Map<String, Any?>

    fun addEventTap(tap: (ControlEvent) -> Unit): Any

    fun removeEventTap(token: Any)

    fun addReachListener(listener: () -> Unit)

    fun removeReachListener(listener: () -> Unit)
}

class HubHostsChannel(val hub: DesktopControlHub) : HostsChannel {
    override val reach: DesktopReach get() = hub.reach

    override fun request(method: String, params: Map<String, Any?>, timeoutMs: Long): Map<String, Any?> =
        hub.request(method, params, timeoutMs)

    override fun addEventTap(tap: (ControlEvent) -> Unit): Any = hub.addEventTap(tap)

    override fun removeEventTap(token: Any) = hub.removeEventTap(token)

    override fun addReachListener(listener: () -> Unit) {
        hub.addListener(listener)
    }

    override fun removeReachListener(listener: () -> Unit) {
        hub.removeListener(listener)
    }
}

fun interface HostsExecutor {
    fun run(task: () -> Unit)

    companion object {
        val Threads = HostsExecutor { task -> Thread(task, "Hosts").apply { isDaemon = true }.start() }
        val Inline = HostsExecutor { task -> task() }
    }
}
