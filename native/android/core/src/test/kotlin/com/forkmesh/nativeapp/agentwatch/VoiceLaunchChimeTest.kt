package com.forkmesh.nativeapp.agentwatch

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class VoiceLaunchChimeTest {
    fun testEveryChimeSoundsBrieflyAndStaysInRange() {
        for (chime in VoiceLaunchChime.entries) {
            val notes = chime.notes
            expect(notes.isNotEmpty(), chime.slug)
            expect(notes.maxOf { it.start + it.seconds } < 1.2, chime.slug)
            expect(notes.all { it.gain > 0 && it.gain <= 0.12 }, chime.slug)
            val pcm = chime.render()
            expect(pcm.isNotEmpty(), chime.slug)
            expect(pcm.any { it.toInt() != 0 }, chime.slug)
        }
    }

    fun testSummonRisesAndFizzleFalls() {
        val up = VoiceLaunchChime.Summon.notes.drop(1).take(6).map { it.frequency }
        assertEq(up.sorted(), up)
        val down = VoiceLaunchChime.Fizzle.notes.take(4).map { it.frequency }
        assertEq(down.sortedDescending(), down)
    }

    fun testLaunchClimbsIntoItsBell() {
        val run = VoiceLaunchChime.Launch.notes.drop(1).take(8).map { it.frequency }
        assertEq(run.sorted(), run)
        expect(VoiceLaunchChime.Launch.notes[9].frequency > run.last())
    }

    fun testSummonBellRingsBeforeTheMicrophoneOpens() {
        val bell = VoiceLaunchChime.Summon.notes.filter { it.timbre == Timbre.bell }.minOf { it.start }
        expect(bell < VoiceLaunchChime.SUMMON_SECONDS)
    }

    fun testChimesAreDistinct() {
        val all = VoiceLaunchChime.entries
        for ((index, chime) in all.withIndex()) {
            for (other in all.take(index)) expect(chime.notes != other.notes, "${chime.slug} ${other.slug}")
        }
    }

    fun testPitchWalksTheScaleAcrossOctaves() {
        assertEq(VoiceLaunchChime.TONIC_HZ, VoiceLaunchChime.pitch(0))
        assertEq(VoiceLaunchChime.TONIC_HZ * 2, VoiceLaunchChime.pitch(5), "one octave up")
        assertEq(VoiceLaunchChime.TONIC_HZ / 2, VoiceLaunchChime.pitch(-5), "one octave down")
        expect(VoiceLaunchChime.pitch(-1) < VoiceLaunchChime.TONIC_HZ)
        assertEq(VoiceLaunchChime.pitch(5), VoiceLaunchChime.pitch(0, octave = 1))
    }

    fun testTheMicrophoneWaitsForTheChimeOnlyHandsFree() {
        assertEq(VoiceLaunchChime.SUMMON_SECONDS, VoiceLaunchChime.leadSeconds(held = true, soundOn = true))
        assertEq(VoiceLaunchChime.SUMMON_SECONDS, VoiceLaunchChime.leadSeconds(held = false, soundOn = false))
        val handsFree = VoiceLaunchChime.leadSeconds(held = false, soundOn = true)
        expect(handsFree >= VoiceLaunchChime.SUMMON_SECONDS && handsFree <= VoiceLaunchChime.LONGEST_LEAD_SECONDS)
    }
}
