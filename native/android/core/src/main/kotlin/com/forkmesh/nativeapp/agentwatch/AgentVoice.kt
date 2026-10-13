package com.forkmesh.nativeapp.agentwatch

import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.ln
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow

const val kSlotSeconds: Double = 0.3
const val kMaxLeadSeconds: Double = 1.8
const val kTonicHz: Double = 146.832
val kScale: IntArray = intArrayOf(0, 2, 4, 7, 9)
const val kPayloadCap: Int = 20000

enum class Timbre { mallet, bell, glass, wood, pad, chirp, thud, trill, buzz, swell, ping, twang }

enum class Instrument { marimba, celesta, pluck, kalimba, chime, reed }

private val instrumentHues = doubleArrayOf(0.08, 0.55, 0.92, 0.33, 0.72, 0.17)

private const val MASK32 = 0xffffffffL

fun fnv1a(text: String, start: Long = 0x811c9dc5L): Long {
    var hash = start and MASK32
    for (unit in text) {
        hash = hash xor unit.code.toLong()
        hash = (hash * 0x01000193L) and MASK32
    }
    return hash
}

internal fun mul32(a: Long, b: Long): Long = ((a and MASK32) * (b and MASK32)) and MASK32

internal fun dartLog1p(x: Double): Double = ln(1 + x)

private fun clamp01(x: Double): Double = min(max(x, 0.0), 1.0)

data class Voice(
    val index: Int = 0,
    val instrument: Instrument = Instrument.marimba,
    val octave: Int = 0,
    val degreeOffset: Int = 0,
    var pan: Double = 0.5,
    val slotOffset: Double = 0.0,
    val detune: Double = 1.0,
    var drift: Int = 0,
    var bloom: Double = 0.0,
) {
    val home: Int get() = degreeOffset + drift

    val hue: Double
        get() {
            val h = instrumentHues[instrument.ordinal] + (degreeOffset - 2) * 0.018
            return h - floor(h)
        }

    val sharpness: Double
        get() = when (instrument) {
            Instrument.celesta -> 0.85
            Instrument.chime -> 0.7
            Instrument.pluck -> 0.6
            Instrument.marimba -> 0.45
            Instrument.kalimba -> 0.3
            Instrument.reed -> 0.2
        }

    fun applyUsage(turns: Int, tokens: Int) {
        drift = min(max(max(0, turns) / 6, 0), 5)
        val burned = max(0, tokens).toDouble()
        bloom = min(max(dartLog1p(burned) / dartLog1p(400_000.0), 0.0), 1.0)
    }

    val description: String
        get() {
            val names = listOf("marimba", "celesta", "plucked string", "kalimba", "chime", "reed")
            val where = when {
                pan < 0.2 -> "hard left"
                pan < 0.45 -> "left"
                pan > 0.8 -> "hard right"
                pan > 0.55 -> "right"
                else -> "centre"
            }
            val register = if (octave == 0) "low" else if (octave == 1) "middle" else "high"
            return "${names[instrument.ordinal]}, $register register, $where"
        }

    fun frequency(degree: Int): Double {
        val d = degree + home
        val octaves = floor(d / 5.0).toInt()
        val step = d - octaves * 5
        val semitones = 12 * (octave + octaves) + kScale[step]
        return kTonicHz * 2.0.pow(semitones / 12.0) * detune
    }

    companion object {
        fun forAgent(id: Int, title: String, salt: Int = 0): Voice {
            var hash = fnv1a(title.trim().lowercase())
            hash = hash xor mul32(id.toLong() and MASK32, 2_654_435_761L)
            hash = hash xor mul32(salt.toLong() and MASK32, 0x9e3779b9L)
            hash = hash xor (hash ushr 15)
            hash = mul32(hash, 0x2545f491L)
            hash = hash xor (hash ushr 13)
            fun draw(sides: Long): Int {
                hash = hash xor ((hash shl 13) and MASK32)
                hash = hash xor (hash ushr 17)
                hash = hash xor ((hash shl 5) and MASK32)
                return (hash % sides).toInt()
            }
            val pans = doubleArrayOf(0.5, 0.26, 0.74, 0.14, 0.86)
            val cents = doubleArrayOf(0.0, 5.0, -5.0, 9.0, -9.0)
            val instrument = Instrument.entries[draw(6)]
            val octave = draw(3)
            val pan = pans[draw(5)]
            val degree = draw(5)
            val slotOffset = if (draw(2) != 0) 0.5 else 0.0
            val detune = 2.0.pow(cents[draw(5)] / 1200.0)
            return Voice(id, instrument, octave, degree, pan, slotOffset, detune)
        }
    }
}

fun desktopVoice(): Voice = Voice(index = -2, instrument = Instrument.chime, octave = 1, pan = 0.5, slotOffset = 0.25)

data class Note(
    val start: Double,
    val frequency: Double,
    val seconds: Double = 0.3,
    val gain: Double = 0.2,
    val pan: Double = 0.5,
    val timbre: Timbre = Timbre.mallet,
    val instrument: Instrument = Instrument.marimba,
    val bloom: Double = 0.0,
    val seed: Long = 0,
) {
    val isRest: Boolean get() = frequency <= 0 || seconds <= 0 || gain <= 0

    companion object {
        val rest = Note(start = 0.0, frequency = 0.0)
    }
}

fun seedUnit(seed: Long, index: Int): Double = ((seed shr (8 * (index and 3))) and 0xff).toDouble() / 255.0

data class Melody(
    var degree: Int = 0,
    var lastSlot: Double = -1.0,
    var lastGrace: Double = -1.0,
    var count: Int = 0,
    var graceCount: Int = 0,
    var graceRun: Int = 0,
)

fun stir(count: Int, bytes: Int): Long {
    var h = mul32(count.toLong() and MASK32, 2_654_435_761L) xor mul32(bytes.toLong() and MASK32, 0x9e3779b9L)
    h = h xor (h ushr 15)
    h = mul32(h, 0x2545f491L)
    h = h xor (h ushr 13)
    return h
}

fun stepForMotion(motion: AgentMotion, bytes: Int, count: Int = 0): Int {
    val h = stir(count, bytes)
    fun pick(vararg steps: Int): Int = steps[(h % steps.size).toInt()]
    val odd = (h % 2).toInt()
    return when (motion) {
        AgentMotion.speak -> pick(1, -1, 2, -1, 1, -2, 3, -2)
        AgentMotion.think -> pick(-1, 1, -2, -1, 2, -1)
        AgentMotion.read -> pick(1, 2, 1, 3)
        AgentMotion.search -> pick(2, 3, 2, 4)
        AgentMotion.web -> pick(4, 3, 5, 3)
        AgentMotion.plan -> pick(2, -1, 1, -1)
        AgentMotion.write -> pick(2, 1, 3, 2)
        AgentMotion.run -> pick(1, 1, 2, 3)
        AgentMotion.delegate -> pick(3, 4, 2)
        AgentMotion.arrive -> (if (bytes > 8000) -3 else if (bytes > 800) -2 else -1) - odd
        AgentMotion.notice -> pick(0, 1, -1)
        AgentMotion.scan -> pick(1, 2, 1, 2)
        AgentMotion.create -> pick(3, 2, 4, 2)
        AgentMotion.test -> pick(1, -1, 2, -1)
        AgentMotion.build -> pick(-1, 1, -2, 1)
        AgentMotion.git -> pick(2, -2, 3, -1)
        AgentMotion.install -> pick(1, 1, 1, 2)
        AgentMotion.spawn -> pick(4, 5, 3)
        AgentMotion.wait -> pick(0, 1, 0, -1)
        AgentMotion.ask -> pick(3, 4)
        AgentMotion.skill -> pick(3, 2, 4)
        AgentMotion.mcp -> pick(2, 3, 5)
        AgentMotion.todo -> pick(1, -1, 2, -1)
        AgentMotion.fumble -> pick(-2, -3)
        AgentMotion.denied -> pick(-3, -4)
        AgentMotion.image -> pick(-1, -2, 2)
        AgentMotion.compact -> -4 - odd
        AgentMotion.retry -> pick(0, 1, 0)
        else -> 0
    }
}

fun timbreForMotion(motion: AgentMotion): Timbre = when (motion) {
    AgentMotion.think -> Timbre.pad
    AgentMotion.read, AgentMotion.search, AgentMotion.web, AgentMotion.notice, AgentMotion.compose,
    AgentMotion.tick, AgentMotion.network, AgentMotion.mcp, AgentMotion.image, AgentMotion.logSync,
    AgentMotion.logInfo -> Timbre.glass
    AgentMotion.run, AgentMotion.frame, AgentMotion.close, AgentMotion.task, AgentMotion.logTask -> Timbre.wood
    AgentMotion.usage, AgentMotion.plan -> Timbre.mallet
    AgentMotion.system, AgentMotion.write, AgentMotion.delegate, AgentMotion.prompt, AgentMotion.start,
    AgentMotion.finish, AgentMotion.ask, AgentMotion.logAccount -> Timbre.bell
    AgentMotion.scan, AgentMotion.seal, AgentMotion.logCloud -> Timbre.ping
    AgentMotion.create, AgentMotion.spawn, AgentMotion.connect -> Timbre.chirp
    AgentMotion.test, AgentMotion.retry -> Timbre.trill
    AgentMotion.build, AgentMotion.install -> Timbre.thud
    AgentMotion.git, AgentMotion.skill, AgentMotion.hook, AgentMotion.logGit -> Timbre.twang
    AgentMotion.wait, AgentMotion.compact, AgentMotion.logStall -> Timbre.swell
    AgentMotion.fumble, AgentMotion.denied, AgentMotion.fail, AgentMotion.logError -> Timbre.buzz
    AgentMotion.todo, AgentMotion.speak, AgentMotion.arrive -> Timbre.mallet
}

fun baseGainForMotion(motion: AgentMotion): Double = when (motion) {
    AgentMotion.think -> 0.10
    AgentMotion.arrive -> 0.15
    AgentMotion.read, AgentMotion.search -> 0.16
    AgentMotion.web -> 0.17
    AgentMotion.plan -> 0.20
    AgentMotion.speak -> 0.19
    AgentMotion.run -> 0.18
    AgentMotion.notice -> 0.20
    AgentMotion.write, AgentMotion.delegate -> 0.23
    AgentMotion.start -> 0.26
    AgentMotion.finish -> 0.27
    AgentMotion.prompt -> 0.28
    AgentMotion.wait -> 0.12
    AgentMotion.scan -> 0.15
    AgentMotion.install, AgentMotion.todo, AgentMotion.image -> 0.18
    AgentMotion.mcp, AgentMotion.retry -> 0.18
    AgentMotion.test, AgentMotion.spawn, AgentMotion.fumble -> 0.20
    AgentMotion.build, AgentMotion.git, AgentMotion.skill -> 0.21
    AgentMotion.denied, AgentMotion.compact -> 0.22
    AgentMotion.create -> 0.23
    AgentMotion.fail -> 0.25
    AgentMotion.ask -> 0.27
    AgentMotion.frame, AgentMotion.compose, AgentMotion.usage, AgentMotion.system, AgentMotion.tick,
    AgentMotion.close, AgentMotion.seal, AgentMotion.hook, AgentMotion.network -> 0.05
    AgentMotion.connect, AgentMotion.task, AgentMotion.logError, AgentMotion.logStall, AgentMotion.logSync,
    AgentMotion.logGit, AgentMotion.logCloud, AgentMotion.logTask, AgentMotion.logAccount,
    AgentMotion.logInfo -> 0.06
}

fun secondsForTimbre(timbre: Timbre): Double = when (timbre) {
    Timbre.bell -> 0.9
    Timbre.pad -> 0.8
    Timbre.glass -> 0.4
    Timbre.wood -> 0.18
    Timbre.chirp -> 0.3
    Timbre.thud -> 0.16
    Timbre.trill -> 0.45
    Timbre.buzz -> 0.3
    Timbre.swell -> 0.7
    Timbre.ping -> 0.12
    Timbre.twang -> 0.5
    Timbre.mallet -> 0.34
}

fun graceNote(motion: AgentMotion, bytes: Int, voice: Voice, melody: Melody, now: Double): Note {
    val graceSeconds = kSlotSeconds / 4
    var grace = ceil(now / graceSeconds - 1e-9)
    if (grace <= melody.lastGrace) grace = melody.lastGrace + 1
    val start = grace * graceSeconds
    if (start - now > kMaxLeadSeconds) return Note.rest
    melody.graceRun = if (grace <= melody.lastGrace + 1.5) melody.graceRun + 1 else 0
    melody.lastGrace = grace
    melody.graceCount += 1
    val h = stir(melody.graceCount, bytes)
    val h3 = (h % 3).toInt()
    val h2 = (h % 2).toInt()

    val tick = motion.isFaint
    var seconds = if (tick) 0.04 + 0.03 * h3 / 2.0 else 0.1
    val shimmer = intArrayOf(5, 7, 4, 8, 6, 9, 3)
    var offset = if (tick) shimmer[(h % 7).toInt()] else stepForMotion(motion, bytes, melody.count)
    var gainScale = 1.0
    when (motion) {
        AgentMotion.frame -> { offset = 7; seconds = 0.03; gainScale = 0.8 }
        AgentMotion.close -> { offset = 5; seconds = 0.03; gainScale = 0.75 }
        AgentMotion.seal -> { offset = 9 + h2; seconds = 0.025; gainScale = 0.9 }
        AgentMotion.hook -> { offset = 2 + h3; seconds = 0.1 }
        AgentMotion.compose -> { offset = 3 + melody.graceRun % 5; seconds = 0.035 }
        AgentMotion.usage -> { offset = -3 - h2; seconds = 0.07; gainScale = 1.1 }
        AgentMotion.system -> { offset = 4 + h2 * 5; seconds = 0.12 }
        else -> {}
    }
    val weight = dartLog1p(bytes.toDouble()) / dartLog1p(kPayloadCap.toDouble())
    val thinning = max(0.3, 1.0 / (1.0 + 0.3 * melody.graceRun))
    val gain = if (tick) (0.035 + 0.02 * weight) * gainScale else baseGainForMotion(motion) * (0.3 + 0.12 * weight)
    return Note(
        start = start,
        frequency = voice.frequency(melody.degree + offset),
        seconds = seconds,
        gain = gain * thinning,
        pan = clamp01(voice.pan + ((h % 5).toDouble() - 2.0) * 0.03),
        timbre = timbreForMotion(motion),
        instrument = voice.instrument,
        bloom = voice.bloom,
    )
}

fun noteForBeat(
    motion: AgentMotion,
    voice: Voice,
    melody: Melody,
    now: Double,
    bytes: Int = 0,
    seed: Long = 0,
): Note {
    if (motion.isBookkeeping) return graceNote(motion, bytes, voice, melody, now).copy(seed = seed)
    var slot = ceil(now / kSlotSeconds - 1e-9)
    val minSlot = melody.lastSlot + 1
    if (slot < minSlot) slot = minSlot
    val start = (slot + voice.slotOffset) * kSlotSeconds
    if (start - now > kMaxLeadSeconds) return graceNote(motion, bytes, voice, melody, now).copy(seed = seed)

    melody.count += 1
    val h = stir(melody.count, bytes)
    var degree = melody.degree
    val root = motion == AgentMotion.start || motion == AgentMotion.prompt ||
        motion == AgentMotion.finish || motion == AgentMotion.fail
    if (root) {
        degree = if (motion == AgentMotion.fail) -2 else 0
    } else {
        degree += stepForMotion(motion, bytes, melody.count + (seed % 11).toInt())
    }
    if (degree > 9) degree -= 5 else if (degree < -2) degree += 5
    if (!root && degree == melody.degree && melody.lastSlot >= 0) {
        degree += if (degree >= 9) -1 else if (degree <= -2) 1 else if ((h and 1L) != 0L) 1 else -1
    }
    val timbre = timbreForMotion(motion)
    var frequency = voice.frequency(degree)
    if (timbre == Timbre.thud) frequency *= 0.5
    val weight = dartLog1p(bytes.toDouble()) / dartLog1p(kPayloadCap.toDouble())
    val touch = 0.88 + 0.24 * (h % 101).toDouble() / 100.0
    melody.degree = degree
    melody.lastSlot = slot
    return Note(
        start = start,
        frequency = frequency,
        seconds = secondsForTimbre(timbre) * (0.85 + 0.3 * seedUnit(seed, 1)),
        gain = baseGainForMotion(motion) * (0.72 + 0.38 * weight) * touch,
        pan = voice.pan,
        timbre = timbre,
        instrument = voice.instrument,
        bloom = voice.bloom,
        seed = seed,
    )
}

fun doneCadence(voice: Voice, success: Boolean, start: Double): List<Note> {
    val step = if (success) 0.18 else 0.22
    val rising = intArrayOf(0, 2, 3, 5)
    val falling = intArrayOf(4, 3, 1, 0)
    val notes = ArrayList<Note>()
    for (i in 0 until 4) {
        notes += Note(
            start = start + i * step,
            frequency = voice.frequency(if (success) rising[i] else falling[i]),
            seconds = 0.5,
            gain = if (success) 0.30 else 0.24,
            pan = voice.pan,
            timbre = Timbre.bell,
            instrument = voice.instrument,
            bloom = voice.bloom,
        )
    }
    for (degree in if (success) intArrayOf(5, 3, 0) else intArrayOf(0, -2)) {
        notes += Note(
            start = start + 4 * step,
            frequency = voice.frequency(degree),
            seconds = 1.8,
            gain = if (success) 0.26 else 0.22,
            pan = voice.pan,
            timbre = Timbre.bell,
            instrument = voice.instrument,
            bloom = voice.bloom,
        )
    }
    return notes
}

fun announce(voice: Voice, melody: Melody, now: Double): List<Note> {
    val slot = ceil(now / kSlotSeconds) + 1
    melody.lastSlot = slot + 1
    return (0 until 2).map { i ->
        Note(
            start = (slot + i + voice.slotOffset) * kSlotSeconds,
            frequency = voice.frequency(if (i == 0) 0 else 3),
            seconds = 0.45,
            gain = 0.22,
            pan = voice.pan,
            timbre = Timbre.bell,
            instrument = voice.instrument,
        )
    }
}
