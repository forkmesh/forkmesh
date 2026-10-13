package com.forkmesh.nativeapp.planning

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str
import java.net.URI
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.ZonedDateTime

data class CalendarAttendee(val name: String, val status: String = "invited")

enum class CalendarRecurrence(val wire: String, val label: String) {
    NONE("none", "Does not repeat"),
    DAILY("daily", "Every day"),
    WEEKLY("weekly", "Every week"),
    MONTHLY("monthly", "Every month"),
    YEARLY("yearly", "Every year");

    companion object {
        fun of(wire: String): CalendarRecurrence = entries.firstOrNull { it.wire == wire } ?: NONE
    }
}

enum class CalendarMode(val wire: String, val label: String) {
    DAY("day", "Day"), WEEK("week", "Week"), MONTH("month", "Month"), AGENDA("agenda", "Agenda");

    companion object {
        fun of(wire: String?): CalendarMode? = entries.firstOrNull { it.wire == wire?.lowercase() }
    }
}

data class CalendarEvent(
    val id: String,
    val owner: String = "",
    val calendar: String = "personal",
    val scope: String = "personal",
    val startAt: Long,
    val endAt: Long,
    val allDay: Boolean = false,
    val timezone: String = "",
    val title: String,
    val description: String = "",
    val location: String = "",
    val meetingUrl: String = "",
    val color: String = "",
    val reminders: List<Int> = emptyList(),
    val recurrence: CalendarRecurrence = CalendarRecurrence.NONE,
    val recurrenceUntil: Long = 0,
    val attendees: List<CalendarAttendee> = emptyList(),
    val updatedAt: Long = 0,
) {
    val durationMs: Long get() = maxOf(0, endAt - startAt)

    fun attendeeStatus(account: String): String? = attendees.firstOrNull { it.name.equals(account, ignoreCase = true) }?.status

    fun isOwnedBy(account: String): Boolean = owner.equals(account, ignoreCase = true)

    companion object {
        fun fromJson(json: JsonMap): CalendarEvent? {
            val id = json.str("id")
            if (id.isEmpty()) return null
            return CalendarEvent(
                id = id,
                owner = json.str("owner"),
                calendar = json.str("calendar", "personal"),
                scope = json.str("scope", "personal"),
                startAt = json.long("startAt"),
                endAt = json.long("endAt"),
                allDay = json.bool("allDay"),
                timezone = json.str("timezone"),
                title = json.str("title"),
                description = json.str("description"),
                location = json.str("location"),
                meetingUrl = json.str("meetingUrl"),
                color = json.str("color"),
                reminders = json.list("reminders").filterIsInstance<Number>().map { it.toInt() },
                recurrence = CalendarRecurrence.of(json.str("recurrence")),
                recurrenceUntil = json.long("recurrenceUntil"),
                attendees = json.maps("attendees").map { CalendarAttendee(it.str("name"), it.str("status", "invited")) },
                updatedAt = json.long("updatedAt"),
            )
        }
    }
}

data class CalendarInfo(val id: String, val name: String, val scope: String, val role: String = "")

data class CalendarOccurrence(val event: CalendarEvent, val startAt: Long, val endAt: Long) {
    val id: String get() = "${event.id}@$startAt"
}

data class CalendarAgendaDay(val day: String, val items: List<CalendarOccurrence>)

data class CalendarRange(val start: Long, val end: Long)

object CalendarMath {
    const val MAX_OCCURRENCES_PER_EVENT = 1_000

    private fun at(ms: Long, zone: ZoneId): ZonedDateTime = Instant.ofEpochMilli(ms).atZone(zone)

    private fun add(base: ZonedDateTime, recurrence: CalendarRecurrence, count: Int): ZonedDateTime = when (recurrence) {
        CalendarRecurrence.DAILY -> base.plusDays(count.toLong())
        CalendarRecurrence.WEEKLY -> base.plusWeeks(count.toLong())
        CalendarRecurrence.MONTHLY -> base.plusMonths(count.toLong())
        CalendarRecurrence.YEARLY -> base.plusYears(count.toLong())
        CalendarRecurrence.NONE -> base
    }

    fun occurrences(event: CalendarEvent, rangeStart: Long, rangeEnd: Long, zone: ZoneId): List<CalendarOccurrence> {
        if (rangeEnd <= rangeStart) return emptyList()
        val duration = event.durationMs
        if (event.recurrence == CalendarRecurrence.NONE) {
            val inside = if (duration == 0L) {
                event.startAt in rangeStart until rangeEnd
            } else {
                event.endAt > rangeStart && event.startAt < rangeEnd
            }
            return if (inside) listOf(CalendarOccurrence(event, event.startAt, event.endAt)) else emptyList()
        }
        val eventZone = runCatching { ZoneId.of(event.timezone) }.getOrNull() ?: zone
        val base = at(event.startAt, eventZone)
        val until = if (event.recurrenceUntil > 0) minOf(event.recurrenceUntil, rangeEnd) else rangeEnd
        val out = ArrayList<CalendarOccurrence>()
        var index = 0
        if (duration > 0 || rangeStart > event.startAt) {
            val span = maxOf(0, rangeStart - duration - event.startAt).toDouble() / 1_000
            val unit = when (event.recurrence) {
                CalendarRecurrence.DAILY -> 86_400.0
                CalendarRecurrence.WEEKLY -> 7 * 86_400.0
                CalendarRecurrence.MONTHLY -> 31 * 86_400.0
                else -> 366 * 86_400.0
            }
            index = maxOf(0, (span / unit).toInt() - 2)
        }
        while (out.size < MAX_OCCURRENCES_PER_EVENT) {
            val start = add(base, event.recurrence, index).toInstant().toEpochMilli()
            if (start >= until) break
            val end = start + duration
            if (end > rangeStart || (duration == 0L && start >= rangeStart)) out.add(CalendarOccurrence(event, start, end))
            index++
        }
        return out
    }

    fun occurrences(events: List<CalendarEvent>, rangeStart: Long, rangeEnd: Long, zone: ZoneId): List<CalendarOccurrence> =
        events.flatMap { occurrences(it, rangeStart, rangeEnd, zone) }
            .sortedWith(compareBy<CalendarOccurrence> { it.startAt }.thenBy { it.event.title })

    fun dayKey(ms: Long, zone: ZoneId): String {
        val date = at(ms, zone).toLocalDate()
        return "%04d-%02d-%02d".format(date.year, date.monthValue, date.dayOfMonth)
    }

    fun parseDayKey(key: String, zone: ZoneId): Long? {
        val parts = key.split('-').mapNotNull { it.toIntOrNull() }
        if (parts.size != 3 || parts[1] !in 1..12 || parts[2] !in 1..31) return null
        val date = runCatching { LocalDate.of(parts[0], parts[1], parts[2]) }.getOrNull() ?: return null
        return date.atStartOfDay(zone).toInstant().toEpochMilli()
    }

    fun startOfDay(ms: Long, zone: ZoneId): Long = at(ms, zone).toLocalDate().atStartOfDay(zone).toInstant().toEpochMilli()

    fun addDays(ms: Long, days: Int, zone: ZoneId): Long = at(ms, zone).plusDays(days.toLong()).toInstant().toEpochMilli()

    fun addMonths(ms: Long, months: Int, zone: ZoneId): Long = at(ms, zone).plusMonths(months.toLong()).toInstant().toEpochMilli()

    fun weekday(ms: Long, zone: ZoneId): Int = at(ms, zone).dayOfWeek.value % 7 + 1

    fun startOfWeek(ms: Long, firstWeekday: Int, zone: ZoneId): Long {
        val day = startOfDay(ms, zone)
        val back = (weekday(day, zone) - firstWeekday + 7) % 7
        return addDays(day, -back, zone)
    }

    fun startOfMonth(ms: Long, zone: ZoneId): Long =
        at(ms, zone).toLocalDate().withDayOfMonth(1).atStartOfDay(zone).toInstant().toEpochMilli()

    fun monthGrid(ms: Long, firstWeekday: Int, zone: ZoneId): List<Long> {
        val start = startOfWeek(startOfMonth(ms, zone), firstWeekday, zone)
        return (0 until 42).map { addDays(start, it, zone) }
    }

    fun weekDays(ms: Long, firstWeekday: Int, zone: ZoneId): List<Long> {
        val start = startOfWeek(ms, firstWeekday, zone)
        return (0 until 7).map { addDays(start, it, zone) }
    }

    fun range(mode: CalendarMode, anchor: Long, firstWeekday: Int, zone: ZoneId): CalendarRange = when (mode) {
        CalendarMode.DAY -> startOfDay(anchor, zone).let { CalendarRange(it, addDays(it, 1, zone)) }
        CalendarMode.WEEK -> startOfWeek(anchor, firstWeekday, zone).let { CalendarRange(it, addDays(it, 7, zone)) }
        CalendarMode.MONTH -> monthGrid(anchor, firstWeekday, zone).let { CalendarRange(it[0], addDays(it[41], 1, zone)) }
        CalendarMode.AGENDA -> startOfDay(anchor, zone).let { CalendarRange(it, addDays(it, 30, zone)) }
    }

    fun agenda(occurrences: List<CalendarOccurrence>, zone: ZoneId): List<CalendarAgendaDay> {
        val groups = LinkedHashMap<String, MutableList<CalendarOccurrence>>()
        for (occurrence in occurrences) groups.getOrPut(dayKey(occurrence.startAt, zone)) { ArrayList() }.add(occurrence)
        return groups.map { CalendarAgendaDay(it.key, it.value) }
    }

    fun overlaps(occurrence: CalendarOccurrence, dayStart: Long, zone: ZoneId): Boolean {
        val dayEnd = addDays(dayStart, 1, zone)
        if (occurrence.endAt == occurrence.startAt) return occurrence.startAt in dayStart until dayEnd
        return occurrence.startAt < dayEnd && occurrence.endAt > dayStart
    }
}

data class CalendarEventDraft(
    val title: String = "",
    val startAt: Long,
    val endAt: Long,
    val allDay: Boolean = false,
    val timezone: String,
    val calendar: String = "personal",
    val description: String = "",
    val location: String = "",
    val meetingUrl: String = "",
    val reminders: List<Int> = DEFAULT_REMINDERS,
    val recurrence: CalendarRecurrence = CalendarRecurrence.NONE,
    val recurrenceUntil: Long = 0,
    val attendees: List<String> = emptyList(),
) {
    val validationError: String?
        get() {
            val trimmed = title.trim()
            if (trimmed.isEmpty()) return "Give the event a title."
            if (trimmed.length > TITLE_MAX_LENGTH) return "Keep the title under 200 characters."
            if (endAt <= startAt) return "The event must end after it starts."
            if (reminders.size > 5) return "At most five reminders."
            if (reminders.any { it !in ALLOWED_REMINDERS }) return "Choose reminders from the list."
            val link = meetingUrl.trim()
            if (link.isNotEmpty() && runCatching { URI(link).scheme?.lowercase() }.getOrNull() != "https") {
                return "Meeting links must start with https://."
            }
            if (recurrence != CalendarRecurrence.NONE && recurrenceUntil > 0 && recurrenceUntil < startAt) {
                return "The repeat must end after the first event."
            }
            if (attendees.size > 50) return "Invite at most 50 people."
            return null
        }

    val body: Map<String, Any?>
        get() {
            val body = linkedMapOf<String, Any?>(
                "title" to title.trim(),
                "startAt" to startAt,
                "endAt" to endAt,
                "allDay" to allDay,
                "description" to description,
                "location" to location.trim(),
                "meetingUrl" to meetingUrl.trim(),
                "reminders" to reminders.toSet().sortedDescending().map { it.toLong() },
                "recurrence" to recurrence.wire,
                "recurrenceUntil" to if (recurrence == CalendarRecurrence.NONE) 0L else recurrenceUntil,
                "attendees" to attendees.map { it.trim().lowercase() }.filter { it.isNotEmpty() },
            )
            if (timezone.isNotEmpty()) body["timezone"] = timezone
            if (calendar == "personal" || calendar.isEmpty()) {
                body["scope"] = "personal"
            } else {
                body["scope"] = "organization"
                body["calendar"] = calendar
            }
            return body
        }

    companion object {
        val ALLOWED_REMINDERS = listOf(0, 5, 10, 15, 30, 60, 120, 1_440, 2_880, 10_080)
        val DEFAULT_REMINDERS = listOf(1_440, 60, 10)
        const val TITLE_MAX_LENGTH = 200

        fun of(event: CalendarEvent) = CalendarEventDraft(
            title = event.title,
            startAt = event.startAt,
            endAt = event.endAt,
            allDay = event.allDay,
            timezone = event.timezone,
            calendar = event.calendar,
            description = event.description,
            location = event.location,
            meetingUrl = event.meetingUrl,
            reminders = event.reminders,
            recurrence = event.recurrence,
            recurrenceUntil = event.recurrenceUntil,
            attendees = event.attendees.map { it.name },
        )

        fun reminderLabel(minutes: Int): String = when (minutes) {
            0 -> "At the start"
            1_440 -> "1 day before"
            2_880 -> "2 days before"
            10_080 -> "1 week before"
            else -> if (minutes >= 60) "${minutes / 60} hour${if (minutes == 60) "" else "s"} before" else "$minutes minutes before"
        }
    }
}
