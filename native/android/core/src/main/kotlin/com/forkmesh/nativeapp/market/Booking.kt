package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale

object Booking {
    const val PACKAGE_ID = "booking"
    const val API = "/api/booking"
    val DAYS: List<Pair<String, String>> = listOf(
        "mon" to "Monday", "tue" to "Tuesday", "wed" to "Wednesday", "thu" to "Thursday",
        "fri" to "Friday", "sat" to "Saturday", "sun" to "Sunday",
    )
    val LOCATION_LABELS: Map<String, String> = linkedMapOf(
        "video" to "Video call", "phone" to "Phone call", "place" to "In person", "custom" to "Other", "none" to "No location",
    )
    val LOCATION_HINTS: Map<String, String> = mapOf(
        "video" to "https://meet.example.com/you", "phone" to "Leave empty to ask the guest for their number",
        "place" to "Address", "custom" to "Shown after booking", "none" to "",
    )
    private val CLOCK = Regex("^([01]\\d|2[0-3]):([0-5]\\d)$|^24:00$")
    private val SLUG = Regex("^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$")

    fun displayLink(link: String): String = link.removePrefix("https://").removePrefix("http://")

    fun host(origin: String): String = displayLink(origin).trimEnd('/')

    fun slugIsValid(value: String): Boolean = SLUG.matches(value)

    fun minutesLabel(value: Int): String {
        if (value < 60) return "$value min"
        val hours = value / 60
        val rest = value % 60
        return if (rest == 0) "$hours h" else "$hours h $rest min"
    }

    fun noticeLabel(minutes: Int): String = when {
        minutes <= 0 -> "Any time"
        minutes < 60 -> "$minutes minutes before"
        minutes < 1440 -> (minutes / 60).let { "$it hour${if (it == 1) "" else "s"} before" }
        else -> (minutes / 1440).let { "$it day${if (it == 1) "" else "s"} before" }
    }

    fun gapLabel(minutes: Int): String = if (minutes <= 0) "No gap" else "$minutes minutes"

    fun stepLabel(minutes: Int): String = if (minutes <= 0) "Meeting length" else "$minutes minutes"

    fun horizonLabel(days: Int): String = "$days days ahead"

    fun dailyLimitLabel(limit: Int): String = if (limit <= 0) "No limit" else limit.toString()

    fun clockIsValid(value: String): Boolean = CLOCK.matches(value)

    fun clockMinutes(value: String): Int {
        if (value == "24:00") return 24 * 60
        val parts = value.split(":")
        return (parts.getOrNull(0)?.toIntOrNull() ?: 0) * 60 + (parts.getOrNull(1)?.toIntOrNull() ?: 0)
    }

    fun shownEnd(value: String): String = if (value == "24:00") "23:59" else value

    fun savedEnd(value: String): String = if (value == "23:59") "24:00" else value

    fun timeRange(startAt: Long, endAt: Long, zone: ZoneId, twentyFour: Boolean, locale: Locale = Locale.getDefault()): String {
        val format = DateTimeFormatter.ofPattern(if (twentyFour) "HH:mm" else "h:mm a", locale)
        return format.format(Instant.ofEpochMilli(startAt).atZone(zone)) + " – " + format.format(Instant.ofEpochMilli(endAt).atZone(zone))
    }

    fun dayLabel(day: LocalDate, locale: Locale = Locale.getDefault()): String =
        DateTimeFormatter.ofPattern("EEEE, MMMM d", locale).format(day)

    fun groupByDay(bookings: List<BookingItem>, zone: ZoneId): List<Pair<LocalDate, List<BookingItem>>> {
        val groups = ArrayList<Pair<LocalDate, MutableList<BookingItem>>>()
        for (booking in bookings) {
            val day = Instant.ofEpochMilli(booking.startAt).atZone(zone).toLocalDate()
            if (groups.isEmpty() || groups.last().first != day) groups.add(day to mutableListOf())
            groups.last().second.add(booking)
        }
        return groups.map { it.first to it.second.toList() }
    }
}

data class BookingLocation(val kind: String = "video", val detail: String = "") {
    val label: String get() = Booking.LOCATION_LABELS[kind] ?: kind

    fun body(): Map<String, Any?> = linkedMapOf("kind" to kind, "detail" to detail.trim())

    companion object {
        fun fromJson(value: Any?): BookingLocation {
            val json = value.asJsonMap()
            return BookingLocation(json.str("kind").ifEmpty { "video" }, json.str("detail"))
        }
    }
}

data class BookingType(
    val id: String = "",
    val title: String = "",
    val minutes: Int = 30,
    val description: String = "",
    val location: BookingLocation = BookingLocation(),
    val enabled: Boolean = true,
) {
    fun body(): Map<String, Any?> = linkedMapOf(
        "id" to id, "title" to title.trim(), "minutes" to minutes, "description" to description.trim(),
        "location" to location.body(), "enabled" to enabled,
    )

    companion object {
        fun fromJson(json: JsonMap) = BookingType(
            json.str("id"), json.str("title"), json.int("minutes", 30), json.str("description"),
            BookingLocation.fromJson(json["location"]), json["enabled"] != false,
        )

        fun blank() = BookingType(title = "New meeting")
    }
}

data class BookingGuest(val name: String = "", val email: String = "", val phone: String = "", val timezone: String = "", val notes: String = "") {
    companion object {
        fun fromJson(json: JsonMap) = BookingGuest(json.str("name"), json.str("email"), json.str("phone"), json.str("timezone"), json.str("notes"))
    }
}

data class BookingItem(
    val id: String,
    val status: String = "confirmed",
    val typeId: String = "",
    val typeTitle: String = "",
    val minutes: Int = 0,
    val startAt: Long = 0,
    val endAt: Long = 0,
    val createdAt: Long = 0,
    val cancelledAt: Long = 0,
    val cancelledBy: String = "",
    val cancelReason: String = "",
    val guest: BookingGuest = BookingGuest(),
    val location: BookingLocation = BookingLocation("none"),
    val calendarEventId: String = "",
    val rescheduledFrom: Long = 0,
) {
    val cancelled: Boolean get() = status == "cancelled"

    fun finished(nowMs: Long): Boolean = !cancelled && endAt < nowMs

    fun where(): String = when (location.kind) {
        "phone" -> location.detail.ifEmpty { guest.phone.ifEmpty { "Phone call" } }
        else -> location.detail.ifEmpty { if (location.kind == "none") "" else location.label }
    }

    fun badge(nowMs: Long): String = when {
        cancelled && cancelledBy == "owner" -> "Cancelled by you"
        cancelled && cancelledBy == "guest" -> "Cancelled by guest"
        cancelled -> "Cancelled"
        finished(nowMs) -> "Done"
        else -> ""
    }

    companion object {
        fun fromJson(json: JsonMap): BookingItem? {
            val id = json.str("id")
            if (id.isEmpty()) return null
            return BookingItem(
                id, json.str("status").ifEmpty { "confirmed" }, json.str("typeId"), json.str("typeTitle"), json.int("minutes"),
                json.long("startAt"), json.long("endAt"), json.long("createdAt"), json.long("cancelledAt"), json.str("cancelledBy"),
                json.str("cancelReason"), BookingGuest.fromJson(json.obj("guest")), BookingLocation.fromJson(json["location"]),
                json.str("calendarEventId"), json.long("rescheduledFrom"),
            )
        }
    }
}

data class BookingStats(
    val upcoming: Int = 0,
    val next7Days: Int = 0,
    val booked30Days: Int = 0,
    val cancelled30Days: Int = 0,
    val bookedMinutes7Days: Int = 0,
) {
    companion object {
        fun fromJson(json: JsonMap) = BookingStats(
            json.int("upcoming"), json.int("next7Days"), json.int("booked30Days"), json.int("cancelled30Days"), json.int("bookedMinutes7Days"),
        )
    }
}

data class BookingOptions(
    val durations: List<Int> = listOf(10, 15, 20, 25, 30, 45, 50, 60, 75, 90, 120, 180, 240),
    val steps: List<Int> = listOf(0, 5, 10, 15, 20, 30, 60),
    val buffers: List<Int> = listOf(0, 5, 10, 15, 30, 45, 60),
    val notices: List<Int> = listOf(0, 30, 60, 120, 240, 480, 720, 1440, 2880, 4320, 10080),
    val horizons: List<Int> = listOf(7, 14, 21, 30, 45, 60, 90, 120, 180, 365),
    val locations: List<String> = Booking.LOCATION_LABELS.keys.toList(),
    val accents: Map<String, String> = linkedMapOf(
        "blue" to "#2563eb", "violet" to "#7c3aed", "teal" to "#0d9488", "rose" to "#e11d48", "amber" to "#d97706", "slate" to "#475569",
    ),
) {
    companion object {
        private fun ints(json: JsonMap, key: String, fallback: List<Int>): List<Int> =
            json.list(key).map { jsonLong(it, -1).toInt() }.filter { it >= 0 }.ifEmpty { fallback }

        fun fromJson(json: JsonMap): BookingOptions {
            val defaults = BookingOptions()
            val accents = json.obj("accents").mapValues { jsonText(it.value) }.filterValues { it.startsWith("#") }
            return BookingOptions(
                ints(json, "durations", defaults.durations), ints(json, "steps", defaults.steps), ints(json, "buffers", defaults.buffers),
                ints(json, "notices", defaults.notices), ints(json, "horizons", defaults.horizons),
                json.list("locations").map(::jsonText).filter { it.isNotEmpty() }.ifEmpty { defaults.locations },
                accents.ifEmpty { defaults.accents },
            )
        }
    }
}

data class BookingWindow(val start: String = "09:00", val end: String = "17:00") {
    val valid: Boolean
        get() = Booking.clockIsValid(start) && Booking.clockIsValid(Booking.savedEnd(end)) && start != "24:00" &&
            Booking.clockMinutes(Booking.savedEnd(end)) > Booking.clockMinutes(start)
}

data class BookingSettings(
    val displayName: String = "",
    val headline: String = "",
    val welcome: String = "",
    val timezone: String = "UTC",
    val timeFormat: String = "12h",
    val accent: String = "blue",
    val availability: Map<String, List<BookingWindow>> = emptyMap(),
    val minNoticeMinutes: Int = 240,
    val maxDaysAhead: Int = 60,
    val bufferMinutes: Int = 0,
    val slotStepMinutes: Int = 0,
    val dailyLimit: Int = 0,
    val replyTo: String = "",
    val notifyEmail: String = "",
    val emailOwner: Boolean = true,
    val addToCalendar: Boolean = true,
    val checkCalendar: Boolean = true,
    val types: List<BookingType> = emptyList(),
) {
    val twentyFour: Boolean get() = timeFormat == "24h"

    fun windows(day: String): List<BookingWindow> = availability[day].orEmpty()

    fun withDay(day: String, on: Boolean): BookingSettings =
        copy(availability = availability + (day to if (on) listOf(BookingWindow()) else emptyList()))

    fun addWindow(day: String): BookingSettings =
        copy(availability = availability + (day to windows(day) + BookingWindow("13:00", "17:00")))

    fun window(day: String, index: Int, change: (BookingWindow) -> BookingWindow): BookingSettings =
        copy(availability = availability + (day to windows(day).mapIndexed { i, window -> if (i == index) change(window) else window }))

    fun removeWindow(day: String, index: Int): BookingSettings =
        copy(availability = availability + (day to windows(day).filterIndexed { i, _ -> i != index }))

    fun type(index: Int, change: (BookingType) -> BookingType): BookingSettings =
        copy(types = types.mapIndexed { i, type -> if (i == index) change(type) else type })

    fun addType(): BookingSettings = copy(types = types + BookingType.blank())

    fun removeType(index: Int): BookingSettings = copy(types = types.filterIndexed { i, _ -> i != index })

    fun availabilityBody(): Map<String, Any?> = linkedMapOf<String, Any?>().apply {
        for ((day, _) in Booking.DAYS) put(day, windows(day).filter { it.valid }.map { listOf(it.start, Booking.savedEnd(it.end)) })
    }

    fun body(): Map<String, Any?> = linkedMapOf(
        "displayName" to displayName.trim(), "headline" to headline.trim(), "welcome" to welcome.trim(),
        "timezone" to timezone, "timeFormat" to timeFormat, "accent" to accent, "availability" to availabilityBody(),
        "minNoticeMinutes" to minNoticeMinutes, "maxDaysAhead" to maxDaysAhead, "bufferMinutes" to bufferMinutes,
        "slotStepMinutes" to slotStepMinutes, "dailyLimit" to dailyLimit.coerceIn(0, 50), "replyTo" to replyTo.trim(),
        "notifyEmail" to notifyEmail.trim(), "emailOwner" to emailOwner, "addToCalendar" to addToCalendar,
        "checkCalendar" to checkCalendar, "types" to types.map { it.body() },
    )

    companion object {
        fun fromJson(json: JsonMap): BookingSettings {
            val week = json.obj("availability")
            val availability = Booking.DAYS.associate { (day, _) ->
                day to week.list(day).mapNotNull { raw ->
                    val pair = raw as? List<*> ?: return@mapNotNull null
                    BookingWindow(jsonText(pair.getOrNull(0)), Booking.shownEnd(jsonText(pair.getOrNull(1))))
                }
            }
            return BookingSettings(
                json.str("displayName"), json.str("headline"), json.str("welcome"), json.str("timezone").ifEmpty { "UTC" },
                json.str("timeFormat").ifEmpty { "12h" }, json.str("accent").ifEmpty { "blue" }, availability,
                json.int("minNoticeMinutes", 240), json.int("maxDaysAhead", 60), json.int("bufferMinutes"),
                json.int("slotStepMinutes"), json.int("dailyLimit"), json.str("replyTo"), json.str("notifyEmail"),
                json["emailOwner"] != false, json["addToCalendar"] != false, json["checkCalendar"] != false,
                json.maps("types").map(BookingType::fromJson),
            )
        }
    }
}

data class BookingOverview(
    val installed: Boolean = false,
    val actor: String = "",
    val suggestedSlug: String = "",
    val origin: String = "https://forkmesh.com",
    val slug: String = "",
    val enabled: Boolean = true,
    val link: String = "",
    val links: Map<String, String> = emptyMap(),
    val settings: BookingSettings = BookingSettings(),
    val options: BookingOptions = BookingOptions(),
    val upcoming: List<BookingItem> = emptyList(),
    val past: List<BookingItem> = emptyList(),
    val stats: BookingStats = BookingStats(),
) {
    val prefix: String get() = Booking.host(origin) + "/meet?"

    fun booking(id: String): BookingItem? = upcoming.firstOrNull { it.id == id } ?: past.firstOrNull { it.id == id }

    fun inPast(id: String): Boolean = past.any { it.id == id }

    companion object {
        fun fromJson(json: JsonMap): BookingOverview = BookingOverview(
            json["installed"] != false, json.str("actor"), json.str("suggestedSlug"),
            json.str("origin").ifEmpty { "https://forkmesh.com" }, json.str("slug"), json["enabled"] != false, json.str("link"),
            json.obj("links").mapValues { jsonText(it.value) }, BookingSettings.fromJson(json.obj("settings")),
            BookingOptions.fromJson(json.obj("options")), json.maps("upcoming").mapNotNull(BookingItem::fromJson),
            json.maps("past").mapNotNull(BookingItem::fromJson), BookingStats.fromJson(json.obj("stats")),
        )
    }
}

data class BookingSlugCheck(val slug: String, val available: Boolean, val reason: String) {
    val sentence: String get() = if (available) "Available" else BookingErrors.sentence(reason) ?: "Not available"
}

data class BookingExport(val filename: String, val json: Map<String, Any?>)

object BookingErrors {
    fun sentence(code: String): String? = when (code) {
        "slug_taken" -> "That link is taken. Try another."
        "invalid_slug" -> "Links use lowercase letters, numbers and dashes."
        "slug_reserved" -> "That link is reserved. Try another."
        "meeting_type_required" -> "Keep at least one meeting type."
        "invalid_meeting_url" -> "A video call link must start with https://."
        "invalid_email" -> "Enter a valid email address."
        "booking_locked" -> "That meeting already happened or was cancelled."
        "booking_not_found" -> "That meeting was removed."
        "plugin_not_installed" -> "Create your booking page first."
        else -> null
    }

    fun describe(error: Throwable): String {
        if (error is PackagesException) sentence(error.code)?.let { return it }
        return PackagesException.describe(error)
    }
}

fun PackagesClient.booking(): BookingOverview = BookingOverview.fromJson(call("GET", Booking.API))

fun PackagesClient.installBooking(slug: String, timezone: String): BookingOverview =
    BookingOverview.fromJson(call("POST", Booking.API + "/install", body = linkedMapOf("slug" to slug.trim().lowercase(), "timezone" to timezone)))

fun PackagesClient.saveBooking(body: Map<String, Any?>): BookingOverview = BookingOverview.fromJson(call("PUT", Booking.API + "/page", body = body))

fun PackagesClient.checkBookingSlug(slug: String): BookingSlugCheck {
    val clean = slug.trim().lowercase()
    if (!Booking.slugIsValid(clean)) return BookingSlugCheck(clean, false, "invalid_slug")
    val reply = call("GET", Booking.API + "/slug/" + clean)
    return BookingSlugCheck(clean, reply.bool("available"), reply.str("reason"))
}

fun PackagesClient.cancelBooking(id: String, reason: String, notifyGuest: Boolean): BookingOverview =
    BookingOverview.fromJson(call("POST", Booking.API + "/bookings/" + item(id) + "/cancel", body = linkedMapOf("reason" to reason.trim(), "notifyGuest" to notifyGuest)))

fun PackagesClient.exportBooking(): BookingExport {
    val reply = call("GET", Booking.API + "/export")
    return BookingExport(reply.str("filename").ifEmpty { "forkmesh-booking.json" }, reply.obj("export"))
}

fun PackagesClient.uninstallBooking() {
    call("POST", Booking.API + "/uninstall", body = emptyMap())
}
