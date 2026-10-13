package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.model.PingSettings
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class DirectMessageAlertTest {
    private val now = 1_800_000_000_000L

    fun testOnlyTheDirectMessagesTopicAlerts() {
        expect(directMessageAlert("pings", null, now, "p", "", false) == null, "pings go through the inbox")
        expect(directMessageAlert("issues", null, now, "p", "", false) == null, "issues are a hint only")
        val alert = directMessageAlert(DIRECT_MESSAGES_TOPIC, null, now, "p", "", false)!!
        assertEq("\uD83D\uDCE8 New direct message", alert.title)
        assertEq("Open Chat in ForkMesh to reply.", alert.body)
        expect(isDirectMessageTarget(alert.target), "tap opens chat")
        assertEq("p", alert.target.projectId)
    }

    fun testOneNotificationPerProjectReplacesTheLast() {
        val a = directMessageAlert(DIRECT_MESSAGES_TOPIC, null, now, "p", "", false)!!
        val b = directMessageAlert(DIRECT_MESSAGES_TOPIC, null, now + 5_000, "p", "", false)!!
        val other = directMessageAlert(DIRECT_MESSAGES_TOPIC, null, now, "q", "", false)!!
        assertEq(a.target.notificationId(), b.target.notificationId())
        expect(a.target.notificationId() != other.target.notificationId(), "projects keep their own")
    }

    fun testPauseAndQuietHoursHoldIt() {
        val paused = PingSettings(notificationPauseUntil = now + 60_000)
        expect(directMessageAlert(DIRECT_MESSAGES_TOPIC, paused, now, "p", "", false) == null, "paused")
        expect(directMessageAlert(DIRECT_MESSAGES_TOPIC, paused, now + 60_000, "p", "", false) != null, "pause over")
    }

    fun testTheProjectIsNamedWhenSeveralAreSignedIn() {
        val alert = directMessageAlert(DIRECT_MESSAGES_TOPIC, null, now, "p", "ForkMesh Next", true)!!
        assertEq("Open Chat in ForkMesh to reply. · ForkMesh Next", alert.body)
    }

    fun testTheSenderIsNamedWhenKnown() {
        val alert = directMessageAlert(DIRECT_MESSAGES_TOPIC, null, now, "p", "", false, " Alice ")!!
        assertEq("\uD83D\uDCE8 Direct message from alice", alert.title)
        assertEq("alice", alert.sender)
        assertEq("", directMessageAlert(DIRECT_MESSAGES_TOPIC, null, now, "p", "", false)!!.sender)
    }

    fun testTheSenderIsTheNewestUnreadConversation() {
        val rows = listOf(
            mapOf("otherUser" to "carol", "unreadCount" to 0),
            mapOf("otherUser" to "Bob", "unreadCount" to 2),
            mapOf("otherUser" to "dave", "unreadCount" to 1),
        )
        assertEq("bob", directMessageSender(rows))
        assertEq("", directMessageSender(listOf(mapOf("otherUser" to "carol", "unreadCount" to 0))))
        assertEq("", directMessageSender(null))
    }

    fun testTheTapPicksAnUnreadDirectConversation() {
        assertEq("@bob", directMessageConversation(listOf("#general", "@bob", "@carol"), "#mesh"))
        assertEq(null, directMessageConversation(listOf("#general"), "#mesh"))
    }

    fun testTheTapNamesTheUnreadConversation() {
        val id = "d".repeat(32)
        val rows = listOf(
            mapOf("id" to "e".repeat(32), "otherUser" to "carol", "unreadCount" to 0),
            mapOf("id" to id, "otherUser" to "Bob", "unreadCount" to 1),
        )
        assertEq(UnreadDirectMessage("bob", id), unreadDirectMessage(rows))
        assertEq(UnreadDirectMessage("bob", ""), unreadDirectMessage(listOf(mapOf("id" to "x", "otherUser" to "bob", "unreadCount" to 1))))
        val alert = directMessageAlert(DIRECT_MESSAGES_TOPIC, null, now, "p", "", false, "bob", id)!!
        expect(isDirectMessageTarget(alert.target), "tap opens chat")
        assertEq("direct:$id", directMessageTargetConversation(alert.target))
        val plain = directMessageAlert(DIRECT_MESSAGES_TOPIC, null, now, "p", "", false, "bob", "not-an-id")!!
        expect(isDirectMessageTarget(plain.target), "tap still opens chat")
        assertEq("", directMessageTargetConversation(plain.target))
        assertEq(plain.target.notificationId(), alert.target.notificationId())
        val reopened = PushTarget.from("p", DIRECT_MESSAGE_PING_ID, alert.target.href)!!
        assertEq("direct:$id", directMessageTargetConversation(reopened))
    }
}
