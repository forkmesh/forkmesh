package com.forkmesh.nativeapp.djstudio

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.exp
import kotlin.math.ln
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.roundToInt
import kotlin.math.roundToLong
import kotlin.math.sqrt

object DjBpm {
    const val SLOWEST = 70.0
    const val FASTEST = 180.0
    const val FAVOURITE = 120.0
    const val SPREAD = 0.8
    const val HOP = 256
    const val LONGEST = 120.0
    const val CONFIDENCE = 0.04
    const val OCTAVE = 0.8
    private val multiples = doubleArrayOf(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0)

    fun detect(audio: DjScratchAudio): Double? = detect(audio.samples, audio.rate)

    fun detect(samples: ShortArray, rate: Double): Double? {
        if (rate < 2_000 || !rate.isFinite()) return null
        val fps = rate / HOP
        val onsets = onsets(samples, rate)
        val lags = ceil(60 / SLOWEST * fps * multiples.last()).toInt() + 2
        if (onsets.size <= lags * 2) return null
        val acf = autocorrelation(onsets, lags)
        var bestBpm = 0.0
        var bestScore = Double.NEGATIVE_INFINITY
        var bestRaw = 0.0
        for (step in (SLOWEST * 10).roundToInt()..(FASTEST * 10).roundToInt()) {
            val bpm = step / 10.0
            val raw = score(acf, 60 / bpm * fps)
            val weighted = raw * prior(bpm)
            if (weighted > bestScore) {
                bestScore = weighted
                bestBpm = bpm
                bestRaw = raw
            }
        }
        if (bestScore == Double.NEGATIVE_INFINITY || bestRaw < CONFIDENCE) return null
        var chosen = bestBpm
        val doubled = listOf(-0.1, 0.0, 0.1).map { bestBpm * 2 + it }.filter { it <= FASTEST }
        val double = doubled.maxByOrNull { score(acf, 60 / it * fps) }
        if (double != null && score(acf, 60 / double * fps) >= bestRaw * OCTAVE) chosen = double
        val period = refine(acf, 60 / chosen * fps)
        return min(FASTEST, max(SLOWEST, (60 * fps / period * 10).roundToLong() / 10.0))
    }

    private fun refine(acf: DoubleArray, period: Double): Double {
        var multiple = 1.0
        while (period * multiple * 2 + 3 < acf.size) multiple *= 2
        val centre = (period * multiple).roundToInt()
        if (centre < 3 || centre + 3 >= acf.size) return period
        var peak = centre
        for (lag in centre - 2..centre + 2) if (acf[lag] > acf[peak]) peak = lag
        val left = acf[peak - 1]
        val middle = acf[peak]
        val right = acf[peak + 1]
        val curve = left - 2 * middle + right
        val shift = if (curve < 0) min(0.5, max(-0.5, 0.5 * (left - right) / curve)) else 0.0
        val refined = (peak + shift) / multiple
        return if (abs(refined - period) < period * 0.02) refined else period
    }

    private fun prior(bpm: Double): Double {
        val octaves = ln(bpm / FAVOURITE) / ln(2.0) / SPREAD
        return exp(-0.5 * octaves * octaves)
    }

    private fun score(acf: DoubleArray, period: Double): Double = multiples.sumOf { value(acf, period * it) } / multiples.size

    private fun value(acf: DoubleArray, lag: Double): Double {
        val base = lag.toInt()
        if (lag < 0 || base + 1 >= acf.size) return 0.0
        val fraction = lag - base
        return acf[base] * (1 - fraction) + acf[base + 1] * fraction
    }

    private fun onsets(samples: ShortArray, rate: Double): DoubleArray {
        val span = min(samples.size, (LONGEST * rate).toInt())
        val start = (samples.size - span) / 2
        val frames = span / HOP
        if (frames <= 2) return DoubleArray(0)
        val smoothing = 1 - exp(-2 * PI * 180 / rate)
        var low = 0.0
        val full = DoubleArray(frames)
        val bass = DoubleArray(frames)
        for (frame in 0 until frames) {
            var fullEnergy = 0.0
            var bassEnergy = 0.0
            val offset = start + frame * HOP
            for (index in offset until offset + HOP) {
                val sample = samples[index] / 32768.0
                low += smoothing * (sample - low)
                fullEnergy += sample * sample
                bassEnergy += low * low
            }
            full[frame] = ln(1 + 1_000 * fullEnergy / HOP)
            bass[frame] = ln(1 + 1_000 * bassEnergy / HOP)
        }
        val flux = DoubleArray(frames)
        for (frame in 1 until frames) flux[frame] = max(0.0, full[frame] - full[frame - 1]) + max(0.0, bass[frame] - bass[frame - 1])
        val reach = max(1, (0.2 * rate / HOP).toInt())
        val running = DoubleArray(frames + 1)
        for (frame in 0 until frames) running[frame + 1] = running[frame] + flux[frame]
        val result = DoubleArray(frames)
        for (frame in 0 until frames) {
            val from = max(0, frame - reach)
            val to = min(frames, frame + reach + 1)
            result[frame] = max(0.0, flux[frame] - (running[to] - running[from]) / (to - from))
        }
        val mean = result.sum() / frames
        for (frame in 0 until frames) result[frame] -= mean
        return result
    }

    private fun autocorrelation(values: DoubleArray, lags: Int): DoubleArray {
        val count = values.size
        val acf = DoubleArray(min(lags, count))
        for (lag in acf.indices) {
            var sum = 0.0
            for (index in 0 until count - lag) sum += values[index] * values[index + lag]
            acf[lag] = sum / (count - lag)
        }
        val zero = acf.firstOrNull() ?: return acf
        if (zero <= 0) return DoubleArray(acf.size)
        for (lag in acf.indices) acf[lag] /= zero
        return acf
    }
}

class DjWaveAccumulator(sampleRate: Double, totalSamples: Int, bars: Int = BAR_COUNT) {
    companion object {
        const val BAR_COUNT = 1200
        const val BARS_PER_SECOND = 20.0
        const val MOST_BARS = 12_000

        fun bars(seconds: Double): Int {
            if (!seconds.isFinite() || seconds <= 0) return BAR_COUNT
            return min(MOST_BARS, max(BAR_COUNT, (seconds * BARS_PER_SECOND).toInt()))
        }
    }

    private val lowAlpha: Float
    private val highAlpha: Float
    private val samplesPerBar: Int
    private var low = 0f
    private var lower = 0f
    private var smooth = 0f
    private var smoother = 0f
    private var filled = 0
    private var lowEnergy = 0.0
    private var midEnergy = 0.0
    private var highEnergy = 0.0
    private val rows = ArrayList<DoubleArray>(bars + 1)

    init {
        val rate = max(1000.0, if (sampleRate.isFinite()) sampleRate else 22_050.0)
        lowAlpha = (1 - exp(-2 * PI * 250 / rate)).toFloat()
        highAlpha = (1 - exp(-2 * PI * 2500 / rate)).toFloat()
        samplesPerBar = max(1, ceil(max(1, totalSamples).toDouble() / max(1, bars)).toInt())
    }

    fun add(samples: FloatArray, count: Int = samples.size) {
        for (index in 0 until count) {
            val sample = samples[index]
            val value = if (sample.isFinite()) sample else 0f
            low += lowAlpha * (value - low)
            lower += lowAlpha * (low - lower)
            smooth += highAlpha * (value - smooth)
            smoother += highAlpha * (smooth - smoother)
            val high = value - smoother
            val mid = smoother - lower
            lowEnergy += (lower * lower).toDouble()
            midEnergy += (mid * mid).toDouble()
            highEnergy += (high * high).toDouble()
            filled += 1
            if (filled >= samplesPerBar) close()
        }
    }

    fun finish(count: Int = BAR_COUNT): List<DjWaveBar> {
        if (filled > 0) close()
        if (rows.isEmpty() || count <= 0) return emptyList()
        val peak = rows.maxOf { it[0] }
        if (peak <= 1e-5) return emptyList()
        return (0 until count).map { index ->
            val row = rows[min(rows.size - 1, (index.toLong() * rows.size / count).toInt())]
            val level = min(1.0, max(0.04, (row[0] / peak).pow(0.8)))
            val weight = row[1] + row[2] + row[3]
            val hue = if (weight > 0) (row[2] * 0.33 + row[3] * 0.6) / weight else 0.0
            DjWaveBar(level, min(0.999, max(0.0, hue)))
        }
    }

    private fun close() {
        val count = max(1, filled).toDouble()
        val total = (lowEnergy + midEnergy + highEnergy) / count
        rows.add(doubleArrayOf(sqrt(total), sqrt(lowEnergy / count), sqrt(midEnergy / count), sqrt(highEnergy / count)))
        filled = 0
        lowEnergy = 0.0
        midEnergy = 0.0
        highEnergy = 0.0
    }
}

object DjWaveRanges {
    const val CHUNK = 2L * 1024 * 1024

    fun declaredLength(url: String): Long? {
        val query = url.substringAfter('?', "")
        val value = query.split('&').firstOrNull { it.startsWith("clen=") }?.removePrefix("clen=") ?: return null
        return value.toLongOrNull()?.takeIf { it > 0 }
    }

    fun total(contentRange: String?): Long? {
        if (contentRange == null) return null
        val slash = contentRange.lastIndexOf('/')
        if (slash < 0) return null
        return contentRange.substring(slash + 1).trim().toLongOrNull()?.takeIf { it > 0 }
    }

    fun spans(total: Long, from: Long = 0, size: Long = CHUNK): List<LongRange> {
        if (from < 0 || total <= from || size <= 0) return emptyList()
        val out = mutableListOf<LongRange>()
        var start = from
        while (start < total) {
            out.add(start..(min(total, start + size) - 1))
            start += size
        }
        return out
    }

    fun header(span: LongRange): String = "bytes=${span.first}-${span.last}"
}

object DjWaveCache {
    fun encode(bars: List<DjWaveBar>): ByteArray {
        val bytes = ByteArray(bars.size * 2)
        for ((index, bar) in bars.withIndex()) {
            bytes[index * 2] = min(255, max(0, (bar.level * 255).roundToInt())).toByte()
            bytes[index * 2 + 1] = min(255, max(0, (bar.hue * 255).roundToInt())).toByte()
        }
        return bytes
    }

    fun decode(data: ByteArray): List<DjWaveBar> {
        if (data.isEmpty() || data.size % 2 != 0) return emptyList()
        return (data.indices step 2).map { index ->
            DjWaveBar(max(0.04, (data[index].toInt() and 0xff) / 255.0), min(0.999, (data[index + 1].toInt() and 0xff) / 255.0))
        }
    }
}

class DjScratchAudio(val samples: ShortArray, val rate: Double) {
    companion object {
        const val LONGEST = 15.0 * 60

        fun fits(seconds: Double): Boolean = seconds.isFinite() && seconds > 0 && seconds <= LONGEST

        fun pcm(value: Float): Short {
            if (!value.isFinite()) return 0
            return (min(1f, max(-1f, value)) * 32767).roundToInt().toShort()
        }
    }

    val duration: Double get() = if (rate > 0) samples.size / rate else 0.0
}

class DjScratchHead(position: Double = 0.0) {
    companion object {
        const val FOLLOW = 2 * PI * 22
        const val FULL_SPEED = 0.15
        const val FASTEST = 10.0
    }

    var position = position
        private set
    var velocity = 0.0
        private set
    var level = 0f
        private set
    private var lastIn = 0f
    private var lastOut = 0f

    val audible: Boolean get() = level > 0.0005f

    fun place(seconds: Double) {
        position = if (seconds.isFinite()) max(0.0, seconds) else 0.0
        velocity = 0.0
        level = 0f
        lastIn = 0f
        lastOut = 0f
    }

    fun render(pcm: ShortArray, sourceRate: Double, outputRate: Double, goal: Double, gain: Float, out: FloatArray, count: Int = out.size) {
        val size = pcm.size
        if (size <= 1 || sourceRate <= 0 || outputRate <= 0) {
            for (index in 0 until count) out[index] = 0f
            level = 0f
            return
        }
        val end = (size - 1) / sourceRate
        val target = if (goal.isFinite()) min(end, max(0.0, goal)) else position
        val step = 1 / outputRate
        val w = FOLLOW
        val fade = (1 - exp(-step / 0.008)).toFloat()
        val wanted = max(0f, min(1f, if (gain.isFinite()) gain else 0f))
        for (index in 0 until count) {
            velocity += (w * w * (target - position) - 2 * w * velocity) * step
            velocity = min(FASTEST, max(-FASTEST, velocity))
            position += velocity * step
            if (position <= 0 || position >= end) {
                position = min(end, max(0.0, position))
                velocity = 0.0
            }
            val place = position * sourceRate
            val base = min(size - 2, place.toInt())
            val fraction = (place - base).toFloat()
            val sample = (pcm[base] * (1 - fraction) + pcm[base + 1] * fraction) / 32768f
            val filtered = sample - lastIn + 0.995f * lastOut
            lastIn = sample
            lastOut = filtered
            val moving = min(1.0, abs(velocity) / FULL_SPEED).toFloat()
            level += (wanted * moving - level) * fade
            out[index] = max(-1f, min(1f, filtered * level))
        }
    }
}
