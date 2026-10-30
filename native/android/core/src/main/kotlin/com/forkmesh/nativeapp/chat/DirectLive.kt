package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.json.jsonMaps
import java.util.concurrent.CopyOnWriteArrayList
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.floor
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sqrt

data class DirectLivePoint private constructor(val x: Double, val y: Double) {
    fun distance(other: DirectLivePoint): Double = hypot(x - other.x, y - other.y)

    companion object {
        operator fun invoke(x: Double, y: Double): DirectLivePoint =
            DirectLivePoint(min(max(x, 0.0), 1.0), min(max(y, -DirectLiveFrames.MAX_Y), DirectLiveFrames.MAX_Y))
    }
}

data class DirectLiveTap(val id: Int, val point: DirectLivePoint, val at: Long, val isSelf: Boolean)

data class DirectLiveStroke(
    val id: String,
    val color: Int,
    val width: Double,
    val points: List<DirectLivePoint>,
    val isSelf: Boolean,
    val ended: Boolean,
    val touchedAt: Long,
) {
    fun opacity(now: Long): Double {
        val age = now - touchedAt
        val fadeStart = DirectLiveFrames.INK_LIFE_MS - DirectLiveFrames.INK_FADE_MS
        if (age <= fadeStart) return 1.0
        return max(0.0, 1.0 - (age - fadeStart).toDouble() / DirectLiveFrames.INK_FADE_MS.toDouble())
    }
}

data class DirectLiveMotionSample private constructor(
    val at: Long,
    val gx: Double,
    val gy: Double,
    val gz: Double,
    val ax: Double,
    val ay: Double,
    val az: Double,
) {
    val roll: Double get() = atan2(gx, -gy)

    val pitch: Double get() = atan2(-gz, hypot(gx, gy))

    val steadyRoll: Double get() = roll * min(1.0, hypot(gx, gy) / 0.35)

    val jolt: Double get() = sqrt(ax * ax + ay * ay + az * az)

    fun shifted(to: Long): DirectLiveMotionSample = DirectLiveMotionSample(to, gx, gy, gz, ax, ay, az)

    fun blended(other: DirectLiveMotionSample, t: Double, at: Long): DirectLiveMotionSample {
        fun mix(a: Double, b: Double) = a + (b - a) * t
        return invoke(at, mix(gx, other.gx), mix(gy, other.gy), mix(gz, other.gz), mix(ax, other.ax), mix(ay, other.ay), mix(az, other.az))
    }

    companion object {
        operator fun invoke(
            at: Long,
            gx: Double,
            gy: Double,
            gz: Double,
            ax: Double = 0.0,
            ay: Double = 0.0,
            az: Double = 0.0,
        ): DirectLiveMotionSample = DirectLiveMotionSample(at, bounded(gx), bounded(gy), bounded(gz), bounded(ax), bounded(ay), bounded(az))

        fun bounded(value: Double): Double =
            if (value.isFinite()) min(max(value, -DirectLiveFrames.MAX_MOTION), DirectLiveFrames.MAX_MOTION) else 0.0
    }
}

data class DirectLiveMotionTrack(
    val samples: List<DirectLiveMotionSample> = emptyList(),
    val lastHeard: Long = 0,
    val stopped: Boolean = false,
) {
    fun isSharing(now: Long): Boolean = !stopped && lastHeard > 0 && now - lastHeard < DirectLiveFrames.MOTION_QUIET_MS

    fun sample(now: Long): DirectLiveMotionSample? {
        val first = samples.firstOrNull() ?: return null
        if (now <= first.at) return first.shifted(now)
        for (index in 1 until samples.size) {
            val after = samples[index]
            if (after.at < now) continue
            val before = samples[index - 1]
            val span = (after.at - before.at).toDouble()
            val t = if (span > 0) (now - before.at).toDouble() / span else 1.0
            return before.blended(after, t, now)
        }
        return samples.last().shifted(now)
    }
}

data class DirectLiveBudget(val perWindow: Int, val windowMs: Int)

object DirectLiveFrames {
    const val TYPE = "live"
    const val BUDGET_KIND = "forkmesh-live"
    const val MAX_ENVELOPE_BYTES = 16 * 1024
    const val MIN_INTERVAL_MS = 80L
    const val FALLBACK_INTERVAL_MS = 120L
    const val MAX_Y = 2.5
    const val MAX_MOTION = 8.0
    const val MAX_TAPS_PER_FRAME = 12
    const val MAX_MOTION_PER_FRAME = 40
    const val MAX_INK_POINTS_PER_FRAME = 160
    const val MAX_STROKES_PER_FRAME = 8
    const val MAX_POINTS_PER_STROKE = 1_500
    const val MAX_STROKES = 80
    const val MIN_POINT_GAP = 0.004
    const val INK_LIFE_MS = 45_000L
    const val INK_FADE_MS = 5_000L
    const val TAP_LIFE_MS = 1_400L
    const val MOTION_QUIET_MS = 4_000L
    const val MOTION_PLAYBACK_MARGIN_MS = 100L
    const val MOTION_KEEP_MS = 1_500L
    const val MAX_STROKE_WIDTH = 24.0
    val COLORS: List<Int> = listOf(0xFFFFFF, 0xFF453A, 0xFF9F0A, 0xFFD60A, 0x32D74B, 0x64D2FF, 0xBF5AF2, 0x000000)

    fun isLiveFrame(plain: Map<String, Any?>): Boolean = plain["type"] == TYPE

    fun interval(perWindow: Int, windowMs: Int): Long {
        if (perWindow <= 0 || windowMs <= 0) return FALLBACK_INTERVAL_MS
        return max(MIN_INTERVAL_MS, windowMs.toLong() * 4 / (perWindow.toLong() * 3))
    }

    fun budget(envelope: Map<String, Any?>): DirectLiveBudget? {
        if (envelope["kind"] != BUDGET_KIND) return null
        val per = integer(envelope["perWindow"]) ?: return null
        val window = integer(envelope["windowMs"]) ?: return null
        if (per <= 0 || window <= 0 || per > Int.MAX_VALUE || window > Int.MAX_VALUE) return null
        return DirectLiveBudget(per.toInt(), window.toInt())
    }

    fun scaled(value: Double, factor: Double): Long {
        val raw = value * factor
        return if (raw >= 0) floor(raw + 0.5).toLong() else -floor(-raw + 0.5).toLong()
    }

    internal fun number(value: Any?, factor: Double): Double? {
        val raw = (value as? Number)?.toDouble() ?: return null
        if (!raw.isFinite()) return null
        return raw / factor
    }

    internal fun integer(value: Any?): Long? = when (value) {
        is Long -> value
        is Int -> value.toLong()
        is Double -> if (value.isFinite() && value == floor(value) && abs(value) < 9.0e18) value.toLong() else null
        is Float -> integer(value.toDouble())
        is Number -> value.toLong()
        else -> null
    }

    fun encodePoints(points: List<DirectLivePoint>): List<Long> {
        val out = ArrayList<Long>(points.size * 2)
        for (point in points) {
            out.add(scaled(point.x, 1000.0))
            out.add(scaled(point.y, 1000.0))
        }
        return out
    }

    fun decodePoints(value: Any?, limit: Int): List<DirectLivePoint> {
        val flat = value as? List<*> ?: return emptyList()
        val out = ArrayList<DirectLivePoint>()
        var index = 0
        while (index + 1 < flat.size && out.size < limit) {
            val x = number(flat[index], 1000.0)
            val y = number(flat[index + 1], 1000.0)
            if (x != null && y != null) out.add(DirectLivePoint(x, y))
            index += 2
        }
        return out
    }

    fun encodeMotion(sample: DirectLiveMotionSample, base: Long): List<Long> = listOf(
        sample.at - base,
        scaled(sample.gx, 100.0),
        scaled(sample.gy, 100.0),
        scaled(sample.gz, 100.0),
        scaled(sample.ax, 100.0),
        scaled(sample.ay, 100.0),
        scaled(sample.az, 100.0),
    )

    fun decodeMotion(value: Any?, base: Long): DirectLiveMotionSample? {
        val parts = value as? List<*> ?: return null
        if (parts.size < 4) return null
        val dt = integer(parts[0]) ?: return null
        if (abs(dt) > 60_000) return null
        val gx = number(parts[1], 100.0) ?: return null
        val gy = number(parts[2], 100.0) ?: return null
        val gz = number(parts[3], 100.0) ?: return null
        val rest = parts.drop(4).map { number(it, 100.0) ?: 0.0 } + listOf(0.0, 0.0, 0.0)
        return DirectLiveMotionSample(base + dt, gx, gy, gz, rest[0], rest[1], rest[2])
    }

    fun color(value: Any?): Int {
        val raw = integer(value) ?: return COLORS[0]
        if (raw < 0 || raw > 0xFFFFFF) return COLORS[0]
        return raw.toInt()
    }

    fun strokeId(value: Any?): String {
        val raw = value as? String ?: return ""
        if (raw.isEmpty() || raw.length > 24) return ""
        if (!raw.all { it in 'a'..'z' || it in 'A'..'Z' || it in '0'..'9' || it == '-' }) return ""
        return raw
    }
}

private class DirectLiveOutgoing {
    val taps = ArrayList<DirectLivePoint>()
    val motion = ArrayList<DirectLiveMotionSample>()
    var motionOff = false
    var clear = false

    val isEmpty: Boolean get() = taps.isEmpty() && motion.isEmpty() && !motionOff && !clear
}

class DirectLiveEngine(
    private val selfId: () -> String,
    private val send: (String, Map<String, Any?>) -> Boolean,
    private val interval: (String) -> Long,
    private val clock: () -> Long = System::currentTimeMillis,
    private val loop: EventLoop = sharedLoop,
) {
    private val lock = Any()
    private val pending = HashMap<String, DirectLiveOutgoing>()
    private val inkSent = HashMap<String, HashMap<String, Int>>()
    private val inkEndSent = HashMap<String, HashSet<String>>()
    private val flushing = HashMap<String, LoopTimer>()
    private val lastSent = HashMap<String, Long>()
    private var pruning: LoopTimer? = null
    private var tapSequence = 0
    private var strokeSequence = 0
    private val tapMap = HashMap<String, List<DirectLiveTap>>()
    private val strokeMap = HashMap<String, List<DirectLiveStroke>>()
    private val motionMap = HashMap<String, DirectLiveMotionTrack>()
    private val sharing = LinkedHashSet<String>()
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    @Volatile
    var onTap: ((String, DirectLiveTap) -> Unit)? = null

    @Volatile
    var onStroke: ((String, DirectLiveStroke) -> Unit)? = null

    val now: Long get() = clock()

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    private fun changed() {
        for (listener in listeners) runCatching { listener() }
    }

    fun strokes(conversation: String): List<DirectLiveStroke> = synchronized(lock) { strokeMap[conversation].orEmpty() }

    fun taps(conversation: String): List<DirectLiveTap> = synchronized(lock) { tapMap[conversation].orEmpty() }

    fun motionTrack(conversation: String): DirectLiveMotionTrack? = synchronized(lock) { motionMap[conversation] }

    fun partnerMotion(conversation: String): DirectLiveMotionTrack? = synchronized(lock) {
        motionMap[conversation]?.takeIf { it.isSharing(now) }
    }

    fun isSharingMotion(conversation: String): Boolean = synchronized(lock) { conversation in sharing }

    val sharingMotion: Set<String> get() = synchronized(lock) { sharing.toSet() }

    fun tap(conversation: String, point: DirectLivePoint) {
        if (conversation.isEmpty()) return
        synchronized(lock) {
            addTap(conversation, point, isSelf = true)
            val out = pending.getOrPut(conversation) { DirectLiveOutgoing() }
            if (out.taps.size < DirectLiveFrames.MAX_TAPS_PER_FRAME) out.taps.add(point)
            schedule(conversation)
        }
        changed()
    }

    fun beginStroke(conversation: String, point: DirectLivePoint, color: Int, width: Double): String {
        if (conversation.isEmpty()) return ""
        val id = synchronized(lock) {
            strokeSequence += 1
            val id = "${now % 1_000_000_000}-$strokeSequence"
            val stroke = DirectLiveStroke(
                id = id,
                color = color and 0xFFFFFF,
                width = min(max(width, 1.0), DirectLiveFrames.MAX_STROKE_WIDTH),
                points = listOf(point),
                isSelf = true,
                ended = false,
                touchedAt = now,
            )
            insert(stroke, conversation)
            schedule(conversation)
            id
        }
        changed()
        return id
    }

    fun extendStroke(conversation: String, id: String, point: DirectLivePoint) {
        val extended = synchronized(lock) {
            val list = strokeMap[conversation] ?: return
            val index = list.indexOfFirst { it.id == id && it.isSelf }
            if (index < 0) return
            val stroke = list[index]
            if (stroke.ended || stroke.points.size >= DirectLiveFrames.MAX_POINTS_PER_STROKE) return
            val last = stroke.points.lastOrNull()
            if (last != null && last.distance(point) < DirectLiveFrames.MIN_POINT_GAP) return
            strokeMap[conversation] = list.toMutableList().also { it[index] = stroke.copy(points = stroke.points + point, touchedAt = now) }
            schedule(conversation)
            true
        }
        if (extended) changed()
    }

    fun endStroke(conversation: String, id: String) {
        synchronized(lock) {
            val list = strokeMap[conversation] ?: return
            val index = list.indexOfFirst { it.id == id && it.isSelf }
            if (index < 0) return
            strokeMap[conversation] = list.toMutableList().also { it[index] = list[index].copy(ended = true, touchedAt = now) }
            schedule(conversation)
        }
        changed()
    }

    fun clearInk(conversation: String) {
        synchronized(lock) {
            strokeMap.remove(conversation)
            inkSent.remove(conversation)
            inkEndSent.remove(conversation)
            pending.getOrPut(conversation) { DirectLiveOutgoing() }.clear = true
            schedule(conversation)
        }
        changed()
    }

    fun setSharingMotion(conversation: String, on: Boolean) {
        synchronized(lock) {
            if (conversation.isEmpty() || on == (conversation in sharing)) return
            val out = pending.getOrPut(conversation) { DirectLiveOutgoing() }
            if (on) {
                sharing.add(conversation)
                out.motionOff = false
            } else {
                sharing.remove(conversation)
                out.motion.clear()
                out.motionOff = true
                schedule(conversation)
            }
        }
        changed()
    }

    fun stopSharingMotion() {
        for (conversation in sharingMotion) setSharingMotion(conversation, false)
    }

    fun addMotion(sample: DirectLiveMotionSample) {
        synchronized(lock) {
            for (conversation in sharing) {
                val out = pending.getOrPut(conversation) { DirectLiveOutgoing() }
                out.motion.add(sample)
                if (out.motion.size > DirectLiveFrames.MAX_MOTION_PER_FRAME) {
                    out.motion.subList(0, out.motion.size - DirectLiveFrames.MAX_MOTION_PER_FRAME).clear()
                }
                schedule(conversation)
            }
        }
    }

    fun receive(plain: Map<String, Any?>, conversation: String) {
        if (!DirectLiveFrames.isLiveFrame(plain) || plain["channel"] != conversation) return
        val sender = (plain["senderId"] as? String ?: "").take(64)
        val me = selfId()
        if (sender.isEmpty() || sender == me) return
        val heardTaps = ArrayList<DirectLiveTap>()
        val heardStrokes = ArrayList<DirectLiveStroke>()
        synchronized(lock) {
            val received = now
            if (plain["clear"] == true) {
                strokeMap.remove(conversation)
                inkSent.remove(conversation)
                inkEndSent.remove(conversation)
            }
            for (value in (plain["taps"] as? List<*>).orEmpty().take(DirectLiveFrames.MAX_TAPS_PER_FRAME)) {
                val point = DirectLiveFrames.decodePoints(value, 1).firstOrNull() ?: continue
                heardTaps.add(addTap(conversation, point, isSelf = false))
            }
            receiveInk(jsonMaps(plain["ink"]), sender, conversation, received, heardStrokes)
            receiveMotion(plain, conversation, received)
            schedulePrune()
        }
        for (tap in heardTaps) onTap?.invoke(conversation, tap)
        for (stroke in heardStrokes) onStroke?.invoke(conversation, stroke)
        changed()
    }

    private fun receiveInk(
        chunks: List<Map<String, Any?>>,
        sender: String,
        conversation: String,
        received: Long,
        heard: MutableList<DirectLiveStroke>,
    ) {
        var budget = DirectLiveFrames.MAX_INK_POINTS_PER_FRAME
        for (chunk in chunks.take(DirectLiveFrames.MAX_STROKES_PER_FRAME)) {
            val strokeId = DirectLiveFrames.strokeId(chunk["s"])
            if (strokeId.isEmpty()) continue
            val points = DirectLiveFrames.decodePoints(chunk["p"], max(0, budget))
            budget -= points.size
            val id = "$sender|$strokeId"
            val list = strokeMap[conversation].orEmpty()
            val index = list.indexOfFirst { it.id == id }
            if (index >= 0) {
                val stroke = list[index]
                val room = DirectLiveFrames.MAX_POINTS_PER_STROKE - stroke.points.size
                val next = stroke.copy(
                    points = stroke.points + points.take(max(0, room)),
                    touchedAt = received,
                    ended = stroke.ended || chunk["e"] == true,
                )
                strokeMap[conversation] = list.toMutableList().also { it[index] = next }
            } else if (points.isNotEmpty()) {
                val width = min(max(DirectLiveFrames.number(chunk["w"], 10.0) ?: 4.0, 1.0), DirectLiveFrames.MAX_STROKE_WIDTH)
                val stroke = DirectLiveStroke(
                    id = id,
                    color = DirectLiveFrames.color(chunk["c"]),
                    width = width,
                    points = points,
                    isSelf = false,
                    ended = chunk["e"] == true,
                    touchedAt = received,
                )
                insert(stroke, conversation)
                heard.add(stroke)
            }
        }
    }

    private fun receiveMotion(plain: Map<String, Any?>, conversation: String, received: Long) {
        val track = motionMap[conversation] ?: DirectLiveMotionTrack()
        if (plain["motionOff"] == true) {
            motionMap[conversation] = track.copy(stopped = true, samples = emptyList())
            return
        }
        val base = DirectLiveFrames.integer(plain["ts"]) ?: 0L
        val incoming = (plain["motion"] as? List<*>).orEmpty().take(DirectLiveFrames.MAX_MOTION_PER_FRAME)
            .mapNotNull { DirectLiveFrames.decodeMotion(it, base) }
            .sortedBy { it.at }
        val first = incoming.firstOrNull() ?: return
        val last = incoming.last()
        val span = max(0L, last.at - first.at)
        val shift = received + DirectLiveFrames.MOTION_PLAYBACK_MARGIN_MS - last.at
        val samples = track.samples.filter { it.at < first.at + shift }.toMutableList()
        samples.addAll(incoming.map { it.shifted(it.at + shift) })
        val keepFrom = received - DirectLiveFrames.MOTION_KEEP_MS
        val keep = samples.indexOfLast { it.at < keepFrom }
        if (keep > 0) samples.subList(0, keep).clear()
        motionMap[conversation] = DirectLiveMotionTrack(samples = samples, lastHeard = received + span, stopped = false)
    }

    private fun addTap(conversation: String, point: DirectLivePoint, isSelf: Boolean): DirectLiveTap {
        tapSequence += 1
        val at = now
        val tap = DirectLiveTap(tapSequence, point, at, isSelf)
        val list = tapMap[conversation].orEmpty().filter { at - it.at < DirectLiveFrames.TAP_LIFE_MS }.toMutableList()
        list.add(tap)
        val cap = DirectLiveFrames.MAX_TAPS_PER_FRAME * 2
        if (list.size > cap) list.subList(0, list.size - cap).clear()
        tapMap[conversation] = list
        schedulePrune()
        return tap
    }

    private fun insert(stroke: DirectLiveStroke, conversation: String) {
        val list = strokeMap[conversation].orEmpty().toMutableList()
        list.add(stroke)
        if (list.size > DirectLiveFrames.MAX_STROKES) list.subList(0, list.size - DirectLiveFrames.MAX_STROKES).clear()
        strokeMap[conversation] = list
        schedulePrune()
    }

    internal fun body(conversation: String): Map<String, Any?>? = synchronized(lock) {
        val out = pending.remove(conversation) ?: DirectLiveOutgoing()
        val fields = LinkedHashMap<String, Any?>()
        val stamp = now
        if (out.clear) fields["clear"] = true
        if (out.taps.isNotEmpty()) fields["taps"] = out.taps.map { DirectLiveFrames.encodePoints(listOf(it)) }
        if (out.motionOff) {
            fields["motionOff"] = true
        } else if (out.motion.isNotEmpty()) {
            fields["motion"] = out.motion.map { DirectLiveFrames.encodeMotion(it, stamp) }
        }
        val ink = inkChunks(conversation)
        if (ink.isNotEmpty()) fields["ink"] = ink
        if (fields.isEmpty()) return@synchronized null
        fields["ts"] = stamp
        fields
    }

    private fun inkChunks(conversation: String): List<Map<String, Any?>> {
        val sent = inkSent.getOrPut(conversation) { HashMap() }
        val ended = inkEndSent.getOrPut(conversation) { HashSet() }
        var budget = DirectLiveFrames.MAX_INK_POINTS_PER_FRAME
        val chunks = ArrayList<Map<String, Any?>>()
        for (stroke in strokeMap[conversation].orEmpty()) {
            if (!stroke.isSelf) continue
            if (chunks.size >= DirectLiveFrames.MAX_STROKES_PER_FRAME || budget <= 0) break
            val from = sent[stroke.id] ?: 0
            val unsent = stroke.points.size - from
            val finishing = stroke.ended && stroke.id !in ended
            if (unsent <= 0 && !finishing) continue
            val take = min(max(0, unsent), budget)
            val chunk = linkedMapOf<String, Any?>(
                "s" to stroke.id,
                "p" to DirectLiveFrames.encodePoints(stroke.points.subList(from, from + take)),
            )
            if (from == 0) {
                chunk["c"] = stroke.color.toLong()
                chunk["w"] = DirectLiveFrames.scaled(stroke.width, 10.0)
            }
            if (stroke.ended && take == unsent) {
                chunk["e"] = true
                ended.add(stroke.id)
            }
            sent[stroke.id] = from + take
            budget -= take
            chunks.add(chunk)
        }
        return chunks
    }

    private fun hasWork(conversation: String): Boolean {
        val out = pending[conversation]
        if (out != null && !out.isEmpty) return true
        val sent = inkSent[conversation].orEmpty()
        val ended = inkEndSent[conversation].orEmpty()
        return strokeMap[conversation].orEmpty().any {
            it.isSelf && (it.points.size > (sent[it.id] ?: 0) || (it.ended && it.id !in ended))
        }
    }

    private fun schedule(conversation: String) {
        if (flushing[conversation] != null) return
        val wait = max(0L, (lastSent[conversation] ?: 0L) + interval(conversation) - now)
        flushing[conversation] = loop.timer(wait) { flush(conversation) }
    }

    private fun flush(conversation: String) {
        val fields = body(conversation)
        if (fields != null) {
            synchronized(lock) { lastSent[conversation] = now }
            runCatching { send(conversation, fields) }
        }
        synchronized(lock) {
            flushing.remove(conversation)
            if (hasWork(conversation)) schedule(conversation)
        }
    }

    private fun schedulePrune() {
        if (pruning != null) return
        pruning = loop.periodic(1_000) {
            synchronized(lock) {
                if (!prune()) {
                    pruning?.cancel()
                    pruning = null
                }
            }
            changed()
        }
    }

    fun prune(): Boolean = synchronized(lock) {
        val at = now
        var left = false
        for ((conversation, list) in tapMap.toList()) {
            val kept = list.filter { at - it.at < DirectLiveFrames.TAP_LIFE_MS }
            if (kept.size != list.size) {
                if (kept.isEmpty()) tapMap.remove(conversation) else tapMap[conversation] = kept
            }
            left = left || kept.isNotEmpty()
        }
        for ((conversation, list) in strokeMap.toList()) {
            val kept = list.filter { at - it.touchedAt < DirectLiveFrames.INK_LIFE_MS || (it.isSelf && !it.ended) }
            if (kept.size != list.size) {
                if (kept.isEmpty()) strokeMap.remove(conversation) else strokeMap[conversation] = kept
                val ids = kept.map { it.id }.toSet()
                inkSent[conversation]?.keys?.retainAll(ids)
                inkEndSent[conversation]?.retainAll(ids)
            }
            left = left || kept.isNotEmpty()
        }
        for ((conversation, track) in motionMap.toList()) {
            if (!track.isSharing(at)) motionMap.remove(conversation)
        }
        left || motionMap.isNotEmpty()
    }

    fun clear() {
        synchronized(lock) {
            for (timer in flushing.values) timer.cancel()
            flushing.clear()
            pruning?.cancel()
            pruning = null
            pending.clear()
            inkSent.clear()
            inkEndSent.clear()
            lastSent.clear()
            tapMap.clear()
            strokeMap.clear()
            motionMap.clear()
            sharing.clear()
        }
        changed()
    }

    companion object {
        private val sharedLoop: EventLoop by lazy { EventLoop("direct-live") }
    }
}
