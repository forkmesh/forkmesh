package com.forkmesh.nativeapp.djstudio

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.exp
import kotlin.math.sin

class DjAudioTest {
    private val rate = 22_050.0
    private val outputRate = 44_100.0

    private fun groove(bpm: Double, seconds: Double = 40.0, hats: Boolean = true, snare: Boolean = true, seed: Long = 7): DjScratchAudio {
        var random = seed
        fun noise(): Double {
            random = random * 6364136223846793005L + 1442695040888963407L
            return random.toDouble() / Long.MAX_VALUE.toDouble()
        }
        val count = (seconds * rate).toInt()
        val signal = DoubleArray(count)
        val beat = 60 / bpm
        var index = 0
        var time = 0.31
        while (time < seconds) {
            val start = (time * rate).toInt()
            for (offset in 0 until (0.12 * rate).toInt()) {
                if (start + offset >= count) continue
                val t = offset / rate
                signal[start + offset] += 0.8 * sin(2 * PI * (55 + 90 * exp(-t * 30)) * t) * exp(-t * 22)
            }
            if (snare && index % 2 == 1) {
                for (offset in 0 until (0.09 * rate).toInt()) {
                    if (start + offset >= count) continue
                    signal[start + offset] += 0.35 * noise() * exp(-offset / rate * 35)
                }
            }
            if (hats) {
                val hat = ((time + beat / 2) * rate).toInt()
                for (offset in 0 until (0.03 * rate).toInt()) {
                    if (hat + offset >= count) continue
                    signal[hat + offset] += 0.2 * noise() * exp(-offset / rate * 120)
                }
            }
            index += 1
            time += beat
        }
        for (sample in signal.indices) signal[sample] += 0.03 * noise()
        return DjScratchAudio(ShortArray(count) { DjScratchAudio.pcm(signal[it].toFloat()) }, rate)
    }

    fun testFindsHouseTempo() {
        near(128.0, DjBpm.detect(groove(128.0))!!, 0.3)
    }

    fun testFindsHipHopTempoWithoutDoubling() {
        near(90.0, DjBpm.detect(groove(90.0, seed = 11))!!, 0.3)
    }

    fun testFindsFractionalTempo() {
        near(123.4, DjBpm.detect(groove(123.4, hats = false, seed = 3))!!, 0.3)
    }

    fun testFindsTempoOfPlainKicks() {
        near(140.0, DjBpm.detect(groove(140.0, hats = false, snare = false, seed = 5))!!, 0.3)
    }

    fun testNoBeatGivesNothing() {
        assertEq(null, DjBpm.detect(DjScratchAudio(ShortArray((rate * 30).toInt()), rate)))
        var random = 1L
        val hiss = ShortArray((rate * 30).toInt()) {
            random = random * 6364136223846793005L + 1442695040888963407L
            (random shr 52).toShort()
        }
        assertEq(null, DjBpm.detect(DjScratchAudio(hiss, rate)))
        assertEq(null, DjBpm.detect(DjScratchAudio(shortArrayOf(1, 2, 3), rate)))
        assertEq(null, DjBpm.detect(DjScratchAudio(ShortArray(0), 0.0)))
    }

    fun testFindsEveryTempoInRangeWithoutOctaveOrThirdErrors() {
        var bpm = 72.0
        while (bpm < 180) {
            for ((hats, snare) in listOf(true to true, false to false)) {
                val found = DjBpm.detect(groove(bpm, 30.0, hats, snare, (bpm * 10).toLong()))
                expect(found != null, "$bpm")
                near(bpm, found!!, 0.3, "$bpm hats $hats snare $snare")
            }
            bpm += 13.3
        }
    }

    fun testWaveformFollowsLoudnessAndBrightness() {
        val quiet = FloatArray(22_050)
        val bass = FloatArray(22_050)
        val treble = FloatArray(22_050)
        for (index in 0 until 22_050) {
            val time = index / rate
            quiet[index] = (0.05 * sin(2 * PI * 60 * time)).toFloat()
            bass[index] = (0.8 * sin(2 * PI * 60 * time)).toFloat()
            treble[index] = (0.8 * sin(2 * PI * 7000 * time)).toFloat()
        }
        val accumulator = DjWaveAccumulator(rate, 66_150, 3)
        accumulator.add(quiet)
        accumulator.add(bass)
        accumulator.add(treble)
        val bars = accumulator.finish(3)
        assertEq(3, bars.size)
        expect(bars[0].level < 0.2)
        expect(bars[1].level > 0.8)
        expect(bars[1].hue < 0.15)
        expect(bars[2].hue > 0.45)
        assertEq(3, DjWaveCache.decode(DjWaveCache.encode(bars)).size)
        near(bars[1].level, DjWaveCache.decode(DjWaveCache.encode(bars))[1].level, 0.01)
        val silent = DjWaveAccumulator(rate, 100)
        silent.add(FloatArray(100))
        expect(silent.finish().isEmpty())
        expect(DjWaveCache.decode(byteArrayOf(1)).isEmpty())
    }

    private fun tone(seconds: Double, frequency: Double): ShortArray =
        ShortArray((seconds * rate).toInt()) { DjScratchAudio.pcm((0.5 * sin(2 * PI * frequency * it / rate)).toFloat()) }

    private fun run(head: DjScratchHead, pcm: ShortArray, from: Double, speed: Double, seconds: Double, gain: Float = 1f): FloatArray {
        val block = 256
        var goal = from
        val blocks = (seconds * outputRate).toInt() / block
        val out = FloatArray(blocks * block)
        val chunk = FloatArray(block)
        for (index in 0 until blocks) {
            goal += speed * block / outputRate
            head.render(pcm, rate, outputRate, goal, gain, chunk)
            chunk.copyInto(out, index * block)
        }
        return out
    }

    private fun crossings(samples: FloatArray, from: Int): Int {
        var count = 0
        for (index in from until samples.size - 1) if ((samples[index] < 0) != (samples[index + 1] < 0)) count += 1
        return count
    }

    fun testPcmConversionClamps() {
        assertEq(32767.toShort(), DjScratchAudio.pcm(2f))
        assertEq((-32767).toShort(), DjScratchAudio.pcm(-2f))
        assertEq(0.toShort(), DjScratchAudio.pcm(Float.NaN))
        assertEq(2.0, DjScratchAudio(ShortArray(44_100), 22_050.0).duration)
        expect(DjScratchAudio.fits(240.0))
        expect(!DjScratchAudio.fits(3600.0))
        expect(!DjScratchAudio.fits(Double.NaN))
    }

    fun testForwardStrokePlaysTheSongAtTheHandsSpeed() {
        val pcm = tone(4.0, 441.0)
        val head = DjScratchHead()
        head.place(1.0)
        val out = run(head, pcm, 1.0, 1.0, 1.0)
        val tail = out.size - (outputRate / 2).toInt()
        near(441.0, crossings(out, tail) / 2.0 / 0.5, 12.0)
        expect((tail until out.size).maxOf { abs(out[it]) } > 0.3f)
        near(2.0, head.position, 0.03)
    }

    fun testBackwardStrokePlaysTheSongInReverse() {
        val pcm = tone(4.0, 441.0)
        val head = DjScratchHead()
        head.place(3.0)
        val out = run(head, pcm, 3.0, -2.0, 0.5)
        val tail = out.size - (outputRate / 4).toInt()
        near(882.0, crossings(out, tail) / 2.0 / 0.25, 25.0)
        expect(head.velocity < -1.5)
        near(2.0, head.position, 0.03)
    }

    fun testHoldingTheRecordStillIsSilent() {
        val pcm = tone(4.0, 441.0)
        val head = DjScratchHead()
        head.place(1.0)
        run(head, pcm, 1.0, 1.0, 0.3)
        val held = run(head, pcm, head.position, 0.0, 0.3)
        expect((held.size - 4410 until held.size).maxOf { abs(held[it]) } < 0.001f)
        expect(!head.audible)
    }

    fun testZeroGainFadesOutAndTheHeadStaysInTheSong() {
        val pcm = tone(2.0, 441.0)
        val head = DjScratchHead()
        head.place(1.5)
        val out = run(head, pcm, 1.5, 3.0, 0.5, 0f)
        expect(out.maxOf { abs(it) } <= 1e-6f)
        expect(head.position <= (pcm.size - 1) / rate)
        expect(head.position >= 0)
    }
}
