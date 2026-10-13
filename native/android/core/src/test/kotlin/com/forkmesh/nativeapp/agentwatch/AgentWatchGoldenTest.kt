package com.forkmesh.nativeapp.agentwatch

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.resourceText
import kotlin.math.abs
import kotlin.math.max

class AgentWatchGoldenTest {
    private val goldens: Map<String, Any?> by lazy {
        @Suppress("UNCHECKED_CAST")
        Json.parse(resourceText("agentwatch/goldens.json")) as Map<String, Any?>
    }

    @Suppress("UNCHECKED_CAST")
    private fun rows(key: String): List<List<Any?>> = goldens[key] as List<List<Any?>>

    private fun num(value: Any?): Number = value as Number

    private fun near(want: Double, got: Double, tolerance: Double, label: String) {
        if (abs(want - got) > tolerance) throw AssertionError("$label: expected <$want> but was <$got>")
    }

    private fun note(row: List<Any?>): Note = Note(
        start = num(row[0]).toDouble(),
        frequency = num(row[1]).toDouble(),
        seconds = num(row[2]).toDouble(),
        gain = num(row[3]).toDouble(),
        pan = num(row[4]).toDouble(),
        timbre = Timbre.entries[num(row[5]).toInt()],
        instrument = Instrument.entries[num(row[6]).toInt()],
        bloom = num(row[7]).toDouble(),
        seed = num(row[8]).toLong(),
    )

    fun testTranscriptBeatsMatchDart() {
        val beats = beatsFromTranscript(goldens["transcript"] as String)
        val want = rows("beats")
        assertEq(want.size, beats.size)
        beats.zip(want).forEach { (beat, row) ->
            val label = row[1] as String
            assertEq(row[0], beat.motion.name, label)
            assertEq(row[1], beat.text)
            assertEq(num(row[2]).toInt(), beat.bytes, label)
            assertEq(row[3], beat.said, label)
            assertEq(num(row[4]).toLong(), beat.seed, label)
        }
    }

    fun testToolsAndCommandsMatchDart() {
        for (row in rows("tools")) assertEq(row[1], motionForTool(row[0] as String).name, row[0] as String)
        for (row in rows("commands")) assertEq(row[1], motionForCommand(row[0] as String).name, row[0] as String)
    }

    fun testVoicesMatchDart() {
        for (row in rows("voices")) {
            val voice = Voice.forAgent(num(row[0]).toInt(), row[1] as String, num(row[2]).toInt())
            val label = "${row[0]} ${row[1]} ${row[2]}"
            assertEq(num(row[3]).toInt(), voice.instrument.ordinal, label)
            assertEq(num(row[4]).toInt(), voice.octave, label)
            assertEq(num(row[5]).toInt(), voice.degreeOffset, label)
            near(num(row[6]).toDouble(), voice.pan, 0.0, label)
            near(num(row[7]).toDouble(), voice.slotOffset, 0.0, label)
            near(num(row[8]).toDouble(), voice.detune, 1e-15, label)
            near(num(row[9]).toDouble(), voice.hue, 1e-15, label)
            assertEq(row[10], voice.description, label)
            @Suppress("UNCHECKED_CAST")
            (row[11] as List<Any?>).forEachIndexed { k, f ->
                val frequency = num(f).toDouble()
                near(frequency, voice.frequency(k - 3), frequency * 1e-13, label)
            }
        }
    }

    fun testMelodyMatchesDart() {
        @Suppress("UNCHECKED_CAST")
        val spec = goldens["melodyVoice"] as Map<String, Any?>
        val voice = Voice.forAgent(num(spec["id"]).toInt(), spec["title"] as String)
        voice.applyUsage(num(spec["turns"]).toInt(), num(spec["tokens"]).toInt())
        val melody = Melody()
        val notes = ArrayList<Note>()
        notes += announce(voice, melody, 0.02)
        var now = 0.5
        AgentMotion.entries.forEachIndexed { i, motion ->
            val bytes = (i * 137) % 9000
            val seed = if (i % 2 == 0) 0L else fnv1a("${motion.name}|$i")
            notes += noteForBeat(motion, voice, melody, now, bytes, seed)
            now += if (i % 3 == 0) 0.31 else 0.07
        }
        for (k in 0 until 20) notes += noteForBeat(AgentMotion.run, voice, melody, now, k * 50, k.toLong())
        notes += doneCadence(voice, true, 30.0)
        notes += doneCadence(voice, false, 33.0)
        val want = rows("melodyNotes").map(::note)
        assertEq(want.size, notes.size)
        notes.zip(want).forEachIndexed { i, (got, w) ->
            val label = "note $i"
            near(w.start, got.start, 1e-12, label)
            near(w.frequency, got.frequency, max(1e-12, w.frequency * 1e-13), label)
            near(w.seconds, got.seconds, 1e-12, label)
            near(w.gain, got.gain, 1e-12, label)
            near(w.pan, got.pan, 1e-12, label)
            assertEq(w.timbre, got.timbre, label)
            assertEq(w.instrument, got.instrument, label)
            near(w.bloom, got.bloom, 1e-12, label)
            assertEq(w.seed, got.seed, label)
        }
        @Suppress("UNCHECKED_CAST")
        val end = goldens["melodyEnd"] as Map<String, Any?>
        assertEq(num(end["degree"]).toInt(), melody.degree)
        near(num(end["lastSlot"]).toDouble(), melody.lastSlot, 1e-9, "lastSlot")
        near(num(end["lastGrace"]).toDouble(), melody.lastGrace, 1e-9, "lastGrace")
        assertEq(num(end["count"]).toInt(), melody.count)
        assertEq(num(end["graceCount"]).toInt(), melody.graceCount)
        assertEq(num(end["graceRun"]).toInt(), melody.graceRun)
    }

    fun testFeelMatchesDart() {
        val voice = Voice.forAgent(3, "x")
        for (row in rows("feel")) {
            val name = row[0] as String
            val got = feelFor(AgentMotion.named(name)!!, voice)
            @Suppress("UNCHECKED_CAST")
            val want = row[1] as List<List<Any?>>
            assertEq(want.size, got.size, name)
            got.zip(want).forEach { (pulse, w) ->
                assertEq(num(w[0]).toInt(), pulse.atMs, name)
                assertEq(num(w[1]).toInt(), pulse.ms, name)
                near(num(w[2]).toDouble(), pulse.intensity, 1e-12, name)
                near(num(w[3]).toDouble(), pulse.sharpness, 1e-12, name)
            }
        }
    }

    fun testPcmMatchesDart() {
        rows("pcm").forEachIndexed { i, row ->
            @Suppress("UNCHECKED_CAST")
            val notes = (row[0] as List<List<Any?>>).map(::note)
            val pcm = renderNotes(notes, num(row[1]).toDouble(), echo = row[2] as Boolean)
            assertEq(num(row[3]).toInt(), pcm.size, "render $i")
            var sum = 0L
            for (s in pcm) sum += abs(s.toInt())
            near(num(row[4]).toDouble(), sum.toDouble(), pcm.size.toDouble(), "render $i sum")
            var k = 0
            @Suppress("UNCHECKED_CAST")
            for (sample in row[5] as List<Any?>) {
                expect(abs(pcm[k].toInt() - num(sample).toInt()) <= 2, "render $i sample $k: ${pcm[k]} vs $sample")
                k += 997
            }
        }
    }
}
