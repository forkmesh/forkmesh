package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.calories.Calories
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.tools.ChangeNotifier
import java.math.BigDecimal
import java.math.RoundingMode
import java.time.Instant
import java.time.ZoneId
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.roundToLong


data class StepsBar(
    val id: String,
    val label: String,
    val steps: Int,
    val ratio: Double,
    val metGoal: Boolean,
    val caption: String,
)

data class StepsStat(val label: String, val value: String)

interface StepsBridge {
    val available: Boolean
    val motionAvailable: Boolean
    fun authorize(): Boolean
    fun stepDays(count: Int, now: Instant): List<HealthStepDay>
    fun startLiveSteps(update: (HealthStepDay) -> Unit): Boolean
    fun stopLiveSteps()
    fun startMotionSamples(update: (List<MotionSample>) -> Unit): Boolean
    fun stopMotionSamples()
}

object StepsText {
    const val METERS_PER_MILE = 1609.344

    fun count(value: Int): String {
        val digits = abs(value).toString()
        val grouped = StringBuilder()
        for ((index, character) in digits.withIndex()) {
            if (index > 0 && (digits.length - index) % 3 == 0) grouped.append(',')
            grouped.append(character)
        }
        return if (value < 0) "-$grouped" else grouped.toString()
    }

    fun fixed(value: Double, digits: Int): String = BigDecimal(value).setScale(digits, RoundingMode.HALF_UP).toPlainString()

    fun distance(meters: Double, imperial: Boolean): String {
        val value = if (imperial) meters / METERS_PER_MILE else meters / 1000
        return "${fixed(value, 2)} ${if (imperial) "mi" else "km"}"
    }

    fun gait(stepsPerMinute: Int): String = if (stepsPerMinute < 140) walkingGait(stepsPerMinute) else "Running"

    fun walkingGait(stepsPerMinute: Int): String = when {
        stepsPerMinute < 90 -> "Strolling"
        stepsPerMinute < 115 -> "Walking"
        else -> "Brisk walk"
    }

    fun speed(metersPerSecond: Double, imperial: Boolean): String {
        val perHour = max(0.0, metersPerSecond) * 3600
        val value = if (imperial) perHour / METERS_PER_MILE else perHour / 1000
        return "${fixed(value, 1)} ${if (imperial) "mph" else "km/h"}"
    }

    fun minutesToGoal(minutes: Int, goal: Int): String = "$minutes min to ${count(goal)}"

    fun shortDay(day: String): String {
        val parts = day.split("-")
        if (parts.size != 3) return day
        val month = parts[1].toIntOrNull() ?: return day
        val date = parts[2].toIntOrNull() ?: return day
        return "$month/$date"
    }

    fun clock(instant: Instant, zone: ZoneId): String {
        val time = instant.atZone(zone)
        val hour = time.hour
        val minute = time.minute
        val twelve = if (hour % 12 == 0) 12 else hour % 12
        return "$twelve:${if (minute < 10) "0" else ""}$minute ${if (hour < 12) "AM" else "PM"}"
    }

    fun tzOffsetMinutes(zone: ZoneId, now: Instant): Int = -(zone.rules.getOffset(now).totalSeconds / 60)

    fun message(error: Throwable, fallback: String): String = (error as? OrgApiException)?.message ?: fallback
}

class StepsModel(
    val api: HealthCall,
    val bridge: StepsBridge?,
    val prefs: PrefStore,
    val zone: ZoneId = ZoneId.systemDefault(),
    private val now: () -> Instant = { Instant.now() },
) : ChangeNotifier() {
    private var lastLive: Pair<Int, Instant>? = null
    private var cadenceExpiresAt: Instant? = null
    private val meter = StrideMeter()
    private val strides = HashMap<StrideGait, Double>()

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
    @Volatile var healthEnabled: Boolean = HealthPrefs.bool(prefs, HealthPrefs.STEPS)
        private set
    @Volatile var live: HealthStepDay? = null
        private set
    @Volatile var cadence = 0.0
        private set
    @Volatile var gait = StrideGait.Still
        private set
    @Volatile var sensing = false
        private set
    @Volatile var notice: HealthNotice? = null
        private set

    private fun changed() = notifyListeners()

    fun dismissNotice() {
        notice = null
        changed()
    }

    val today: String get() = HealthDates.isoDay(now(), zone)

    fun load(syncIfDue: Boolean = true) {
        loading = true
        error = ""
        changed()
        try {
            fetch()
            loading = false
            changed()
            val source = bridge
            if (syncIfDue && installed && healthEnabled && source != null) {
                val synced = sync(source, 7, false)
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

    private fun sync(source: StepsBridge, days: Int, force: Boolean): Int {
        val at = now()
        return StepsHealthSync.runIfDue(
            source.available, { source.stepDays(days, at) }, prefs, api, at.toEpochMilli() / 1000.0, force,
        )
    }

    private fun fetch() {
        val payload = api.call("GET", "?day=$today", null)
        installed = payload["installed"] == true
        overview = if (installed) payload else null
        if (installed) prefs.putBool(HealthPrefs.STEPS_IMPERIAL, imperial)
    }

    private fun message(failure: Throwable): String = StepsText.message(failure, "Pedometer is unavailable right now.")

    private fun show(text: String, error: Boolean = false) {
        notice = HealthNotice(text, error)
        changed()
    }

    val showsSpinner: Boolean get() = loading && overview == null && error.isEmpty()
    val showsError: Boolean get() = error.isNotEmpty() && overview == null
    val showsInstall: Boolean get() = !loading && error.isEmpty() && !installed
    val healthAvailable: Boolean get() = bridge?.available == true

    fun install() {
        try {
            api.call("POST", "/install", linkedMapOf("tzOffsetMinutes" to StepsText.tzOffsetMinutes(zone, now()).toLong()))
            show("Pedometer installed.")
            load(false)
            if (healthAvailable && !healthEnabled) enableHealth()
        } catch (cancelled: java.util.concurrent.CancellationException) {
            throw cancelled
        } catch (failure: Exception) {
            show(message(failure), true)
        }
    }

    fun enableHealth() {
        val source = bridge
        if (source == null || !source.available) {
            show(HEALTH_UNAVAILABLE_MESSAGE, true)
            return
        }
        if (!source.authorize()) {
            show("ForkMesh was not allowed to count your steps. Allow Physical activity for ForkMesh in your phone's app settings.", true)
            return
        }
        prefs.putBool(HealthPrefs.STEPS, true)
        healthEnabled = true
        changed()
        syncNow(30)
    }

    fun disableHealth() {
        prefs.putBool(HealthPrefs.STEPS, false)
        healthEnabled = false
        stopLive()
        show("Step syncing is off. The steps already synced stay on your account.")
    }

    fun syncNow(days: Int = 7) {
        val source = bridge
        if (source == null || !healthEnabled || !installed) return
        syncing = true
        changed()
        val synced = sync(source, days, true)
        syncing = false
        changed()
        if (synced > 0) {
            quietly { fetch() }
            changed()
        } else if ((live?.steps ?: 0) == 0 && todaySteps == 0) {
            show("No steps counted yet. Carry your phone and check that ForkMesh may use Physical activity.")
        }
    }

    fun startLive() {
        val source = bridge
        if (source == null || !healthEnabled) return
        source.startLiveSteps { day ->
            if (day.day == today) {
                track(day)
                live = day
                changed()
            }
        }
        sensing = source.startMotionSamples { samples -> feed(samples) }
        changed()
    }

    fun stopLive() {
        bridge?.stopLiveSteps()
        bridge?.stopMotionSamples()
        cadenceExpiresAt = null
        lastLive = null
        meter.reset()
        sensing = false
        cadence = 0.0
        gait = StrideGait.Still
        changed()
    }

    fun tick() {
        val expires = cadenceExpiresAt ?: return
        if (now().isBefore(expires)) return
        cadenceExpiresAt = null
        if (cadence != 0.0) {
            cadence = 0.0
            changed()
        }
    }

    fun feed(samples: List<MotionSample>) {
        if (!sensing || samples.isEmpty()) return
        var reading = meter.reading
        for (sample in samples) reading = meter.add(sample)
        if (reading.cadence != cadence || reading.gait != gait) {
            cadence = reading.cadence
            gait = reading.gait
            changed()
        }
    }

    fun track(day: HealthStepDay) {
        if (day.cadence > 0 && day.pace > 0) {
            val stride = 1 / (day.pace * day.cadence)
            val runningNow = if (sensing) running else StepsText.gait((day.cadence * 60).roundToInt()) == "Running"
            if (stride >= 0.3 && stride <= 2.5) strides[if (runningNow) StrideGait.Running else StrideGait.Walking] = stride
        }
        if (sensing) return
        val at = now()
        var rate = day.cadence
        val last = lastLive
        if (rate <= 0 && last != null && day.steps > last.first) {
            val elapsed = secondsBetween(last.second, at)
            if (elapsed >= 1 && elapsed <= CADENCE_STALE_AFTER * 2) rate = (day.steps - last.first) / elapsed
        }
        lastLive = day.steps to at
        cadence = min(max(0.0, rate), 4.0)
        cadenceExpiresAt = if (cadence > 0) at.plusMillis((CADENCE_STALE_AFTER * 1000).toLong()) else null
    }

    val stepsPerMinute: Int get() = (cadence * 60).roundToInt()

    val running: Boolean
        get() = if (sensing) gait == StrideGait.Running && cadence > 0 else StepsText.gait(stepsPerMinute) == "Running"

    val strideM: Double
        get() {
            strides[if (running) StrideGait.Running else StrideGait.Walking]?.let { return it }
            val reading = todayReading
            val average = if (reading.steps >= 100 && reading.distanceM > 0) reading.distanceM / reading.steps else 0.0
            val base = if (average >= 0.3 && average <= 1.5) average else DEFAULT_STRIDE_M
            return if (running) base * RUN_STRIDE_SCALE else base
        }

    val speedMetersPerSecond: Double get() = cadence * strideM

    val speedText: String get() = StepsText.speed(speedMetersPerSecond, true)

    val minutesToGoal: Int?
        get() {
            val perMinute = stepsPerMinute
            if (goal <= 0 || perMinute <= 0 || goalMet) return null
            return ceil((goal - todaySteps).toDouble() / perMinute).toInt()
        }

    val gaitText: String
        get() {
            val perMinute = stepsPerMinute
            if (running) return "Running"
            return if (sensing) StepsText.walkingGait(perMinute) else StepsText.gait(perMinute)
        }

    val paceLine: String
        get() {
            if (!healthAvailable || !healthEnabled) return PACE_OFF_MESSAGE
            val perMinute = stepsPerMinute
            if (perMinute <= 0) return "Standing still"
            val line = "$gaitText · $perMinute steps/min · $speedText"
            val minutes = minutesToGoal ?: return line
            return "$line · ${StepsText.minutesToGoal(minutes, goal)}"
        }

    fun leave() {
        stopLive()
        val source = bridge
        val reading = live
        if (source == null || !healthEnabled || !installed || reading == null || reading.steps <= serverToday.steps) return
        sync(source, 2, true)
    }

    fun saveGoal(goal: Int, units: String) {
        try {
            api.call("PUT", "/profile", linkedMapOf("goal" to max(0, goal).toLong(), "unitsPreference" to units))
            show("Goal saved.")
            fetch()
            changed()
        } catch (cancelled: java.util.concurrent.CancellationException) {
            throw cancelled
        } catch (failure: Exception) {
            show(message(failure), true)
        }
    }

    fun addSteps(day: String, steps: Int, distance: Double) {
        val meters = if (imperial) distance * StepsText.METERS_PER_MILE else distance * 1000
        try {
            api.call(
                "PUT", "/days/$day",
                linkedMapOf("steps" to max(0, steps).toLong(), "distanceM" to (meters * 10).roundToLong() / 10.0),
            )
            show("Saved ${StepsText.count(steps)} steps for ${dayLabel(day)}.")
            fetch()
            changed()
        } catch (cancelled: java.util.concurrent.CancellationException) {
            throw cancelled
        } catch (failure: Exception) {
            show(message(failure), true)
        }
    }

    fun removeAddedSteps(day: String) {
        try {
            api.call("DELETE", "/days/$day?source=manual", null)
            show("Removed the steps you added for ${dayLabel(day)}.")
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
            prefs.putBool(HealthPrefs.STEPS, false)
            healthEnabled = false
            stopLive()
            show("Pedometer uninstalled from your account.")
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
    val imperial: Boolean get() = profile["units"] == "imperial"
    val goal: Int get() = HealthJsonRead.int(profile["goal"]).toInt()
    val unitsPreference: String get() = (profile["unitsPreference"] as? String) ?: "auto"

    val serverToday: HealthStepDay
        get() {
            val day = HealthJsonRead.obj(overview?.get("today"))
            return HealthStepDay(
                day = (day["day"] as? String) ?: today,
                steps = HealthJsonRead.int(day["steps"]).toInt(),
                distanceM = HealthJsonRead.double(day["distanceM"]),
                floors = HealthJsonRead.int(day["floors"]).toInt(),
                activeKcal = HealthJsonRead.double(day["activeKcal"]),
            )
        }

    val todayReading: HealthStepDay
        get() {
            val server = serverToday
            val current = live
            if (current == null || current.day != server.day) return server
            return server.merged(current)
        }

    val todaySteps: Int get() = todayReading.steps
    val stepsText: String get() = StepsText.count(todaySteps)

    val ringRatio: Double get() = if (goal > 0) min(1.0, todaySteps.toDouble() / goal) else 0.0
    val goalMet: Boolean get() = goal > 0 && todaySteps >= goal

    val goalLine: String get() = if (goal > 0) "of ${StepsText.count(goal)} steps" else "No daily goal"

    val detailLine: String
        get() {
            val reading = todayReading
            val parts = ArrayList<String>()
            if (goal > 0) parts.add(if (goalMet) "Goal reached" else "${StepsText.count(goal - reading.steps)} to go")
            parts.add(StepsText.distance(reading.distanceM, imperial))
            if (reading.floors > 0) parts.add("${reading.floors} floors")
            if (reading.activeKcal > 0) parts.add("${reading.activeKcal.roundToInt()} active kcal")
            return parts.joinToString(" · ")
        }

    val sourceLine: String
        get() {
            if (!healthAvailable) return HEALTH_UNAVAILABLE_MESSAGE
            if (!healthEnabled) return HEALTH_OFF_MESSAGE
            val health = HealthJsonRead.obj(HealthJsonRead.obj(HealthJsonRead.obj(overview?.get("today"))["sources"])["health"])
            val synced = HealthJsonRead.int(health["updatedAt"])
            if (synced <= 0) return "Counting with the step sensor. Today's steps sync when you open this screen."
            return "Counting with the step sensor · synced ${StepsText.clock(Instant.ofEpochMilli(synced), zone)}"
        }

    val stats: List<StepsStat>
        get() {
            val streak = HealthJsonRead.int(summary["streak"]).toInt()
            val best = (summary["bestDay"] as? String) ?: ""
            return listOf(
                StepsStat("Last 7 days", "${StepsText.count(HealthJsonRead.int(summary["weekSteps"]).toInt())} steps"),
                StepsStat("30-day average", "${StepsText.count(HealthJsonRead.int(summary["averageSteps"]).toInt())} steps"),
                StepsStat("Goal streak", if (streak == 1) "1 day" else "$streak days"),
                StepsStat(
                    "Best day",
                    if (best.isEmpty()) "—" else "${StepsText.count(HealthJsonRead.int(summary["bestSteps"]).toInt())} · ${dayLabel(best)}",
                ),
            )
        }

    private fun peak(days: List<HealthJson>): Double {
        var peak = max(2000, (goal * 1.15).toInt()).toDouble()
        for (day in days) peak = max(peak, HealthJsonRead.double(day["steps"]) * 1.05)
        return peak
    }

    val bars: List<StepsBar>
        get() {
            val days = HealthJsonRead.objects(overview?.get("days"))
            val top = peak(days)
            val reading = todayReading
            return days.mapIndexed { index, day ->
                val id = (day["day"] as? String) ?: index.toString()
                var steps = HealthJsonRead.int(day["steps"]).toInt()
                if (id == reading.day) steps = max(steps, reading.steps)
                StepsBar(
                    id = id,
                    label = if (index % 5 == 0 || index == days.size - 1) StepsText.shortDay(id) else "",
                    steps = steps,
                    ratio = if (top > 0) steps / top else 0.0,
                    metGoal = goal > 0 && steps >= goal,
                    caption = "${dayLabel(id)}: ${StepsText.count(steps)} steps",
                )
            }
        }

    val goalRatio: Double
        get() {
            val top = peak(HealthJsonRead.objects(overview?.get("days")))
            return if (goal > 0 && top > 0) goal / top else 0.0
        }

    val distanceUnit: String get() = if (imperial) "mi" else "km"

    fun dayLabel(day: String): String = if (day == today) "Today" else Calories.dayLabel(day)

    companion object {
        const val TITLE = "Pedometer"
        const val INSTALL_TITLE = "Count your steps"
        const val INSTALL_MESSAGE =
            "Pedometer is private to your ForkMesh account. Install it, turn on step counting, and the steps your phone counts are synced to your account so you can see them on the web and on your desktop. Every day is encrypted at rest with a key minted for your account alone."
        const val INSTALL_BUTTON = "Install Pedometer"
        const val UNAVAILABLE_TITLE = "Pedometer is unavailable"
        const val HEALTH_OFF_MESSAGE =
            "Turn on step counting. ForkMesh reads your phone's step counter and sends one total per day to your account."
        const val HEALTH_UNAVAILABLE_MESSAGE = "This phone has no step counter."
        const val PACE_OFF_MESSAGE = "Turn on step counting to see your pace as you walk."
        const val CADENCE_STALE_AFTER = 6.0
        const val DEFAULT_STRIDE_M = 0.72
        const val RUN_STRIDE_SCALE = 1.35
    }
}
