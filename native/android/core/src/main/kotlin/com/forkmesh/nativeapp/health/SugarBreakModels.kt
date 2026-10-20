package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.calories.Calories
import com.forkmesh.nativeapp.model.OrgApiException
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.concurrent.atomic.AtomicInteger
import java.util.Locale
import kotlin.math.max
import kotlin.math.min

enum class SugarDayStatus(val raw: String) {
    SugarFree("sugar-free"),
    Slip("slip"),
    Open("open"),
    Upcoming("upcoming");

    companion object {
        fun of(raw: String): SugarDayStatus? = entries.firstOrNull { it.raw == raw }
    }
}

data class SugarBreakDay(
    val day: String,
    val number: Int,
    val status: SugarDayStatus,
    val skips: Int,
    val slips: Int,
    val isToday: Boolean,
    val caption: String,
)

data class SugarBreakEventRow(val id: String, val slip: Boolean, val title: String, val time: String)

data class SugarBreakStat(val label: String, val value: String)

data class SugarBreakHistoryRow(val id: String, val title: String, val detail: String, val status: String)

data class SugarBreakLength(val days: Int, val label: String)

data class SugarNotice(val message: String, val isError: Boolean = false)

class SugarBreakModel(
    private val call: HealthCall,
    private val prefs: PrefStore? = null,
    val zone: ZoneId = ZoneId.systemDefault(),
    private val now: () -> Instant = { Instant.now() },
) {
    @Volatile var overview: HealthJson? = null
        private set
    @Volatile var installed = false
        private set
    @Volatile var loading = true
        private set
    @Volatile var error = ""
        private set
    @Volatile var busy = false
        private set
    private val pending = AtomicInteger(0)
    val pendingSkips: Int get() = pending.get()
    @Volatile var selectedLength: Int = DEFAULT_LENGTH
        private set
    @Volatile var notice: SugarNotice? = null
        private set
    @Volatile var onChange: (() -> Unit)? = null

    val today: String get() = HealthDates.isoDay(now(), zone)

    private fun changed() {
        onChange?.invoke()
    }

    fun load() {
        loading = true
        error = ""
        changed()
        try {
            fetch()
            loading = false
        } catch (failure: OrgApiException) {
            loading = false
            if (failure.code == "plugin_not_installed") {
                installed = false
                overview = null
            } else {
                error = message(failure)
            }
        } catch (failure: Exception) {
            loading = false
            error = message(failure)
        }
        changed()
    }

    private fun fetch() {
        val first = overview == null
        val payload = call.call("GET", "?day=$today", null)
        installed = HealthJsonRead.bool(payload["installed"])
        overview = if (installed) payload else null
        if (installed && first) {
            val preferred = HealthJsonRead.int(profile["defaultLengthDays"]).toInt()
            selectedLength = if (preferred in 1..365) preferred else DEFAULT_LENGTH
        }
    }

    fun message(error: Throwable): String {
        val failure = error as? OrgApiException ?: return "Sugar Break is unavailable right now."
        return when (failure.code) {
            "no_active_break" -> "Start a sugar break first."
            "day_outside_break" -> "That day is outside your sugar break."
            "day_full" -> "That day already holds as many entries as it can."
            "break_not_found", "event_not_found" -> "That was already removed."
            "invalid_kind" -> "Sugar Break did not understand that entry."
            else -> failure.message
        }
    }

    fun show(text: String, error: Boolean = false) {
        notice = SugarNotice(text, error)
        changed()
    }

    fun selectLength(days: Int) {
        selectedLength = days
        changed()
    }

    fun dismissNotice() {
        notice = null
        changed()
    }

    val showsSpinner: Boolean get() = loading && overview == null && error.isEmpty()
    val showsError: Boolean get() = error.isNotEmpty() && overview == null
    val showsInstall: Boolean get() = !loading && error.isEmpty() && !installed
    val showsStart: Boolean get() = installed && overview != null && !isActive

    private fun busyRun(block: () -> Unit) {
        busy = true
        changed()
        try {
            block()
        } finally {
            busy = false
            changed()
        }
    }

    fun install() {
        try {
            call.call("POST", "/install", emptyMap())
            show("Sugar Break installed.")
            load()
        } catch (failure: Exception) {
            show(message(failure), true)
        }
    }

    fun startBreak(lengthDays: Int? = null) {
        val length = min(365, max(1, lengthDays ?: selectedLength))
        busyRun {
            try {
                call.call("POST", "/breaks", linkedMapOf("lengthDays" to length.toLong(), "startDay" to today))
                show("Sugar break started: ${SugarBreakText.days(length)}. Tap +1 each time you skip sugar.")
                load()
            } catch (failure: Exception) {
                show(message(failure), true)
            }
        }
    }

    fun skip() {
        pending.incrementAndGet()
        changed()
        try {
            call.call("POST", "/events", linkedMapOf("kind" to "skip", "day" to today))
            try {
                fetch()
            } catch (_: Exception) {
            }
        } catch (failure: Exception) {
            show(message(failure), true)
        } finally {
            pending.decrementAndGet()
            changed()
        }
    }

    fun slip() {
        busyRun {
            try {
                call.call("POST", "/events", linkedMapOf("kind" to "slip", "day" to today))
                show("Logged. One slip is one day; keep going tomorrow.")
                try {
                    fetch()
                } catch (_: Exception) {
                }
            } catch (failure: Exception) {
                show(message(failure), true)
            }
        }
    }

    fun undo(eventId: String) {
        if (eventId.isEmpty()) return
        busyRun {
            try {
                call.call("DELETE", "/events/$eventId", null)
                show("Removed.")
                try {
                    fetch()
                } catch (_: Exception) {
                }
            } catch (failure: Exception) {
                show(message(failure), true)
            }
        }
    }

    fun endEarly() {
        if (!isActive || currentId.isEmpty()) return
        busyRun {
            try {
                call.call("PATCH", "/breaks/$currentId", linkedMapOf("end" to true))
                show("Sugar break ended with ${SugarBreakText.days(sugarFreeDays)} sugar-free.")
                load()
            } catch (failure: Exception) {
                show(message(failure), true)
            }
        }
    }

    fun deleteBreak(id: String) {
        if (id.isEmpty()) return
        busyRun {
            try {
                call.call("DELETE", "/breaks/$id", null)
                show("Sugar break deleted.")
                load()
            } catch (failure: Exception) {
                show(message(failure), true)
            }
        }
    }

    fun uninstall(): Boolean = try {
        call.call("POST", "/uninstall", emptyMap())
        prefs?.putBool(HealthPrefs.SUGAR_BREAK, false)
        show("Sugar Break uninstalled from your account.")
        load()
        true
    } catch (failure: Exception) {
        show(message(failure), true)
        false
    }

    val profile: HealthJson get() = HealthJsonRead.obj(overview?.get("profile"))
    val summary: HealthJson get() = HealthJsonRead.obj(overview?.get("summary"))
    val todayInfo: HealthJson get() = HealthJsonRead.obj(overview?.get("today"))
    val currentBreak: HealthJson? get() = (overview?.get("current") as? Map<*, *>)?.let { HealthJsonRead.obj(it) }
    val status: String get() = HealthJsonRead.string(currentBreak?.get("status"))
    val isActive: Boolean get() = status == "active"
    val currentId: String get() = HealthJsonRead.string(currentBreak?.get("id"))
    val startDay: String get() = HealthJsonRead.string(currentBreak?.get("startDay"))
    val endDay: String get() = HealthJsonRead.string(currentBreak?.get("endDay"))
    val endedDay: String get() = HealthJsonRead.string(currentBreak?.get("endedDay"))

    private fun int(value: Any?): Int = HealthJsonRead.int(value).toInt()

    val lengthDays: Int
        get() {
            val fromSummary = int(summary["lengthDays"])
            return if (fromSummary > 0) fromSummary else int(currentBreak?.get("lengthDays"))
        }

    val dayNumber: Int get() = int(summary["dayNumber"])
    val sugarFreeDays: Int get() = int(summary["sugarFreeDays"]) + if (optimisticSugarFree) 1 else 0
    val skipsTotal: Int get() = int(summary["skips"]) + pendingSkips
    val slipsTotal: Int get() = int(summary["slips"])
    val streak: Int get() = int(summary["streak"]) + if (optimisticSugarFree) 1 else 0
    val bestStreak: Int get() = max(int(summary["bestStreak"]), streak)
    val remainingDays: Int get() = int(summary["remainingDays"])
    val todaySkips: Int get() = int(todayInfo["skips"]) + pendingSkips
    val todaySlips: Int get() = int(todayInfo["slips"])

    private val optimisticSugarFree: Boolean
        get() = pendingSkips > 0 && isActive && int(todayInfo["skips"]) == 0 && todaySlips == 0

    val progressRatio: Double get() = if (lengthDays > 0) min(1.0, sugarFreeDays.toDouble() / lengthDays) else 0.0

    val headline: String
        get() {
            if (currentBreak == null) return ""
            return when (status) {
                "active" -> if (dayNumber > 0) "Day $dayNumber of $lengthDays" else "Starts ${dayLabel(startDay)}"
                "completed" -> "Sugar break complete"
                else -> "Sugar break ended"
            }
        }

    val progressLine: String get() = "$sugarFreeDays of ${SugarBreakText.days(lengthDays)} sugar-free"

    val remainingLine: String
        get() {
            if (currentBreak == null) return ""
            return when (status) {
                "active" ->
                    if (remainingDays == 0) "Last day today" else "${SugarBreakText.days(remainingDays)} left · ends ${dayLabel(endDay)}"
                "completed" -> "${SugarBreakText.monthDay(startDay)} – ${SugarBreakText.monthDay(endDay)}"
                else -> {
                    val ended = endedDay.ifEmpty { endDay }
                    "${SugarBreakText.monthDay(startDay)} – ended early on ${SugarBreakText.monthDay(ended)}"
                }
            }
        }

    val resultLine: String
        get() {
            if (currentBreak == null || isActive) return ""
            val lead = if (status == "completed") "You finished" else "You stopped"
            return "$lead with $sugarFreeDays of ${SugarBreakText.days(lengthDays)} sugar-free and +$skipsTotal in all. Best streak ${SugarBreakText.days(bestStreak)}."
        }

    val todayLine: String
        get() = when {
            todaySlips > 0 -> "You had sugar today. Tomorrow is a fresh start."
            todaySkips > 0 -> "+$todaySkips today. Sugar-free so far."
            else -> EMPTY_TODAY
        }

    val stats: List<SugarBreakStat>
        get() = listOf(
            SugarBreakStat("+1 total", skipsTotal.toString()),
            SugarBreakStat("Sugar-free days", "$sugarFreeDays of $lengthDays"),
            SugarBreakStat("Streak", SugarBreakText.days(streak)),
            SugarBreakStat("Best streak", SugarBreakText.days(bestStreak)),
            SugarBreakStat("Days left", remainingDays.toString()),
        )

    val days: List<SugarBreakDay>
        get() {
            val current = today
            val queued = pendingSkips
            return HealthJsonRead.objects(overview?.get("days")).map { entry ->
                val day = HealthJsonRead.string(entry["day"])
                val isToday = day == current
                val slips = int(entry["slips"])
                var skips = int(entry["skips"])
                var status = SugarDayStatus.of(HealthJsonRead.string(entry["status"])) ?: SugarDayStatus.Open
                if (isToday && queued > 0) {
                    skips += queued
                    if (status == SugarDayStatus.Open && slips == 0) status = SugarDayStatus.SugarFree
                }
                SugarBreakDay(
                    day, SugarBreakText.dayOfMonth(day), status, skips, slips, isToday,
                    caption(if (isToday) "Today" else dayLabel(day), status, skips),
                )
            }
        }

    val events: List<SugarBreakEventRow>
        get() = HealthJsonRead.objects(overview?.get("events")).reversed().map { event ->
            val slip = HealthJsonRead.string(event["kind"]) == "slip"
            var at = HealthJsonRead.int(event["at"])
            if (at <= 0) at = HealthJsonRead.int(event["loggedAt"])
            val time = if (at > 0) SugarBreakText.clock(Instant.ofEpochMilli(at), zone) else ""
            SugarBreakEventRow(HealthJsonRead.string(event["id"]), slip, if (slip) "Had sugar" else "+1 Skipped sugar", time)
        }

    val history: List<SugarBreakHistoryRow>
        get() = HealthJsonRead.objects(overview?.get("history")).map { past ->
            val totals = HealthJsonRead.obj(past["summary"])
            val length = int(past["lengthDays"])
            val start = HealthJsonRead.string(past["startDay"])
            val end = HealthJsonRead.string(past["endDay"])
            val label = when (HealthJsonRead.string(past["status"])) {
                "completed" -> "Completed"
                "ended" -> "Ended early"
                else -> "Active"
            }
            SugarBreakHistoryRow(
                HealthJsonRead.string(past["id"]),
                "${SugarBreakText.monthDay(start)} – ${SugarBreakText.monthDay(end)}",
                "${int(totals["sugarFreeDays"])} of ${SugarBreakText.days(length)} sugar-free · +${int(totals["skips"])} · best streak ${SugarBreakText.days(int(totals["bestStreak"]))}",
                label,
            )
        }

    val lengths: List<SugarBreakLength>
        get() {
            var values = ((overview?.get("lengths") as? List<*>) ?: emptyList<Any?>())
                .filterIsInstance<Number>().map { it.toInt() }.filter { it in 1..365 }
            if (values.isEmpty()) values = DEFAULT_LENGTHS
            if (selectedLength !in values) values = (values + selectedLength).sorted()
            return values.map { SugarBreakLength(it, SugarBreakText.length(it)) }
        }

    fun dayLabel(day: String): String = Calories.dayLabel(day)

    private fun caption(label: String, status: SugarDayStatus, skips: Int): String {
        val parts = arrayListOf(label)
        when (status) {
            SugarDayStatus.SugarFree -> parts.add("sugar-free, +$skips")
            SugarDayStatus.Slip -> parts.add("had sugar")
            SugarDayStatus.Open -> parts.add("nothing logged")
            SugarDayStatus.Upcoming -> parts.add("upcoming")
        }
        return parts.joinToString(", ")
    }

    companion object {
        const val TITLE = "Sugar Break"
        const val INSTALL_TITLE = "Take a break from sugar"
        const val INSTALL_MESSAGE = "Sugar Break is private to your ForkMesh account. Pick how long, one month by default, press Start, and tap +1 every time you skip sugar. Your progress shows here, on the web, and on your desktop."
        const val INSTALL_BUTTON = "Install Sugar Break"
        const val UNAVAILABLE_TITLE = "Sugar Break is unavailable"
        const val START_TITLE = "Start a sugar break"
        const val START_MESSAGE = "Choose how long. A day counts as sugar-free when you log at least one +1 and no sugar."
        const val EMPTY_TODAY = "Nothing logged today. Tap +1 each time you skip sugar."
        val DEFAULT_LENGTHS = listOf(7, 14, 21, 30, 60, 90)
        const val DEFAULT_LENGTH = 30
    }
}

object SugarBreakText {
    private val months = listOf("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
    private val clock = DateTimeFormatter.ofPattern("h:mm a", Locale.US)

    fun length(days: Int): String = when (days) {
        7 -> "1 week (7 days)"
        14, 21 -> "${days / 7} weeks ($days days)"
        30 -> "1 month (30 days)"
        60, 90 -> "${days / 30} months ($days days)"
        else -> days(days)
    }

    fun days(count: Int): String = if (count == 1) "1 day" else "$count days"

    fun dayOfMonth(day: String): Int {
        val parts = day.split("-")
        if (parts.size != 3) return 0
        return parts[2].toIntOrNull() ?: 0
    }

    fun monthDay(day: String): String {
        val parts = day.split("-")
        if (parts.size != 3) return day
        val month = parts[1].toIntOrNull()
        val date = parts[2].toIntOrNull()
        if (month == null || date == null || month !in 1..12) return day
        return "${months[month - 1]} $date"
    }

    fun clock(instant: Instant, zone: ZoneId): String = clock.format(instant.atZone(zone))
}
