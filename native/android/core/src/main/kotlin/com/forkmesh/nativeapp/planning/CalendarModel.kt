package com.forkmesh.nativeapp.planning

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import java.time.ZoneId
import java.time.temporal.WeekFields
import java.util.Locale

class CalendarModel(
    private val api: ApiService,
    var mode: CalendarMode = CalendarMode.MONTH,
    anchorDay: String? = null,
    val zone: ZoneId = ZoneId.systemDefault(),
    val firstWeekday: Int = WeekFields.of(Locale.getDefault()).firstDayOfWeek.value % 7 + 1,
    private val clock: () -> Long = System::currentTimeMillis,
) {
    @Volatile
    var events: List<CalendarEvent> = emptyList()
        private set
    var calendars: List<CalendarInfo> = listOf(CalendarInfo("personal", "Personal", "personal"))
        private set
    var actor: String = ""
        private set
    var error: String? = null
    var anchor: Long = anchorDay?.let { CalendarMath.parseDayKey(it, zone) } ?: CalendarMath.startOfDay(clock(), zone)
        private set
    val hiddenCalendars: MutableSet<String> = linkedSetOf()
    private var loadedRange: CalendarRange? = null
    private val lock = Any()
    private var loadToken = 0
    private var revision = 0
    private val localChanges = ArrayList<LocalChange>()

    private class LocalChange(val revision: Int, val id: String, val event: CalendarEvent?)

    val range: CalendarRange get() = CalendarMath.range(mode, anchor, firstWeekday, zone)

    val visibleEvents: List<CalendarEvent> get() = events.filter { it.calendar !in hiddenCalendars }

    val occurrences: List<CalendarOccurrence> get() = range.let { CalendarMath.occurrences(visibleEvents, it.start, it.end, zone) }

    fun occurrencesOnDay(dayStart: Long): List<CalendarOccurrence> = occurrences.filter { CalendarMath.overlaps(it, dayStart, zone) }

    val days: List<Long>
        get() = when (mode) {
            CalendarMode.MONTH -> CalendarMath.monthGrid(anchor, firstWeekday, zone)
            CalendarMode.WEEK -> CalendarMath.weekDays(anchor, firstWeekday, zone)
            CalendarMode.DAY -> listOf(CalendarMath.startOfDay(anchor, zone))
            CalendarMode.AGENDA -> emptyList()
        }

    val agenda: List<CalendarAgendaDay> get() = CalendarMath.agenda(occurrences, zone)

    val today: Long get() = CalendarMath.startOfDay(clock(), zone)

    val anchorKey: String get() = CalendarMath.dayKey(anchor, zone)

    fun isInAnchorMonth(day: Long): Boolean = CalendarMath.startOfMonth(day, zone) == CalendarMath.startOfMonth(anchor, zone)

    fun step(direction: Int) {
        anchor = when (mode) {
            CalendarMode.DAY -> CalendarMath.addDays(anchor, direction, zone)
            CalendarMode.WEEK -> CalendarMath.addDays(anchor, 7 * direction, zone)
            CalendarMode.MONTH -> CalendarMath.addMonths(CalendarMath.startOfMonth(anchor, zone), direction, zone)
            CalendarMode.AGENDA -> CalendarMath.addDays(anchor, 30 * direction, zone)
        }
    }

    fun goToToday() {
        anchor = today
    }

    fun select(day: Long) {
        anchor = CalendarMath.startOfDay(day, zone)
    }

    fun needsLoad(): Boolean {
        val wanted = range
        val loaded = loadedRange ?: return true
        return !(loaded.start <= wanted.start && loaded.end >= wanted.end)
    }

    fun loadIfNeeded() {
        if (needsLoad()) load()
    }

    fun load() {
        val wanted = range
        val start = CalendarMath.addDays(wanted.start, -31, zone)
        val end = CalendarMath.addDays(wanted.end, 31, zone)
        val (token, seen) = synchronized(lock) {
            loadToken++
            loadToken to revision
        }
        try {
            val data = api.accountApiCall("GET", "/api/calendar?start=$start&end=$end", null)
            synchronized(lock) {
                if (token != loadToken) return
                events = merged(data.maps("events").take(500).mapNotNull(CalendarEvent::fromJson), seen)
            }
            val infos = data.maps("calendars").mapNotNull { map ->
                val id = map.str("id", map.str("name"))
                if (id.isEmpty()) null else CalendarInfo(id, map.str("name", id), map.str("scope"), map.str("role"))
            }
            if (infos.isNotEmpty()) calendars = infos
            actor = data.str("actor").lowercase()
            loadedRange = CalendarRange(start, end)
            error = null
        } catch (thrown: Exception) {
            synchronized(lock) {
                if (token != loadToken) return
                error = PlanningErrors.text(thrown, "The calendar could not be loaded.")
            }
        }
    }

    private fun merged(server: List<CalendarEvent>, seen: Int): List<CalendarEvent> {
        localChanges.removeAll { it.revision <= seen }
        var result = server
        for (change in localChanges) {
            result = result.filter { it.id != change.id }
            change.event?.let { result = result + it }
        }
        return result
    }

    private fun recordChange(id: String, event: CalendarEvent?) {
        revision++
        localChanges.add(LocalChange(revision, id, event))
    }

    fun clearError() {
        error = null
    }

    fun event(id: String): CalendarEvent? = events.firstOrNull { it.id == id }

    fun loadEvent(id: String) {
        if (!validId(id)) {
            error = "That event link is not valid."
            return
        }
        try {
            val data = api.accountApiCall("GET", "/api/calendar/$id", null)
            upsert(data, local = false)
            if (actor.isEmpty()) actor = data.str("actor").lowercase()
            error = null
        } catch (thrown: Exception) {
            error = PlanningErrors.text(thrown, "The event could not be opened.")
        }
    }

    fun canEdit(event: CalendarEvent): Boolean {
        if (event.isOwnedBy(actor)) return true
        val role = calendars.firstOrNull { it.name == event.calendar || it.id == event.calendar }?.role ?: ""
        return event.scope == "organization" && (role == "owner" || role == "admin")
    }

    fun newDraft(day: Long? = null): CalendarEventDraft {
        val base = day ?: anchor
        val now = clock()
        val hour = 3_600_000L
        val start = if (CalendarMath.startOfDay(now, zone) == CalendarMath.startOfDay(base, zone)) {
            (now / hour + 1) * hour
        } else {
            CalendarMath.startOfDay(base, zone) + 9 * hour
        }
        return CalendarEventDraft(startAt = start, endAt = start + hour, timezone = zone.id)
    }

    private fun mutate(method: String, path: String, body: Map<String, Any?>?, fallback: String): JsonMap? = try {
        api.accountApiCall(method, path, body).also { error = null }
    } catch (thrown: Exception) {
        error = PlanningErrors.text(thrown, fallback)
        null
    }

    private fun upsert(data: JsonMap, local: Boolean = true): CalendarEvent? {
        val event = data.obj("event").takeIf { it.isNotEmpty() }?.let(CalendarEvent::fromJson) ?: return null
        synchronized(lock) {
            events = events.filter { it.id != event.id } + event
            if (local) recordChange(event.id, event)
        }
        return event
    }

    fun create(draft: CalendarEventDraft): CalendarEvent? {
        draft.validationError?.let { error = it; return null }
        val data = mutate("POST", "/api/calendar", draft.body, "The event was not saved.") ?: return null
        return upsert(data)
    }

    fun update(id: String, draft: CalendarEventDraft): CalendarEvent? {
        draft.validationError?.let { error = it; return null }
        if (!validId(id)) return null
        val data = mutate("PATCH", "/api/calendar/$id", draft.body, "The event was not saved.") ?: return null
        return upsert(data)
    }

    fun delete(id: String): Boolean {
        if (!validId(id) || mutate("DELETE", "/api/calendar/$id", null, "The event was not deleted.") == null) return false
        synchronized(lock) {
            events = events.filter { it.id != id }
            recordChange(id, null)
        }
        return true
    }

    fun rsvp(id: String, status: String): Boolean {
        if (!validId(id) || status !in listOf("accepted", "declined", "tentative")) return false
        val data = mutate("POST", "/api/calendar/$id/rsvp", mapOf("status" to status), "Your answer was not sent.") ?: return false
        if (upsert(data) == null) {
            synchronized(lock) {
                events = events.map { event ->
                    if (event.id != id) event else event.copy(
                        attendees = event.attendees.map { if (it.name.lowercase() == actor) it.copy(status = status) else it },
                    )
                }
                event(id)?.let { recordChange(id, it) }
            }
        }
        return true
    }

    companion object {
        fun validId(id: String): Boolean = id.length == 32 && id.all { it in '0'..'9' || it in 'a'..'f' }
    }
}
