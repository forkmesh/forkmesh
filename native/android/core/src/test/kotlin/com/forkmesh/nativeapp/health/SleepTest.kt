package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.Instant
import java.time.ZoneOffset

class SleepTest {
    private val utc = ZoneOffset.UTC

    private class FakeSleep : SleepBridge {
        override var motionAvailable = true
        var allow = true
        var rest: List<SleepRestSample>? = emptyList()
        var authorized = 0

        override fun authorizeSleep(): Boolean {
            authorized++
            return allow
        }

        override fun restHistory(from: Instant, to: Instant): List<SleepRestSample>? = rest
    }

    private fun at(day: String, clock: String): Instant =
        HealthDates.startOfDay(day, utc)!!.plusSeconds((SleepSchedule.minutes(clock) ?: 0) * 60L)

    private fun still(day: String, clock: String, confident: Boolean = true) = SleepRestSample(at(day, clock), true, confident)

    private fun moving(day: String, clock: String) = SleepRestSample(at(day, clock), false, true)

    private val night: List<SleepRestSample>
        get() = listOf(
            moving("2026-09-15", "22:00"),
            still("2026-09-15", "23:10"),
            SleepRestSample(at("2026-09-16", "02:00"), false, false),
            still("2026-09-16", "02:05"),
            moving("2026-09-16", "06:50"),
        )

    private fun detect(rest: List<SleepRestSample>, usage: List<SleepInterval> = emptyList(), now: Instant = at("2026-09-16", "09:00")) =
        SleepDetector.detect("2026-09-16", rest, usage, SleepSchedule(), utc, now)

    fun testScheduleNightKeysAndWindows() {
        val schedule = SleepSchedule.parse("23:00", "07:00")
        assertEq(480, schedule.lengthMinutes)
        assertEq("2026-09-16", schedule.nightKey(at("2026-09-15", "23:30"), utc))
        assertEq("2026-09-16", schedule.nightKey(at("2026-09-16", "07:05"), utc))
        assertEq("2026-09-17", schedule.nightKey(at("2026-09-16", "12:30"), utc))
        val window = schedule.window("2026-09-16", utc)
        assertEq(at("2026-09-15", "19:00"), window?.start)
        assertEq(at("2026-09-16", "12:00"), window?.end)
        assertEq(SleepSchedule(), SleepSchedule.parse("nope", "25:00"))
        assertEq(450, SleepSchedule.parse("02:30", "10:00").lengthMinutes)
    }

    fun testAPhoneLyingStillByTheBedCountsAsSleep() {
        val found = detect(night)
        assertEq(at("2026-09-15", "23:10"), found?.bedAt)
        assertEq(at("2026-09-16", "06:50"), found?.wakeAt)
        assertEq(445, found?.asleepMinutes)
        assertEq(15, found?.awakeMinutes)
        assertEq(1, found?.interruptions)
        assertEq("high", found?.confidence)
        assertEq(445L, found?.body?.get("asleepMinutes"))
    }

    fun testDetectingSeveralNightsMatchesDetectingEachInTurn() {
        val now = at("2026-09-16", "09:00")
        val keys = listOf("2026-09-15", "2026-09-16")
        val found = SleepDetector.detect(keys, night.reversed(), emptyList(), SleepSchedule(), utc, now)
        val one = detect(night, now = now)
        assertEq(listOfNotNull(one), found.second)
        assertEq(listOfNotNull(SleepNight.cleared("2026-09-15"), one?.body), found.first)
    }

    fun testUsingThePhoneAtNightIsAwakeTimeNotANewNight() {
        val usage = listOf(SleepInterval(at("2026-09-16", "03:00"), at("2026-09-16", "03:30")))
        val found = detect(night, usage)
        assertEq(at("2026-09-15", "23:10"), found?.bedAt)
        assertEq(at("2026-09-16", "06:50"), found?.wakeAt)
        assertEq(415, found?.asleepMinutes)
        assertEq(2, found?.interruptions)
    }

    fun testHeldPhonesShortRestsAndDesksAreNotSleep() {
        val held = night.map { SleepRestSample(it.start, it.still, false) }
        assertEq(null, detect(held))
        val nap = listOf(still("2026-09-15", "20:00"), moving("2026-09-15", "22:30"))
        assertEq(null, detect(nap))
        val desk = listOf(still("2026-09-16", "08:00"), moving("2026-09-16", "11:30"))
        assertEq(null, detect(desk, now = at("2026-09-16", "12:00")))
    }

    fun testUsageLogRecordsForegroundSessions() {
        val prefs = HealthMemoryPrefs()
        SleepUsageLog.begin(prefs, at("2026-09-16", "03:00"))
        SleepUsageLog.end(prefs, at("2026-09-16", "03:20"))
        SleepUsageLog.begin(prefs, at("2026-09-16", "08:00"))
        val intervals = SleepUsageLog.intervals(prefs, at("2026-09-16", "08:05"))
        assertEq(
            listOf(
                SleepInterval(at("2026-09-16", "03:00"), at("2026-09-16", "03:20")),
                SleepInterval(at("2026-09-16", "08:00"), at("2026-09-16", "08:05")),
            ),
            intervals,
        )
        SleepUsageLog.end(prefs, at("2026-09-25", "08:00"))
        assertEq(1, SleepUsageLog.intervals(prefs, at("2026-09-25", "08:00")).size)
    }

    fun testSyncPostsPhoneNights() {
        val prefs = HealthMemoryPrefs()
        prefs.putBool(HealthPrefs.SLEEP, true)
        val bridge = FakeSleep()
        bridge.rest = night
        val api = FakeHealthApi()
        api.replies["POST /sync"] = mapOf("ok" to true, "synced" to 1L)
        val synced = SleepSync.runIfDue(bridge, prefs, api.call, at("2026-09-16", "09:00"), 1, utc)
        assertEq(1, synced)
        val body = api.bodies("POST", "/sync").first()
        assertEq("phone", body["source"])
        assertEq("android", body["device"])
        assertEq(0L, body["tzOffsetMinutes"])
        val posted = HealthJsonRead.objects(body["nights"])
        assertEq("2026-09-16", posted.first()["night"])
        assertEq(445L, posted.first()["asleepMinutes"])
        val again = SleepSync.runIfDue(bridge, prefs, api.call, at("2026-09-16", "09:05"), 1, utc)
        assertEq(0, again)
    }

    fun testSyncDoesNothingWhenTrackingIsOff() {
        val bridge = FakeSleep()
        bridge.rest = night
        val api = FakeHealthApi()
        val synced = SleepSync.runIfDue(bridge, HealthMemoryPrefs(), api.call, at("2026-09-16", "09:00"), zone = utc, force = true)
        assertEq(0, synced)
        expect(api.calls.isEmpty(), "no calls")
    }

    fun testSyncSkipsWhenThereIsNoRestHistory() {
        val prefs = HealthMemoryPrefs()
        prefs.putBool(HealthPrefs.SLEEP, true)
        val bridge = FakeSleep()
        bridge.rest = null
        val api = FakeHealthApi()
        assertEq(0, SleepSync.runIfDue(bridge, prefs, api.call, at("2026-09-16", "09:00"), 1, utc, force = true))
        expect(api.calls.isEmpty(), "no calls")
    }

    fun testModelShowsLastNightAndSummary() {
        val api = FakeHealthApi()
        api.replies["GET"] = mapOf(
            "ok" to true, "installed" to true, "night" to "2026-09-16",
            "profile" to mapOf("goalMinutes" to 480, "bedtime" to "23:00", "wakeTime" to "07:00"),
            "lastNight" to mapOf(
                "night" to "2026-09-16", "source" to "phone", "asleepMinutes" to 445, "awakeMinutes" to 15,
                "interruptions" to 1, "confidence" to "high",
                "bedAt" to at("2026-09-15", "23:10").toEpochMilli(),
                "wakeAt" to at("2026-09-16", "06:50").toEpochMilli(),
            ),
            "summary" to mapOf(
                "weekNights" to 1, "weekAverageMinutes" to 445, "debtMinutes" to 35, "loggedNights" to 1,
                "averageMinutes" to 445, "streak" to 0, "averageBedtimeMinutes" to 1390, "averageWakeMinutes" to 410,
            ),
            "nights" to emptyList<Any>(),
        )
        val prefs = HealthMemoryPrefs()
        val model = SleepModel(api.call, FakeSleep(), prefs, utc) { at("2026-09-16", "09:00") }
        model.load(false)
        expect(model.installed, "installed")
        assertEq("?night=2026-09-16", api.calls.first().suffix)
        assertEq("7 h 25 min", model.durationText)
        assertEq("of 8 h", model.goalLine)
        assertEq("11:10 PM – 6:50 AM · 15 min awake · 1 interruption · 35 min short", model.detailLine)
        assertEq("11:10 PM", model.stats.first { it.label == "Average bedtime" }.value)
        assertEq("35 min", model.stats.first { it.label == "Sleep debt" }.value)
        assertEq("23:00", prefs.getString(HealthPrefs.SLEEP_BEDTIME))
        assertEq("Last night", model.nightLabel("2026-09-16"))
        val span = model.span("2026-09-16", 22 * 60 + 30, 6 * 60 + 45)
        assertEq(at("2026-09-15", "22:30"), span?.first)
        assertEq(at("2026-09-16", "06:45"), span?.second)
    }

    fun testModelEnablesTrackingAndSyncsOnce() {
        val api = FakeHealthApi()
        api.replies["GET"] = mapOf("ok" to true, "installed" to true, "profile" to mapOf("goalMinutes" to 480))
        api.replies["POST /sync"] = mapOf("ok" to true, "synced" to 1L)
        val prefs = HealthMemoryPrefs()
        val bridge = FakeSleep()
        bridge.rest = night
        val model = SleepModel(api.call, bridge, prefs, utc) { at("2026-09-16", "09:00") }
        model.load(false)
        model.enableTracking()
        expect(model.trackingEnabled, "tracking on")
        assertEq(true, prefs.getBool(HealthPrefs.SLEEP))
        assertEq(1, bridge.authorized)
        assertEq(1, api.bodies("POST", "/sync").size)
        bridge.allow = false
        model.disableTracking()
        model.enableTracking()
        expect(!model.trackingEnabled, "refused permission leaves tracking off")
        expect(model.notice?.isError == true, "error notice")
    }

    fun testRestLogTurnsStepCounterReadingsIntoStillAndMovingSpans() {
        val prefs = HealthMemoryPrefs()
        val boot = at("2026-09-10", "00:00").toEpochMilli()
        fun reading(day: String, clock: String, counter: Long) = StepsDailyLog.Reading(counter, at(day, clock).toEpochMilli(), boot)
        SleepRestLog.observe(prefs, reading("2026-09-15", "22:00", 5000))
        SleepRestLog.observe(prefs, reading("2026-09-16", "02:00", 5008))
        SleepRestLog.observe(prefs, reading("2026-09-16", "02:00", 5030).let { StepsDailyLog.Reading(it.counter, it.atMs + 30_000, boot) })
        SleepRestLog.observe(prefs, reading("2026-09-16", "06:50", 5032).let { StepsDailyLog.Reading(it.counter, it.atMs + 3_600_000, boot) })
        val from = at("2026-09-15", "20:00").toEpochMilli()
        val to = at("2026-09-16", "09:00").toEpochMilli()
        val samples = SleepRestLog.samples(prefs, from, to)
        assertEq(listOf(true, false, true), samples.map { it.still })
        assertEq(at("2026-09-15", "22:00"), samples.first().start)
        assertEq(at("2026-09-16", "02:00"), samples[1].start)
        expect(samples.all { it.confident }, "confident")
    }

    fun testRestLogMarksRebootsAsUnknown() {
        val prefs = HealthMemoryPrefs()
        val boot = at("2026-09-10", "00:00").toEpochMilli()
        SleepRestLog.observe(prefs, StepsDailyLog.Reading(900, at("2026-09-15", "22:00").toEpochMilli(), boot))
        SleepRestLog.observe(prefs, StepsDailyLog.Reading(12, at("2026-09-16", "05:00").toEpochMilli(), at("2026-09-16", "04:50").toEpochMilli()))
        val samples = SleepRestLog.samples(prefs, 0L, at("2026-09-17", "00:00").toEpochMilli())
        assertEq(1, samples.size)
        expect(!samples.first().confident && !samples.first().still, "unknown after a reboot")
    }

    fun testSleepText() {
        assertEq("45 min", SleepText.duration(45))
        assertEq("8 h", SleepText.duration(480))
        assertEq("0 min", SleepText.duration(-3))
        assertEq("12:00 AM", SleepText.clock(0))
        assertEq("1:05 PM", SleepText.clock(13 * 60 + 5))
        assertEq("—", SleepText.clock(-1))
    }
}
