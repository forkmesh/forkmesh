package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonOf
import com.forkmesh.nativeapp.json.str
import java.text.Normalizer
import java.time.DayOfWeek
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZoneId
import java.time.ZonedDateTime
import java.time.format.DateTimeFormatter
import java.time.format.TextStyle
import java.time.temporal.ChronoUnit
import java.time.temporal.TemporalAdjusters
import java.util.Locale

const val USER_STATUS_TEXT_MAX = 100

const val USER_STATUS_MAX_DURATION_MS = 366L * 24 * 60 * 60 * 1000

data class UserStatus(
    val away: Boolean = false,
    val emoji: String = "",
    val text: String = "",
    val expiresAt: Long = 0,
) {
    fun isExpired(nowMs: Long): Boolean = expiresAt != 0L && expiresAt <= nowMs

    val hasCustom: Boolean get() = emoji.isNotEmpty() || text.isNotEmpty()

    fun visible(nowMs: Long): UserStatus =
        if (isExpired(nowMs) || !hasCustom) UserStatus(away = away) else this

    fun displayEmoji(nowMs: Long): String = visible(nowMs).emoji

    fun toJson(): LinkedHashMap<String, Any?> = jsonOf(
        "away" to away,
        "emoji" to emoji,
        "text" to text,
        "expiresAt" to expiresAt,
    )

    companion object {
        fun fromJson(value: Any?): UserStatus {
            val json = value as? Map<*, *> ?: return UserStatus()
            @Suppress("UNCHECKED_CAST")
            val map = json as Map<String, Any?>
            return UserStatus(
                away = map["away"] == true,
                emoji = map.str("emoji").trim(),
                text = map.str("text"),
                expiresAt = jsonLong(map["expiresAt"]).coerceAtLeast(0),
            )
        }

        fun fromJsonOrNull(value: Any?): UserStatus? = if (value is Map<*, *>) fromJson(value) else null
    }
}

class UserStatusException(val code: String, message: String) : Exception(message)

fun userStatusErrorMessage(code: String): String = when (code) {
    "bad_status_emoji" -> "Pick a single emoji for your status."
    "status_text_too_long" -> "Keep your status to $USER_STATUS_TEXT_MAX characters."
    "bad_status_expiry" -> "That clear-after time is not valid."
    "status_expiry_in_past" -> "Choose a clear-after time in the future."
    "status_expiry_too_far" -> "A status can clear at most a year from now. Choose \"Don't clear\" to keep it."
    "unauthorized", "http_401" -> "Your session expired. Please sign in again."
    "invalid_json" -> "Could not update your status. Try again."
    else -> "Could not update your status. Check your connection and try again."
}

fun userStatusSetBody(emoji: String, text: String, expiresAt: Long): LinkedHashMap<String, Any?> = jsonOf(
    "emoji" to emoji.trim(),
    "text" to cleanUserStatusText(text),
    "expiresAt" to expiresAt,
)

fun userStatusAwayBody(away: Boolean): LinkedHashMap<String, Any?> = jsonOf("away" to away)

fun userStatusClearBody(): LinkedHashMap<String, Any?> = jsonOf("clear" to true)

fun userStatusDraftError(emoji: String, text: String, expiresAt: Long, nowMs: Long): String? {
    if (emoji.isNotBlank() && cleanStatusEmoji(emoji).isEmpty()) return "bad_status_emoji"
    if (text.codePointCount(0, text.length) > USER_STATUS_TEXT_MAX) return "status_text_too_long"
    if (expiresAt < 0) return "bad_status_expiry"
    if (expiresAt != 0L && expiresAt <= nowMs) return "status_expiry_in_past"
    if (expiresAt > nowMs + USER_STATUS_MAX_DURATION_MS) return "status_expiry_too_far"
    return null
}

fun cleanUserStatusText(value: String): String {
    val oneLine = value.trim().split(Regex("\\s+")).filter { it.isNotEmpty() }.joinToString(" ")
    val count = oneLine.codePointCount(0, oneLine.length)
    if (count <= USER_STATUS_TEXT_MAX) return oneLine
    return oneLine.substring(0, oneLine.offsetByCodePoints(0, USER_STATUS_TEXT_MAX)).trim()
}

private fun isEmojiBase(codepoint: Int): Boolean =
    codepoint in EMOJI_SINGLES ||
        codepoint in 0x2194..0x21FF ||
        codepoint in 0x231A..0x231B || codepoint == 0x2328 || codepoint == 0x23CF ||
        codepoint in 0x23E9..0x23F3 ||
        codepoint in 0x23F8..0x23FA ||
        codepoint in 0x25AA..0x25AB ||
        codepoint == 0x25B6 || codepoint == 0x25C0 ||
        codepoint in 0x25FB..0x25FE ||
        codepoint in 0x2600..0x27BF ||
        codepoint in 0x2934..0x2935 ||
        codepoint in 0x2B05..0x2B07 ||
        codepoint in 0x2B1B..0x2B1C ||
        codepoint == 0x2B50 || codepoint == 0x2B55 ||
        (codepoint in 0x1F000..0x1FAFF && codepoint !in 0x1F1E6..0x1F1FF)

private val EMOJI_SINGLES = setOf(0x00A9, 0x00AE, 0x203C, 0x2049, 0x2122, 0x2139, 0x24C2, 0x3030, 0x303D, 0x3297, 0x3299)

fun cleanStatusEmoji(value: String): String {
    val emoji = Normalizer.normalize(value.trim(), Normalizer.Form.NFC)
    if (emoji.isEmpty()) return ""
    val cps = emoji.codePoints().toArray()
    if (cps.size > 24 || emoji.toByteArray(Charsets.UTF_8).size > 96) return ""
    if (cps.size == 2 && cps.all { it in 0x1F1E6..0x1F1FF }) return emoji
    if (cps.size in 2..3 && cps[0] < 0x80 && cps[0].toChar() in "#*0123456789" && cps.last() == 0x20E3 &&
        (cps.size == 2 || cps[1] == 0xFE0F)
    ) {
        return emoji
    }

    fun consumePictograph(start: Int): Int {
        var offset = start
        if (offset >= cps.size || !isEmojiBase(cps[offset])) return -1
        val base = cps[offset]
        offset++
        if (offset < cps.size && (cps[offset] == 0xFE0E || cps[offset] == 0xFE0F)) offset++
        if (offset < cps.size && cps[offset] in 0x1F3FB..0x1F3FF) offset++
        if (offset < cps.size && cps[offset] in 0xE0020..0xE007E) {
            if (base != 0x1F3F4) return -1
            while (offset < cps.size && cps[offset] in 0xE0020..0xE007E) offset++
            if (offset >= cps.size || cps[offset] != 0xE007F) return -1
            offset++
        }
        return offset
    }

    var index = consumePictograph(0)
    if (index < 0) return ""
    while (index < cps.size) {
        if (cps[index] != 0x200D) return ""
        index = consumePictograph(index + 1)
        if (index < 0) return ""
    }
    return emoji
}

enum class StatusClearAfter(val label: String) {
    Never("Don't clear"),
    ThirtyMinutes("30 minutes"),
    OneHour("1 hour"),
    FourHours("4 hours"),
    Today("Today"),
    ThisWeek("This week"),
    Custom("Custom"),
}

fun statusExpiresAt(
    choice: StatusClearAfter,
    nowMs: Long,
    zone: ZoneId = ZoneId.systemDefault(),
    customAtMs: Long = 0,
): Long = when (choice) {
    StatusClearAfter.Never -> 0
    StatusClearAfter.ThirtyMinutes -> nowMs + 30 * 60_000L
    StatusClearAfter.OneHour -> nowMs + 60 * 60_000L
    StatusClearAfter.FourHours -> nowMs + 4 * 60 * 60_000L
    StatusClearAfter.Today -> localDate(nowMs, zone).plusDays(1).atStartOfDay(zone).toInstant().toEpochMilli()
    StatusClearAfter.ThisWeek -> localDate(nowMs, zone)
        .with(TemporalAdjusters.next(DayOfWeek.MONDAY))
        .atStartOfDay(zone).toInstant().toEpochMilli()
    StatusClearAfter.Custom -> customAtMs
}

fun statusCustomExpiresAt(date: LocalDate, time: LocalTime, zone: ZoneId = ZoneId.systemDefault()): Long =
    ZonedDateTime.of(date, time, zone).toInstant().toEpochMilli()

private fun localDate(epochMs: Long, zone: ZoneId): LocalDate = Instant.ofEpochMilli(epochMs).atZone(zone).toLocalDate()

data class UserStatusPreset(val emoji: String, val text: String, val clearAfter: StatusClearAfter) {
    val label: String get() = "$emoji $text — ${clearAfter.label}"
}

val USER_STATUS_QUICK_EMOJI: List<String> = listOf("📅", "🚌", "🤒", "🌴", "🏡", "🎧", "🍔", "💻", "🎉", "🔕")

val USER_STATUS_PRESETS: List<UserStatusPreset> = listOf(
    UserStatusPreset("📅", "In a meeting", StatusClearAfter.OneHour),
    UserStatusPreset("🚌", "Commuting", StatusClearAfter.ThirtyMinutes),
    UserStatusPreset("🤒", "Out sick", StatusClearAfter.Today),
    UserStatusPreset("🌴", "Vacationing", StatusClearAfter.Never),
    UserStatusPreset("🏡", "Working remotely", StatusClearAfter.Today),
)

fun userStatusLabel(status: UserStatus?, nowMs: Long): String {
    val shown = status?.visible(nowMs) ?: return ""
    return listOf(shown.emoji, shown.text).filter { it.isNotEmpty() }.joinToString(" ")
}

fun nameWithStatus(name: String, status: UserStatus?, nowMs: Long): String {
    val emoji = status?.displayEmoji(nowMs).orEmpty()
    return if (emoji.isEmpty()) name else "$name $emoji"
}

fun userStatusExpiryLabel(
    expiresAt: Long,
    nowMs: Long,
    zone: ZoneId = ZoneId.systemDefault(),
    use24Hour: Boolean = false,
    locale: Locale = Locale.getDefault(),
): String {
    if (expiresAt == 0L || expiresAt <= nowMs) return ""
    val diff = expiresAt - nowMs
    if (diff < 60 * 60_000L) {
        val minutes = ((diff + 59_999) / 60_000).coerceAtLeast(1)
        return "Clears in $minutes minute${if (minutes == 1L) "" else "s"}"
    }
    val at = Instant.ofEpochMilli(expiresAt).atZone(zone)
    val today = localDate(nowMs, zone)
    if (at.toLocalTime() == LocalTime.MIDNIGHT) {
        return "Clears at the end of ${dayPhrase(at.toLocalDate().minusDays(1), today, locale)}"
    }
    val time = at.format(DateTimeFormatter.ofPattern(if (use24Hour) "HH:mm" else "h:mm a", locale))
    return "Clears ${dayPhrase(at.toLocalDate(), today, locale)} at $time"
}

private fun dayPhrase(date: LocalDate, today: LocalDate, locale: Locale): String {
    val days = ChronoUnit.DAYS.between(today, date)
    return when {
        days == 0L -> "today"
        days == 1L -> "tomorrow"
        days in 2..6 -> date.dayOfWeek.getDisplayName(TextStyle.FULL, locale)
        date.year == today.year -> date.format(DateTimeFormatter.ofPattern("MMM d", locale))
        else -> date.format(DateTimeFormatter.ofPattern("MMM d, yyyy", locale))
    }
}

fun statusClearAfterFor(expiresAt: Long): StatusClearAfter =
    if (expiresAt == 0L) StatusClearAfter.Never else StatusClearAfter.Custom
