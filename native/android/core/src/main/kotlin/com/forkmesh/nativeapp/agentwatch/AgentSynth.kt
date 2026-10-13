package com.forkmesh.nativeapp.agentwatch

import kotlin.math.PI
import kotlin.math.ceil
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.sin

const val kWatchSampleRate: Int = 44100

private const val TABLE_SIZE = 4096

private val sineTable = FloatArray(TABLE_SIZE + 1) { i -> sin(2 * PI * i / TABLE_SIZE).toFloat() }

private fun osc(phase: Double): Double {
    val wrapped = phase - floor(phase)
    val x = wrapped * TABLE_SIZE
    val i = x.toInt()
    val f = x - i
    val a = sineTable[i].toDouble()
    return a + (sineTable[i + 1].toDouble() - a) * f
}

fun releaseSeconds(timbre: Timbre): Double = when (timbre) {
    Timbre.bell -> 1.6
    Timbre.pad -> 0.45
    Timbre.glass -> 0.3
    Timbre.wood -> 0.12
    Timbre.chirp -> 0.3
    Timbre.thud -> 0.1
    Timbre.trill -> 0.25
    Timbre.buzz -> 0.15
    Timbre.swell -> 0.6
    Timbre.ping -> 0.2
    Timbre.twang -> 0.45
    Timbre.mallet -> 0.25
}

fun noteLife(note: Note): Double = note.seconds + releaseSeconds(note.timbre)

private class Partial(val ratio: Double, val amp: Double, val decay: Double)

private class InstrumentSpec(
    val partials: List<Partial>,
    val attackSeconds: Double,
    val vibratoHz: Double = 0.0,
    val vibratoDepth: Double = 0.0,
)

private fun parts(vararg p: Double): List<Partial> = (p.indices step 3).map { Partial(p[it], p[it + 1], p[it + 2]) }

private val instrumentSpecs = listOf(
    InstrumentSpec(parts(1.0, 1.0, 5.5, 3.9, 0.30, 13.0, 9.2, 0.10, 22.0), 0.003),
    InstrumentSpec(parts(1.0, 1.0, 3.2, 3.0, 0.40, 5.5, 5.0, 0.15, 8.0), 0.002),
    InstrumentSpec(parts(1.0, 1.0, 4.5, 2.0, 0.55, 7.0, 3.0, 0.30, 9.0, 4.0, 0.18, 12.0, 5.0, 0.10, 15.0), 0.0015),
    InstrumentSpec(parts(1.0, 1.0, 4.0, 2.0, 0.22, 7.0, 3.1, 0.12, 9.0), 0.004),
    InstrumentSpec(parts(1.0, 1.0, 1.7, 2.01, 0.50, 2.6, 2.76, 0.28, 4.0, 5.41, 0.14, 6.5), 0.002),
    InstrumentSpec(parts(1.0, 1.0, 1.0, 2.0, 0.28, 1.4, 3.0, 0.14, 1.8, 4.0, 0.07, 2.2), 0.050, 5.2, 0.004),
)

private data class Shape(
    var decayScale: Double,
    var attackScale: Double,
    var brightness: Double,
    var bendDepth: Double = 0.0,
    var bendRate: Double = 22.0,
    var trillDepth: Double = 0.0,
    var trillHz: Double = 0.0,
    var tremoloDepth: Double = 0.0,
    var tremoloHz: Double = 0.0,
    var swellSeconds: Double = 0.0,
)

private fun shapeFor(timbre: Timbre): Shape = when (timbre) {
    Timbre.bell -> Shape(0.5, 1.0, 1.0)
    Timbre.glass -> Shape(1.1, 0.7, 1.7)
    Timbre.wood -> Shape(2.4, 0.6, 0.8, bendDepth = 0.35, bendRate = 22.0)
    Timbre.pad -> Shape(0.35, 12.0, 0.6)
    Timbre.chirp -> Shape(1.0, 1.0, 1.3, bendDepth = -0.45, bendRate = 16.0)
    Timbre.thud -> Shape(3.2, 0.5, 0.45, bendDepth = 0.9, bendRate = 30.0)
    Timbre.trill -> Shape(0.8, 1.0, 1.1, trillDepth = 0.06, trillHz = 13.0)
    Timbre.buzz -> Shape(1.4, 0.6, 1.9, bendDepth = 0.08, bendRate = 10.0, tremoloDepth = 0.55, tremoloHz = 31.0)
    Timbre.swell -> Shape(0.2, 1.0, 0.9, swellSeconds = 0.35)
    Timbre.ping -> Shape(1.9, 0.5, 0.25)
    Timbre.twang -> Shape(0.9, 0.8, 1.5, bendDepth = 0.12, bendRate = 5.0, trillDepth = 0.012, trillHz = 5.5)
    Timbre.mallet -> Shape(1.0, 1.0, 1.0)
}

private fun addNote(note: Note, rate: Int, left: DoubleArray, right: DoubleArray, offset: Int) {
    val spec = instrumentSpecs[note.instrument.ordinal]
    val shape = shapeFor(note.timbre)
    val bloom = min(max(note.bloom, 0.0), 1.0)
    var stretch = 0.0
    if (note.seed != 0L) {
        shape.brightness *= 0.78 + 0.44 * seedUnit(note.seed, 0)
        shape.decayScale *= 0.8 + 0.4 * seedUnit(note.seed, 1)
        shape.bendDepth += (seedUnit(note.seed, 2) - 0.5) * 0.08
        stretch = (seedUnit(note.seed, 3) - 0.5) * 0.03
    }
    val ratios = ArrayList<Double>()
    val amps = ArrayList<Double>()
    val decays = ArrayList<Double>()
    var norm = 0.0
    spec.partials.forEachIndexed { i, partial ->
        val a = partial.amp * (if (i == 0) 1.0 else shape.brightness.pow(i.toDouble()))
        ratios += partial.ratio * (1.0 + stretch * i)
        amps += a
        decays += partial.decay * shape.decayScale * (1.0 - 0.25 * bloom)
        norm += a
    }
    if (bloom > 0) {
        val extra = 0.16 * bloom
        ratios += listOf(2.0, 3.0)
        amps += listOf(extra, extra * 0.5)
        decays += listOf(3.0 * shape.decayScale, 4.0 * shape.decayScale)
        norm += extra * 1.5
    }
    val low = min(max((260.0 - note.frequency) / 130.0, 0.0), 1.0)
    if (low > 0) {
        ratios += listOf(2.0, 3.0)
        amps += listOf(0.55 * low, 0.30 * low)
        decays += listOf(2.6 * shape.decayScale, 3.4 * shape.decayScale)
        norm += 0.5 * low
    }
    val scale = 1 / max(1.0, 0.55 * norm) * 0.75 * note.gain
    val terms = ratios.size
    val ratioArr = ratios.toDoubleArray()
    val level = amps.toDoubleArray()
    val step = DoubleArray(terms) { exp(-decays[it] / rate) }

    val angle = note.pan * PI / 2
    val gl = cos(angle) * scale
    val gr = sin(angle) * scale
    val life = noteLife(note)
    val release = releaseSeconds(note.timbre)
    val attackSeconds = spec.attackSeconds * shape.attackScale
    val frames = min(ceil(life * rate).toInt(), left.size - offset)
    if (frames <= 0) return
    val f = note.frequency
    val bend = shape.bendDepth
    val bendRate = shape.bendRate
    val bendStep = exp(-bendRate / rate)
    var bendDecay = 1.0
    val trill = if (shape.trillDepth > 0) shape.trillDepth * f / (2.0 * PI * shape.trillHz) else 0.0
    val rateD = rate.toDouble()
    for (i in 0 until frames) {
        val rel = i / rateD
        var phase = rel * f
        if (bend != 0.0) {
            phase = f * (rel + bend * (1.0 - bendDecay) / bendRate)
            bendDecay *= bendStep
        }
        if (spec.vibratoDepth > 0) phase += spec.vibratoDepth * osc(rel * spec.vibratoHz)
        if (trill != 0.0) phase += trill * osc(rel * shape.trillHz)
        var v = 0.0
        for (k in 0 until terms) {
            v += level[k] * osc(phase * ratioArr[k])
            level[k] *= step[k]
        }
        var env = if (rel < attackSeconds) rel / attackSeconds else 1.0
        if (shape.swellSeconds > 0) {
            val rise = min(1.0, rel / shape.swellSeconds)
            env *= rise * rise
        }
        if (shape.tremoloDepth > 0) env *= 1.0 - shape.tremoloDepth * (0.5 + 0.5 * osc(rel * shape.tremoloHz))
        if (rel > note.seconds) env *= max(0.0, 1.0 - (rel - note.seconds) / release)
        val s = v * env
        left[offset + i] += s * gl
        right[offset + i] += s * gr
    }
}

private const val ECHO_SECONDS = 0.45
private const val ECHO_FEEDBACK = 0.34
private const val ECHO_WET = 0.26
private const val ECHO_TAIL = ECHO_SECONDS * 4

private fun softClip(x: Double): Double {
    if (x > -0.7 && x < 0.7) return x
    val e = exp(2 * x)
    return (e - 1) / (e + 1)
}

fun renderNotes(notes: List<Note>, from: Double, rate: Int = kWatchSampleRate, echo: Boolean = true): ShortArray {
    val sounding = notes.filter { !it.isRest }
    if (sounding.isEmpty()) return ShortArray(0)
    var end = from
    for (note in sounding) end = max(end, note.start + noteLife(note))
    if (echo) end += ECHO_TAIL
    val frames = ceil((end - from) * rate).toInt()
    val left = DoubleArray(frames)
    val right = DoubleArray(frames)
    for (note in sounding) {
        val offset = max(0, floor((note.start - from) * rate + 0.5).toInt())
        if (offset < frames) addNote(note, rate, left, right, offset)
    }
    val out = ShortArray(frames * 2)
    val echoFrames = (ECHO_SECONDS * rate).toInt()
    val echoLeft = DoubleArray(if (echo) echoFrames else 0)
    val echoRight = DoubleArray(if (echo) echoFrames else 0)
    var e = 0
    for (i in 0 until frames) {
        var l = left[i]
        var r = right[i]
        if (echo) {
            val wetL = echoLeft[e]
            val wetR = echoRight[e]
            echoLeft[e] = l + wetR * ECHO_FEEDBACK
            echoRight[e] = r + wetL * ECHO_FEEDBACK
            e = (e + 1) % echoFrames
            l += wetL * ECHO_WET
            r += wetR * ECHO_WET
        }
        out[i * 2] = (softClip(l) * 30000).toInt().toShort()
        out[i * 2 + 1] = (softClip(r) * 30000).toInt().toShort()
    }
    return out
}
