package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.calories.Calories
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.tools.ChangeNotifier
import java.time.Instant
import java.time.ZoneId
import kotlin.math.max
import kotlin.math.min

interface SleepBridge {
    val motionAvailable: Boolean
    fun authorizeSleep(): Boolean
    fun restHistory(from: Instant, to: Instant): List<SleepRestSample>?
}

object SleepText {
    fun duration(minutes: Int): String {
        val total = max(0, minutes)
        val hours = total / 60
        val rest = total % 60
        if (hours == 0) return "$rest min"
        return if (rest == 0) "$hours h" else "$hours h $rest min"
    }

    fun clock(minutes: Int): String {
        if (minutes < 0) return "—"
        val hour = minutes / 60 % 24
        val minute = minutes % 60
        val twelve = if (hour % 12 == 0) 12 else hour % 12
        return "$twelve:${if (minute < 10) "0" else ""}$minute ${if (hour < 12) "AM" else "PM"}"
    }

    fun clockText(hour: Int, minute: Int): String = "${hour.toString().padStart(2, '0')}:${minute.toString().padStart(2, '0')}"
}

object SleepSync {
    const val MAX_NIGHTS_PER_REQUEST = 62
    const val MOTION_HISTORY_SECONDS = 7L * 86400 - 600
    const val DEVICE = "android"

    fun schedule(prefs: PrefStore): SleepSchedule = SleepSchedule.parse(
        prefs.getString(HealthPrefs.SLEEP_BEDTIME) ?: "",
        prefs.getString(HealthPrefs.SLEEP_WAKE_TIME) ?: "",
    )

    fun nightKeys(ending: Instant, count: Int, schedule: SleepSchedule, zone: ZoneId): List<String> =
        HealthDates.days(schedule.nightKey(ending, zone), max(1, count))

    fun push(nights: List<HealthJson>, source: String, zone: ZoneId, now: Instant, call: HealthCall): Int {
        var synced = 0
        var index = 0
        while (index < nights.size) {
            val chunk = nights.subList(index, min(nights.size, index + MAX_NIGHTS_PER_REQUEST))
            val reply = call.call(
                "POST", "/sync",
                linkedMapOf(
                    "source" to source, "device" to DEVICE,
                    "tzOffsetMinutes" to StepsText.tzOffsetMinutes(zone, now).toLong(),
                    "nights" to chunk,
                ),
            )
            synced += HealthJsonRead.int(reply["synced"]).toInt()
            index += MAX_NIGHTS_PER_REQUEST
        }
        return synced
    }

    fun collect(bridge: SleepBridge, prefs: PrefStore, now: Instant, nights: Int, zone: ZoneId): List<HealthJson> {
        val plan = schedule(prefs)
        val keys = nightKeys(now, nights, plan, zone)
        val windows = keys.mapNotNull { key -> plan.window(key, zone)?.let { key to it } }
        val first = windows.firstOrNull()?.second?.start ?: return emptyList()
        val motionStart = if (first.isAfter(now.minusSeconds(MOTION_HISTORY_SECONDS))) first else now.minusSeconds(MOTION_HISTORY_SECONDS)
        if (!bridge.motionAvailable) return emptyList()
        val rest = bridge.restHistory(motionStart, now)
        if (rest.isNullOrEmpty()) return emptyList()
        val usage = SleepUsageLog.intervals(prefs, now)
        val keyed = windows.filter { !it.second.start.isBefore(motionStart) && it.second.start.isBefore(now) }.map { it.first }
        return SleepDetector.detect(keyed, rest, usage, plan, zone, now).first
    }

    fun runIfDue(
        bridge: SleepBridge,
        prefs: PrefStore,
        call: HealthCall,
        now: Instant = Instant.now(),
        nights: Int = 7,
        zone: ZoneId = ZoneId.systemDefault(),
        force: Boolean = false,
    ): Int {
        if (!HealthPrefs.bool(prefs, HealthPrefs.SLEEP)) return 0
        val last = HealthPrefs.double(prefs, HealthPrefs.SLEEP_SYNCED_AT)
        val nowSeconds = now.toEpochMilli() / 1000.0
        if (!force && nowSeconds - last < HealthPrefs.SYNC_INTERVAL_SECONDS) return 0
        val phone = collect(bridge, prefs, now, nights, zone)
        if (phone.isEmpty()) return 0
        val synced = try {
            push(phone, "phone", zone, now, call)
        } catch (cancelled: java.util.concurrent.CancellationException) {
            throw cancelled
        } catch (_: Exception) {
            return 0
        }
        prefs.putDouble(HealthPrefs.SLEEP_SYNCED_AT, nowSeconds)
        return synced
    }
}

data class SleepBar(
    val id: String,
    val label: String,
    val minutes: Int,
    val ratio: Double,
    val metGoal: Boolean,
    val caption: String,
)

data class SleepStat(val label: String, val value: String)

class SleepModel(
    val api: HealthCall,
    val bridge: SleepBridge?,
    val prefs: PrefStore,
    val zone: ZoneId = ZoneId.systemDefault(),
    private val now: () -> Instant = { Instant.now() },
) : ChangeNotifier() {
    @Volatile var overview: HealthJson? = null
        private set
    @Volatile var installed = false
        private set
    @Volatile var loading = true
        private set
    @Volatile var syncing = false
        private set
    @Volatile var error = ""
        private set
    @Volatile var trackingEnabled: Boolean = HealthPrefs.bool(prefs, HealthPrefs.SLEEP)
        private set
    @Volatile var notice: HealthNotice? = null
        private set

    private fun changed() = notifyListeners()

    fun dismissNotice() {
        notice = null
        changed()
    }

    val currentNight: String get() = SleepSync.schedule(prefs).nightKey(now(), zone)

    fun load(syncIfDue: Boolean = true) {
        loading = true
        error = ""
        changed()
        try {
            fetch()
            loading = false
            changed()
            val source = bridge
            if (syncIfDue && installed && trackingEnabled && source != null) {
                val synced = SleepSync.runIfDue(source, prefs, api, now(), 7, zone)
                if (synced > 0) quietly { fetch() }
                changed()
            }
        } catch (cancelled: java.util.concurrent.CancellationException) {
            throw cancelled
        } catch (failure: OrgApiException) {
            if (failure.code == "plugin_not_installed") {
                installed = false
                overview = null
                loading = false
            } else {
                loading = false
                error = failure.message
            }
            changed()
        } catch (failure: Exception) {
            loading = false
            error = message(failure)
            changed()
        }
    }

    private fun quietly(block: () -> Unit) {
        try {
            block()
        } catch (cancelled: java.util.concurrent.CancellationException) {
            throw cancelled
        } catch (_: Exception) {
        }
    }

    private fun fetch() {
        val payload = api.call("GET", "?night=$currentNight", null)
        installed = payload["installed"] == true
        overview = if (installed) payload else null
        if (installed) {
            prefs.putString(HealthPrefs.SLEEP_BEDTIME, (profile["bedtime"] as? String) ?: "")
            prefs.putString(HealthPrefs.SLEEP_WAKE_TIME, (profile["wakeTime"] as? String) ?: "")
        }
    }

    private fun message(failure: Throwable): String = StepsText.message(failure, "Sleep is unavailable right now.")

    private fun show(text: String, error: Boolean = false) {
        notice = HealthNotice(text, error)
        changed()
    }

    val showsSpinner: Boolean get() = loading && overview == null && error.isEmpty()
    val showsError: Boolean get() = error.isNotEmpty() && overview == null
    val showsInstall: Boolean get() = !loading && error.isEmpty() && !installed
    val trackingAvailable: Boolean get() = bridge?.motionAvailable == true

    fun install() {
        try {
            api.call("POST", "/install", linkedMapOf("tzOffsetMinutes" to StepsText.tzOffsetMinutes(zone, now()).toLong()))
            show("Sleep installed.")
            load(false)
            if (trackingAvailable && !trackingEnabled) enableTracking()
        } catch (cancelled: java.util.concurrent.CancellationException) {
            throw cancelled
        } catch (failure: Exception) {
            show(message(failure), true)
        }
    }

    fun enableTracking() {
        val source = bridge
        if (source == null || !trackingAvailable) {
            show(UNAVAILABLE_MESSAGE, true)
            return
        }
        if (!source.authorizeSleep()) {
            show("ForkMesh was not allowed to count your steps. Allow Physical activity for ForkMesh in your phone's app settings.", true)
            return
        }
        prefs.putBool(HealthPrefs.SLEEP, true)
        trackingEnabled = true
        SleepUsageLog.begin(prefs, now())
        changed()
        syncNow()
    }

    fun disableTracking() {
        prefs.putBool(HealthPrefs.SLEEP, false)
        SleepUsageLog.end(prefs, now())
        trackingEnabled = false
        show("Sleep tracking is off. The nights already synced stay on your account.")
    }

    fun syncNow(nights: Int = 7) {
        val source = bridge
        if (source == null || !trackingEnabled || !installed) return
        syncing = true
        changed()
        val synced = SleepSync.runIfDue(source, prefs, api, now(), nights, zone, true)
        syncing = false
        changed()
        if (synced > 0) {
            quietly { fetch() }
            changed()
        } else if (lastMinutes == 0) {
            show("No sleep found yet. Leave your phone by your bed tonight, or check that ForkMesh may use Physical activity.")
        }
    }

    fun saveGoal(minutes: Int, bedtime: String, wakeTime: String) {
        try {
            api.call(
                "PUT", "/profile",
                linkedMapOf(
                    "goalMinutes" to max(0, minutes).toLong(), "bedtime" to bedtime, "wakeTime" to wakeTime,
                    "tzOffsetMinutes" to StepsText.tzOffsetMinutes(zone, now()).toLong(),
                ),
            )
            show("Goal and schedule saved.")
            fetch()
            changed()
        } catch (cancelled: java.util.concurrent.CancellationException) {
            throw cancelled
        } catch (failure: Exception) {
            show(message(failure), true)
        }
    }

    fun addNight(night: String, bedAt: Instant, wakeAt: Instant, awakeMinutes: Int) {
        try {
            api.call(
                "PUT", "/nights/$night",
                linkedMapOf("bedAt" to bedAt.toEpochMilli(), "wakeAt" to wakeAt.toEpochMilli(), "awakeMinutes" to max(0, awakeMinutes).toLong()),
            )
            show("Saved the night you added for ${nightLabel(night)}.")
            fetch()
            changed()
        } catch (cancelled: java.util.concurrent.CancellationException) {
            throw cancelled
        } catch (failure: Exception) {
            show(message(failure), true)
        }
    }

    fun removeAddedNight(night: String) {
        try {
            api.call("DELETE", "/nights/$night?source=manual", null)
            show("Removed the night you added for ${nightLabel(night)}.")
            fetch()
            changed()
        } catch (cancelled: java.util.concurrent.CancellationException) {
            throw cancelled
        } catch (failure: Exception) {
            show(message(failure), true)
        }
    }

    fun uninstall(): Boolean {
        return try {
            api.call("POST", "/uninstall", emptyMap())
            prefs.putBool(HealthPrefs.SLEEP, false)
            trackingEnabled = false
            show("Sleep uninstalled from your account.")
            load(false)
            true
        } catch (cancelled: java.util.concurrent.CancellationException) {
            throw cancelled
        } catch (failure: Exception) {
            show(message(failure), true)
            false
        }
    }

    val profile: HealthJson get() = HealthJsonRead.obj(overview?.get("profile"))
    val summary: HealthJson get() = HealthJsonRead.obj(overview?.get("summary"))
    val lastNight: HealthJson get() = HealthJsonRead.obj(overview?.get("lastNight"))
    val goal: Int get() = HealthJsonRead.int(profile["goalMinutes"]).toInt()
    val bedtime: String get() = (profile["bedtime"] as? String) ?: "23:00"
    val wakeTime: String get() = (profile["wakeTime"] as? String) ?: "07:00"
    val lastMinutes: Int get() = HealthJsonRead.int(lastNight["asleepMinutes"]).toInt()

    val durationText: String get() = if (lastMinutes > 0) SleepText.duration(lastMinutes) else "—"
    val ringRatio: Double get() = if (goal > 0) min(1.0, lastMinutes.toDouble() / goal) else 0.0
    val goalMet: Boolean get() = goal > 0 && lastMinutes >= goal

    val goalLine: String get() = if (goal > 0) "of ${SleepText.duration(goal)}" else "No sleep goal"

    val detailLine: String
        get() {
            val night = lastNight
            if (lastMinutes <= 0) return "Nothing for last night yet."
            val parts = arrayListOf("${clock(night["bedAt"])} – ${clock(night["wakeAt"])}")
            val awake = HealthJsonRead.int(night["awakeMinutes"]).toInt()
            if (awake > 0) parts.add("${SleepText.duration(awake)} awake")
            val interruptions = HealthJsonRead.int(night["interruptions"]).toInt()
            if (interruptions > 0) parts.add(if (interruptions == 1) "1 interruption" else "$interruptions interruptions")
            val deep = HealthJsonRead.int(night["deepMinutes"]).toInt()
            if (deep > 0) parts.add("${SleepText.duration(deep)} deep")
            val rem = HealthJsonRead.int(night["remMinutes"]).toInt()
            if (rem > 0) parts.add("${SleepText.duration(rem)} REM")
            if (goal > 0) parts.add(if (goalMet) "Goal reached" else "${SleepText.duration(goal - lastMinutes)} short")
            return parts.joinToString(" · ")
        }

    val sourceLine: String
        get() {
            if (!trackingAvailable) return UNAVAILABLE_MESSAGE
            if (!trackingEnabled) return TRACKING_OFF_MESSAGE
            val source = (lastNight["source"] as? String) ?: ""
            if (source.isEmpty()) return "Tracking. Leave your phone by your bed; last night fills in when you open ForkMesh in the morning."
            val labels = HealthJsonRead.obj(overview?.get("sourceLabels"))
            val parts = arrayListOf((labels[source] as? String) ?: source)
            val confidence = (lastNight["confidence"] as? String) ?: ""
            if (source == "phone" && confidence.isNotEmpty()) parts.add("$confidence confidence")
            val reading = HealthJsonRead.obj(HealthJsonRead.obj(lastNight["sources"])[source])
            val synced = HealthJsonRead.int(reading["updatedAt"])
            if (synced > 0) parts.add("synced ${StepsText.clock(Instant.ofEpochMilli(synced), zone)}")
            return parts.joinToString(" · ")
        }

    private fun averageMinutes(key: String): Int = if (summary[key] == null) -1 else HealthJsonRead.int(summary[key]).toInt()

    val stats: List<SleepStat>
        get() {
            val weekNights = HealthJsonRead.int(summary["weekNights"]).toInt()
            val logged = HealthJsonRead.int(summary["loggedNights"]).toInt()
            val streak = HealthJsonRead.int(summary["streak"]).toInt()
            val spread = HealthJsonRead.int(summary["bedtimeSpreadMinutes"]).toInt()
            var averageBed = SleepText.clock(averageMinutes("averageBedtimeMinutes"))
            if (spread > 0 && averageBed != "—") averageBed += " ± ${SleepText.duration(spread)}"
            return listOf(
                SleepStat("Last 7 nights", if (weekNights > 0) SleepText.duration(HealthJsonRead.int(summary["weekAverageMinutes"]).toInt()) else "—"),
                SleepStat("Sleep debt", if (goal > 0) SleepText.duration(HealthJsonRead.int(summary["debtMinutes"]).toInt()) else "—"),
                SleepStat("30-night average", if (logged > 0) SleepText.duration(HealthJsonRead.int(summary["averageMinutes"]).toInt()) else "—"),
                SleepStat("Goal streak", if (streak == 1) "1 night" else "$streak nights"),
                SleepStat("Average bedtime", averageBed),
                SleepStat("Average wake", SleepText.clock(averageMinutes("averageWakeMinutes"))),
            )
        }

    private val peak: Double
        get() {
            var top = max(600, (goal * 1.15).toInt()).toDouble()
            for (night in HealthJsonRead.objects(overview?.get("nights"))) top = max(top, HealthJsonRead.double(night["asleepMinutes"]) * 1.05)
            return top
        }

    val bars: List<SleepBar>
        get() {
            val nights = HealthJsonRead.objects(overview?.get("nights"))
            val top = peak
            return nights.mapIndexed { index, night ->
                val id = (night["night"] as? String) ?: index.toString()
                val minutes = HealthJsonRead.int(night["asleepMinutes"]).toInt()
                SleepBar(
                    id = id,
                    label = if (index % 5 == 0 || index == nights.size - 1) StepsText.shortDay(id) else "",
                    minutes = minutes,
                    ratio = if (top > 0) minutes / top else 0.0,
                    metGoal = goal > 0 && minutes >= goal,
                    caption = "${nightLabel(id)}: ${if (minutes > 0) SleepText.duration(minutes) else "no sleep recorded"}",
                )
            }
        }

    val goalRatio: Double
        get() {
            val top = peak
            return if (goal > 0 && top > 0) goal / top else 0.0
        }

    private fun clock(value: Any?): String = StepsText.clock(Instant.ofEpochMilli(HealthJsonRead.int(value)), zone)

    fun nightLabel(night: String): String = if (night == currentNight) "Last night" else Calories.dayLabel(night)

    fun span(night: String, bedMinutes: Int, wakeMinutes: Int): Pair<Instant, Instant>? {
        val day = HealthDates.parseDay(night) ?: return null
        val midnight = day.atStartOfDay(zone)
        val wakeAt = midnight.plusMinutes(wakeMinutes.toLong())
        val sameDay = midnight.plusMinutes(bedMinutes.toLong())
        val bedAt = if (bedMinutes >= wakeMinutes) sameDay.minusDays(1) else sameDay
        return bedAt.toInstant() to wakeAt.toInstant()
    }

    companion object {
        const val TITLE = "Sleep"
        const val INSTALL_TITLE = "Track your sleep"
        const val INSTALL_MESSAGE =
            "Sleep is private to your ForkMesh account. Install it, turn on sleep tracking, and leave your phone by your bed at night: the stretch it lies still and unused counts as sleep. Every night is encrypted at rest with a key minted for your account alone."
        const val INSTALL_BUTTON = "Install Sleep"
        const val UNAVAILABLE_TITLE = "Sleep is unavailable"
        const val TRACKING_OFF_MESSAGE =
            "Turn on sleep tracking. ForkMesh reads when your phone lay still from the step sensor and sends one entry per night to your account."
        const val UNAVAILABLE_MESSAGE = "This phone has no step sensor."
    }
}
