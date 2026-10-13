package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.api.PrefStore
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.LocalTime
import java.util.UUID
import kotlin.math.ceil

object ActivityGrid {
    const val HOURS = 24
    const val ROWS = 12
    const val SLOT_MINUTES = 60 / ROWS
    const val SLOTS = HOURS * ROWS
    const val LEVELS = 5

    fun slotFor(time: LocalTime): Int = time.hour * ROWS + time.minute / SLOT_MINUTES

    fun level(value: Int, peak: Int): Int {
        if (value <= 0 || peak <= 0) return 0
        return ceil(4.0 * value / peak).toInt().coerceIn(1, LEVELS - 1)
    }

    fun slotLabel(slot: Int): String =
        "%02d:%02d".format(slot / ROWS, (slot % ROWS) * SLOT_MINUTES)

    fun sourcesText(selfKind: String, otherKinds: List<String>): String {
        fun label(kind: String) = when (kind) {
            "desktop" -> "desktop"
            "ios" -> "iOS device"
            "android" -> "Android device"
            "web" -> "browser"
            else -> "other device"
        }
        val parts = mutableListOf(
            when (selfKind) {
                "desktop" -> "this desktop"
                "web" -> "this browser"
                else -> "this device"
            },
        )
        otherKinds.distinct().forEach { kind -> parts += if (kind == selfKind) "another " + label(kind) else label(kind) }
        return if (parts.size == 1) parts[0] else parts.dropLast(1).joinToString(", ") + " and " + parts.last()
    }
}

data class ActivitySummary(
    val total: Int,
    val peak: Int,
    val peakSlot: Int,
    val peakHour: Int,
    val peakHourTotal: Int,
    val activeSlots: Int,
) {
    val activeMinutes: Int get() = activeSlots * ActivityGrid.SLOT_MINUTES

    val activeText: String
        get() = if (activeMinutes >= 60) "${activeMinutes / 60}h ${activeMinutes % 60}m" else "${activeMinutes}m"

    companion object {
        fun of(counts: IntArray): ActivitySummary {
            var total = 0
            var peak = 0
            var peakSlot = -1
            var active = 0
            val hours = IntArray(ActivityGrid.HOURS)
            for (slot in 0 until minOf(counts.size, ActivityGrid.SLOTS)) {
                val value = counts[slot]
                if (value <= 0) continue
                total += value
                active += 1
                hours[slot / ActivityGrid.ROWS] += value
                if (value > peak) {
                    peak = value
                    peakSlot = slot
                }
            }
            var peakHour = -1
            var peakHourTotal = 0
            hours.forEachIndexed { hour, value ->
                if (value > peakHourTotal) {
                    peakHourTotal = value
                    peakHour = hour
                }
            }
            return ActivitySummary(total, peak, peakSlot, peakHour, peakHourTotal, active)
        }
    }
}

class UserActivityLog(
    private val store: PrefStore?,
    private val clock: () -> LocalDateTime = LocalDateTime::now,
    private val wallMs: () -> Long = System::currentTimeMillis,
) {
    private var day: LocalDate? = null
    private val counts = IntArray(ActivityGrid.SLOTS)
    private var lastSaveMs = 0L
    private var dirty = false
    private var lastDownMs = Long.MIN_VALUE
    private var othersDay: LocalDate? = null
    private var others = IntArray(ActivityGrid.SLOTS)
    private var otherKinds: List<String> = emptyList()

    val deviceKey: String = loadDeviceKey()

    @get:Synchronized
    var revision = 0L
        private set

    init {
        load()
    }

    @Synchronized
    fun record(at: LocalDateTime = clock()) {
        val date = at.toLocalDate()
        if (date != day) {
            day = date
            counts.fill(0)
        }
        counts[ActivityGrid.slotFor(at.toLocalTime())] += 1
        revision += 1
        dirty = true
        if (wallMs() - lastSaveMs >= SAVE_INTERVAL_MS) flush()
    }

    @Synchronized
    fun touchDown(uptimeMs: Long) {
        if (uptimeMs == lastDownMs) return
        lastDownMs = uptimeMs
        record()
    }

    @Synchronized
    fun localCounts(date: LocalDate = clock().toLocalDate()): IntArray =
        if (date == day) counts.copyOf() else IntArray(ActivityGrid.SLOTS)

    @Synchronized
    fun counts(date: LocalDate = clock().toLocalDate()): IntArray {
        val merged = localCounts(date)
        if (date == othersDay) for (slot in merged.indices) merged[slot] += others[slot]
        return merged
    }

    @Synchronized
    fun setOthers(date: LocalDate, values: IntArray, kinds: List<String>) {
        if (values.size != ActivityGrid.SLOTS) return
        othersDay = date
        others = IntArray(ActivityGrid.SLOTS) { maxOf(0, values[it]) }
        otherKinds = kinds
    }

    @Synchronized
    fun otherKinds(date: LocalDate = clock().toLocalDate()): List<String> =
        if (date == othersDay) otherKinds else emptyList()

    @Synchronized
    fun clearOthers() {
        othersDay = null
        others = IntArray(ActivityGrid.SLOTS)
        otherKinds = emptyList()
    }

    fun today(): LocalDate = clock().toLocalDate()

    fun nowSlot(): Int = ActivityGrid.slotFor(clock().toLocalTime())

    @Synchronized
    fun flush() {
        lastSaveMs = wallMs()
        val date = day ?: return
        if (!dirty || store == null) return
        store.putString(KEY, date.toString() + "|" + counts.joinToString(","))
        dirty = false
    }

    private fun loadDeviceKey(): String {
        val stored = store?.getString(DEVICE_KEY)
        if (stored != null && stored.length in 8..64) return stored
        val fresh = UUID.randomUUID().toString()
        store?.putString(DEVICE_KEY, fresh)
        return fresh
    }

    private fun load() {
        val raw = store?.getString(KEY) ?: return
        val date = runCatching { LocalDate.parse(raw.substringBefore('|')) }.getOrNull() ?: return
        val values = raw.substringAfter('|', "").split(',').map { it.toIntOrNull() ?: return }
        if (values.size != ActivityGrid.SLOTS) return
        day = date
        values.forEachIndexed { slot, value -> counts[slot] = maxOf(0, value) }
    }

    companion object {
        const val KEY = "user_activity_today"
        const val DEVICE_KEY = "user_activity_device"
        const val SAVE_INTERVAL_MS = 60_000L
    }
}
