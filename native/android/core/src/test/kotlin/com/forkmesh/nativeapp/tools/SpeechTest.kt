package com.forkmesh.nativeapp.tools

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.ByteArrayOutputStream
import java.io.File
import kotlin.math.abs

class SpeechTest {
    private class FakeEngine : SpeechEngine {
        override var onRange: ((Int, Int) -> Unit)? = null
        override var onFinish: (() -> Unit)? = null
        override var onStatus: (() -> Unit)? = null
        override var status: SpeechEngineStatus = SpeechEngineStatus.READY
        var installed: List<SpeechVoice> = emptyList()
        val spoken = ArrayList<Triple<String, String, Pair<Float, Float>>>()
        val calls = ArrayList<String>()
        var renderFails = false

        override fun voices(): List<SpeechVoice> = installed
        override fun speak(text: String, voiceId: String, rate: Float, pitch: Float) {
            spoken += Triple(text, voiceId, rate to pitch)
            calls += "speak"
        }
        override fun pause() { calls += "pause" }
        override fun resume() { calls += "resume" }
        override fun stop() { calls += "stop" }
        override fun render(text: String, voiceId: String, rate: Float, pitch: Float, name: String): File {
            if (renderFails) throw IllegalStateException("disk full")
            return File("speech", name)
        }
    }

    private val standard = SpeechVoice("en-us-x-tpc-local", "Voice TPC", "en-US", SpeechVoiceQuality.STANDARD)
    private val ava = SpeechVoice("en-us-x-iol-local", "Ava", "en-US", SpeechVoiceQuality.PREMIUM)
    private val avaOnline = SpeechVoice("en-us-x-iol-network", "Ava", "en-US", SpeechVoiceQuality.PREMIUM, online = true)
    private val serena = SpeechVoice("en-gb-x-gba-local", "Serena", "en-GB", SpeechVoiceQuality.PREMIUM)
    private val zoe = SpeechVoice("en-us-x-sfg-local", "Zoe", "en-US", SpeechVoiceQuality.ENHANCED)
    private val thomas = SpeechVoice("fr-fr-x-frd-local", "Thomas", "fr-FR", SpeechVoiceQuality.PREMIUM)
    private val eddy = SpeechVoice("en-us-x-eddy-local", "Eddy", "en-US", SpeechVoiceQuality.STANDARD)

    private fun model(voices: List<SpeechVoice>, prefs: MemoryPrefStore = MemoryPrefStore()): Pair<SpeechPlaygroundModel, FakeEngine> {
        val engine = FakeEngine()
        engine.installed = voices
        val model = SpeechPlaygroundModel(engine, prefs, "en-US")
        model.load()
        return model to engine
    }

    private fun near(expected: Double, actual: Double) = expect(abs(expected - actual) < 0.0001, "expected $expected but was $actual")

    fun testBestVoicePrefersQualityInTheLanguage() {
        assertEq(ava, SpeechVoices.best(listOf(standard, zoe, ava, thomas), "en-US"))
        assertEq(serena, SpeechVoices.best(listOf(standard, serena, zoe), "en-US"))
        assertEq(standard, SpeechVoices.best(listOf(standard, thomas), "en-US"))
        assertEq(thomas, SpeechVoices.best(listOf(standard, thomas), "fr_FR"))
        assertEq(ava, SpeechVoices.best(listOf(avaOnline, ava), "en-US"))
        assertEq(eddy, SpeechVoices.best(listOf(standard, eddy), "en-US"))
        assertEq(null, SpeechVoices.best(emptyList(), "en-US"))
    }

    fun testLanguageMatchAndNaturalHint() {
        assertEq(2, SpeechVoices.languageMatch("en-US", "en_us"))
        assertEq(1, SpeechVoices.languageMatch("en-GB", "en-US"))
        assertEq(0, SpeechVoices.languageMatch("fr-FR", "en-US"))
        expect(!SpeechVoices.hasNatural(listOf(standard, eddy, thomas), "en-US"))
        expect(SpeechVoices.hasNatural(listOf(standard, serena), "en-US"))
    }

    fun testDisplayNames() {
        assertEq("Voice IOL", SpeechVoices.displayName("en-us-x-iol-local"))
        assertEq("Voice SFG", SpeechVoices.displayName("en-US-x-sfg-network"))
        assertEq("Default voice", SpeechVoices.displayName("en-US-language"))
        assertEq("SMTl01", SpeechVoices.displayName("SMTl01"))
        assertEq(SpeechVoiceQuality.PREMIUM, SpeechVoiceQuality.ofAndroid(500))
        assertEq(SpeechVoiceQuality.ENHANCED, SpeechVoiceQuality.ofAndroid(400))
        assertEq(SpeechVoiceQuality.STANDARD, SpeechVoiceQuality.ofAndroid(300))
    }

    fun testSectionsPutTheLanguageFirst() {
        val sections = SpeechVoices.sections(listOf(thomas, standard, eddy, serena, avaOnline, ava, zoe), "en-US") { it }
        assertEq(listOf("en-us", "en-gb", "fr-fr"), sections.map { it.id })
        assertEq(listOf(ava, avaOnline, zoe, eddy, standard), sections[0].voices)
        assertEq("en-US", sections[0].title)
    }

    fun testTuning() {
        assertEq(1f, SpeechTuning.engineRate(1.0))
        assertEq(0.5f, SpeechTuning.engineRate(0.1))
        assertEq(2f, SpeechTuning.engineRate(9.0))
        assertEq("1×", SpeechTuning.speedLabel(1.0))
        assertEq("1.25×", SpeechTuning.speedLabel(1.25))
        assertEq("0.5×", SpeechTuning.speedLabel(0.5))
        assertEq("Natural", SpeechTuning.pitchLabel(1.01))
        assertEq("Lower", SpeechTuning.pitchLabel(0.8))
        assertEq("Higher", SpeechTuning.pitchLabel(1.3))
        assertEq(0, SpeechTuning.estimatedSeconds("", 1.0))
        assertEq(60, SpeechTuning.estimatedSeconds("word ".repeat(165), 1.0))
        assertEq(30, SpeechTuning.estimatedSeconds("word ".repeat(165), 2.0))
        near(1.25, SpeechTuning.snap(1.2649))
    }

    fun testHighlightUsesUtf16Ranges() {
        val text = "Hi 👋 there friend"
        assertEq(SpeechHighlight("Hi 👋 ", "there", " friend"), SpeechText.highlight(text, 6, 5))
        assertEq(null, SpeechText.highlight(text, 4, 1))
        assertEq(null, SpeechText.highlight(text, 40, 2))
        assertEq(null, SpeechText.highlight(text, 0, 0))
    }

    fun testFileNameAndDuration() {
        assertEq("hello-world-how-are-you.wav", SpeechText.fileName("Hello, World! How are you today?"))
        assertEq("speech.wav", SpeechText.fileName("!!!"))
        assertEq("42 s", SpeechText.duration(42))
        assertEq("2 min 5 s", SpeechText.duration(125))
    }

    fun testChunksSplitAtSentencesThenSpacesAndCoverTheText() {
        val text = "One two. Three four five. Six"
        val chunks = SpeechText.chunks(text, 12)
        assertEq(listOf(SpeechChunk(0, "One two. "), SpeechChunk(9, "Three four "), SpeechChunk(20, "five. Six")), chunks)
        assertEq(text, chunks.joinToString("") { it.text })
        for (chunk in chunks) assertEq(chunk.text, text.substring(chunk.offset, chunk.offset + chunk.text.length))
        assertEq(listOf(SpeechChunk(0, "abcd"), SpeechChunk(4, "efgh"), SpeechChunk(8, "ij")), SpeechText.chunks("abcdefghij", 4))
        val emoji = "abc👋def"
        assertEq(listOf(SpeechChunk(0, "abc"), SpeechChunk(3, "👋de"), SpeechChunk(7, "f")), SpeechText.chunks(emoji, 4))
        assertEq(listOf(SpeechChunk(0, "short")), SpeechText.chunks("short", 4000))
        assertEq(emptyList<SpeechChunk>(), SpeechText.chunks("", 10))
    }

    fun testUtteranceIdsRoundTrip() {
        val utterance = SpeechUtterance(7, 4000, true)
        assertEq(utterance, SpeechUtterance.parse(utterance.id))
        assertEq(SpeechUtterance(1, 0, false), SpeechUtterance.parse("speech-1-0-0"))
        assertEq(null, SpeechUtterance.parse("genie"))
        assertEq(null, SpeechUtterance.parse("speech-1--4-1"))
        assertEq(null, SpeechUtterance.parse(null))
    }

    private fun wav(format: ByteArray, data: ByteArray, extra: Boolean = false): ByteArray {
        val out = ByteArrayOutputStream()
        fun int32(value: Int) = listOf(0, 8, 16, 24).forEach { out.write((value shr it) and 0xff) }
        out.write("RIFF".toByteArray())
        int32(0)
        out.write("WAVE".toByteArray())
        if (extra) {
            out.write("LIST".toByteArray())
            int32(3)
            out.write(byteArrayOf(1, 2, 3, 0))
        }
        out.write("fmt ".toByteArray())
        int32(format.size)
        out.write(format)
        out.write("data".toByteArray())
        int32(data.size)
        out.write(data)
        return out.toByteArray()
    }

    fun testWavJoinConcatenatesSamples() {
        val format = ByteArray(16) { it.toByte() }
        val joined = SpeechWav.join(listOf(wav(format, byteArrayOf(1, 2)), wav(format, byteArrayOf(3, 4, 5, 6), extra = true)))
        assertEq("RIFF", String(joined, 0, 4))
        assertEq("WAVE", String(joined, 8, 4))
        assertEq(44 + 6, joined.size)
        assertEq(joined.size - 8, (joined[4].toInt() and 0xff) or ((joined[5].toInt() and 0xff) shl 8))
        assertEq(format.toList(), joined.copyOfRange(20, 36).toList())
        assertEq("data", String(joined, 36, 4))
        assertEq(6, joined[40].toInt())
        assertEq(listOf<Byte>(1, 2, 3, 4, 5, 6), joined.copyOfRange(44, 50).toList())
        val single = wav(format, byteArrayOf(9))
        expect(single.contentEquals(SpeechWav.join(listOf(single))))
        var failed = false
        try {
            SpeechWav.join(listOf(single, wav(ByteArray(16), byteArrayOf(1))))
        } catch (_: IllegalArgumentException) {
            failed = true
        }
        expect(failed, "mismatched formats are refused")
    }

    fun testWavLayoutReadsAHeadOnly() {
        val format = ByteArray(16) { 7 }
        val whole = wav(format, ByteArray(10_000) { 1 }, extra = true)
        val layout = SpeechWav.layout(whole.copyOfRange(0, SpeechWav.HEAD_BYTES), whole.size.toLong())
        assertEq(format.toList(), layout.format.toList())
        assertEq(56, layout.dataOffset)
        assertEq(10_000, layout.dataSize)
        whole[52] = 0xff.toByte()
        whole[53] = 0xff.toByte()
        whole[54] = 0xff.toByte()
        whole[55] = 0xff.toByte()
        assertEq(10_000, SpeechWav.layout(whole.copyOfRange(0, 64), whole.size.toLong()).dataSize)
        assertEq(44, SpeechWav.header(format, 0).size)
    }

    fun testLoadPicksTheBestVoiceUnlessOneWasSaved() {
        assertEq(ava, model(listOf(standard, ava, zoe)).first.voice)
        val prefs = MemoryPrefStore()
        prefs.putString(SpeechPrefs.VOICE, zoe.id)
        assertEq(zoe, model(listOf(standard, ava, zoe), prefs).first.voice)
        prefs.putString(SpeechPrefs.VOICE, "gone")
        assertEq(ava, model(listOf(standard, ava), prefs).first.voice)
    }

    fun testEngineStartingLateLoadsVoices() {
        val engine = FakeEngine()
        engine.status = SpeechEngineStatus.STARTING
        val model = SpeechPlaygroundModel(engine, MemoryPrefStore(), "en-US")
        var changes = 0
        model.onChange = { changes++ }
        model.load()
        assertEq(null, model.voice)
        engine.installed = listOf(ava)
        engine.status = SpeechEngineStatus.READY
        engine.onStatus?.invoke()
        assertEq(ava, model.voice)
        assertEq(SpeechEngineStatus.READY, model.status)
        assertEq(1, changes)
    }

    fun testSpeakPauseResumeAndFinish() {
        val (model, engine) = model(listOf(ava))
        model.edit("  Hello there  ")
        model.speed = 2.0
        model.pitch = 1.2
        model.toggle()
        assertEq(SpeechPlaygroundModel.State.SPEAKING, model.state)
        assertEq("Hello there", engine.spoken.last().first)
        assertEq(ava.id, engine.spoken.last().second)
        assertEq(2f, engine.spoken.last().third.first)
        near(1.2, engine.spoken.last().third.second.toDouble())
        engine.onRange?.invoke(6, 5)
        assertEq(SpeechHighlight("Hello ", "there", ""), model.highlight)
        model.toggle()
        assertEq(SpeechPlaygroundModel.State.PAUSED, model.state)
        model.toggle()
        assertEq(SpeechPlaygroundModel.State.SPEAKING, model.state)
        engine.onFinish?.invoke()
        assertEq(SpeechPlaygroundModel.State.IDLE, model.state)
        assertEq(null, model.highlight)
        assertEq(listOf("speak", "pause", "resume"), engine.calls)
    }

    fun testTextPersists() {
        val prefs = MemoryPrefStore()
        val (model, _) = model(listOf(ava), prefs)
        assertEq(SpeechPlaygroundModel.SAMPLES[0].text, model.text)
        model.edit("  Hello there  ")
        assertEq("  Hello there  ", prefs.getString(SpeechPrefs.TEXT))
        assertEq("  Hello there  ", model(listOf(ava), prefs).first.text)
    }

    fun testNothingToSayDoesNothing() {
        val (model, engine) = model(listOf(ava))
        model.edit("   \n")
        expect(!model.canSpeak)
        model.speak()
        assertEq(SpeechPlaygroundModel.State.IDLE, model.state)
        expect(engine.spoken.isEmpty())
        expect(!model(emptyList()).first.canSpeak)
    }

    fun testPreviewTogglesAndHidesTheHighlight() {
        val (model, engine) = model(listOf(ava, zoe))
        model.preview(zoe)
        assertEq(zoe.id, model.previewing)
        assertEq("Hi, I'm Zoe. This is how I sound reading your words aloud.", engine.spoken.last().first)
        engine.onRange?.invoke(0, 2)
        assertEq(null, model.highlight)
        model.preview(zoe)
        assertEq(SpeechPlaygroundModel.State.IDLE, model.state)
        assertEq(null, model.previewing)
        assertEq("Thomas", SpeechPlaygroundModel.previewLine(thomas))
    }

    fun testSampleAndClearStopSpeech() {
        val (model, engine) = model(listOf(ava))
        model.speak()
        model.useSample(SpeechPlaygroundModel.SAMPLES[1])
        assertEq(SpeechPlaygroundModel.State.IDLE, model.state)
        assertEq(SpeechPlaygroundModel.SAMPLES[1].text, model.text)
        model.clear()
        assertEq("", model.text)
        assertEq("", model.summary)
        assertEq(listOf("speak", "stop"), engine.calls)
    }

    fun testSummary() {
        val (model, _) = model(listOf(ava))
        model.edit("one")
        assertEq("1 word · about 1 s", model.summary)
        model.edit("word ".repeat(330))
        assertEq("330 words · about 2 min 0 s", model.summary)
    }

    fun testTuningPersistsAndClamps() {
        val prefs = MemoryPrefStore()
        val (model, _) = model(listOf(ava), prefs)
        model.speed = 1.5
        model.pitch = 0.75
        model.tuningChanged()
        expect(model.tuned)
        val (reloaded, _) = model(listOf(ava), prefs)
        assertEq(1.5, reloaded.speed)
        assertEq(0.75, reloaded.pitch)
        prefs.putDouble(SpeechPrefs.SPEED, 7.0)
        assertEq(2.0, model(listOf(ava), prefs).first.speed)
        reloaded.resetTuning()
        assertEq(1.0, reloaded.speed)
        assertEq(1.0, prefs.getDouble(SpeechPrefs.PITCH))
        expect(!reloaded.tuned)
    }

    fun testVoiceHint() {
        expect(model(listOf(standard, eddy)).first.showsVoiceHint)
        expect(!model(listOf(ava)).first.showsVoiceHint)
        expect(!model(emptyList()).first.showsVoiceHint)
    }

    fun testRenderAudio() {
        val (model, engine) = model(listOf(ava))
        model.edit("Save this line")
        assertEq("save-this-line.wav", model.renderAudio()?.name)
        expect(!model.rendering)
        engine.renderFails = true
        assertEq(null, model.renderAudio())
        assertEq("Could not save the audio: disk full", model.notice?.message)
        assertEq(true, model.notice?.isError)
    }
}
