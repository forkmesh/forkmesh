package com.forkmesh.nativeapp.ui

import com.forkmesh.nativeapp.agentwatch.Instrument
import com.forkmesh.nativeapp.agentwatch.Note
import com.forkmesh.nativeapp.agentwatch.Timbre
import com.forkmesh.nativeapp.agentwatch.Voice
import com.forkmesh.nativeapp.agentwatch.kWatchSampleRate
import com.forkmesh.nativeapp.agentwatch.renderNotes
import com.forkmesh.nativeapp.api.PrefStore
import java.io.ByteArrayOutputStream

enum class SoundCue(val slug: String, val weight: Int) {
    KEYSTROKE("keystroke", 0),
    CLICK("click", 3),
    TOGGLE_ON("toggle-on", 4),
    TOGGLE_OFF("toggle-off", 4),
    SELECT("select", 1),
    NAVIGATE("navigate", 5),
    OPEN("open", 6),
    CLOSE("close", 2),
    SEND("send", 8),
    ARRIVE("arrive", 7),
    SUCCESS("success", 10),
    WARN("warn", 9),
    ERROR("error", 12),
    DELETE("delete", 11),
    ;

    companion object {
        private val destroys = setOf("delete", "remove", "discard", "uninstall", "trash", "erase", "purge")
        private val sends = setOf("send", "post", "submit", "publish", "push")

        fun toggle(on: Boolean): SoundCue = if (on) TOGGLE_ON else TOGGLE_OFF

        fun forLabel(label: String, fallback: SoundCue): SoundCue {
            val text = label.trim()
            if (text.endsWith("…") || text.endsWith("...")) return fallback
            val word = text.replace("&", "").split(' ').firstOrNull { it.isNotEmpty() }?.lowercase().orEmpty()
            return when (word) {
                in destroys -> DELETE
                in sends -> SEND
                else -> fallback
            }
        }
    }
}

object SoundCuePrefs {
    const val SOUNDS_KEY = "device/sounds"

    fun enabled(prefs: PrefStore?): Boolean = prefs?.getBool(SOUNDS_KEY) ?: true

    fun setEnabled(prefs: PrefStore, on: Boolean) {
        prefs.putBool(SOUNDS_KEY, on)
    }

    fun shouldPlay(prefs: PrefStore?, cue: SoundCue): SoundCue? = cue.takeIf { enabled(prefs) }
}

object SoundCueSynth {
    const val THROTTLE_MS = 45L
    const val KEYSTROKE_VARIANTS = 6

    private class Step(val degree: Int, val delay: Double, val seconds: Double, val gain: Double, val timbre: Timbre)

    fun voice(): Voice = Voice(index = -3, instrument = Instrument.celesta, octave = 2, degreeOffset = 0, pan = 0.5, slotOffset = 0.0, detune = 1.0)

    private fun steps(cue: SoundCue): List<Step> = when (cue) {
        SoundCue.KEYSTROKE -> listOf(Step(1, 0.0, 0.022, 0.04, Timbre.wood))
        SoundCue.CLICK -> listOf(Step(0, 0.0, 0.05, 0.085, Timbre.mallet))
        SoundCue.TOGGLE_ON -> listOf(Step(0, 0.0, 0.05, 0.075, Timbre.mallet), Step(2, 0.045, 0.07, 0.085, Timbre.glass))
        SoundCue.TOGGLE_OFF -> listOf(Step(2, 0.0, 0.05, 0.075, Timbre.glass), Step(0, 0.045, 0.07, 0.08, Timbre.mallet))
        SoundCue.SELECT -> listOf(Step(-1, 0.0, 0.035, 0.05, Timbre.wood))
        SoundCue.NAVIGATE -> listOf(Step(3, 0.0, 0.07, 0.09, Timbre.glass))
        SoundCue.OPEN -> listOf(
            Step(0, 0.0, 0.06, 0.07, Timbre.glass),
            Step(2, 0.04, 0.06, 0.075, Timbre.glass),
            Step(4, 0.08, 0.13, 0.085, Timbre.bell),
        )
        SoundCue.CLOSE -> listOf(
            Step(4, 0.0, 0.05, 0.075, Timbre.glass),
            Step(2, 0.04, 0.05, 0.07, Timbre.glass),
            Step(0, 0.08, 0.11, 0.07, Timbre.mallet),
        )
        SoundCue.SEND -> listOf(
            Step(0, 0.0, 0.05, 0.085, Timbre.mallet),
            Step(3, 0.05, 0.05, 0.09, Timbre.glass),
            Step(5, 0.1, 0.16, 0.095, Timbre.bell),
        )
        SoundCue.ARRIVE -> listOf(Step(5, 0.0, 0.06, 0.085, Timbre.bell), Step(2, 0.06, 0.18, 0.09, Timbre.bell))
        SoundCue.SUCCESS -> listOf(
            Step(0, 0.0, 0.09, 0.1, Timbre.bell),
            Step(3, 0.09, 0.09, 0.105, Timbre.bell),
            Step(5, 0.18, 0.45, 0.115, Timbre.bell),
            Step(10, 0.18, 0.45, 0.06, Timbre.bell),
        )
        SoundCue.WARN -> listOf(Step(-2, 0.0, 0.3, 0.1, Timbre.pad), Step(-2, 0.16, 0.26, 0.075, Timbre.pad))
        SoundCue.ERROR -> listOf(
            Step(-1, 0.0, 0.13, 0.13, Timbre.wood),
            Step(-4, 0.11, 0.42, 0.13, Timbre.pad),
            Step(-6, 0.11, 0.42, 0.08, Timbre.pad),
        )
        SoundCue.DELETE -> listOf(Step(2, 0.0, 0.05, 0.09, Timbre.wood), Step(-3, 0.055, 0.2, 0.1, Timbre.wood))
    }

    fun notes(cue: SoundCue, start: Double, volume: Double = 1.0, seed: Long = 0): List<Note> {
        val voice = voice()
        val level = volume.coerceIn(0.0, 1.0)
        return steps(cue).map { step ->
            Note(
                start = start + step.delay,
                frequency = voice.frequency(step.degree),
                seconds = step.seconds,
                gain = step.gain * level,
                pan = voice.pan,
                timbre = step.timbre,
                instrument = voice.instrument,
                bloom = 0.0,
                seed = seed,
            )
        }.filterNot { it.isRest }
    }

    fun keystrokeSeed(strike: Int): Long = ((strike.toLong() * 2654435761L) and 0xffffffffL) or 1L

    fun render(cue: SoundCue, volume: Double = 1.0, seed: Long = 0, rate: Int = kWatchSampleRate): ShortArray =
        renderNotes(notes(cue, 0.0, volume, seed), 0.0, rate, echo = false)

    fun wav(pcm: ShortArray, rate: Int = kWatchSampleRate, channels: Int = 2): ByteArray {
        val out = ByteArrayOutputStream(44 + pcm.size * 2)
        fun word(value: Int) = repeat(4) { out.write((value ushr (8 * it)) and 0xff) }
        fun half(value: Int) = repeat(2) { out.write((value ushr (8 * it)) and 0xff) }
        val bytes = pcm.size * 2
        out.write("RIFF".toByteArray())
        word(36 + bytes)
        out.write("WAVEfmt ".toByteArray())
        word(16)
        half(1)
        half(channels)
        word(rate)
        word(rate * channels * 2)
        half(channels * 2)
        half(16)
        out.write("data".toByteArray())
        word(bytes)
        for (sample in pcm) half(sample.toInt())
        return out.toByteArray()
    }
}

enum class RatchetPawl(val snap: Double, val ring: Double, val partials: List<Pair<Double, Double>>) {
    Soft(0.05, 0.0008, listOf(2150.0 to 0.05, 3420.0 to 0.032)),
    Crisp(0.1, 0.0012, listOf(3150.0 to 0.07, 4870.0 to 0.05, 7330.0 to 0.03)),
}

data class RatchetBurst(val teeth: Int, val pawl: RatchetPawl) {
    val spacing: Double get() = BATCH_MS / 1000.0 / teeth

    fun render(seed: Long = 0, rate: Int = kWatchSampleRate): ShortArray {
        val seconds = (teeth - 1) * spacing + TOOTH_SECONDS
        val mono = DoubleArray(Math.ceil(seconds * rate).toInt())
        for (tooth in 0 until teeth) {
            ratchetTooth(mono, Math.round(tooth * spacing * rate).toInt(), pawl, seed + tooth * 7919L, rate)
        }
        return ratchetStereo(mono)
    }

    val slug: String get() = "ratchet-${pawl.name.lowercase()}-$teeth"

    companion object {
        const val BATCH_MS = 16L
        const val MAX_TEETH = 8
        const val VARIANTS = 2
        const val TOOTH_SECONDS = 0.008
        const val CATCH_SECONDS = 0.06

        fun of(teeth: Int, pawl: RatchetPawl): RatchetBurst = RatchetBurst(teeth.coerceIn(1, MAX_TEETH), pawl)

        val all: List<RatchetBurst> by lazy {
            RatchetPawl.entries.flatMap { pawl -> (1..MAX_TEETH).map { RatchetBurst(it, pawl) } }
        }

        fun renderCatch(seed: Long = 0, rate: Int = kWatchSampleRate): ShortArray {
            val mono = DoubleArray(Math.ceil(CATCH_SECONDS * rate).toInt())
            for (i in mono.indices) {
                val t = i.toDouble() / rate
                mono[i] = 0.12 * Math.sin(2 * Math.PI * 380 * t) * Math.exp(-t / 0.012) +
                    0.05 * Math.sin(2 * Math.PI * 1140 * t) * Math.exp(-t / 0.005)
            }
            ratchetTooth(mono, 0, RatchetPawl.Crisp, seed, rate)
            return ratchetStereo(mono)
        }
    }
}

data class RatchetStroke(val burst: RatchetBurst, val caught: Boolean)

class RatchetGate {
    var isOpen = false
        private set
    private var lines = 0
    private var changed = 0
    private var flagged = 0

    fun pass(lines: Int, changed: Int, flagged: Int): RatchetStroke? {
        if (lines <= 0) return null
        if (isOpen) {
            this.lines += lines
            this.changed += changed
            this.flagged += flagged
            return null
        }
        isOpen = true
        return stroke(lines, changed, flagged)
    }

    fun close(): RatchetStroke? {
        val stroke = stroke(lines, changed, flagged)
        lines = 0
        changed = 0
        flagged = 0
        isOpen = stroke != null
        return stroke
    }

    private fun stroke(lines: Int, changed: Int, flagged: Int): RatchetStroke? {
        if (lines <= 0) return null
        val pawl = if (changed > 0 || flagged > 0) RatchetPawl.Crisp else RatchetPawl.Soft
        return RatchetStroke(RatchetBurst.of(lines, pawl), flagged > 0)
    }
}

private class RatchetNoise(seed: Long) {
    private var state: Int = (seed.toInt() xor 0x9E3779B9.toInt()).let { if (it == 0) 1 else it }

    fun next(): Double {
        state = state xor (state shl 13)
        state = state xor (state ushr 17)
        state = state xor (state shl 5)
        return (state.toLong() and 0xffffffffL).toDouble() / 0xffffffffL.toDouble()
    }
}

private fun ratchetTooth(mono: DoubleArray, start: Int, pawl: RatchetPawl, seed: Long, rate: Int) {
    val noise = RatchetNoise(seed)
    val detune = 1 + (noise.next() - 0.5) * 0.06
    val level = 0.9 + noise.next() * 0.2
    val length = minOf(mono.size - start, (RatchetBurst.TOOTH_SECONDS * rate).toInt())
    if (start < 0 || length <= 0) return
    for (i in 0 until length) {
        val t = i.toDouble() / rate
        var x = pawl.snap * (noise.next() * 2 - 1) * Math.exp(-t / 0.00025)
        for ((hz, gain) in pawl.partials) x += gain * Math.sin(2 * Math.PI * hz * detune * t) * Math.exp(-t / pawl.ring)
        mono[start + i] += x * level
    }
}

private fun ratchetStereo(mono: DoubleArray): ShortArray {
    val out = ShortArray(mono.size * 2)
    for (i in mono.indices) {
        val sample = Math.round(mono[i].coerceIn(-1.5, 1.5) * 30000).coerceIn(Short.MIN_VALUE.toLong(), Short.MAX_VALUE.toLong()).toInt().toShort()
        out[i * 2] = sample
        out[i * 2 + 1] = sample
    }
    return out
}

class SoundCueGate {
    private var lastMs: Long? = null
    private var last: SoundCue = SoundCue.SELECT

    fun admit(cue: SoundCue, atMs: Long): Boolean {
        val previous = lastMs
        if (previous != null && atMs - previous < SoundCueSynth.THROTTLE_MS && cue.weight <= last.weight) return false
        lastMs = atMs
        last = cue
        return true
    }
}

class SoundCueBatch {
    var held: SoundCue? = null
        private set

    fun hold(cue: SoundCue): Boolean {
        val current = held
        if (current == null) {
            held = cue
            return true
        }
        if (cue.weight > current.weight) held = cue
        return false
    }

    fun take(): SoundCue? = held.also { held = null }
}

class SoundCueFallback {
    @Volatile private var lastMs = Long.MIN_VALUE

    fun heard(atMs: Long) {
        lastMs = atMs
    }

    fun silentSince(atMs: Long): Boolean = lastMs < atMs

    companion object {
        const val DELAY_MS = 80L
    }
}
