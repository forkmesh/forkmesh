package com.forkmesh.nativeapp.control

interface ControlCalling {
    fun request(method: String, params: Map<String, Any?> = emptyMap(), timeoutMs: Long = 20_000): Map<String, Any?>

    fun addEventTap(tap: (ControlEvent) -> Unit): Any

    fun removeEventTap(token: Any)
}

class HubControlCalling(private val hub: DesktopControlHub) : ControlCalling {
    override fun request(method: String, params: Map<String, Any?>, timeoutMs: Long): Map<String, Any?> =
        hub.request(method, params, timeoutMs)

    override fun addEventTap(tap: (ControlEvent) -> Unit): Any = hub.addEventTap(tap)

    override fun removeEventTap(token: Any) = hub.removeEventTap(token)
}

class ControlFamilyChannel(val name: DesktopControlFamilyName, private val channel: ControlCalling) {
    val prefix: String get() = name.prefix

    fun method(suffix: String): String = if (suffix.startsWith(prefix)) suffix else prefix + suffix

    fun call(suffix: String, params: Map<String, Any?> = emptyMap(), timeoutMs: Long = 20_000): Map<String, Any?> =
        channel.request(method(suffix), params, timeoutMs)

    fun addEventTap(tap: (String, Map<String, Any?>) -> Unit): Any = channel.addEventTap { event ->
        if (event.event.startsWith(prefix)) tap(event.event.removePrefix(prefix), event.data)
    }

    fun removeEventTap(token: Any) = channel.removeEventTap(token)
}
