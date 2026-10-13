package com.forkmesh.nativeapp.planning

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.str
import java.util.UUID

enum class PlanningTimerKind(val wire: String, val label: String, val icon: String) {
    STOPWATCH("stopwatch", "Timer", "timer"),
    COUNTDOWN("countdown", "Countdown", "hourglass_bottom"),
    ALARM("alarm", "Alarm", "alarm");

    companion object {
        fun of(wire: String): PlanningTimerKind? = entries.firstOrNull { it.wire == wire }
    }
}

data class PlanningTimer(
    val id: String,
    val kind: PlanningTimerKind,
    val label: String,
    val createdAtMs: Long,
    val startedAtMs: Long = 0,
    val accumulatedMs: Long = 0,
    val durationMs: Long = 0,
    val alarmAtMs: Long = 0,
    val finishedAtMs: Long = 0,
) {
    val isRunning: Boolean get() = startedAtMs > 0 && finishedAtMs == 0L
    val isPaused: Boolean get() = startedAtMs == 0L && finishedAtMs == 0L && kind != PlanningTimerKind.ALARM
    val isFinished: Boolean get() = finishedAtMs > 0

    fun elapsedMs(now: Long): Long {
        val current = if (isRunning) maxOf(0, now - startedAtMs) else 0
        return maxOf(0, accumulatedMs + current)
    }

    fun remainingMs(now: Long): Long? = when (kind) {
        PlanningTimerKind.STOPWATCH -> null
        PlanningTimerKind.COUNTDOWN -> maxOf(0, durationMs - elapsedMs(now))
        PlanningTimerKind.ALARM -> maxOf(0, alarmAtMs - now)
    }

    val fireAtMs: Long?
        get() = if (isFinished) null else when (kind) {
            PlanningTimerKind.STOPWATCH -> null
            PlanningTimerKind.COUNTDOWN -> if (isRunning) startedAtMs + maxOf(0, durationMs - accumulatedMs) else null
            PlanningTimerKind.ALARM -> if (alarmAtMs > 0) alarmAtMs else null
        }

    fun isDue(now: Long): Boolean = fireAtMs?.let { now >= it } ?: false

    fun display(now: Long): String {
        if (isFinished && kind != PlanningTimerKind.STOPWATCH) return "Done"
        remainingMs(now)?.let { return clock(it, roundUp = true) }
        return clock(elapsedMs(now), roundUp = false)
    }

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "id" to id,
        "kind" to kind.wire,
        "label" to label,
        "createdAtMs" to createdAtMs,
        "startedAtMs" to startedAtMs,
        "accumulatedMs" to accumulatedMs,
        "durationMs" to durationMs,
        "alarmAtMs" to alarmAtMs,
        "finishedAtMs" to finishedAtMs,
    )

    companion object {
        fun fromJson(map: Map<String, Any?>): PlanningTimer? {
            val id = map.str("id")
            val kind = PlanningTimerKind.of(map.str("kind")) ?: return null
            if (id.isEmpty()) return null
            return PlanningTimer(
                id, kind, map.str("label"), map.long("createdAtMs"), map.long("startedAtMs"), map.long("accumulatedMs"),
                map.long("durationMs"), map.long("alarmAtMs"), map.long("finishedAtMs"),
            )
        }

        fun clock(ms: Long, roundUp: Boolean): String {
            val clamped = maxOf(0, ms)
            val seconds = if (roundUp) (clamped + 999) / 1_000 else clamped / 1_000
            val h = seconds / 3_600
            val m = (seconds % 3_600) / 60
            val s = seconds % 60
            fun two(n: Long) = if (n < 10) "0$n" else "$n"
            return if (h > 0) "$h:${two(m)}:${two(s)}" else "$m:${two(s)}"
        }
    }
}

data class PlanningTimerNotification(val id: String, val fireAtMs: Long, val title: String, val body: String)

data class TimerActivitySnapshot(
    val timerId: String,
    val kind: PlanningTimerKind,
    val label: String,
    val endsAtMs: Long,
    val countsFromMs: Long,
    val paused: Boolean,
    val pausedText: String,
)

class PlanningTimerException(val reason: Reason) : Exception(reason.text) {
    enum class Reason(val text: String) {
        FULL("Remove a timer first: at most ${PlanningTimerStore.MAX_TIMERS} are kept."),
        INVALID_DURATION("Choose a duration between one second and 24 hours."),
        ALARM_IN_PAST("Choose a time in the future."),
    }
}

class PlanningTimerStore(
    private val store: PrefStore,
    private val clock: () -> Long = System::currentTimeMillis,
    private val makeId: () -> String = { UUID.randomUUID().toString().uppercase() },
) {
    var timers: List<PlanningTimer> = decode(store.getString(STORAGE_KEY))
        private set
    var onChange: ((List<PlanningTimer>) -> Unit)? = null

    private val now: Long get() = clock()

    private fun save() {
        store.putString(STORAGE_KEY, Json.stringify(timers.map { it.toJson() }))
        onChange?.invoke(timers)
    }

    private fun cleanLabel(label: String, kind: PlanningTimerKind): String {
        val trimmed = label.trim()
        return if (trimmed.isEmpty()) kind.label else trimmed.take(MAX_LABEL_LENGTH)
    }

    private fun insert(timer: PlanningTimer): PlanningTimer {
        if (timers.size >= MAX_TIMERS) throw PlanningTimerException(PlanningTimerException.Reason.FULL)
        timers = listOf(timer) + timers
        save()
        return timer
    }

    fun startStopwatch(label: String): PlanningTimer =
        insert(PlanningTimer(makeId(), PlanningTimerKind.STOPWATCH, cleanLabel(label, PlanningTimerKind.STOPWATCH), now, startedAtMs = now))

    fun startCountdown(label: String, seconds: Int): PlanningTimer {
        if (seconds < 1 || seconds > 24 * 3_600) throw PlanningTimerException(PlanningTimerException.Reason.INVALID_DURATION)
        return insert(
            PlanningTimer(
                makeId(), PlanningTimerKind.COUNTDOWN, cleanLabel(label, PlanningTimerKind.COUNTDOWN), now,
                startedAtMs = now, durationMs = seconds * 1_000L,
            ),
        )
    }

    fun setAlarm(label: String, atMs: Long): PlanningTimer {
        if (atMs <= now) throw PlanningTimerException(PlanningTimerException.Reason.ALARM_IN_PAST)
        return insert(PlanningTimer(makeId(), PlanningTimerKind.ALARM, cleanLabel(label, PlanningTimerKind.ALARM), now, alarmAtMs = atMs))
    }

    private fun update(id: String, change: (PlanningTimer) -> PlanningTimer) {
        val index = timers.indexOfFirst { it.id == id }
        if (index < 0) return
        timers = timers.toMutableList().also { it[index] = change(it[index]) }
        save()
    }

    fun pause(id: String) {
        val now = now
        update(id) { if (!it.isRunning || it.kind == PlanningTimerKind.ALARM) it else it.copy(accumulatedMs = it.elapsedMs(now), startedAtMs = 0) }
    }

    fun resume(id: String) {
        val now = now
        update(id) { if (!it.isPaused) it else it.copy(startedAtMs = now) }
    }

    fun restart(id: String) {
        val now = now
        update(id) { if (it.kind == PlanningTimerKind.ALARM) it else it.copy(accumulatedMs = 0, finishedAtMs = 0, startedAtMs = now) }
    }

    fun stop(id: String) {
        val now = now
        update(id) { it.copy(accumulatedMs = it.elapsedMs(now), startedAtMs = 0, finishedAtMs = now) }
    }

    fun remove(id: String) {
        timers = timers.filter { it.id != id }
        save()
    }

    fun removeFinished() {
        timers = timers.filter { !it.isFinished }
        save()
    }

    fun settle(): List<PlanningTimer> {
        val now = now
        val rung = ArrayList<PlanningTimer>()
        timers = timers.map { timer ->
            if (!timer.isDue(now)) return@map timer
            val fire = timer.fireAtMs ?: now
            val settled = timer.copy(
                accumulatedMs = if (timer.kind == PlanningTimerKind.COUNTDOWN) timer.durationMs else timer.accumulatedMs,
                startedAtMs = 0,
                finishedAtMs = fire,
            )
            rung.add(settled)
            settled
        }
        if (rung.isNotEmpty()) save()
        return rung
    }

    fun notificationPlan(): List<PlanningTimerNotification> = timers.mapNotNull { timer ->
        val fire = timer.fireAtMs ?: return@mapNotNull null
        PlanningTimerNotification("timer-${timer.id}", fire, timer.label, if (timer.kind == PlanningTimerKind.ALARM) "Alarm" else "Countdown finished")
    }

    fun activityTimer(): PlanningTimer? {
        val live = timers.filter { !it.isFinished }
        live.filter { it.kind == PlanningTimerKind.COUNTDOWN && it.isRunning }.minByOrNull { it.fireAtMs ?: Long.MAX_VALUE }?.let { return it }
        live.filter { it.kind == PlanningTimerKind.ALARM }.minByOrNull { it.alarmAtMs }?.let { return it }
        return live.firstOrNull { it.kind == PlanningTimerKind.STOPWATCH && it.isRunning }
    }

    fun activitySnapshot(timer: PlanningTimer): TimerActivitySnapshot = TimerActivitySnapshot(
        timerId = timer.id,
        kind = timer.kind,
        label = timer.label,
        endsAtMs = timer.fireAtMs ?: 0,
        countsFromMs = if (timer.kind == PlanningTimerKind.STOPWATCH && timer.isRunning) timer.startedAtMs - timer.accumulatedMs else 0,
        paused = !timer.isRunning && timer.kind != PlanningTimerKind.ALARM,
        pausedText = timer.display(now),
    )

    companion object {
        const val STORAGE_KEY = "planning/timers.v1"
        const val MAX_TIMERS = 50
        const val MAX_LABEL_LENGTH = 80

        fun decode(text: String?): List<PlanningTimer> {
            if (text.isNullOrEmpty()) return emptyList()
            val list = runCatching { Json.parse(text) as? List<*> }.getOrNull() ?: return emptyList()
            return list.mapNotNull { PlanningTimer.fromJson(it.asJsonMap()) }.take(MAX_TIMERS)
        }
    }
}
