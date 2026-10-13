package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.model.PingQuietHours
import com.forkmesh.nativeapp.model.PingSettings
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.ZonedDateTime
import java.time.ZoneId

class QuietHoursTest {
    private fun at(text: String, zone: String = "UTC"): Long = ZonedDateTime.of(java.time.LocalDateTime.parse(text), ZoneId.of(zone)).toInstant().toEpochMilli()

    private val overnight = PingQuietHours(enabled = true, start = 22 * 60, end = 7 * 60, days = listOf(0, 1, 2, 3, 4, 5, 6), timezone = "UTC")

    fun testDisabledNeverHolds() {
        assertEq(0L, pingQuietUntil(overnight.copy(enabled = false), at("2026-09-24T23:00")))
    }

    fun testAnOvernightWindowEndsTheNextMorning() {
        assertEq(at("2026-09-25T07:00"), pingQuietUntil(overnight, at("2026-09-24T23:30")))
        assertEq(at("2026-09-25T07:00"), pingQuietUntil(overnight, at("2026-09-25T06:59")))
        assertEq(0L, pingQuietUntil(overnight, at("2026-09-25T07:00")))
        assertEq(0L, pingQuietUntil(overnight, at("2026-09-24T21:59")))
    }

    fun testDaysNameTheNightAWindowStartsMondayFirst() {
        val thursdayOnly = overnight.copy(days = listOf(3))
        expect(pingQuietUntil(thursdayOnly, at("2026-09-24T23:00")) > 0, "Thursday night")
        expect(pingQuietUntil(thursdayOnly, at("2026-09-25T03:00")) > 0, "early Friday belongs to Thursday")
        assertEq(0L, pingQuietUntil(thursdayOnly, at("2026-09-25T23:00")))
        assertEq(0L, pingQuietUntil(thursdayOnly, at("2026-09-24T03:00")))
    }

    fun testASameDayWindowAndTheZone() {
        val lunch = PingQuietHours(enabled = true, start = 12 * 60, end = 13 * 60, days = listOf(3), timezone = "America/New_York")
        assertEq(at("2026-09-24T13:00", "America/New_York"), pingQuietUntil(lunch, at("2026-09-24T12:15", "America/New_York")))
        assertEq(0L, pingQuietUntil(lunch, at("2026-09-24T12:15")))
    }

    fun testAnUnknownZoneFallsBackToUtc() {
        assertEq(at("2026-09-25T07:00"), pingQuietUntil(overnight.copy(timezone = "Mars/Olympus"), at("2026-09-24T23:30")))
    }

    fun testPauseAndQuietHoldAlerts() {
        val now = at("2026-09-24T15:00")
        expect(!pingAlertsHeld(null, now))
        expect(!pingAlertsHeld(PingSettings(), now))
        expect(pingAlertsHeld(PingSettings(notificationPauseUntil = now + 1), now))
        expect(!pingAlertsHeld(PingSettings(notificationPauseUntil = now), now))
        expect(pingAlertsHeld(PingSettings(notificationQuietHours = overnight), at("2026-09-24T23:00")))
    }
}
