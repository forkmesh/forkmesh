package com.forkmesh.nativeapp.pocket

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class PocketTextTest {
    fun testHubRowFollowsWalking() {
        assertEq("Pocket mode", PocketText.hubTitle(false))
        assertEq("Pocket mode, walking", PocketText.hubTitle(true))
        assertEq("phonelink_lock", PocketText.hubIcon(false))
        assertEq("directions_walk", PocketText.hubIcon(true))
    }

    fun testGlassesAndPingsCopy() {
        assertEq("No smart glasses or cameras", PocketText.glassesTitle(0))
        assertEq("Smart glasses or camera nearby", PocketText.glassesTitle(1))
        assertEq("3 smart glasses or cameras nearby", PocketText.glassesTitle(3))
        assertEq("1 device heard", PocketText.devicesHeard(1))
        assertEq("4 devices heard", PocketText.devicesHeard(4))
        expect(PocketText.alarmed(true, 0), "ringing alarms")
        expect(PocketText.alarmed(false, 1), "glasses nearby alarm")
        expect(!PocketText.alarmed(false, 0), "quiet otherwise")
        assertEq("No new pings", PocketText.pingsTitle(0))
        assertEq("1 new ping", PocketText.pingsTitle(1))
        assertEq("7 new pings", PocketText.pingsTitle(7))
    }

    fun testWalkingCard() {
        assertEq("Brisk walk", PocketText.walkingTitle(moving = true, stepping = true, gaitText = "Brisk walk", detecting = true))
        assertEq("Walking", PocketText.walkingTitle(moving = true, stepping = false, gaitText = "", detecting = false))
        assertEq("Not walking", PocketText.walkingTitle(moving = false, stepping = false, gaitText = "", detecting = true))
        assertEq("Walk detection is off", PocketText.walkingTitle(moving = false, stepping = false, gaitText = "", detecting = false))
        assertEq("112 steps/min", PocketText.cadence(112))
    }

    fun testSliderUnlocksPastEightyFivePercent() {
        expect(!PocketText.unlocks(84.0, 100.0), "short drag springs back")
        expect(PocketText.unlocks(85.0, 100.0), "85 percent unlocks")
        expect(!PocketText.unlocks(10.0, 0.0), "no travel never unlocks")
        assertEq(0.7, PocketText.hintAlpha(0.0, 100.0))
        assertEq(0.0, PocketText.hintAlpha(100.0, 100.0))
    }
}
