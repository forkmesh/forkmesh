package com.forkmesh.nativeapp.tools

import java.math.BigDecimal
import java.math.RoundingMode
import java.time.Instant
import java.time.LocalDateTime
import java.time.ZoneId

fun dartRound(value: Double): Long = BigDecimal(value).setScale(0, RoundingMode.HALF_UP).toLong()

fun dartFixed(value: Double, digits: Int): String {
    if (value.isNaN()) return "NaN"
    if (value.isInfinite()) return if (value > 0) "Infinity" else "-Infinity"
    return BigDecimal(value).setScale(digits, RoundingMode.HALF_UP).toPlainString()
}

fun dartIsoLocal(epochMs: Long, zone: ZoneId = ZoneId.systemDefault()): String {
    val t = LocalDateTime.ofInstant(Instant.ofEpochMilli(epochMs), zone)
    val year = t.year.toString().padStart(4, '0')
    return "$year-${two(t.monthValue)}-${two(t.dayOfMonth)}T${two(t.hour)}:${two(t.minute)}:${two(t.second)}." +
        (t.nano / 1_000_000).toString().padStart(3, '0')
}

fun clockStamp(epochMs: Long, zone: ZoneId = ZoneId.systemDefault()): String {
    val t = LocalDateTime.ofInstant(Instant.ofEpochMilli(epochMs), zone)
    return "${two(t.hour)}:${two(t.minute)}:${two(t.second)}"
}

private fun two(value: Int): String = value.toString().padStart(2, '0')

fun clockTime12From24(stamp: String): String {
    val parts = stamp.split(":")
    if (parts.size < 2 || parts.size > 3) return stamp
    val hour = parts[0].toIntOrNull()
    val minute = parts[1].toIntOrNull()
    val second = if (parts.size == 3) parts[2].toIntOrNull() else 0
    if (hour == null || minute == null || second == null) return stamp
    if (hour !in 0..23 || minute !in 0..59 || second !in 0..59) return stamp
    val meridiem = if (hour < 12) "AM" else "PM"
    val h = if (hour % 12 == 0) 12 else hour % 12
    return "$h:${minute.toString().padStart(2, '0')}:${second.toString().padStart(2, '0')} $meridiem"
}
