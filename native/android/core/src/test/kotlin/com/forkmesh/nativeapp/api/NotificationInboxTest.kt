package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.NotificationPage
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class NotificationInboxTest {
    private class NodeSwitchApi : ApiService(TestSettings(), FakeTransport { error("no network") }) {
        var failWith: Exception? = null
        val marked = ArrayList<Triple<String, List<String>, Boolean>>()
        val cleared = ArrayList<String>()
        var clearFails: Exception? = null
        var unreadIds = listOf("a", "b")

        var inboxTotal: Int? = null

        override fun notifications(node: String, limit: Int): NotificationPage {
            failWith?.let { throw it }
            val items = unreadIds.map { id ->
                ForkNotification(id = "$id-$node", kind = "mention", title = "Notification for $node")
            }
            return NotificationPage(
                unread = unreadIds.size,
                notifications = items,
                total = inboxTotal ?: items.size,
            )
        }

        override fun markNotificationsRead(node: String, ids: List<String>, all: Boolean) {
            marked.add(Triple(node, ids, all))
        }

        override fun deleteAllNotifications(node: String) {
            cleared.add(node)
            clearFails?.let { throw it }
        }

        val deleted = ArrayList<Pair<String, String>>()
        var deleteFails: Exception? = null

        override fun deleteNotification(node: String, id: String) {
            deleted.add(node to id)
            deleteFails?.let { throw it }
        }
    }

    fun testDeletingOnePingRemovesItAndPutsItBackWhenRefused() {
        val api = NodeSwitchApi()
        api.unreadIds = listOf("a", "b", "c")
        val inbox = NotificationInbox(api, nodeName = { "alice" })
        inbox.refresh()

        inbox.delete(inbox.notifications[1])
        assertEq(listOf("alice" to "b-alice"), api.deleted)
        assertEq(listOf("a-alice", "c-alice"), inbox.notifications.map { it.id })
        assertEq(2, inbox.unreadCount)
        assertEq(2, inbox.inboxTotal)

        api.deleteFails = IllegalStateException("refused")
        var thrown = false
        try {
            inbox.delete(inbox.notifications[1])
        } catch (_: IllegalStateException) {
            thrown = true
        }
        expect(thrown)
        assertEq(listOf("a-alice", "c-alice"), inbox.notifications.map { it.id })
        assertEq(2, inbox.unreadCount)
        assertEq(2, inbox.inboxTotal)
    }

    fun testServiceHidesStaleNotificationsImmediatelyWhenTheNodeChanges() {
        val api = NodeSwitchApi()
        api.unreadIds = listOf("n")
        var node = "alice"
        val inbox = NotificationInbox(api, nodeName = { node })

        inbox.refresh()
        assertEq(1, inbox.unreadCount)
        assertEq("Notification for alice", inbox.notifications.single().title)

        node = "bob"
        assertEq(0, inbox.unreadCount)
        assertEq(0, inbox.notifications.size)
        expect(inbox.needsRefresh())

        inbox.refresh()
        assertEq(1, inbox.unreadCount)
        assertEq("Notification for bob", inbox.notifications.single().title)
        expect(!inbox.needsRefresh())
    }

    fun testASilentRefreshDoesNotFlashTheSpinnerOverALoadedList() {
        val api = NodeSwitchApi()
        val inbox = NotificationInbox(api, nodeName = { "Alice" })
        val loadingSeen = ArrayList<Boolean>()
        inbox.addListener { loadingSeen.add(inbox.loading) }

        inbox.refresh(silent = true)
        expect(loadingSeen.contains(true))
        loadingSeen.clear()

        inbox.refresh(silent = true)
        expect(!loadingSeen.contains(true), "$loadingSeen")
        loadingSeen.clear()
        inbox.refresh()
        expect(loadingSeen.first())
        expect(!inbox.loading)
    }

    fun testErrorsAreKeptAsTheirDartStringAndClearedOnSuccess() {
        val api = NodeSwitchApi()
        val inbox = NotificationInbox(api, nodeName = { "alice" })
        api.failWith = DartException("HTTP 503 for /api/notifications")
        inbox.refresh()
        assertEq("Exception: HTTP 503 for /api/notifications", inbox.error)
        expect(inbox.hasLoaded)
        api.failWith = null
        inbox.refresh(silent = true)
        assertEq("", inbox.error)
        assertEq(2, inbox.unreadCount)
    }

    fun testNoAccountMeansNoRead() {
        val api = NodeSwitchApi()
        api.failWith = IllegalStateException("must not read")
        val inbox = NotificationInbox(api, nodeName = { "  " })
        inbox.refresh()
        assertEq("", inbox.error)
        expect(!inbox.hasLoaded)
        inbox.markAllRead()
        assertEq(0, api.marked.size)
    }

    fun testDeleteAllEmptiesTheInboxOnlyAfterTheWorkerAccepts() {
        val api = NodeSwitchApi()
        api.inboxTotal = 137
        val inbox = NotificationInbox(api, nodeName = { "Alice" })

        inbox.deleteAll()
        assertEq(0, api.cleared.size)

        inbox.refresh()
        assertEq(2, inbox.notifications.size)
        assertEq(137, inbox.inboxTotal)

        api.clearFails = DartException("HTTP 503 for /api/notifications")
        var refused = false
        try {
            inbox.deleteAll()
        } catch (_: Exception) {
            refused = true
        }
        expect(refused)
        assertEq(listOf("alice"), api.cleared)
        assertEq(2, inbox.notifications.size, "a refused clear keeps the list")
        assertEq(2, inbox.unreadCount)
        assertEq(137, inbox.inboxTotal)

        api.clearFails = null
        inbox.deleteAll()
        assertEq(listOf("alice", "alice"), api.cleared)
        assertEq(0, inbox.notifications.size)
        assertEq(0, inbox.unreadCount)
        assertEq(0, inbox.inboxTotal)

        inbox.deleteAll()
        assertEq(2, api.cleared.size)
    }

    fun testAnAccountSwitchHidesTheInboxTotalToo() {
        val api = NodeSwitchApi()
        api.inboxTotal = 9
        var node = "alice"
        val inbox = NotificationInbox(api, nodeName = { node })
        inbox.refresh()
        assertEq(9, inbox.inboxTotal)
        node = "bob"
        assertEq(0, inbox.inboxTotal)
    }

    fun testMarkReadAndMarkAllRead() {
        val api = NodeSwitchApi()
        val inbox = NotificationInbox(api, nodeName = { "alice" }) { 42L }
        inbox.refresh()
        val first = inbox.notifications.first()
        inbox.markRead(first)
        assertEq(Triple("alice", listOf("a-alice"), false), api.marked.single())
        assertEq(1, inbox.unreadCount)
        assertEq(42L, inbox.notifications.first().readAt)

        inbox.markRead(inbox.notifications.first())
        inbox.markRead(ForkNotification(id = "", kind = "mention", title = "x"))
        assertEq(1, api.marked.size)

        inbox.markAllRead()
        assertEq(Triple("alice", emptyList<String>(), true), api.marked.last())
        assertEq(0, inbox.unreadCount)
        expect(inbox.notifications.none { it.isUnread })
    }
}
