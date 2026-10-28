package com.forkmesh.nativeapp.account

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class PermissionPacingTest {
    fun testAFreshInstallShowsTheIntroAndHoldsNearbyUntilMeshIsOpened() {
        val prefs = MemoryPrefStore()
        PermissionPacing.settle(prefs, freshInstall = true)
        expect(PermissionPacing.needsIntro(prefs))
        expect(!PermissionPacing.nearbyAllowed(prefs))
        PermissionPacing.finishIntro(prefs)
        PermissionPacing.markNearbyOpened(prefs)
        expect(!PermissionPacing.needsIntro(prefs))
        expect(PermissionPacing.nearbyAllowed(prefs))
    }

    fun testAnUpdatedInstallSkipsTheIntroAndKeepsTheMeshAsItWas() {
        val prefs = MemoryPrefStore()
        PermissionPacing.settle(prefs, freshInstall = false)
        expect(!PermissionPacing.needsIntro(prefs))
        expect(PermissionPacing.nearbyAllowed(prefs))
    }

    fun testTheFirstDecisionSticksOnLaterLaunches() {
        val prefs = MemoryPrefStore()
        PermissionPacing.settle(prefs, freshInstall = true)
        PermissionPacing.settle(prefs, freshInstall = false)
        expect(PermissionPacing.needsIntro(prefs))
        expect(!PermissionPacing.nearbyAllowed(prefs))
        PermissionPacing.finishIntro(prefs)
        PermissionPacing.settle(prefs, freshInstall = true)
        expect(!PermissionPacing.needsIntro(prefs))
    }

    fun testTheIntroStaysShort() {
        expect(PermissionPacing.reasons.size <= 5)
        assertEq(PermissionPacing.reasons.size, PermissionPacing.reasons.map { it.id }.toSet().size)
        for (reason in PermissionPacing.reasons) expect(reason.reason.length <= 90, reason.id)
    }
}
