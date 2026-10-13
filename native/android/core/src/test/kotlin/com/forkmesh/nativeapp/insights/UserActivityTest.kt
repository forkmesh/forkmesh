package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.LocalTime

class UserActivityTest {
    private val day = LocalDate.of(2026, 10, 2)

    fun testSlotsAreHourColumnsOfFiveMinuteRows() {
        assertEq(288, ActivityGrid.SLOTS)
        assertEq(0, ActivityGrid.slotFor(LocalTime.of(0, 4, 59)))
        assertEq(1, ActivityGrid.slotFor(LocalTime.of(0, 5)))
        assertEq(14 * 12 + 7, ActivityGrid.slotFor(LocalTime.of(14, 37)))
        assertEq(287, ActivityGrid.slotFor(LocalTime.of(23, 59)))
        assertEq("14:35", ActivityGrid.slotLabel(14 * 12 + 7))
    }

    fun testRecordCountsAndRollsOverAtMidnight() {
        var now = LocalDateTime.of(day, LocalTime.of(9, 1))
        val log = UserActivityLog(null, clock = { now })
        log.record()
        now = now.plusMinutes(2)
        log.record()
        log.record(LocalDateTime.of(day, LocalTime.of(17, 55)))
        val counts = log.counts(day)
        assertEq(2, counts[9 * 12])
        assertEq(1, counts[17 * 12 + 11])
        assertEq(0, log.counts(day.plusDays(1)).sum(), "another day reads empty")
        log.record(LocalDateTime.of(day.plusDays(1), LocalTime.of(0, 2)))
        assertEq(0, log.counts(day).sum(), "rollover drops yesterday")
        assertEq(1, log.counts(day.plusDays(1))[0])
    }

    fun testOneTouchSeenByNestedHandlersCountsOnce() {
        val log = UserActivityLog(null, clock = { LocalDateTime.of(day, LocalTime.of(10, 0)) })
        log.touchDown(500L)
        log.touchDown(500L)
        log.touchDown(640L)
        assertEq(2, log.counts(day).sum())
    }

    fun testCountsSurviveARestart() {
        val store = MemoryPrefStore()
        var wall = 0L
        val at = LocalDateTime.of(day, LocalTime.of(11, 20))
        val log = UserActivityLog(store, clock = { at }, wallMs = { wall })
        wall = 120_000L
        repeat(5) { log.record() }
        assertEq(1, UserActivityLog(store, clock = { at }).counts(day)[11 * 12 + 4], "saved once, then held a minute")
        log.flush()
        assertEq(5, UserActivityLog(store, clock = { at }).counts(day)[11 * 12 + 4])
        store.putString(UserActivityLog.KEY, "garbage")
        assertEq(0, UserActivityLog(store, clock = { at }).counts(day).sum())
    }

    fun testDeviceKeyIsKeptAndOtherDevicesAddToTheDay() {
        val store = MemoryPrefStore()
        val at = LocalDateTime.of(day, LocalTime.of(8, 0))
        val log = UserActivityLog(store, clock = { at })
        expect(log.deviceKey.length in 8..64, "a device key the server accepts")
        assertEq(log.deviceKey, UserActivityLog(store, clock = { at }).deviceKey, "the key survives a restart")
        expect(UserActivityLog(MemoryPrefStore()).deviceKey != log.deviceKey, "another install gets its own key")
        val before = log.revision
        log.record()
        assertEq(before + 1, log.revision)
        val others = IntArray(ActivityGrid.SLOTS)
        others[8 * 12] = 4
        others[20 * 12] = 2
        log.setOthers(day, others, listOf("desktop", "web"))
        assertEq(5, log.counts(day)[8 * 12], "other devices add to this phone's slot")
        assertEq(2, log.counts(day)[20 * 12])
        assertEq(1, log.localCounts(day)[8 * 12], "local counts stay this phone's own")
        assertEq(0, log.counts(day.plusDays(1)).sum(), "other devices' counts belong to their day")
        assertEq(listOf("desktop", "web"), log.otherKinds(day))
        log.setOthers(day, IntArray(3), emptyList())
        assertEq(5, log.counts(day)[8 * 12], "a short answer is ignored")
        log.clearOthers()
        assertEq(1, log.counts(day).sum())
        assertEq(emptyList<String>(), log.otherKinds(day))
    }

    fun testSourcesText() {
        assertEq("this device", ActivityGrid.sourcesText("android", emptyList()))
        assertEq("this device, desktop and browser", ActivityGrid.sourcesText("android", listOf("desktop", "web", "desktop")))
        assertEq("this device and another Android device", ActivityGrid.sourcesText("android", listOf("android")))
        assertEq("this desktop and iOS device", ActivityGrid.sourcesText("desktop", listOf("ios")))
    }

    fun testSummaryAndLevels() {
        val counts = IntArray(ActivityGrid.SLOTS)
        counts[10 * 12 + 2] = 8
        counts[10 * 12 + 3] = 4
        counts[15 * 12] = 10
        val summary = ActivitySummary.of(counts)
        assertEq(22, summary.total)
        assertEq(10, summary.peak)
        assertEq(15 * 12, summary.peakSlot)
        assertEq(10, summary.peakHour)
        assertEq(12, summary.peakHourTotal)
        assertEq(3, summary.activeSlots)
        assertEq("15m", summary.activeText)
        assertEq(-1, ActivitySummary.of(IntArray(0)).peakSlot)
        assertEq(0, ActivityGrid.level(0, 10))
        assertEq(1, ActivityGrid.level(1, 100))
        assertEq(2, ActivityGrid.level(50, 100))
        assertEq(4, ActivityGrid.level(100, 100))
        expect(ActivitySummary(0, 0, -1, -1, 0, 14).activeText == "1h 10m")
    }
}
