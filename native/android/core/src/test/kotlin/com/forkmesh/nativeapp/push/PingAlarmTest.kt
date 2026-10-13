package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.PingSettings
import com.forkmesh.nativeapp.testing.assertEq

class PingAlarmTest {
    private val t0 = 1_790_000_000_000L

    fun testOnlyADesktopPowerPingIsAnAlarm() {
        assertEq(PING_ALARM_POWER, pingAlarm("desktop_ping", mapOf("alarm" to "power")))
        assertEq("", pingAlarm("desktop_ping", mapOf("alarm" to "fire")))
        assertEq("", pingAlarm("desktop_ping", emptyMap()))
        assertEq("", pingAlarm("mention", mapOf("alarm" to "power")))
    }

    fun testThePowerAlarmShowsALowBattery() {
        assertEq("🪫", pingIcon("desktop_ping", mapOf("alarm" to "power", "category" to "agent-failed")))
    }

    fun testAnAlarmRingsThroughPauseQuietHoursAndAMutedKind() {
        val ledger = PingAlertLedger(baseline = t0)
        val alarm = ForkNotification(
            id = "a", kind = "desktop_ping", title = "tower lost power", ts = t0 + 10,
            meta = mapOf("alarm" to "power"),
        )
        val plain = ForkNotification(id = "p", kind = "desktop_ping", title = "Agent #1", ts = t0 + 11)
        val held = PingSettings(notificationPauseUntil = t0 + 100, pingMutedKinds = setOf("desktop_ping"))
        assertEq(listOf("a"), ledger.decide(listOf(alarm, plain), held, t0 + 12).post.map { it.id })
    }
}
