package com.forkmesh.nativeapp.composer

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class VoiceLaunchTest {
    fun testLettingGoSendsOnlyWhenSomethingWasHeard() {
        val listening = VoiceLaunchPhase.Listening
        assertEq(VoiceRelease.Send, VoiceLaunchRules.release(listening, held = true, heard = "fix the build", voicing = false))
        assertEq(VoiceRelease.Send, VoiceLaunchRules.release(listening, held = true, heard = "", voicing = true))
        assertEq(VoiceRelease.KeepListening, VoiceLaunchRules.release(listening, held = true, heard = " ", voicing = false))
    }

    fun testLettingGoBeforeTheMicrophoneOpensChangesNothing() {
        assertEq(VoiceRelease.Ignore, VoiceLaunchRules.release(VoiceLaunchPhase.Waking, held = true, heard = "", voicing = false))
        assertEq(VoiceRelease.Ignore, VoiceLaunchRules.release(VoiceLaunchPhase.Listening, held = false, heard = "hi", voicing = true))
        assertEq(VoiceRelease.Ignore, VoiceLaunchRules.release(VoiceLaunchPhase.Sending, held = true, heard = "hi", voicing = true))
    }

    fun testThePauseArcFillsOverThreeQuietSeconds() {
        assertEq(0f, VoiceLaunchRules.quietFill(100, loud = false))
        assertEq(0.5f, VoiceLaunchRules.quietFill(1500, loud = false))
        assertEq(1f, VoiceLaunchRules.quietFill(9000, loud = false))
        assertEq(0f, VoiceLaunchRules.quietFill(2000, loud = true))
    }

    fun testHintsFollowTheModeAndWhatWasHeard() {
        val listening = VoiceLaunchPhase.Listening
        assertEq("Say what the agent should do · let go to send", VoiceLaunchRules.hint(listening, held = true, heard = false))
        assertEq("Let go to send", VoiceLaunchRules.hint(listening, held = true, heard = true))
        assertEq("Say what the agent should do", VoiceLaunchRules.hint(listening, held = false, heard = false))
        assertEq("Pause to send · tap the orb to send now", VoiceLaunchRules.hint(listening, held = false, heard = true))
        assertEq("Starting your agent…", VoiceLaunchRules.hint(VoiceLaunchPhase.Sending, held = false, heard = true))
        assertEq("On its way", VoiceLaunchRules.hint(VoiceLaunchPhase.Sent, held = false, heard = true))
        assertEq(null, VoiceLaunchRules.hint(VoiceLaunchPhase.Missed("x"), held = false, heard = false))
    }

    fun testOnlyAnOpenIdleScreenCanBeCancelled() {
        expect(VoiceLaunchRules.canCancel(VoiceLaunchPhase.Waking))
        expect(VoiceLaunchRules.canCancel(VoiceLaunchPhase.Listening))
        expect(VoiceLaunchRules.canCancel(VoiceLaunchPhase.Missed("x")))
        expect(!VoiceLaunchRules.canCancel(VoiceLaunchPhase.Closed))
        expect(!VoiceLaunchRules.canCancel(VoiceLaunchPhase.Sending))
        expect(!VoiceLaunchRules.canCancel(VoiceLaunchPhase.Sent))
        expect(!VoiceLaunchRules.canCancel(VoiceLaunchPhase.Leaving))
    }

    fun testTheOrbShowsAnIconPerPhase() {
        assertEq("graphic_eq", VoiceLaunchRules.symbol(VoiceLaunchPhase.Listening))
        assertEq("check", VoiceLaunchRules.symbol(VoiceLaunchPhase.Sent))
        assertEq("mic_off", VoiceLaunchRules.symbol(VoiceLaunchPhase.Missed("x")))
        assertEq("Send now", VoiceLaunchRules.orbLabel(VoiceLaunchPhase.Listening, heard = true))
        assertEq("Listening", VoiceLaunchRules.orbLabel(VoiceLaunchPhase.Listening, heard = false))
        assertEq("Get ready…", VoiceLaunchRules.placeholder(VoiceLaunchPhase.Waking))
    }

    fun testTypingTheWordsKeepsWhatWasAlreadyInTheComposer() {
        assertEq("hello there", VoiceLaunchRules.typed("", " hello there "))
        assertEq("draft more words", VoiceLaunchRules.typed("draft", "more words"))
        assertEq("draft ", VoiceLaunchRules.typed("draft ", ""))
    }

    fun testWordsAccumulateAcrossRecognizerRestarts() {
        val transcript = VoiceTranscript()
        expect(transcript.partial("fix the"))
        expect(!transcript.partial("fix the"))
        assertEq("fix the", transcript.text)
        transcript.partial("fix the login bug")
        transcript.commit()
        assertEq("fix the login bug", transcript.text)
        transcript.partial("and add a test")
        assertEq("fix the login bug and add a test", transcript.text)
        transcript.commit()
        transcript.commit()
        assertEq("fix the login bug and add a test", transcript.text)
        transcript.clear()
        assertEq("", transcript.text)
    }

    fun testWordsStopAtTheAgentPromptLimit() {
        val transcript = VoiceTranscript(limit = 10)
        transcript.partial("abcdefghijklmnop")
        assertEq("abcdefghij", transcript.text)
    }

    fun testTheMeterMapsDecibelsToALevelAndFallsGently() {
        assertEq(0f, VoiceMeter.level(-10f))
        assertEq(1f, VoiceMeter.level(20f))
        assertEq(0.5f, VoiceMeter.level(4f))
        val meter = VoiceMeter(size = 4)
        expect(!meter.voicing)
        meter.push(10f)
        expect(meter.voicing)
        assertEq(1f, meter.loudness)
        meter.push(-2f)
        val fallen = meter.levels.last()
        expect(fallen < 1f && fallen > 0f, "falls but not at once")
        repeat(6) { meter.push(-2f) }
        expect(!meter.voicing)
        assertEq(4, meter.levels.size)
        meter.clear()
        assertEq(0f, meter.loudness)
    }

    fun testTheMeterListsOldestLevelFirst() {
        val meter = VoiceMeter(size = 3)
        meter.push(10f)
        meter.push(10f)
        val levels = meter.levels
        assertEq(0f, levels[0])
        assertEq(1f, levels[2])
    }
}
