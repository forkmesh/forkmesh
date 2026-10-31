package com.forkmesh.nativeapp.tools

import com.forkmesh.nativeapp.api.PrefStore
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.Locale
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.roundToInt

enum class SpeechVoiceQuality(val rank: Int, val label: String) {
    STANDARD(1, "Standard"),
    ENHANCED(2, "Enhanced"),
    PREMIUM(3, "Premium");

    companion object {
        fun ofAndroid(quality: Int): SpeechVoiceQuality = when {
            quality >= 500 -> PREMIUM
            quality >= 400 -> ENHANCED
            else -> STANDARD
        }
    }
}

data class SpeechVoice(
    val id: String,
    val name: String,
    val language: String,
    val quality: SpeechVoiceQuality,
    val online: Boolean = false,
)

data class SpeechVoiceSection(val id: String, val title: String, val voices: List<SpeechVoice>)

data class SpeechSample(val title: String, val text: String)

data class SpeechHighlight(val before: String, val word: String, val after: String)

data class SpeechNotice(val message: String, val isError: Boolean = false)

data class SpeechChunk(val offset: Int, val text: String)

data class SpeechUtterance(val session: Int, val offset: Int, val last: Boolean) {
    val id: String get() = "speech-$session-$offset-${if (last) 1 else 0}"

    companion object {
        fun parse(id: String?): SpeechUtterance? {
            val parts = id?.split('-') ?: return null
            if (parts.size != 4 || parts[0] != "speech") return null
            val session = parts[1].toIntOrNull() ?: return null
            val offset = parts[2].toIntOrNull()?.takeIf { it >= 0 } ?: return null
            val last = when (parts[3]) {
                "1" -> true
                "0" -> false
                else -> return null
            }
            return SpeechUtterance(session, offset, last)
        }
    }
}

enum class SpeechEngineStatus { STARTING, READY, FAILED }

object SpeechVoices {
    private val googleVoice = Regex("^[a-z]{2,3}(-[a-z0-9]{2,4})?-x-([a-z0-9]+)-(local|network)$", RegexOption.IGNORE_CASE)
    private val languageVoice = Regex("^[a-z]{2,3}(-[a-z0-9]{2,4})?-language$", RegexOption.IGNORE_CASE)

    fun normalized(language: String): String = language.replace('_', '-').lowercase()

    fun base(language: String): String = normalized(language).split('-').firstOrNull().orEmpty()

    fun languageMatch(voice: String, preferred: String): Int {
        val lhs = normalized(voice)
        val rhs = normalized(preferred)
        if (lhs == rhs) return 2
        return if (base(lhs) == base(rhs)) 1 else 0
    }

    fun displayName(raw: String): String {
        googleVoice.matchEntire(raw)?.let { return "Voice " + it.groupValues[2].uppercase() }
        if (languageVoice.matches(raw)) return "Default voice"
        return raw
    }

    private fun rank(voice: SpeechVoice, language: String): List<Int> = listOf(
        if (languageMatch(voice.language, language) > 0) 1 else 0,
        voice.quality.rank,
        languageMatch(voice.language, language),
        if (voice.online) 0 else 1,
    )

    private fun compareRanks(left: List<Int>, right: List<Int>): Int {
        for (index in left.indices) {
            val diff = left[index].compareTo(right[index])
            if (diff != 0) return diff
        }
        return 0
    }

    fun best(voices: List<SpeechVoice>, language: String): SpeechVoice? = voices.maxWithOrNull { lhs, rhs ->
        val ranked = compareRanks(rank(lhs, language), rank(rhs, language))
        if (ranked != 0) ranked else rhs.name.compareTo(lhs.name)
    }

    fun hasNatural(voices: List<SpeechVoice>, language: String): Boolean =
        voices.any { it.quality >= SpeechVoiceQuality.ENHANCED && languageMatch(it.language, language) > 0 }

    fun sections(voices: List<SpeechVoice>, language: String, title: (String) -> String): List<SpeechVoiceSection> {
        val groups = voices.groupBy { normalized(it.language) }
        val ordered = groups.keys.sortedWith { lhs, rhs ->
            val left = languageMatch(lhs, language)
            val right = languageMatch(rhs, language)
            if (left != right) right.compareTo(left) else title(lhs).compareTo(title(rhs), ignoreCase = true)
        }
        return ordered.map { key ->
            val members = groups[key].orEmpty().sortedWith(
                compareByDescending<SpeechVoice> { it.quality.rank }
                    .thenBy { it.online }
                    .thenBy(String.CASE_INSENSITIVE_ORDER) { it.name },
            )
            SpeechVoiceSection(key, title(members.firstOrNull()?.language ?: key), members)
        }
    }
}

object SpeechTuning {
    const val MIN_SPEED = 0.5
    const val MAX_SPEED = 2.0
    const val MIN_PITCH = 0.5
    const val MAX_PITCH = 2.0
    const val DEFAULT_SPEED = 1.0
    const val DEFAULT_PITCH = 1.0
    const val STEP = 0.05

    fun clampSpeed(speed: Double): Double = speed.coerceIn(MIN_SPEED, MAX_SPEED)

    fun clampPitch(pitch: Double): Double = pitch.coerceIn(MIN_PITCH, MAX_PITCH)

    fun engineRate(speed: Double): Float = clampSpeed(speed).toFloat()

    fun snap(value: Double): Double = (value / STEP).roundToInt() * STEP

    fun speedLabel(speed: Double): String =
        String.format(Locale.ROOT, "%.2f", speed).trimEnd('0').trimEnd('.') + "×"

    fun pitchLabel(pitch: Double): String = when {
        abs(pitch - 1) < 0.025 -> "Natural"
        pitch < 1 -> "Lower"
        else -> "Higher"
    }

    fun estimatedSeconds(text: String, speed: Double): Int {
        val words = SpeechText.wordCount(text)
        if (words == 0) return 0
        return max(1, (words.toDouble() / (165 * max(speed, 0.1)) * 60).roundToInt())
    }
}

object SpeechText {
    const val MAX_LENGTH = 20_000
    const val AUDIO_EXTENSION = "wav"

    private val whitespace = Regex("\\s+")
    private val nonWord = Regex("[^\\p{L}\\p{N}]+")

    fun wordCount(text: String): Int = text.split(whitespace).count { it.isNotEmpty() }

    private fun splitsPair(text: String, index: Int): Boolean =
        index in 1 until text.length && text[index].isLowSurrogate() && text[index - 1].isHighSurrogate()

    fun highlight(text: String, location: Int, length: Int): SpeechHighlight? {
        if (location < 0 || length <= 0 || location + length > text.length) return null
        val end = location + length
        if (splitsPair(text, location) || splitsPair(text, end)) return null
        return SpeechHighlight(text.substring(0, location), text.substring(location, end), text.substring(end))
    }

    fun duration(seconds: Int): String = if (seconds < 60) "$seconds s" else "${seconds / 60} min ${seconds % 60} s"

    fun fileName(text: String): String {
        val stem = text.split(nonWord).filter { it.isNotEmpty() }.take(5).joinToString("-") { it.lowercase() }
        return (if (stem.isEmpty()) "speech" else stem.take(48)) + "." + AUDIO_EXTENSION
    }

    fun chunks(text: String, limit: Int): List<SpeechChunk> {
        if (text.isEmpty()) return emptyList()
        val size = max(limit, 2)
        val out = ArrayList<SpeechChunk>()
        var start = 0
        while (start < text.length) {
            if (text.length - start <= size) {
                out += SpeechChunk(start, text.substring(start))
                break
            }
            val hardEnd = start + size
            var cut = -1
            for (index in hardEnd - 1 downTo start + 1) {
                val char = text[index]
                if (char.isWhitespace() && text[index - 1] in ".!?…;:") {
                    cut = index + 1
                    break
                }
            }
            if (cut < 0) {
                for (index in hardEnd - 1 downTo start + 1) {
                    if (text[index].isWhitespace()) {
                        cut = index + 1
                        break
                    }
                }
            }
            if (cut < 0) cut = if (splitsPair(text, hardEnd)) hardEnd - 1 else hardEnd
            out += SpeechChunk(start, text.substring(start, cut))
            start = cut
        }
        return out
    }
}

class SpeechWavLayout(val format: ByteArray, val dataOffset: Int, val dataSize: Int)

object SpeechWav {
    const val HEAD_BYTES = 4096

    private fun int32(bytes: ByteArray, at: Int): Int =
        (bytes[at].toInt() and 0xff) or ((bytes[at + 1].toInt() and 0xff) shl 8) or
            ((bytes[at + 2].toInt() and 0xff) shl 16) or ((bytes[at + 3].toInt() and 0xff) shl 24)

    private fun tag(bytes: ByteArray, at: Int): String = String(bytes, at, 4, Charsets.US_ASCII)

    fun layout(head: ByteArray, fileSize: Long = head.size.toLong()): SpeechWavLayout {
        require(head.size >= 12 && tag(head, 0) == "RIFF" && tag(head, 8) == "WAVE") { "not a WAV file" }
        var at = 12
        var format: ByteArray? = null
        while (at + 8 <= head.size) {
            val id = tag(head, at)
            val size = int32(head, at + 4)
            val body = at + 8
            if (id == "data") {
                val available = (fileSize - body).coerceIn(0, Int.MAX_VALUE.toLong()).toInt()
                val dataSize = if (size < 0 || size > available) available else size
                return SpeechWavLayout(requireNotNull(format) { "WAV has no format" }, body, dataSize)
            }
            if (size < 0 || body + size > head.size) break
            if (id == "fmt ") format = head.copyOfRange(body, body + size)
            at = body + size + size % 2
        }
        throw IllegalArgumentException("WAV has no data")
    }

    fun header(format: ByteArray, dataSize: Int): ByteArray {
        val pad = format.size % 2
        val out = ByteArrayOutputStream(28 + format.size + pad)
        fun int32(value: Int) {
            out.write(value and 0xff)
            out.write((value shr 8) and 0xff)
            out.write((value shr 16) and 0xff)
            out.write((value shr 24) and 0xff)
        }
        out.write("RIFF".toByteArray(Charsets.US_ASCII))
        int32(4 + 8 + format.size + pad + 8 + dataSize)
        out.write("WAVE".toByteArray(Charsets.US_ASCII))
        out.write("fmt ".toByteArray(Charsets.US_ASCII))
        int32(format.size)
        out.write(format)
        if (pad == 1) out.write(0)
        out.write("data".toByteArray(Charsets.US_ASCII))
        int32(dataSize)
        return out.toByteArray()
    }

    fun requireSameFormat(layouts: List<SpeechWavLayout>) {
        require(layouts.isNotEmpty()) { "nothing to join" }
        val format = layouts[0].format
        require(layouts.all { it.format.contentEquals(format) }) { "WAV formats differ" }
    }

    fun join(parts: List<ByteArray>): ByteArray {
        require(parts.isNotEmpty()) { "nothing to join" }
        if (parts.size == 1) return parts[0]
        val layouts = parts.map { layout(it) }
        requireSameFormat(layouts)
        val out = ByteArrayOutputStream()
        out.write(header(layouts[0].format, layouts.sumOf { it.dataSize }))
        parts.forEachIndexed { index, bytes -> out.write(bytes, layouts[index].dataOffset, layouts[index].dataSize) }
        return out.toByteArray()
    }
}

object SpeechPrefs {
    const val VOICE = "speech.voice"
    const val SPEED = "speech.speed"
    const val PITCH = "speech.pitch"
    const val TEXT = "speech.text"
}

interface SpeechEngine {
    var onRange: ((Int, Int) -> Unit)?
    var onFinish: (() -> Unit)?
    var onStatus: (() -> Unit)?
    val status: SpeechEngineStatus

    fun voices(): List<SpeechVoice>
    fun speak(text: String, voiceId: String, rate: Float, pitch: Float)
    fun pause()
    fun resume()
    fun stop()
    fun render(text: String, voiceId: String, rate: Float, pitch: Float, name: String): File
}

class SpeechPlaygroundModel(
    val engine: SpeechEngine,
    private val prefs: PrefStore? = null,
    val language: String = "en-US",
) {
    enum class State { IDLE, SPEAKING, PAUSED }

    var onChange: (() -> Unit)? = null

    var text: String = prefs?.getString(SpeechPrefs.TEXT) ?: SAMPLES[0].text
    var voices: List<SpeechVoice> = emptyList()
        private set
    var voiceId: String = prefs?.getString(SpeechPrefs.VOICE).orEmpty()
        private set
    var speed: Double = SpeechTuning.clampSpeed(prefs?.getDouble(SpeechPrefs.SPEED) ?: SpeechTuning.DEFAULT_SPEED)
    var pitch: Double = SpeechTuning.clampPitch(prefs?.getDouble(SpeechPrefs.PITCH) ?: SpeechTuning.DEFAULT_PITCH)
    var state: State = State.IDLE
        private set
    var spoken: Pair<Int, Int>? = null
        private set
    var spokenText: String = ""
        private set
    var previewing: String? = null
        private set

    @Volatile
    var rendering: Boolean = false
        private set
    var notice: SpeechNotice? = null

    init {
        engine.onRange = { location, length -> spokenRange(location, length) }
        engine.onFinish = { finished() }
        engine.onStatus = {
            load()
            onChange?.invoke()
        }
    }

    val status: SpeechEngineStatus get() = engine.status

    fun load() {
        voices = engine.voices()
        if (voice == null) SpeechVoices.best(voices, language)?.let { voiceId = it.id }
    }

    val voice: SpeechVoice? get() = voices.firstOrNull { it.id == voiceId }

    val showsVoiceHint: Boolean get() = voices.isNotEmpty() && !SpeechVoices.hasNatural(voices, language)

    val trimmed: String get() = text.trim().take(SpeechText.MAX_LENGTH)

    val canSpeak: Boolean get() = trimmed.isNotEmpty() && voice != null

    val tuned: Boolean get() = speed != SpeechTuning.DEFAULT_SPEED || pitch != SpeechTuning.DEFAULT_PITCH

    val highlight: SpeechHighlight?
        get() {
            val range = spoken ?: return null
            if (previewing != null || state == State.IDLE) return null
            return SpeechText.highlight(spokenText, range.first, range.second)
        }

    val summary: String
        get() {
            val words = SpeechText.wordCount(text)
            if (words == 0) return ""
            val seconds = SpeechTuning.estimatedSeconds(text, speed)
            return "$words word${if (words == 1) "" else "s"} · about ${SpeechText.duration(seconds)}"
        }

    fun select(voice: SpeechVoice) {
        voiceId = voice.id
        prefs?.putString(SpeechPrefs.VOICE, voice.id)
    }

    fun tuningChanged() {
        prefs?.putDouble(SpeechPrefs.SPEED, speed)
        prefs?.putDouble(SpeechPrefs.PITCH, pitch)
    }

    fun resetTuning() {
        speed = SpeechTuning.DEFAULT_SPEED
        pitch = SpeechTuning.DEFAULT_PITCH
        tuningChanged()
    }

    fun edit(value: String) {
        text = value
        prefs?.putString(SpeechPrefs.TEXT, value)
    }

    fun useSample(sample: SpeechSample) {
        stop()
        edit(sample.text)
    }

    fun clear() {
        stop()
        edit("")
    }

    fun toggle() {
        when (state) {
            State.IDLE -> speak()
            State.SPEAKING -> pause()
            State.PAUSED -> resume()
        }
    }

    fun speak() {
        val voice = voice ?: return
        val line = trimmed
        if (line.isEmpty()) return
        if (state != State.IDLE) engine.stop()
        prefs?.putString(SpeechPrefs.TEXT, text)
        previewing = null
        start(line, voice)
    }

    fun preview(voice: SpeechVoice) {
        if (previewing == voice.id && state != State.IDLE) {
            stop()
            return
        }
        if (state != State.IDLE) engine.stop()
        previewing = voice.id
        start(previewLine(voice), voice)
    }

    private fun start(line: String, voice: SpeechVoice) {
        spokenText = line
        spoken = null
        state = State.SPEAKING
        engine.speak(line, voice.id, SpeechTuning.engineRate(speed), pitch.toFloat())
    }

    fun pause() {
        if (state != State.SPEAKING) return
        engine.pause()
        state = State.PAUSED
    }

    fun resume() {
        if (state != State.PAUSED) return
        engine.resume()
        state = State.SPEAKING
    }

    fun stop() {
        if (state == State.IDLE) return
        engine.stop()
        reset()
    }

    private fun reset() {
        state = State.IDLE
        spoken = null
        previewing = null
    }

    fun spokenRange(location: Int, length: Int) {
        if (state == State.IDLE) return
        spoken = location to length
        onChange?.invoke()
    }

    fun finished() {
        reset()
        onChange?.invoke()
    }

    fun renderAudio(): File? {
        val voice = voice ?: return null
        val line = trimmed
        if (line.isEmpty() || rendering) return null
        rendering = true
        return try {
            engine.render(line, voice.id, SpeechTuning.engineRate(speed), pitch.toFloat(), SpeechText.fileName(line))
        } catch (error: Exception) {
            notice = SpeechNotice("Could not save the audio: ${error.message ?: "unknown error"}", isError = true)
            null
        } finally {
            rendering = false
        }
    }

    companion object {
        const val TITLE = "Speech"
        const val UNAVAILABLE_TITLE = "Speech is unavailable"
        const val UNAVAILABLE_MESSAGE = "This phone has no text-to-speech engine. Install one from the Play Store, such as Speech Services by Google, then come back."
        const val PLACEHOLDER = "Type or paste anything, then tap Speak."
        const val NO_NATURAL_VOICE =
            "For the most natural sound, install a high-quality voice: Settings › System › Languages › Text-to-speech output › Preferred engine › Install voice data. It appears here as soon as the download finishes."
        const val GET_VOICES = "Get more voices"
        const val NO_VOICES = "No voices on this device"

        val SAMPLES: List<SpeechSample> = listOf(
            SpeechSample(
                "Hello",
                "Hi there! This is ForkMesh speaking. Type anything you like into the box, pick a voice, and I'll read it back to you.",
            ),
            SpeechSample(
                "Story",
                "The lighthouse keeper climbed the spiral stairs one last time. Below, the sea was calm and silver, and far out on the water a single boat was turning for home.",
            ),
            SpeechSample(
                "Update",
                "Good morning. Three agents finished overnight: two pull requests are ready for review, and one test run needs your attention. Your next meeting starts at 10:30.",
            ),
            SpeechSample(
                "Poem",
                "Shall I compare thee to a summer's day? Thou art more lovely and more temperate. Rough winds do shake the darling buds of May, and summer's lease hath all too short a date.",
            ),
        )

        fun previewLine(voice: SpeechVoice): String {
            if (SpeechVoices.base(voice.language) != "en") return voice.name
            return "Hi, I'm ${voice.name}. This is how I sound reading your words aloud."
        }
    }
}
