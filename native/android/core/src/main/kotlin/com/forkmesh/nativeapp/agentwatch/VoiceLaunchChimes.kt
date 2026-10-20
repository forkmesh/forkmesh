package com.forkmesh.nativeapp.agentwatch

import kotlin.math.pow

enum class VoiceLaunchChime(val slug: String) {
    Summon("summon"),
    Launch("launch"),
    Fizzle("fizzle"),
    ;

    val notes: List<Note>
        get() = when (this) {
            Summon -> {
                val out = mutableListOf(note(0.0, -5, 0.4, 0.05, Timbre.swell, instrument = Instrument.reed))
                for (step in 0 until 6) {
                    out += note(0.04 + step * 0.04, step, 0.1, 0.05 + step * 0.006, Timbre.glass, pan = 0.85 - step * 0.07)
                }
                out += note(0.28, 7, 0.55, 0.09, Timbre.bell, instrument = Instrument.chime)
                out += note(0.28, 2, 0.55, 0.05, Timbre.bell, instrument = Instrument.chime)
                out += note(0.3, 5, 0.6, 0.04, Timbre.pad)
                out
            }
            Launch -> {
                val out = mutableListOf(note(0.0, -7, 0.32, 0.06, Timbre.swell, instrument = Instrument.reed))
                for (step in 0 until 8) {
                    out += note(0.1 + step * 0.028, step * 2 - 3, 0.07, 0.05 + step * 0.004, Timbre.chirp, pan = 0.15 + step * 0.1)
                }
                out += note(0.36, 12, 0.7, 0.1, Timbre.bell)
                out += note(0.36, 7, 0.7, 0.06, Timbre.bell, instrument = Instrument.chime)
                out += note(0.42, 15, 0.5, 0.05, Timbre.ping, pan = 0.8)
                out += note(0.5, 17, 0.4, 0.04, Timbre.ping, pan = 0.9)
                out
            }
            Fizzle -> {
                val out = mutableListOf<Note>()
                for (step in 0 until 4) {
                    out += note(step * 0.06, 4 - step * 2, 0.1, 0.06 - step * 0.008, Timbre.glass, pan = 0.7 - step * 0.1)
                }
                out += note(0.24, -3, 0.35, 0.05, Timbre.pad)
                out
            }
        }

    val seconds: Double get() = notes.maxOf { it.start + it.seconds }

    fun render(rate: Int = kWatchSampleRate): ShortArray = renderNotes(notes, 0.0, rate, echo = this != Summon)

    private fun note(
        at: Double,
        degree: Int,
        seconds: Double,
        gain: Double,
        timbre: Timbre,
        pan: Double = 0.5,
        instrument: Instrument = Instrument.celesta,
    ): Note = Note(start = at, frequency = pitch(degree), seconds = seconds, gain = gain, pan = pan, timbre = timbre, instrument = instrument)

    companion object {
        const val SUMMON_SECONDS = 0.42
        const val LONGEST_LEAD_SECONDS = 0.8
        const val TONIC_HZ = 523.25

        fun pitch(degree: Int, octave: Int = 0): Double {
            val count = kScale.size
            val shift = Math.floorDiv(degree, count) + octave
            val index = Math.floorMod(degree, count)
            return TONIC_HZ * 2.0.pow((12 * shift + kScale[index]) / 12.0)
        }

        fun leadSeconds(held: Boolean, soundOn: Boolean): Double =
            if (held || !soundOn) SUMMON_SECONDS else Summon.seconds.coerceIn(SUMMON_SECONDS, LONGEST_LEAD_SECONDS)
    }
}
