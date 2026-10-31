package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.push.NotificationContent
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class MeshAlertTest {
    private val now = 1_800_000_000_000L

    private fun message(text: String = "lunch is here", sender: String = "grace", self: Boolean = false, ts: Long = now - 1_000) =
        ChatMessage(id = "m-1", conversation = MESH_CONVERSATION, senderId = "g", senderName = sender, text = text, timestamp = ts, self = self)

    fun testAMessageFromSomeoneNearbyAlerts() {
        val alert = meshAlert(message(), unread = 1, watching = false, nowMs = now)!!
        assertEq("grace in #mesh", alert.title)
        assertEq("lunch is here", alert.body)
        assertEq("grace", alert.sender)
    }

    fun testWatchingTheRoomOrOwnMessagesStayQuiet() {
        expect(meshAlert(message(), unread = 1, watching = true, nowMs = now) == null, "already on screen")
        expect(meshAlert(message(self = true), unread = 0, watching = false, nowMs = now) == null, "own message")
        expect(meshAlert(message().copy(conversation = "#general"), unread = 1, watching = false, nowMs = now) == null, "not the mesh")
    }

    fun testOldBackfillStaysQuiet() {
        expect(meshAlert(message(ts = now - MESH_ALERT_FRESH_MS - 1), unread = 1, watching = false, nowMs = now) == null, "stale")
        expect(meshAlert(message(ts = now - MESH_ALERT_FRESH_MS), unread = 1, watching = false, nowMs = now) != null, "edge")
    }

    fun testUnreadCountAndPreview() {
        val alert = meshAlert(message(text = "  a\n\nb  ".plus("x".repeat(300)), sender = " "), unread = 3, watching = false, nowMs = now)!!
        assertEq("Someone nearby in #mesh · 3 new", alert.title)
        assertEq(MESH_ALERT_PREVIEW_CHARS, alert.body.length)
        expect(alert.body.startsWith("a b xxx"), alert.body)
        expect(alert.body.endsWith("..."), alert.body)
        assertEq("New message", meshAlert(message(text = " "), unread = 1, watching = false, nowMs = now)!!.body)
    }

    fun testTheNotificationContentSettingHidesTheMeshText() {
        val alert = meshAlert(message(), unread = 1, watching = false, nowMs = now)!!
        assertEq(alert, alert.shown(NotificationContent.NAME_AND_MESSAGE))
        val nameOnly = alert.shown(NotificationContent.NAME_ONLY)
        assertEq("grace in #mesh", nameOnly.title)
        assertEq("New message", nameOnly.body)
        val nothing = alert.shown(NotificationContent.NOTHING)
        assertEq("ForkMesh", nothing.title)
        assertEq("New message", nothing.body)
        assertEq("", nothing.sender)
        expect(!nothing.title.contains("grace") && !nothing.body.contains("lunch"))
    }
}
