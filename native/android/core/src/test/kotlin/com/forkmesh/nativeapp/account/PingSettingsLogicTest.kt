package com.forkmesh.nativeapp.account

import com.forkmesh.nativeapp.api.DartException
import com.forkmesh.nativeapp.model.PingKind
import com.forkmesh.nativeapp.model.PingQuietHours
import com.forkmesh.nativeapp.model.PingSettings
import com.forkmesh.nativeapp.model.StatusMonitorAlert
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.ZoneId
import java.time.ZonedDateTime

class PingSettingsLogicTest {
    private val zone = ZoneId.of("Europe/Berlin")

    private val now = ZonedDateTime.of(2026, 9, 17, 10, 0, 0, 0, zone).toInstant().toEpochMilli()

    private val mention = PingKind("mention", "Mentions", true)
    private val webhook = PingKind("chat_webhook", "Chat webhook posts", false)

    fun testKindAndEmailSwitches() {
        val settings = PingSettings(pingMutedKinds = setOf("subscribed"), notificationPreferences = mapOf("mention" to false))
        expect(isKindEnabled(settings, "mention"))
        expect(!isKindEnabled(settings, "subscribed"))
        assertEq(false, emailSwitchChecked(settings, mention))
        assertEq(true, emailSwitchEnabled(settings, mention))
        assertEq(false, emailSwitchChecked(settings, webhook))
        assertEq(false, emailSwitchEnabled(settings, webhook))
        expect(isKindEmailOn(settings, "issue_opened"))
        expect(!isKindEmailOn(settings, "host_offline"))
        val on = settings.copy(notificationPreferences = mapOf("mention" to true))
        assertEq(true, emailSwitchChecked(on, mention))
        assertEq(false, emailSwitchEnabled(on.copy(emailNotifications = false), mention))
        assertEq(false, emailSwitchChecked(on.copy(emailNotifications = false), mention))
        val muted = on.copy(pingMutedKinds = setOf("mention"))
        assertEq(false, emailSwitchEnabled(muted, mention))
        assertEq(false, emailSwitchChecked(muted, mention))
        expect(isGeneralChatDigestOn(PingSettings()))
        expect(!isGeneralChatDigestOn(PingSettings(notificationPreferences = mapOf("general_chat" to false))))
    }

    fun testChangesCarryTheMinimalPatch() {
        val base = PingSettings()
        val off = withKindEnabled(base, "mention", false)
        assertEq(setOf("mention"), off.next.pingMutedKinds)
        assertEq(mapOf("pingMutedKinds" to mapOf("mention" to false)), off.patch)
        val back = withKindEnabled(off.next, "mention", true)
        assertEq(emptySet<String>(), back.next.pingMutedKinds)
        assertEq(mapOf("pingMutedKinds" to mapOf("mention" to true)), back.patch)

        val email = withKindEmail(base, "subscribed", false)
        assertEq(mapOf("subscribed" to false), email.next.notificationPreferences)
        assertEq(mapOf("notificationPreferences" to mapOf("subscribed" to false)), email.patch)

        assertEq(mapOf("emailNotifications" to false), withEmailNotifications(base, false).patch)
        assertEq(mapOf("notificationDelivery" to "instant"), withDelivery(base, "instant").patch)
        assertEq("digest", withDelivery(base, "hourly").next.notificationDelivery)
        assertEq(mapOf("notificationPauseUntil" to 0L), withPauseUntil(base.copy(notificationPauseUntil = 5), -1).patch)

        val quiet = withQuietHours(base, PingQuietHours(true, 1320, 420, listOf(6, 0, 0, 8), "Europe/Berlin"))
        assertEq(listOf(0, 6), quiet.next.notificationQuietHours.days)
        assertEq(
            mapOf(
                "notificationQuietHours" to mapOf(
                    "enabled" to true,
                    "start" to 1320,
                    "end" to 420,
                    "days" to listOf(0, 6),
                    "timezone" to "Europe/Berlin",
                ),
            ),
            quiet.patch,
        )

        val admin = base.copy(
            statusMonitorAlerts = listOf(
                StatusMonitorAlert("website", "Website", true, true, false),
                StatusMonitorAlert("d1", "D1", true, true, false),
            ),
        )
        val continual = withMonitorAlert(admin, "d1", MonitorAlertField.CONTINUAL, true)
        assertEq(listOf(false, true), continual.next.statusMonitorAlerts!!.map { it.continual })
        assertEq(mapOf("statusMonitorAlerts" to mapOf("d1" to mapOf("continual" to true))), continual.patch)
        assertEq(false, withMonitorAlert(admin, "website", MonitorAlertField.EMAILS, false).next.statusMonitorAlerts!![0].emails)
        assertEq(false, withMonitorAlert(admin, "website", MonitorAlertField.PINGS, false).next.statusMonitorAlerts!![0].pings)
    }

    fun testPausePresetsAreCappedAtThirtyDays() {
        assertEq(listOf("1 hour", "1 day", "1 week"), PING_PAUSE_PRESETS.map { it.label })
        assertEq(now + 3_600_000L, pauseUntilFor(PING_PAUSE_PRESETS[0].durationMs, now))
        assertEq(now + PING_PAUSE_MAX_MS, pauseUntilFor(90L * 24 * 60 * 60 * 1000, now))
        expect(isPaused(PingSettings(notificationPauseUntil = now + 1), now))
        expect(!isPaused(PingSettings(notificationPauseUntil = now), now))
    }

    fun testCustomPauseIsFutureAndCapped() {
        val today = java.time.LocalDate.of(2026, 9, 17)
        assertEq(now + 3_600_000L, customPauseUntil(today, 11 * 60, zone, now))
        assertEq(null, customPauseUntil(today, 10 * 60, zone, now))
        assertEq(null, customPauseUntil(today.minusDays(1), 23 * 60, zone, now))
        assertEq(now + PING_PAUSE_MAX_MS, customPauseUntil(today.plusDays(60), 0, zone, now))
        assertEq(java.time.LocalDate.of(2026, 10, 17), lastPauseDay(now, zone))
    }

    fun testTimezoneValidation() {
        expect(isValidTimezone("Europe/Berlin"))
        expect(isValidTimezone(" UTC "))
        expect(isValidTimezone("America/Argentina/Buenos_Aires"))
        expect(!isValidTimezone("Mars/Olympus"))
        expect(!isValidTimezone(""))
        expect(!isValidTimezone("Europe/Berlin; drop"))
    }

    fun testDeadlineAndStatusLabels() {
        val hour = 3_600_000L
        assertEq("today 11:00", pingDeadlineLabel(now + hour, now, zone))
        assertEq("tomorrow 07:00", pingDeadlineLabel(now + 21 * hour, now, zone))
        assertEq("Sat 10:00", pingDeadlineLabel(now + 48 * hour, now, zone))
        assertEq("Thu 24 Sep 10:00", pingDeadlineLabel(now + 7 * 24 * hour, now, zone))
        assertEq(null, pingStatusLine(PingSettings(), now, zone))
        assertEq("Paused until today 11:00", pingStatusLine(PingSettings(notificationPauseUntil = now + hour), now, zone))
        assertEq(
            "Quiet hours until tomorrow 07:00",
            pingStatusLine(PingSettings(notificationQuietUntil = now + 21 * hour), now, zone),
        )
    }

    fun testClockConversions() {
        assertEq("22:00", minutesToClock(1320))
        assertEq("00:05", minutesToClock(5))
        assertEq("23:59", minutesToClock(-1))
        assertEq(1320, clockToMinutes("22:00"))
        assertEq(425, clockToMinutes(" 7:05 "))
        assertEq(null, clockToMinutes("24:00"))
        assertEq(null, clockToMinutes("7pm"))
        for (minute in listOf(0, 59, 60, 719, 1439)) assertEq(minute, clockToMinutes(minutesToClock(minute)))
    }

    fun testQuietHoursLabels() {
        assertEq("22:00–07:00 (next day)", quietHoursRangeLabel(PingQuietHours(start = 1320, end = 420)))
        assertEq("12:00–13:30", quietHoursRangeLabel(PingQuietHours(start = 720, end = 810)))
        assertEq("Every day", quietDaysLabel(listOf(6, 5, 4, 3, 2, 1, 0)))
        assertEq("Weekdays", quietDaysLabel(listOf(0, 1, 2, 3, 4)))
        assertEq("Weekends", quietDaysLabel(listOf(6, 5)))
        assertEq("Never", quietDaysLabel(emptyList()))
        assertEq("Mon, Wed, Fri", quietDaysLabel(listOf(4, 0, 2)))
        assertEq("Off", quietHoursSummary(PingQuietHours()))
        assertEq("12:00–13:30, Weekdays", quietHoursSummary(PingQuietHours(true, 720, 810, listOf(0, 1, 2, 3, 4))))
        assertEq(listOf("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"), PING_DAY_LABELS)
        assertEq(listOf(0, 2), toggleQuietDay(listOf(2, 0, 6), 6))
        assertEq(listOf(0, 3), toggleQuietDay(listOf(0), 3))
    }

    fun testUnsetTimezoneBecomesTheDevicesZone() {
        assertEq("Europe/Berlin", effectiveQuietTimezone("UTC", "Europe/Berlin"))
        assertEq("Europe/Berlin", effectiveQuietTimezone("", "Europe/Berlin"))
        assertEq("Europe/Berlin", effectiveQuietTimezone("Etc/UTC", "Europe/Berlin"))
        assertEq("America/New_York", effectiveQuietTimezone("America/New_York", "Europe/Berlin"))
        assertEq("UTC", effectiveQuietTimezone("", ""))
    }

    fun testGroupLookupAndLabels() {
        val settings = PingSettings()
        assertEq(0, pingGroupIndexOf(settings, "mention"))
        assertEq(1, pingGroupIndexOf(settings, "repo_activity"))
        assertEq(-1, pingGroupIndexOf(settings, "nope"))
        assertEq(-1, pingGroupIndexOf(settings, null))
        assertEq("Settings for mention pings", pingKindSettingsLabel("Mention"))
        assertEq("boom", pingSettingsErrorText(DartException("boom")))
        assertEq("Could not reach the server.", pingSettingsErrorText(IllegalStateException("x")))
        assertEq("", pingSettingsErrorText(null))
    }

    fun testSettingsRowNeedsARealSignedInAccount() {
        val real = AuthSession.fromJson(mapOf("nodeName" to "alice", "sessionToken" to "t"))
        val preview = AuthSession.fromJson(mapOf("nodeName" to "alice", "sessionKind" to "preview"))
        expect(showsPingSettings(real, guest = false))
        expect(!showsPingSettings(real, guest = true))
        expect(!showsPingSettings(preview, guest = false))
        expect(!showsPingSettings(null, guest = false))
    }

    fun testCodePushesSwitchPatchesItsOwnKey() {
        val change = withCodePushes(PingSettings(), false)
        assertEq(false, change.next.codePushes)
        assertEq(mapOf("codePushes" to false), change.patch)
    }

    fun testCodePushesDefaultsOnAndReadsFalse() {
        assertEq(true, PingSettings.fromJson(emptyMap()).codePushes)
        assertEq(false, PingSettings.fromJson(mapOf("codePushes" to false)).codePushes)
        assertEq(true, PingSettings.fromJson(mapOf("codePushes" to "no")).codePushes)
    }
}
