package com.forkmesh.nativeapp.chat.voice

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.resourceText

object CallVoiceParity {
    const val SAMPLE_TOLERANCE = 1e-6
    const val RMS_TOLERANCE = 1e-5

    val fixture: Map<String, Any?> by lazy { Json.parseObject(resourceText("chat/call-voice-parity.json")) }

    @Suppress("UNCHECKED_CAST")
    val cases: List<Map<String, Any?>> get() = fixture["cases"] as List<Map<String, Any?>>

    @Suppress("UNCHECKED_CAST")
    fun numbers(value: Any?): DoubleArray = (value as List<Any?>).map { (it as Number).toDouble() }.toDoubleArray()

    fun int(value: Any?): Int = (value as Number).toInt()

    private fun envelope(i: Int, a: Int, b: Int): Double =
        if (i < a || i >= b) 0.0 else minOf(1.0, minOf((i - a) / 480.0, (b - i) / 480.0))

    fun input(): FloatArray {
        val x = FloatArray(48000)
        var seed = 0x1234567
        var phase = 0.0
        var lp = 0.0
        for (i in 0 until 48000) {
            seed = seed xor (seed shl 13)
            seed = seed xor (seed ushr 17)
            seed = seed xor (seed shl 5)
            val noise = (seed.toLong() and 0xffffffffL).toDouble() / 4294967296.0 - 0.5
            val glide = if (i < 1920) 0.0 else if (i >= 24960) 1.0 else (i - 1920) / 23040.0
            val f0 = if (i < 28000) 220 - 40 * glide else 125.0
            phase += f0 / 48000
            if (phase >= 1) phase -= 1
            val saw = 2 * phase - 1
            lp += 0.3 * (saw - lp)
            val voiced = 0.4 * lp * (envelope(i, 1920, 24960) + envelope(i, 31680, 46080))
            x[i] = (voiced + noise * (0.3 * envelope(i, 24960, 30720) + 0.006)).toFloat()
        }
        return x
    }

    fun run(spec: Map<String, Any?>, chunk: Int = 48000): Pair<FloatArray, CallVoiceProcessor> {
        val samples = input()
        val processor = CallVoiceProcessor(48000.0, (spec["userPitchHz"] as Number).toDouble())
        processor.setVoice(spec["voice"] as String)
        val switchAt = int(spec["switchAt"]).takeIf { it > 0 } ?: samples.size
        var at = 0
        while (at < samples.size) {
            if (at == switchAt) processor.setVoice(spec["switchTo"] as String)
            val end = minOf(at + chunk, samples.size, if (at < switchAt) switchAt else samples.size)
            processor.process(samples, at, end - at)
            at = end
        }
        return samples to processor
    }

    fun rms(samples: FloatArray, block: Int): DoubleArray = DoubleArray(samples.size / block) { b ->
        var sum = 0.0
        for (i in b * block until (b + 1) * block) sum += samples[i].toDouble() * samples[i].toDouble()
        StrictMath.sqrt(sum / block)
    }

    fun hash(samples: FloatArray): Long {
        var h = 0x811c9dc5.toInt()
        for (v in samples) h = (h xor v.toRawBits()) * 0x01000193
        return h.toLong() and 0xffffffffL
    }

    fun maxStep(samples: FloatArray, from: Int, to: Int): Double {
        var top = 0.0
        for (i in maxOf(1, from) until minOf(samples.size, to)) top = maxOf(top, Math.abs(samples[i].toDouble() - samples[i - 1].toDouble()))
        return top
    }
}

class CallVoiceProcessorTest {
    private val p = CallVoiceParity

    fun testTheCatalogMatchesTheJsVoices() {
        @Suppress("UNCHECKED_CAST")
        val voices = p.fixture["voices"] as List<Map<String, Any?>>
        assertEq(voices.map { it["id"] }, CallVoiceCatalog.voices.map { it.id })
        for ((row, voice) in voices.zip(CallVoiceCatalog.voices)) {
            assertEq(row["label"], voice.label)
            for ((key, value) in listOf(
                "pitchHz" to voice.pitchHz, "tract" to voice.tract, "brightness" to voice.brightness, "breath" to voice.breath,
                "range" to voice.range, "jitter" to voice.jitter, "tremor" to voice.tremor,
            )) {
                assertEq((row[key] as Number).toDouble(), value, "${voice.id}.$key")
            }
        }
    }

    fun testChoicesResolveLikeTheWeb() {
        assertEq("off", CallVoiceCatalog.normalizeChoice(" OFF "))
        assertEq("kofi", CallVoiceCatalog.normalizeChoice("Kofi"))
        assertEq("face", CallVoiceCatalog.normalizeChoice("nobody"))
        assertEq("face", CallVoiceCatalog.normalizeChoice(null))
        assertEq("", CallVoiceCatalog.resolve("off", "maya"))
        assertEq("maya", CallVoiceCatalog.resolve("face", "maya"))
        assertEq("", CallVoiceCatalog.resolve("face", "custom"))
        assertEq("teen", CallVoiceCatalog.resolve("teen", "maya"))
        assertEq("Your own", CallVoiceCatalog.label(""))
        assertEq("Deep man", CallVoiceCatalog.label("deep"))
        assertEq(16, CallVoiceCatalog.choices().size)
        assertEq("face" to "Match face", CallVoiceCatalog.choices().first())
    }

    fun testTheInputMatchesTheJsGenerator() {
        val x = p.input()
        val expected = p.numbers(p.fixture["inputSamples"])
        val stride = p.int(p.fixture["stride"])
        for (k in expected.indices) expect(Math.abs(x[k * stride] - expected[k]) <= 1e-8, "input sample $k")
        assertEq(p.int(p.fixture["latency"]), CallVoiceProcessor().latencySamples)
    }

    fun testEveryVoiceMatchesTheJsEngine() {
        val stride = p.int(p.fixture["stride"])
        val block = p.int(p.fixture["block"])
        var worstSample = 0.0
        var worstRms = 0.0
        val differing = ArrayList<String>()
        for (case in p.cases) {
            val name = case["name"] as String
            val (out, processor) = p.run(case)
            val samples = p.numbers(case["samples"])
            for (k in samples.indices) {
                val error = Math.abs(out[k * stride] - samples[k])
                worstSample = maxOf(worstSample, error)
                expect(error <= CallVoiceParity.SAMPLE_TOLERANCE, "$name sample ${k * stride}: ${out[k * stride]} vs ${samples[k]}")
            }
            val rms = p.numbers(case["rms"])
            val ours = p.rms(out, block)
            for (k in rms.indices) {
                val error = Math.abs(ours[k] - rms[k]) / maxOf(rms[k], 1e-3)
                worstRms = maxOf(worstRms, error)
                expect(error <= CallVoiceParity.RMS_TOLERANCE, "$name rms block $k: ${ours[k]} vs ${rms[k]}")
            }
            if (p.hash(out) != (case["hash"] as Number).toLong()) differing.add(name)
            val pitch = (case["finalUserPitchHz"] as Number).toDouble()
            expect(Math.abs(processor.userPitchHz - pitch) <= 1e-6, "$name caller pitch ${processor.userPitchHz} vs $pitch")
        }
        println("call voice parity: worst sample error $worstSample, worst relative rms error $worstRms, not bit-identical ${differing.size} of ${p.cases.size} $differing")
    }

    fun testOffIsTheInputDelayedByTheLatency() {
        val input = p.input()
        val (out, processor) = p.run(mapOf("voice" to "off", "userPitchHz" to 0, "switchAt" to 0, "switchTo" to ""))
        val latency = processor.latencySamples
        for (i in 0 until latency) assertEq(0f, out[i])
        for (i in 0 until input.size - latency) {
            if (out[i + latency] != input[i]) throw AssertionError("off changed sample $i")
        }
    }

    fun testSwitchingVoicesMidStreamDoesNotClick() {
        for ((from, to) in listOf("maya" to "kofi", "off" to "teen", "robot" to "off", "deep" to "ingrid")) {
            val at = 24000
            val spec = mapOf("voice" to from, "userPitchHz" to 0, "switchAt" to at, "switchTo" to to)
            val (out, processor) = p.run(spec)
            val steady = maxOf(
                p.maxStep(p.run(spec + ("switchAt" to 0)).first, 0, 48000),
                p.maxStep(p.run(spec + ("voice" to to) + ("switchAt" to 0)).first, 0, 48000),
            )
            val around = p.maxStep(out, at, at + processor.latencySamples + 4800)
            expect(around <= 1.2 * steady, "$from to $to steps $around against steady $steady")
        }
    }

    fun testBufferSizesMatchOneLongBuffer() {
        for (name in listOf("maya-to-kofi", "older-woman")) {
            val spec = p.cases.first { it["name"] == name }
            val whole = p.run(spec).first
            for (chunk in listOf(128, 480, 960, 1024)) {
                val pieces = p.run(spec, chunk).first
                expect(pieces.contentEquals(whole), "$name in $chunk-sample buffers")
            }
        }
    }

    fun testANonFiniteSampleDoesNotPoisonTheStream() {
        val samples = p.input()
        samples[20000] = Float.NaN
        samples[20001] = Float.POSITIVE_INFINITY
        val processor = CallVoiceProcessor()
        processor.setVoice("maya")
        processor.process(samples)
        expect(samples.all { it.isFinite() }, "output stays finite")
        expect(p.rms(samples.copyOfRange(30000, 40000), 10000)[0] > 0.05, "voice carries on")
    }
}
