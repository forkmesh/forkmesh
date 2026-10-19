package com.forkmesh.nativeapp.net

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class RadioPolicyTest {
    fun testAnyNonNormalAudioModeIsACall() {
        assertEq(AudioActivity.IDLE, RadioPolicy.activity(audioMode = 0, musicActive = false))
        assertEq(AudioActivity.PLAYING, RadioPolicy.activity(audioMode = 0, musicActive = true))
        for (mode in 1..6) assertEq(AudioActivity.CALL, RadioPolicy.activity(audioMode = mode, musicActive = true), "mode $mode")
        assertEq(AudioActivity.IDLE, RadioPolicy.activity(audioMode = -2, musicActive = false))
    }

    fun testMeshSearchStaysOffTheRadioDuringACall() {
        val pass = RadioPolicy.meshPass(AudioActivity.CALL)
        expect(!pass.dialBonded && !pass.inquiry)
        assertEq(null, pass.bleScan)
        assertEq(RadioPolicy.CALL_RECHECK_MS, pass.nextPassMs)
        expect(RadioPolicy.CALL_RECHECK_MS < RadioPolicy.SEARCH_INTERVAL_MS)
    }

    fun testMeshSearchSkipsInquiryWhileAudioPlays() {
        val pass = RadioPolicy.meshPass(AudioActivity.PLAYING)
        expect(pass.dialBonded && !pass.inquiry)
        assertEq(ScanDuty.LOW_POWER, pass.bleScan)
        val idle = RadioPolicy.meshPass(AudioActivity.IDLE)
        expect(idle.dialBonded && idle.inquiry)
        assertEq(ScanDuty.LOW_LATENCY, idle.bleScan)
        assertEq(RadioPolicy.SEARCH_INTERVAL_MS, idle.nextPassMs)
    }

    fun testMeshDialsOnlyPhonesAndComputers() {
        expect(RadioPolicy.meshCandidate(0x0100))
        expect(RadioPolicy.meshCandidate(0x0200))
        expect(RadioPolicy.meshCandidate(0x1F00))
        expect(RadioPolicy.meshCandidate(null))
        expect(!RadioPolicy.meshCandidate(0x0400), "headsets and car kits")
        expect(!RadioPolicy.meshCandidate(0x0700), "wearables")
        expect(!RadioPolicy.meshCandidate(0x0500), "peripherals")
    }

    fun testGlassesScanBacksOffWhileAudioIsBusy() {
        assertEq(ScanDuty.LOW_LATENCY, RadioPolicy.glassesDuty(AudioActivity.IDLE))
        assertEq(ScanDuty.LOW_POWER, RadioPolicy.glassesDuty(AudioActivity.PLAYING))
        assertEq(ScanDuty.LOW_POWER, RadioPolicy.glassesDuty(AudioActivity.CALL))
        expect(RadioPolicy.glassesCensus(AudioActivity.IDLE, foreground = true))
        expect(RadioPolicy.glassesCensus(AudioActivity.PLAYING, foreground = true))
        expect(!RadioPolicy.glassesCensus(AudioActivity.CALL, foreground = true))
        expect(!RadioPolicy.glassesCensus(AudioActivity.IDLE, foreground = false))
    }
}
