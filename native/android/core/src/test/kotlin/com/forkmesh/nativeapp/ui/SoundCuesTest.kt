package com.forkmesh.nativeapp.ui

import com.forkmesh.nativeapp.agentwatch.Instrument
import com.forkmesh.nativeapp.agentwatch.kTonicHz
import com.forkmesh.nativeapp.agentwatch.kWatchSampleRate
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.abs

class SoundCuesTest {
    fun testSoundsAreOnUntilTurnedOff() {
        val prefs = MemoryPrefStore()
        expect(SoundCuePrefs.enabled(prefs))
        expect(SoundCuePrefs.enabled(null))
        assertEq(SoundCue.SEND, SoundCuePrefs.shouldPlay(prefs, SoundCue.SEND))
        SoundCuePrefs.setEnabled(prefs, false)
        assertEq(null, SoundCuePrefs.shouldPlay(prefs, SoundCue.ERROR))
        SoundCuePrefs.setEnabled(prefs, true)
        assertEq(SoundCue.ERROR, SoundCuePrefs.shouldPlay(prefs, SoundCue.ERROR))
    }

    fun testEveryCueSoundsWithTheDesktopSlug() {
        assertEq(
            listOf(
                "keystroke", "click", "toggle-on", "toggle-off", "select", "navigate", "open", "close", "send",
                "arrive", "success", "warn", "error", "delete",
            ),
            SoundCue.entries.map { it.slug },
        )
        for (cue in SoundCue.entries) {
            val notes = SoundCueSynth.notes(cue, 1.0)
            expect(notes.isNotEmpty(), cue.slug)
            assertEq(1.0, notes.first().start, cue.slug)
            expect(notes.all { it.instrument == Instrument.celesta && it.pan == 0.5 }, cue.slug)
        }
    }

    fun testCuesSitOnTheScaleTwoOctavesUp() {
        val click = SoundCueSynth.notes(SoundCue.CLICK, 0.0)
        assertEq(1, click.size)
        expect(abs(click[0].frequency - kTonicHz * 4) < 1e-9)
        val on = SoundCueSynth.notes(SoundCue.TOGGLE_ON, 0.0).map { it.frequency }
        val off = SoundCueSynth.notes(SoundCue.TOGGLE_OFF, 0.0).map { it.frequency }
        expect(on[0] < on[1])
        assertEq(on, off.reversed())
        expect(SoundCueSynth.notes(SoundCue.ERROR, 0.0).all { it.frequency < kTonicHz * 4 })
    }

    fun testVolumeScalesAndZeroIsSilent() {
        expect(SoundCueSynth.notes(SoundCue.SUCCESS, 0.0, volume = 0.0).isEmpty())
        expect(abs(SoundCueSynth.notes(SoundCue.CLICK, 0.0, volume = 0.5)[0].gain - 0.0425) < 1e-12)
        expect(abs(SoundCueSynth.notes(SoundCue.CLICK, 0.0, volume = 3.0)[0].gain - 0.085) < 1e-12)
        expect(SoundCueSynth.render(SoundCue.CLICK, volume = 0.0).isEmpty())
    }

    fun testRatchetBurstsSpreadTheirTeethAcrossOneBatch() {
        assertEq(RatchetPawl.entries.size * RatchetBurst.MAX_TEETH, RatchetBurst.all.size)
        assertEq(RatchetBurst.all.size, RatchetBurst.all.toSet().size)
        assertEq(RatchetBurst.all.size, RatchetBurst.all.map { it.slug }.toSet().size)
        assertEq(1, RatchetBurst.of(0, RatchetPawl.Soft).teeth)
        assertEq(RatchetBurst.MAX_TEETH, RatchetBurst.of(500, RatchetPawl.Crisp).teeth)
        val one = RatchetBurst(1, RatchetPawl.Crisp).render()
        val six = RatchetBurst(6, RatchetPawl.Crisp).render()
        assertEq(Math.ceil(RatchetBurst.TOOTH_SECONDS * kWatchSampleRate).toInt() * 2, one.size)
        expect(six.size < Math.ceil((RatchetBurst.BATCH_MS / 1000.0 + RatchetBurst.TOOTH_SECONDS) * kWatchSampleRate).toInt() * 2)
        expect(six.size > one.size * 2)
        expect(peak(RatchetBurst(1, RatchetPawl.Soft).render()) < peak(one))
        expect(!RatchetBurst(2, RatchetPawl.Crisp).render(SoundCueSynth.keystrokeSeed(1)).contentEquals(RatchetBurst(2, RatchetPawl.Crisp).render(SoundCueSynth.keystrokeSeed(2))))
        val frame = (RatchetBurst(3, RatchetPawl.Crisp).spacing * kWatchSampleRate).toInt()
        val three = RatchetBurst(3, RatchetPawl.Crisp).render()
        val at = peak(three.copyOfRange(frame * 2, frame * 2 + 40))
        expect(at > 1000)
        expect(peak(three.copyOfRange(frame * 2 - 40, frame * 2)) < at)
        val caught = RatchetBurst.renderCatch()
        expect(caught.size > one.size)
        expect(peak(caught) > peak(one) / 2)
        expect(peak(caught) < Short.MAX_VALUE.toInt())
    }

    fun testRatchetGateStrikesAtOnceThenOncePerBatch() {
        val gate = RatchetGate()
        assertEq(null, gate.pass(0, 0, 0))
        expect(!gate.isOpen)
        assertEq(RatchetStroke(RatchetBurst(1, RatchetPawl.Soft), false), gate.pass(1, 0, 0))
        expect(gate.isOpen)
        assertEq(null, gate.pass(2, 0, 0))
        assertEq(null, gate.pass(3, 1, 0))
        assertEq(RatchetStroke(RatchetBurst(5, RatchetPawl.Crisp), false), gate.close())
        expect(gate.isOpen)
        assertEq(null, gate.pass(40, 0, 1))
        assertEq(RatchetStroke(RatchetBurst(RatchetBurst.MAX_TEETH, RatchetPawl.Crisp), true), gate.close())
        assertEq(null, gate.close())
        expect(!gate.isOpen)
        assertEq(RatchetBurst(2, RatchetPawl.Crisp), gate.pass(2, 2, 0)?.burst)
    }

    private fun peak(pcm: ShortArray): Int = pcm.maxOfOrNull { abs(it.toInt()) } ?: 0

    fun testWeightsRankOutcomesAboveGestures() {
        val ranked = SoundCue.entries.sortedBy { it.weight }
        assertEq(SoundCue.KEYSTROKE, ranked.first())
        assertEq(SoundCue.ERROR, ranked.last())
        expect(SoundCue.CLOSE.weight < SoundCue.CLICK.weight)
        expect(SoundCue.CLICK.weight < SoundCue.OPEN.weight)
        expect(SoundCue.SEND.weight < SoundCue.SUCCESS.weight)
    }

    fun testLabelsThatDestroyOrSendChooseTheirCue() {
        assertEq(SoundCue.DELETE, SoundCue.forLabel("Delete branch", SoundCue.CLICK))
        assertEq(SoundCue.DELETE, SoundCue.forLabel("&Remove", SoundCue.CLICK))
        assertEq(SoundCue.SEND, SoundCue.forLabel("  Send ", SoundCue.CLICK))
        assertEq(SoundCue.SEND, SoundCue.forLabel("Publish", SoundCue.CLICK))
        assertEq(SoundCue.OPEN, SoundCue.forLabel("Delete…", SoundCue.OPEN))
        assertEq(SoundCue.CLICK, SoundCue.forLabel("Delete...", SoundCue.CLICK))
        assertEq(SoundCue.CLICK, SoundCue.forLabel("Open", SoundCue.CLICK))
        assertEq(SoundCue.SELECT, SoundCue.forLabel("", SoundCue.SELECT))
        assertEq(SoundCue.TOGGLE_ON, SoundCue.toggle(true))
        assertEq(SoundCue.TOGGLE_OFF, SoundCue.toggle(false))
    }

    fun testGateCollapsesARattleButLetsWeightierNewsThrough() {
        val gate = SoundCueGate()
        expect(gate.admit(SoundCue.CLICK, 1000))
        expect(!gate.admit(SoundCue.CLICK, 1010))
        expect(!gate.admit(SoundCue.SELECT, 1020))
        expect(gate.admit(SoundCue.ERROR, 1030))
        expect(!gate.admit(SoundCue.SUCCESS, 1040))
        expect(gate.admit(SoundCue.CLICK, 1030 + SoundCueSynth.THROTTLE_MS))
    }

    fun testBatchKeepsTheWeightiestCueOfOneGesture() {
        val batch = SoundCueBatch()
        expect(batch.hold(SoundCue.CLICK))
        expect(!batch.hold(SoundCue.OPEN))
        expect(!batch.hold(SoundCue.CLOSE))
        assertEq(SoundCue.OPEN, batch.take())
        assertEq(null, batch.take())
        expect(batch.hold(SoundCue.SELECT))
    }

    fun testKeystrokesAreShadedApart() {
        val seeds = (1..SoundCueSynth.KEYSTROKE_VARIANTS).map(SoundCueSynth::keystrokeSeed)
        assertEq(SoundCueSynth.KEYSTROKE_VARIANTS, seeds.toSet().size)
        expect(seeds.all { it != 0L && it and 1L == 1L })
        expect(!SoundCueSynth.render(SoundCue.KEYSTROKE, seed = seeds[0]).contentEquals(SoundCueSynth.render(SoundCue.KEYSTROKE, seed = seeds[1])))
    }

    fun testRenderIsDryAndShort() {
        for (cue in SoundCue.entries) {
            val pcm = SoundCueSynth.render(cue)
            val seconds = (pcm.size / 2).toDouble() / kWatchSampleRate
            expect(seconds > 0.05, cue.slug)
            expect(seconds < 2.5, cue.slug)
            expect(pcm.any { it.toInt() != 0 }, cue.slug)
        }
        expect(SoundCueSynth.render(SoundCue.CLICK).size < SoundCueSynth.render(SoundCue.SUCCESS).size)
    }

    fun testWavHeaderDescribesThePcm() {
        val wav = SoundCueSynth.wav(shortArrayOf(1, -1, 300, -300), 44100, 2)
        assertEq(44 + 8, wav.size)
        assertEq("RIFF", String(wav, 0, 4))
        assertEq("WAVEfmt ", String(wav, 8, 8))
        assertEq("data", String(wav, 36, 4))
        fun le32(at: Int): Int = (0 until 4).fold(0) { acc, i -> acc or ((wav[at + i].toInt() and 0xff) shl (8 * i)) }
        assertEq(44, le32(4))
        assertEq(44100, le32(24))
        assertEq(44100 * 4, le32(28))
        assertEq(8, le32(40))
        assertEq(1.toByte(), wav[44])
        assertEq(0xFF.toByte(), wav[46])
        assertEq(0xFF.toByte(), wav[47])
    }

    fun testATouchFallsBackOnlyWhenNothingElseSounded() {
        val fallback = SoundCueFallback()
        expect(fallback.silentSince(0))
        fallback.heard(100)
        expect(!fallback.silentSince(90))
        expect(!fallback.silentSince(100))
        expect(fallback.silentSince(101))
        expect(SoundCueFallback.DELAY_MS > SoundCueSynth.THROTTLE_MS)
    }
}
