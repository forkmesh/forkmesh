package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.PingSettings
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class PingAlertLedgerTest {
    private val t0 = 1_790_000_000_000L

    private fun ping(id: String, ts: Long, read: Boolean = false, kind: String = "mention") =
        ForkNotification(id = id, kind = kind, title = "t-$id", ts = ts, readAt = if (read) ts + 1 else 0)

    fun testTheFirstReadPrimesWithoutAlertingTheBacklog() {
        val ledger = PingAlertLedger()
        val first = ledger.decide(listOf(ping("a", t0 - 10), ping("b", t0 - 5)), null, t0)
        assertEq(0, first.post.size)
        assertEq(2, first.unread)
        expect(ledger.isPrimed)
        val second = ledger.decide(listOf(ping("c", t0 + 5), ping("a", t0 - 10), ping("b", t0 - 5)), null, t0 + 6)
        assertEq(listOf("c"), second.post.map { it.id })
    }

    fun testEachPingAlertsOnceEvenWhenItsRowIsFolded() {
        val ledger = PingAlertLedger(baseline = t0)
        assertEq(listOf("p"), ledger.decide(listOf(ping("p", t0 + 1, kind = "code_push")), null, t0 + 2).post.map { it.id })
        assertEq(0, ledger.decide(listOf(ping("p", t0 + 60_000, kind = "code_push")), null, t0 + 60_001).post.size)
    }

    fun testRowsReadElsewhereCancelTheirNotification() {
        val ledger = PingAlertLedger(baseline = t0)
        ledger.decide(listOf(ping("x", t0 + 1), ping("y", t0 + 2)), null, t0 + 3)
        val later = ledger.decide(listOf(ping("x", t0 + 1, read = true), ping("y", t0 + 2)), null, t0 + 4)
        assertEq(listOf("x"), later.cancel)
        assertEq(1, later.unread)
        assertEq(listOf("y"), ledger.shownIds)
    }

    fun testAlreadyReadNewRowsNeverAlert() {
        val ledger = PingAlertLedger(baseline = t0)
        assertEq(0, ledger.decide(listOf(ping("r", t0 + 1, read = true)), null, t0 + 2).post.size)
    }

    fun testPauseAndMutedKindsHoldAlertsButStillMarkThemSeen() {
        val ledger = PingAlertLedger(baseline = t0)
        val paused = PingSettings(notificationPauseUntil = t0 + 100)
        assertEq(0, ledger.decide(listOf(ping("q", t0 + 10)), paused, t0 + 11).post.size)
        assertEq(0, ledger.decide(listOf(ping("q", t0 + 10)), null, t0 + 200).post.size)
        val muted = PingSettings(pingMutedKinds = setOf("code_push"))
        assertEq(0, ledger.decide(listOf(ping("m", t0 + 300, kind = "code_push")), muted, t0 + 301).post.size)
    }

    fun testATaskReminderAlertsThroughPauseAndMutedMentions() {
        val ledger = PingAlertLedger(baseline = t0)
        val reminder = ForkNotification(
            id = "r", kind = "mention", title = "t-r", ts = t0 + 10,
            meta = mapOf("context" to "reminder"),
        )
        val held = PingSettings(notificationPauseUntil = t0 + 100, pingMutedKinds = setOf("mention"))
        assertEq(listOf("r"), ledger.decide(listOf(reminder, ping("q", t0 + 11)), held, t0 + 12).post.map { it.id })
    }

    fun testOldUnseenRowsAreNotAlerted() {
        val ledger = PingAlertLedger(baseline = t0)
        val now = t0 + PingAlertLedger.MAX_AGE_MS + 10_000
        assertEq(0, ledger.decide(listOf(ping("old", t0 + 1)), null, now).post.size)
    }

    fun testRestartMovesTheBaselineSoAReEnableSkipsTheGap() {
        val ledger = PingAlertLedger(baseline = t0)
        ledger.restart(t0 + 1_000)
        assertEq(0, ledger.decide(listOf(ping("gap", t0 + 500)), null, t0 + 1_001).post.size)
        assertEq(1, ledger.decide(listOf(ping("new", t0 + 1_500)), null, t0 + 1_501).post.size)
    }

    fun testPostsInTimeOrder() {
        val ledger = PingAlertLedger(baseline = t0)
        val decision = ledger.decide(listOf(ping("late", t0 + 9), ping("early", t0 + 3)), null, t0 + 10)
        assertEq(listOf("early", "late"), decision.post.map { it.id })
    }

    fun testJsonRoundTripAndCapacity() {
        val ledger = PingAlertLedger(baseline = t0, capacity = 3)
        ledger.decide((1..5).map { ping("id$it", t0 + it) }, null, t0 + 10)
        val restored = PingAlertLedger.fromJson(ledger.toJson())
        assertEq(t0, restored.baseline)
        assertEq(listOf("id3", "id4", "id5"), ledger.shownIds)
        assertEq(0, restored.decide(listOf(ping("id5", t0 + 5)), null, t0 + 11).post.size)
        assertEq(0L, PingAlertLedger.fromJson("not json").baseline)
        assertEq(0L, PingAlertLedger.fromJson(null).baseline)
    }
}
