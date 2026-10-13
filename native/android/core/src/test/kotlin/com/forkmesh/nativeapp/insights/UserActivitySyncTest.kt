package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.LocalTime

class UserActivitySyncTest {
    private val day = LocalDate.of(2026, 10, 6)
    private val at = LocalDateTime.of(day, LocalTime.of(9, 0))

    private fun answer(others: List<Int>, sources: List<Map<String, Any?>> = emptyList(), node: String = "alice") =
        ApiJsonResult(200, mapOf("ok" to true, "nodeName" to node, "day" to day.toString(), "others" to others, "sources" to sources))

    private fun slots(vararg pairs: Pair<Int, Int>): List<Int> {
        val counts = MutableList(ActivityGrid.SLOTS) { 0 }
        pairs.forEach { (slot, value) -> counts[slot] = value }
        return counts
    }

    fun testFirstSyncSendsTheDayAndAddsTheOtherDevices() {
        val log = UserActivityLog(null, clock = { at })
        log.record()
        val sent = mutableListOf<Map<String, Any?>>()
        val sync = UserActivitySync(log, "android", { payload ->
            sent += payload
            answer(slots(108 to 3), listOf(mapOf("kind" to "desktop", "total" to 3, "self" to false), mapOf("kind" to "android", "total" to 1, "self" to true)))
        }, nowMs = { 1_000L })
        expect(sync.sync("alice"))
        val body = sent.single()
        assertEq("2026-10-06", body["day"])
        assertEq(log.deviceKey, body["device"])
        assertEq("android", body["kind"])
        assertEq(slots(108 to 1), body["counts"])
        assertEq(4, log.counts(day)[108])
        assertEq(listOf("desktop"), log.otherKinds(day))
    }

    fun testUnchangedCountsAreOnlyReadAfterTheReadInterval() {
        val log = UserActivityLog(null, clock = { at })
        var now = 0L
        val sent = mutableListOf<Map<String, Any?>>()
        val sync = UserActivitySync(log, "android", { payload -> sent += payload; answer(slots()) }, nowMs = { now })
        expect(sync.sync("alice"))
        expect(sent.single().containsKey("counts"), "the first sync after launch sends the counts")
        now += UserActivitySync.INTERVAL_MS
        expect(!sync.sync("alice"), "nothing new and read recently")
        expect(sync.sync("alice", force = true), "a forced sync reads once the floor has passed")
        assertEq(2, sent.size)
        now += UserActivitySync.READ_MS
        expect(sync.sync("alice"))
        expect(!sent.last().containsKey("counts"), "a read sends no counts")
        log.record()
        now += 1
        expect(sync.sync("alice"))
        expect(sent.last().containsKey("counts"), "new input is sent")
    }

    fun testForcedSyncReadsAfterTheFloor() {
        val log = UserActivityLog(null, clock = { at })
        var now = 0L
        var calls = 0
        val sync = UserActivitySync(log, "android", { calls++; answer(slots()) }, nowMs = { now })
        sync.sync("alice")
        now += 5_000L
        expect(!sync.sync("alice", force = true))
        now += UserActivitySync.FORCED_FLOOR_MS
        expect(sync.sync("alice", force = true))
        assertEq(2, calls)
    }

    fun testAnotherAccountsAnswerOrAFailureIsDropped() {
        val log = UserActivityLog(null, clock = { at })
        log.record()
        var reply = answer(slots(5 to 9), node = "bob")
        val sync = UserActivitySync(log, "android", { reply })
        expect(!sync.sync("alice"))
        assertEq(1, log.counts(day).sum())
        reply = ApiJsonResult(500, emptyMap())
        expect(!sync.sync("alice"))
        reply = ApiJsonResult(200, mapOf("nodeName" to "alice", "others" to listOf(1, 2)))
        expect(!sync.sync("alice"))
        val failing = UserActivitySync(log, "android", { throw java.io.IOException("offline") })
        expect(!failing.sync("alice"))
    }

    fun testSigningOutOrSwitchingAccountsDropsTheOtherDevices() {
        val log = UserActivityLog(null, clock = { at })
        var node = "alice"
        val sent = mutableListOf<Map<String, Any?>>()
        val sync = UserActivitySync(log, "android", { payload -> sent += payload; answer(slots(1 to 2), node = node) })
        sync.sync("alice")
        assertEq(2, log.counts(day).sum())
        expect(!sync.sync(""))
        assertEq(0, log.counts(day).sum())
        node = "bob"
        expect(sync.sync("bob"))
        expect(sent.last().containsKey("counts"), "a new account gets this device's counts")
    }
}
