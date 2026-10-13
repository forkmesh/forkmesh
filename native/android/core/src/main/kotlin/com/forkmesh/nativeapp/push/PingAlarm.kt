package com.forkmesh.nativeapp.push

const val PING_ALARM_POWER = "power"

val PING_ALARMS: Set<String> = setOf(PING_ALARM_POWER)

fun pingAlarm(kind: String, meta: Map<String, Any?>): String {
    if (kind.trim() != "desktop_ping") return ""
    val alarm = (meta["alarm"] as? String)?.trim().orEmpty()
    return if (alarm in PING_ALARMS) alarm else ""
}
