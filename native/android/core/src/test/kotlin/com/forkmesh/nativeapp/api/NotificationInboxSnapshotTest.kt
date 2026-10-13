package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.inbox.PingSnapshots
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.NotificationPage
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class NotificationInboxSnapshotTest {
    private class PageApi : ApiService(TestSettings(), FakeTransport { error("no network") }) {
        var rows: List<ForkNotification> = emptyList()
        var fetches = 0

        override fun notifications(node: String, limit: Int): NotificationPage {
            fetches++
            return NotificationPage(rows, rows.count { it.isUnread })
        }

        override fun markNotificationsRead(node: String, ids: List<String>, all: Boolean) {}

        override fun deleteNotification(node: String, id: String) {}
    }

    private fun row(id: String, ts: Long = 0) = ForkNotification(id = id, kind = "mention", title = "t$id", ts = ts)

    fun testColdStartShowsTheSnapshotBeforeTheNetwork() {
        val snapshots = PingSnapshots(MemoryPrefStore())
        snapshots.save("proj", "ann", NotificationPage(listOf(row("a"), row("b")), unread = 2, total = 5))
        val api = PageApi()
        val inbox = NotificationInbox(api, nodeName = { "Ann" }, snapshots = snapshots, project = "proj")
        assertEq(listOf("a", "b"), inbox.notifications.map { it.id })
        assertEq(2, inbox.unreadCount)
        assertEq(5, inbox.inboxTotal)
        assertEq(0, api.fetches)
        expect(inbox.needsRefresh())

        api.rows = listOf(row("c"))
        inbox.refresh(silent = true)
        assertEq(listOf("c"), inbox.notifications.map { it.id })
        expect(!inbox.needsRefresh())
        assertEq(listOf("c"), snapshots.load("proj", "ann")!!.notifications.map { it.id })
    }

    fun testInsertedRowSurvivesARefreshThatLacksIt() {
        val snapshots = PingSnapshots(MemoryPrefStore())
        val api = PageApi()
        api.rows = listOf(row("old", 1))
        val inbox = NotificationInbox(api, nodeName = { "ann" }, snapshots = snapshots, project = "proj")
        inbox.refresh()
        expect(inbox.insert(row("tapped", 9)))
        expect(!inbox.insert(row("tapped", 9)))
        assertEq(listOf("tapped", "old"), inbox.notifications.map { it.id })
        assertEq(2, inbox.unreadCount)

        inbox.refresh(silent = true)
        assertEq(listOf("tapped", "old"), inbox.notifications.map { it.id })

        api.rows = listOf(row("tapped", 9), row("old", 1))
        inbox.refresh(silent = true)
        api.rows = listOf(row("old", 1))
        inbox.refresh(silent = true)
        assertEq(listOf("old"), inbox.notifications.map { it.id })
    }

    fun testAcceptTakesABackgroundPageForTheCurrentNodeOnly() {
        val snapshots = PingSnapshots(MemoryPrefStore())
        val inbox = NotificationInbox(PageApi(), nodeName = { "ann" }, snapshots = snapshots, project = "proj")
        inbox.accept("bob", NotificationPage(listOf(row("x")), 1))
        assertEq(emptyList<String>(), inbox.notifications.map { it.id })
        inbox.accept("Ann", NotificationPage(listOf(row("y")), 1))
        assertEq(listOf("y"), inbox.notifications.map { it.id })
        expect(!inbox.needsRefresh())
    }

    fun testDeleteAndReadReachTheSnapshot() {
        val snapshots = PingSnapshots(MemoryPrefStore())
        val api = PageApi()
        api.rows = listOf(row("a"), row("b"))
        val inbox = NotificationInbox(api, nodeName = { "ann" }, snapshots = snapshots, project = "proj") { 50L }
        inbox.refresh()
        inbox.markRead(inbox.notifications.first { it.id == "a" })
        inbox.delete(inbox.notifications.first { it.id == "b" })
        val saved = snapshots.load("proj", "ann")!!
        assertEq(listOf("a"), saved.notifications.map { it.id })
        assertEq(50L, saved.notifications.single().readAt)
        assertEq(0, saved.unread)
    }
}
