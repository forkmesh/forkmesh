package com.forkmesh.nativeapp.account

import com.forkmesh.nativeapp.api.DartException
import com.forkmesh.nativeapp.model.PingKind
import com.forkmesh.nativeapp.model.PingQuietHours
import com.forkmesh.nativeapp.model.PingSettings
import com.forkmesh.nativeapp.model.auth.AuthSession
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZoneId
import java.time.ZonedDateTime
import java.time.format.TextStyle
import java.util.Locale

const val PING_SETTINGS_TITLE = "Ping settings"

const val PING_SETTINGS_MENU_LABEL = "Ping settings"

const val PING_SETTINGS_SUBTITLE = "Choose which pings you get, pause them, or set quiet hours"

const val PING_PAUSE_NOTE = "Pause every banner and email for a while. Pings keep landing in your inbox."

const val PING_SETTINGS_INTRO =
    "A ping type switched off is never delivered anywhere. Pause and quiet hours still file pings in your " +
        "inbox to read later, but raise no email, phone banner or desktop card."

const val PING_PAUSE_MAX_MS: Long = 30L * 24 * 60 * 60 * 1000

val PING_DAY_LABELS: List<String> = listOf("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")

fun showsPingSettings(session: AuthSession?, guest: Boolean): Boolean = !guest && hasRealSession(session)

fun isKindEnabled(settings: PingSettings, kind: String): Boolean = kind !in settings.pingMutedKinds

fun emailDefaultFor(kind: String): Boolean = kind != "host_online" && kind != "host_offline"

fun isKindEmailOn(settings: PingSettings, kind: String): Boolean =
    settings.notificationPreferences[kind] ?: emailDefaultFor(kind)

fun emailSwitchChecked(settings: PingSettings, kind: PingKind): Boolean =
    kind.email && settings.emailNotifications && isKindEnabled(settings, kind.kind) && isKindEmailOn(settings, kind.kind)

fun emailSwitchEnabled(settings: PingSettings, kind: PingKind): Boolean =
    kind.email && settings.emailNotifications && isKindEnabled(settings, kind.kind)

fun isGeneralChatDigestOn(settings: PingSettings): Boolean = settings.notificationPreferences["general_chat"] ?: true

data class PingSettingsChange(val next: PingSettings, val patch: Map<String, Any?>)

fun withKindEnabled(settings: PingSettings, kind: String, enabled: Boolean): PingSettingsChange {
    val muted = if (enabled) settings.pingMutedKinds - kind else settings.pingMutedKinds + kind
    return PingSettingsChange(
        settings.copy(pingMutedKinds = muted),
        mapOf("pingMutedKinds" to mapOf(kind to enabled)),
    )
}

fun withKindEmail(settings: PingSettings, kind: String, enabled: Boolean): PingSettingsChange = PingSettingsChange(
    settings.copy(notificationPreferences = settings.notificationPreferences + (kind to enabled)),
    mapOf("notificationPreferences" to mapOf(kind to enabled)),
)

fun withEmailNotifications(settings: PingSettings, enabled: Boolean): PingSettingsChange = PingSettingsChange(
    settings.copy(emailNotifications = enabled),
    mapOf("emailNotifications" to enabled),
)

fun withCodePushes(settings: PingSettings, enabled: Boolean): PingSettingsChange = PingSettingsChange(
    settings.copy(codePushes = enabled),
    mapOf("codePushes" to enabled),
)

fun withDelivery(settings: PingSettings, mode: String): PingSettingsChange {
    val clean = if (mode == "instant") "instant" else "digest"
    return PingSettingsChange(settings.copy(notificationDelivery = clean), mapOf("notificationDelivery" to clean))
}

fun withPauseUntil(settings: PingSettings, until: Long): PingSettingsChange {
    val clean = until.coerceAtLeast(0)
    return PingSettingsChange(settings.copy(notificationPauseUntil = clean), mapOf("notificationPauseUntil" to clean))
}

fun withQuietHours(settings: PingSettings, quiet: PingQuietHours): PingSettingsChange {
    val clean = quiet.copy(
        start = quiet.start.coerceIn(0, 1439),
        end = quiet.end.coerceIn(0, 1439),
        days = quiet.days.filter { it in 0..6 }.distinct().sorted(),
    )
    return PingSettingsChange(
        settings.copy(notificationQuietHours = clean),
        mapOf("notificationQuietHours" to clean.toJson()),
    )
}

enum class MonitorAlertField(val wire: String) { PINGS("pings"), EMAILS("emails"), CONTINUAL("continual") }

fun withMonitorAlert(settings: PingSettings, id: String, field: MonitorAlertField, enabled: Boolean): PingSettingsChange {
    val rows = settings.statusMonitorAlerts?.map { row ->
        if (row.id != id) {
            row
        } else {
            when (field) {
                MonitorAlertField.PINGS -> row.copy(pings = enabled)
                MonitorAlertField.EMAILS -> row.copy(emails = enabled)
                MonitorAlertField.CONTINUAL -> row.copy(continual = enabled)
            }
        }
    }
    return PingSettingsChange(
        settings.copy(statusMonitorAlerts = rows),
        mapOf("statusMonitorAlerts" to mapOf(id to mapOf(field.wire to enabled))),
    )
}

data class PingPausePreset(val label: String, val durationMs: Long)

val PING_PAUSE_PRESETS: List<PingPausePreset> = listOf(
    PingPausePreset("1 hour", 60L * 60 * 1000),
    PingPausePreset("1 day", 24L * 60 * 60 * 1000),
    PingPausePreset("1 week", 7L * 24 * 60 * 60 * 1000),
)

fun pauseUntilFor(durationMs: Long, now: Long): Long = now + durationMs.coerceIn(0, PING_PAUSE_MAX_MS)

fun isPaused(settings: PingSettings, now: Long): Boolean = settings.notificationPauseUntil > now

fun customPauseUntil(date: LocalDate, minutes: Int, zone: ZoneId, now: Long): Long? {
    val clean = minutes.coerceIn(0, 1439)
    val at = ZonedDateTime.of(date, LocalTime.of(clean / 60, clean % 60), zone).toInstant().toEpochMilli()
    if (at <= now) return null
    return minOf(at, now + PING_PAUSE_MAX_MS)
}

fun lastPauseDay(now: Long, zone: ZoneId): LocalDate = Instant.ofEpochMilli(now + PING_PAUSE_MAX_MS).atZone(zone).toLocalDate()

fun pingDeadlineLabel(ms: Long, now: Long, zone: ZoneId): String {
    val at = Instant.ofEpochMilli(ms).atZone(zone)
    val today = Instant.ofEpochMilli(now).atZone(zone).toLocalDate()
    val day = when (at.toLocalDate()) {
        today -> "today"
        today.plusDays(1) -> "tomorrow"
        else -> {
            val weekday = at.dayOfWeek.getDisplayName(TextStyle.SHORT, Locale.ENGLISH)
            if (at.toLocalDate().isBefore(today.plusDays(7))) {
                weekday
            } else {
                "$weekday ${at.dayOfMonth} ${at.month.getDisplayName(TextStyle.SHORT, Locale.ENGLISH)}"
            }
        }
    }
    return "$day ${minutesToClock(at.hour * 60 + at.minute)}"
}

fun pingStatusLine(settings: PingSettings, now: Long, zone: ZoneId): String? = when {
    isPaused(settings, now) -> "Paused until ${pingDeadlineLabel(settings.notificationPauseUntil, now, zone)}"
    settings.notificationQuietUntil > now ->
        "Quiet hours until ${pingDeadlineLabel(settings.notificationQuietUntil, now, zone)}"
    else -> null
}

fun minutesToClock(minutes: Int): String {
    val clean = ((minutes % 1440) + 1440) % 1440
    return "%02d:%02d".format(clean / 60, clean % 60)
}

fun clockToMinutes(text: String): Int? {
    val match = Regex("""^\s*(\d{1,2}):(\d{2})\s*$""").find(text) ?: return null
    val hour = match.groupValues[1].toInt()
    val minute = match.groupValues[2].toInt()
    if (hour > 23 || minute > 59) return null
    return hour * 60 + minute
}

fun quietHoursRangeLabel(quiet: PingQuietHours): String {
    val range = "${minutesToClock(quiet.start)}–${minutesToClock(quiet.end)}"
    return if (quiet.end <= quiet.start) "$range (next day)" else range
}

fun quietDaysLabel(days: List<Int>): String {
    val set = days.filter { it in 0..6 }.toSortedSet()
    return when (set) {
        (0..6).toSortedSet() -> "Every day"
        (0..4).toSortedSet() -> "Weekdays"
        sortedSetOf(5, 6) -> "Weekends"
        emptySet<Int>() -> "Never"
        else -> set.joinToString(", ") { PING_DAY_LABELS[it] }
    }
}

fun quietHoursSummary(quiet: PingQuietHours): String =
    if (!quiet.enabled) "Off" else "${quietHoursRangeLabel(quiet)}, ${quietDaysLabel(quiet.days).lowercase().replaceFirstChar { it.uppercase() }}"

fun toggleQuietDay(days: List<Int>, day: Int): List<Int> =
    (if (day in days) days - day else days + day).filter { it in 0..6 }.distinct().sorted()

fun effectiveQuietTimezone(stored: String, device: String): String {
    val clean = stored.trim()
    return if (clean.isEmpty() || clean.equals("UTC", ignoreCase = true) || clean == "Etc/UTC") {
        device.ifBlank { "UTC" }
    } else {
        clean
    }
}

fun isValidTimezone(id: String): Boolean {
    val clean = id.trim()
    if (clean.isEmpty() || !clean.all { it.isLetterOrDigit() || it in "/_+-" }) return false
    return try {
        ZoneId.of(clean)
        true
    } catch (_: Exception) {
        false
    }
}

fun pingGroupIndexOf(settings: PingSettings, kind: String?): Int {
    if (kind.isNullOrBlank()) return -1
    return settings.pingKindCatalog.indexOfFirst { group -> group.kinds.any { it.kind == kind } }
}

fun pingKindSettingsLabel(kindLabel: String): String = "Settings for ${kindLabel.lowercase()} pings"

fun pingSettingsErrorText(error: Throwable?): String = when {
    error == null -> ""
    error is DartException -> error.message.orEmpty().ifEmpty { "Could not reach the server." }
    else -> "Could not reach the server."
}
