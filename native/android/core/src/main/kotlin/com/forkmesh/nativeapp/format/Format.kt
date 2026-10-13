package com.forkmesh.nativeapp.format

import java.time.Instant
import java.time.ZoneOffset

fun relativeTimeFrom(timeMillis: Long, nowMillis: Long = System.currentTimeMillis()): String {
    val diff = nowMillis - timeMillis
    if (diff < 0) return "just now"
    val seconds = diff / 1_000
    val minutes = diff / 60_000
    val hours = diff / 3_600_000
    val days = diff / 86_400_000
    if (seconds < 60) return "just now"
    if (minutes < 60) return "${minutes}m ago"
    if (hours < 24) return "${hours}h ago"
    if (days < 30) return "${days}d ago"
    val months = days / 30
    if (months < 12) return "${months}mo ago"
    return "${days / 365}y ago"
}

fun relativeTime(epochMs: Long, nowMillis: Long = System.currentTimeMillis()): String {
    if (epochMs <= 0) return "never"
    return relativeTimeFrom(epochMs, nowMillis)
}

fun absoluteDate(epochMs: Long): String {
    if (epochMs <= 0) return "never"
    val date = Instant.ofEpochMilli(epochMs).atZone(ZoneOffset.UTC).toLocalDate()
    val month = date.monthValue.toString().padStart(2, '0')
    val day = date.dayOfMonth.toString().padStart(2, '0')
    return "${date.year}-$month-$day"
}

fun countOrNull(value: Int, singular: String? = null, plural: String? = null): String? {
    if (value <= 0) return null
    if (singular == null) return "$value"
    return "$value ${if (value == 1) singular else (plural ?: "${singular}s")}"
}
