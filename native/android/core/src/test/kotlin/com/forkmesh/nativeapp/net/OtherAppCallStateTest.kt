package com.forkmesh.nativeapp.net

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class OtherAppCallStateTest {
    private val normal = 0
    private val ringtone = 1
    private val inCall = 2
    private val communication = 3

    fun testAnotherAppsCallOrVoipCallIsSeenOnceUntilItEnds() {
        val state = OtherAppCallState()
        expect(!state.update(normal, ownCall = false))
        expect(state.update(communication, ownCall = false))
        expect(state.active)
        expect(!state.update(inCall, ownCall = false))
        expect(state.update(normal, ownCall = false))
        assertEq(false, state.active)
        expect(state.update(ringtone, ownCall = false))
        expect(state.active)
    }

    fun testForkMeshsOwnCallCountsSoAppSoundsStepBackDuringIt() {
        val state = OtherAppCallState()
        expect(state.update(normal, ownCall = true))
        expect(state.active)
        expect(!state.update(communication, ownCall = true))
        expect(!state.update(communication, ownCall = false))
        expect(state.update(normal, ownCall = false))
        assertEq(false, state.active)
    }
}
