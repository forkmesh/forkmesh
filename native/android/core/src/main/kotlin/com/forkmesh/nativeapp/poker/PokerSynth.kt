package com.forkmesh.nativeapp.poker

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin
import kotlin.random.Random

class PokerSynth(val rate: Int = 44_100, seed: Int = 7) {
    private val random = Random(seed)

    fun render(sound: PokerSound): ShortArray = pcm(
        when (sound) {
            PokerSound.SHUFFLE -> shuffle()
            PokerSound.DEAL -> deal()
            PokerSound.CHIP -> chips(3, 0.045)
            PokerSound.CHECK -> check()
            PokerSound.FOLD -> fold()
            PokerSound.TURN -> turn()
            PokerSound.CLOCK -> clock()
            PokerSound.ALLIN -> allIn()
            PokerSound.WIN -> win()
        },
        when (sound) {
            PokerSound.TURN, PokerSound.CLOCK -> 0.55
            PokerSound.WIN, PokerSound.ALLIN -> 0.8
            else -> 0.7
        },
    )

    private fun samples(seconds: Double): Int = (seconds * rate).toInt().coerceAtLeast(1)

    private fun buffer(seconds: Double) = DoubleArray(samples(seconds))

    private fun noise(): Double = random.nextDouble() * 2 - 1

    private class BandPass(rate: Int, centre: Double, q: Double) {
        private var b0 = 0.0
        private var b2 = 0.0
        private var a1 = 0.0
        private var a2 = 0.0
        private var x1 = 0.0
        private var x2 = 0.0
        private var y1 = 0.0
        private var y2 = 0.0
        private val sampleRate = rate.toDouble()
        private val quality = q

        init {
            tune(centre)
        }

        fun tune(centre: Double) {
            val w = 2 * PI * centre / sampleRate
            val alpha = sin(w) / (2 * quality)
            val a0 = 1 + alpha
            b0 = alpha / a0
            b2 = -alpha / a0
            a1 = -2 * cos(w) / a0
            a2 = (1 - alpha) / a0
        }

        fun next(x: Double): Double {
            val y = b0 * x + b2 * x2 - a1 * y1 - a2 * y2
            x2 = x1
            x1 = x
            y2 = y1
            y1 = y
            return y
        }
    }

    private class LowPass(rate: Int, cutoff: Double) {
        private val k = 1 - exp(-2 * PI * cutoff / rate)
        private var y = 0.0

        fun next(x: Double): Double {
            y += k * (x - y)
            return y
        }
    }

    private fun mix(into: DoubleArray, start: Int, part: DoubleArray, gain: Double = 1.0) {
        for (i in part.indices) {
            val at = start + i
            if (at in into.indices) into[at] += part[i] * gain
        }
    }

    private fun shuffle(): DoubleArray {
        val ticks = 10
        val gap = 0.032
        val out = buffer(gap * ticks + 0.05)
        var at = 0.0
        for (i in 0 until ticks) {
            val swell = 0.35 + 0.65 * sin(PI * (i + 0.5) / ticks)
            val tick = buffer(0.012)
            val filter = BandPass(rate, 3000.0, 1.4)
            for (n in tick.indices) {
                val t = n.toDouble() / tick.size
                tick[n] = filter.next(noise()) * exp(-5 * t) * swell
            }
            mix(out, (at * rate).toInt(), tick, 2.2)
            at += gap + (random.nextDouble() * 2 - 1) * 0.006
        }
        return out
    }

    private fun deal(): DoubleArray {
        val out = buffer(0.045)
        val filter = BandPass(rate, 5000.0, 1.1)
        for (n in out.indices) {
            val t = n.toDouble() / out.size
            filter.tune(5000.0 - 3500.0 * t)
            out[n] = filter.next(noise()) * exp(-7 * t)
        }
        return out
    }

    private fun clack(): DoubleArray {
        val out = buffer(0.03)
        val tickLength = samples(0.005)
        for (n in out.indices) {
            val time = n.toDouble() / rate
            val decay = exp(-time / 0.025 * 4)
            var v = 0.55 * sin(2 * PI * 3200 * time) * decay + 0.35 * sin(2 * PI * 5100 * time) * decay
            if (n < tickLength) v += noise() * 0.5 * (1 - n.toDouble() / tickLength)
            out[n] = v
        }
        return out
    }

    private fun chips(count: Int, spacing: Double, fade: Double = 1.0, jitter: Double = 0.0): DoubleArray {
        val out = buffer(spacing * count + 0.04)
        var at = 0.0
        var gain = 1.0
        for (i in 0 until count) {
            mix(out, (at * rate).toInt(), clack(), gain)
            at += spacing + if (jitter > 0) (random.nextDouble() * 2 - 1) * jitter else 0.0
            gain *= fade
        }
        return out
    }

    private fun knock(): DoubleArray {
        val out = buffer(0.07)
        val filter = LowPass(rate, 900.0)
        for (n in out.indices) {
            val time = n.toDouble() / rate
            val decay = exp(-time / 0.06 * 5)
            out[n] = (sin(2 * PI * 150 * time) + 0.25 * filter.next(noise())) * decay
        }
        return out
    }

    private fun check(): DoubleArray {
        val out = buffer(0.2)
        mix(out, 0, knock())
        mix(out, samples(0.12), knock(), 0.9)
        return out
    }

    private fun fold(): DoubleArray {
        val out = buffer(0.18)
        val filter = LowPass(rate, 1200.0)
        for (n in out.indices) {
            val t = n.toDouble() / out.size
            out[n] = filter.next(noise()) * sin(PI * t) * 2.5
        }
        return out
    }

    private fun tone(frequency: Double, seconds: Double, triangle: Boolean = false): DoubleArray {
        val out = buffer(seconds)
        val attack = samples(0.008)
        for (n in out.indices) {
            val time = n.toDouble() / rate
            val phase = (frequency * time) % 1.0
            val wave = if (triangle) 1 - 4 * abs(phase - 0.5) else sin(2 * PI * phase)
            val release = (out.size - n).toDouble() / out.size
            val envelope = min(1.0, n.toDouble() / attack) * release.coerceAtMost(1.0) * exp(-2.5 * n / out.size)
            out[n] = wave * envelope
        }
        return out
    }

    private fun turn(): DoubleArray {
        val out = buffer(0.29)
        mix(out, 0, tone(880.0, 0.14), 0.6)
        mix(out, samples(0.145), tone(1320.0, 0.14), 0.6)
        return out
    }

    private fun clock(): DoubleArray {
        val out = buffer(0.02)
        for (n in out.indices) {
            val time = n.toDouble() / rate
            out[n] = sin(2 * PI * 1000 * time) * exp(-time / 0.02 * 4)
        }
        return out
    }

    private fun cascade(): DoubleArray = chips(7, 0.05, 0.86, 0.015)

    private fun allIn(): DoubleArray {
        val out = buffer(0.48)
        var phase = 0.0
        val length = samples(0.4)
        for (n in 0 until length) {
            val t = n.toDouble() / length
            val frequency = 110 * Math.pow(2.0, t)
            phase += frequency / rate
            val envelope = min(1.0, n / (0.02 * rate)) * (1 - t * 0.6)
            out[n] += sin(2 * PI * phase) * envelope * 0.7
        }
        mix(out, samples(0.05), cascade(), 0.6)
        return out
    }

    private fun win(): DoubleArray {
        val notes = listOf(523.25, 659.25, 783.99, 1046.5)
        val step = 0.11
        val cascadeAt = step * notes.size
        val out = buffer(cascadeAt + 0.45)
        notes.forEachIndexed { i, frequency -> mix(out, samples(step * i), tone(frequency, step + 0.06, triangle = true), 0.6) }
        mix(out, samples(cascadeAt), cascade(), 0.7)
        return out
    }

    private fun pcm(signal: DoubleArray, level: Double): ShortArray {
        var peak = 0.0
        for (v in signal) peak = max(peak, abs(v))
        val scale = if (peak > 0) level / peak else 0.0
        return ShortArray(signal.size) { i -> (signal[i] * scale * Short.MAX_VALUE).toInt().coerceIn(-32767, 32767).toShort() }
    }

    companion object {
        fun durationMs(samples: Int, rate: Int): Long = Math.round(samples * 1000.0 / rate)
    }
}
