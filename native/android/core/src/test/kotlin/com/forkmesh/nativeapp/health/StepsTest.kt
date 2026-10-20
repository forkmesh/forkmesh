package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.Instant
import java.time.ZoneOffset
import kotlin.math.abs

class StepsTest {
    private val utc = ZoneOffset.UTC
    private val now: Instant = Instant.ofEpochSecond(1_789_561_860)

    private class FakeSteps : StepsBridge {
        override var available = true
        override var motionAvailable = false
        var allow = true
        var authorized = 0
        var stepDaysResult: List<HealthStepDay> = emptyList()
        var liveUpdate: ((HealthStepDay) -> Unit)? = null
        var liveStopped = 0
        var motionUpdate: ((List<MotionSample>) -> Unit)? = null
        var motionStopped = 0

        override fun authorize(): Boolean {
            authorized++
            return allow
        }

        override fun stepDays(count: Int, now: Instant): List<HealthStepDay> = stepDaysResult.takeLast(count)

        override fun startLiveSteps(update: (HealthStepDay) -> Unit): Boolean {
            liveUpdate = update
            return true
        }

        override fun stopLiveSteps() {
            liveStopped++
        }

        override fun startMotionSamples(update: (List<MotionSample>) -> Unit): Boolean {
            motionUpdate = update
            return motionAvailable
        }

        override fun stopMotionSamples() {
            motionStopped++
        }
    }

    private fun near(expected: Double, actual: Double, accuracy: Double) =
        expect(abs(expected - actual) <= accuracy, "expected $expected within $accuracy but was $actual")

    private fun overview(steps: Int, synced: Long = 0): HealthJson {
        val days = (0 until 30).map { index ->
            mapOf(
                "day" to "2026-%02d-%02d".format(if (index < 14) 8 else 9, if (index < 14) index + 18 else index - 13),
                "steps" to (if (index == 29) steps else 8000 + index * 100),
                "distanceM" to 5000,
            )
        }
        val sources: Map<String, Any?> = if (synced > 0) mapOf("health" to mapOf("updatedAt" to synced, "steps" to steps)) else emptyMap()
        return mapOf(
            "ok" to true, "installed" to true, "actor" to "alice", "day" to "2026-09-16",
            "profile" to mapOf("goal" to 10000, "units" to "metric", "unitsPreference" to "auto"),
            "today" to mapOf("day" to "2026-09-16", "steps" to steps, "distanceM" to 3200.5, "floors" to 4, "activeKcal" to 0, "sources" to sources),
            "days" to days,
            "summary" to mapOf("weekSteps" to 61204, "averageSteps" to 9100, "streak" to 1, "goalDays" to 3, "bestDay" to "2026-09-15", "bestSteps" to 13050),
        )
    }

    fun testStepSyncChunksAndRespectsTheInterval() {
        val api = FakeHealthApi()
        api.replies["POST"] = mapOf("ok" to true, "synced" to 62L)
        val days = (0 until 70).map { HealthStepDay("2026-07-%02d".format(it % 28 + 1), it) }
        assertEq(124, StepsHealthSync.push(days, "android", api.call))
        assertEq(listOf(62, 8), api.bodies("POST", "/sync").map { (it["days"] as List<*>).size })
        assertEq("health", api.bodies("POST", "/sync").first()["source"])
    }

    fun testModelInstallsTurnsOnCountingAndSyncs() {
        val api = FakeHealthApi()
        api.replies["GET ?day=2026-09-16"] = mapOf("ok" to true, "installed" to false, "actor" to "alice")
        val bridge = FakeSteps()
        bridge.stepDaysResult = listOf(HealthStepDay("2026-09-15", 12000), HealthStepDay("2026-09-16", 5400))
        val prefs = HealthMemoryPrefs()
        val model = StepsModel(api.call, bridge, prefs, utc) { now }
        model.load()
        expect(model.showsInstall, "shows install")
        api.replies["GET ?day=2026-09-16"] = overview(0)
        api.replies["POST /sync"] = mapOf("ok" to true, "synced" to 2L)
        model.install()
        assertEq(0L, api.bodies("POST", "/install").first()["tzOffsetMinutes"])
        assertEq(1, bridge.authorized)
        expect(model.healthEnabled, "enabled")
        assertEq(true, prefs.getBool(HealthPrefs.STEPS))
        assertEq(2, (api.bodies("POST", "/sync").first()["days"] as List<*>).size)
        assertEq("android", api.bodies("POST", "/sync").first()["device"])

        api.replies["GET ?day=2026-09-16"] = overview(5400, now.toEpochMilli())
        model.load()
        assertEq("5,400", model.stepsText)
        assertEq("of 10,000 steps", model.goalLine)
        assertEq("4,600 to go · 3.20 km · 4 floors", model.detailLine)
        near(0.54, model.ringRatio, 0.0001)
        assertEq("Counting with the step sensor · synced 12:31 PM", model.sourceLine)
        assertEq(listOf("61,204 steps", "9,100 steps", "1 day", "13,050 · Tue Sep 15"), model.stats.map { it.value })
        assertEq(30, model.bars.size)
        assertEq(5400, model.bars.last().steps)
        assertEq("8/18", model.bars.first().label)

        model.startLive()
        bridge.liveUpdate?.invoke(HealthStepDay("2026-09-16", 6100, distanceM = 3900.0))
        bridge.liveUpdate?.invoke(HealthStepDay("2026-09-15", 99999))
        assertEq("6,100", model.stepsText)
        assertEq(6100, model.bars.last().steps)
        model.leave()
        assertEq(1, bridge.liveStopped)
        assertEq(2, api.bodies("POST", "/sync").size)
    }

    fun testPaceFollowsLiveCadence() {
        val api = FakeHealthApi()
        api.replies["GET ?day=2026-09-16"] = overview(5400)
        val bridge = FakeSteps()
        val prefs = HealthMemoryPrefs()
        var clock = now
        val model = StepsModel(api.call, bridge, prefs, utc) { clock }
        model.load(false)
        assertEq(StepsModel.PACE_OFF_MESSAGE, model.paceLine)
        prefs.putBool(HealthPrefs.STEPS, true)
        val enabled = StepsModel(api.call, bridge, prefs, utc) { clock }
        enabled.load(false)
        enabled.startLive()
        assertEq("Standing still", enabled.paceLine)
        bridge.liveUpdate?.invoke(HealthStepDay("2026-09-16", 5500, cadence = 1.8))
        assertEq(108, enabled.stepsPerMinute)
        assertEq("Walking · 108 steps/min · 2.3 mph · 42 min to 10,000", enabled.paceLine)
        clock = now.plusSeconds(4)
        bridge.liveUpdate?.invoke(HealthStepDay("2026-09-16", 5510))
        assertEq("Running · 150 steps/min · 4.4 mph · 30 min to 10,000", enabled.paceLine)
        expect(enabled.running, "running")
        clock = now.plusSeconds(60)
        bridge.liveUpdate?.invoke(HealthStepDay("2026-09-16", 5520))
        assertEq(0.0, enabled.cadence)
        bridge.liveUpdate?.invoke(HealthStepDay("2026-09-16", 5600, cadence = 1.4))
        assertEq("Strolling · 84 steps/min · 1.8 mph · 53 min to 10,000", enabled.paceLine)
        bridge.liveUpdate?.invoke(HealthStepDay("2026-09-16", 5610, cadence = 2.0, pace = 0.6))
        near(1 / 1.2, enabled.strideM, 0.001)
        assertEq("Brisk walk · 120 steps/min · 3.7 mph · 37 min to 10,000", enabled.paceLine)
        enabled.stopLive()
        assertEq("Standing still", enabled.paceLine)
        assertEq("Brisk walk", StepsText.gait(130))
        assertEq("2.2 mph", StepsText.speed(1.0, true))
        assertEq("5 min to 10,000", StepsText.minutesToGoal(5, 10000))
    }

    fun testStaleCadenceFallsToZeroOnTick() {
        val api = FakeHealthApi()
        api.replies["GET ?day=2026-09-16"] = overview(5400)
        val bridge = FakeSteps()
        val prefs = HealthMemoryPrefs()
        prefs.putBool(HealthPrefs.STEPS, true)
        var clock = now
        val model = StepsModel(api.call, bridge, prefs, utc) { clock }
        model.load(false)
        model.startLive()
        bridge.liveUpdate?.invoke(HealthStepDay("2026-09-16", 5500, cadence = 1.8))
        model.tick()
        assertEq(1.8, model.cadence)
        clock = now.plusSeconds(7)
        model.tick()
        assertEq(0.0, model.cadence)
    }

    fun testAccelerometerDrivesSpeedAndDetectsRunning() {
        val api = FakeHealthApi()
        api.replies["GET ?day=2026-09-16"] = overview(5400)
        val bridge = FakeSteps()
        bridge.motionAvailable = true
        val prefs = HealthMemoryPrefs()
        prefs.putBool(HealthPrefs.STEPS, true)
        val model = StepsModel(api.call, bridge, prefs, utc) { now }
        model.load(false)
        model.startLive()
        expect(model.sensing, "sensing")
        bridge.motionUpdate?.invoke(strideSignal(100.0, 5.0, 1.8, 0.35))
        assertEq(StrideGait.Walking, model.gait)
        assertEq(108, model.stepsPerMinute)
        expect(!model.running, "walking")
        bridge.liveUpdate?.invoke(HealthStepDay("2026-09-16", 5500, cadence = 0.5))
        assertEq(108, model.stepsPerMinute)
        assertEq("Walking · 108 steps/min · 2.3 mph · 42 min to 10,000", model.paceLine)
        bridge.motionUpdate?.invoke(strideSignal(105.0, 4.0, 2.4, 1.5))
        assertEq(StrideGait.Running, model.gait)
        expect(model.running, "running")
        assertEq(144, model.stepsPerMinute)
        assertEq("Running · 144 steps/min · 4.2 mph · 32 min to 10,000", model.paceLine)
        bridge.motionUpdate?.invoke(strideSignal(109.0, 3.0, 0.0, 0.0))
        assertEq("Standing still", model.paceLine)
        expect(!model.running, "stopped")
        model.stopLive()
        expect(!model.sensing, "not sensing")
        assertEq(1, bridge.motionStopped)
    }

    fun testRefusedPermissionStaysOffAndManualStepsConvertUnits() {
        val api = FakeHealthApi()
        val data = LinkedHashMap(overview(0))
        data["profile"] = mapOf("goal" to 8000, "units" to "imperial", "unitsPreference" to "imperial")
        api.replies["GET ?day=2026-09-16"] = data
        val bridge = FakeSteps()
        bridge.allow = false
        val model = StepsModel(api.call, bridge, HealthMemoryPrefs(), utc) { now }
        model.load()
        model.enableHealth()
        expect(!model.healthEnabled, "stays off")
        expect(model.notice?.message?.contains("not allowed") == true, "notice")
        assertEq("8,000 to go · 1.99 mi · 4 floors", model.detailLine)
        model.addSteps("2026-09-15", 3000, 1.0)
        assertEq(1609.3, api.bodies("PUT", "/days/2026-09-15").first()["distanceM"])
        model.saveGoal(12000, "metric")
        assertEq(12000L, api.bodies("PUT", "/profile").first()["goal"])
        expect(api.calls.all { it.suffix != "/sync" }, "never synced")
    }

    fun testDailyLogCountsCounterDeltasAndSurvivesReboots() {
        val prefs = HealthMemoryPrefs()
        val noon = Instant.parse("2026-09-16T12:00:00Z").toEpochMilli()
        val boot = Instant.parse("2026-09-01T00:00:00Z").toEpochMilli()
        assertEq(0, StepsDailyLog.record(prefs, StepsDailyLog.Reading(40_000, noon, boot), utc))
        assertEq(120, StepsDailyLog.record(prefs, StepsDailyLog.Reading(40_120, noon + 600_000, boot), utc))
        assertEq(170, StepsDailyLog.record(prefs, StepsDailyLog.Reading(40_170, noon + 900_000, boot), utc))
        assertEq(170, StepsDailyLog.record(prefs, StepsDailyLog.Reading(40_100, noon + 800_000, boot), utc))
        val rebooted = Instant.parse("2026-09-16T13:00:00Z").toEpochMilli()
        assertEq(230, StepsDailyLog.record(prefs, StepsDailyLog.Reading(60, rebooted + 120_000, rebooted), utc))
        val days = StepsDailyLog.stepDays(prefs, 3, noon + 7_200_000, utc)
        assertEq(listOf("2026-09-16"), days.map { it.day })
        assertEq(230, days.first().steps)
        assertEq(165.6, days.first().distanceM)
    }

    fun testDailyLogSeedsFromTheCounterWhenThePhoneBootedToday() {
        val prefs = HealthMemoryPrefs()
        val boot = Instant.parse("2026-09-16T06:00:00Z").toEpochMilli()
        val at = Instant.parse("2026-09-16T09:00:00Z").toEpochMilli()
        assertEq(2100, StepsDailyLog.record(prefs, StepsDailyLog.Reading(2100, at, boot), utc))
        val older = HealthMemoryPrefs()
        assertEq(0, StepsDailyLog.record(older, StepsDailyLog.Reading(90_000, at, Instant.parse("2026-08-01T00:00:00Z").toEpochMilli()), utc))
    }

    fun testDailyLogSplitsStepsAcrossMidnightByWakingHours() {
        val prefs = HealthMemoryPrefs()
        val boot = Instant.parse("2026-09-01T00:00:00Z").toEpochMilli()
        val evening = Instant.parse("2026-09-15T20:00:00Z").toEpochMilli()
        StepsDailyLog.record(prefs, StepsDailyLog.Reading(1000, evening, boot), utc)
        val morning = Instant.parse("2026-09-16T10:00:00Z").toEpochMilli()
        StepsDailyLog.record(prefs, StepsDailyLog.Reading(1500, morning, boot), utc)
        val days = StepsDailyLog.days(prefs)
        assertEq(500, days.values.sum())
        assertEq(200, days["2026-09-15"])
        assertEq(300, days["2026-09-16"])
    }

    fun testStepsText() {
        assertEq("1,234,567", StepsText.count(1234567))
        assertEq("-1,200", StepsText.count(-1200))
        assertEq("3.20 km", StepsText.distance(3200.5, false))
        assertEq("8/9", StepsText.shortDay("2026-08-09"))
        assertEq("12:31 PM", StepsText.clock(now, utc))
    }
}
