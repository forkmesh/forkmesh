package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.model.PingQuietHours
import com.forkmesh.nativeapp.model.PingSettings
import java.time.Instant
import java.time.ZoneId
import java.time.ZoneOffset

fun pingQuietUntil(quiet: PingQuietHours, nowMs: Long): Long {
    if (!quiet.enabled) return 0
    val zone = runCatching { ZoneId.of(quiet.timezone) }.getOrElse { ZoneOffset.UTC }
    val local = Instant.ofEpochMilli(nowMs).atZone(zone)
    val minute = local.hour * 60 + local.minute
    val weekday = local.dayOfWeek.value - 1
    val start = quiet.start
    val end = quiet.end
    val midnight = local.toLocalDate().atStartOfDay(zone).toLocalDateTime()
    val finish = when {
        start < end -> if (weekday in quiet.days && minute in start until end) midnight.plusMinutes(end.toLong()) else return 0
        minute >= start && weekday in quiet.days -> midnight.plusDays(1).plusMinutes(end.toLong())
        minute < end && Math.floorMod(weekday - 1, 7) in quiet.days -> midnight.plusMinutes(end.toLong())
        else -> return 0
    }
    val deadline = finish.atZone(zone).toInstant().toEpochMilli()
    return if (deadline > nowMs) deadline else 0
}

fun pingAlertsHeld(settings: PingSettings?, atMs: Long): Boolean {
    if (settings == null) return false
    if (settings.notificationPauseUntil > atMs) return true
    return pingQuietUntil(settings.notificationQuietHours, atMs) > 0
}
