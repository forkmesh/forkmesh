package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.Member
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.time.ZoneOffset

class ChatScreenLogicTest {
    private fun relay(): RelayRoom {
        val settings = AccountSettings.create(MemoryPrefStore(), MemorySecretStore())
        return RelayRoom(settings, Identity.fromSeed(Bytes.random(32)), MemorySecretStore(), loop = EventLoop("test", virtualTime = true))
    }

    private fun message(id: String, sender: String, ts: Long, self: Boolean = false) =
        ChatMessage(id = id, conversation = "#general", senderId = sender, senderName = sender, text = id, timestamp = ts, self = self)

    fun testAGuestCanPostInGeneralAndMesh() {
        val room = relay()
        try {
            assertEq("#$GUEST_CHAT_CHANNEL", room.currentConversation)
            assertEq(null, guestComposeBlocked(isGuest = true, room.currentConversation))

            room.switchConversation("#random")
            assertEq("Sign in to post in #random", "Sign in to ${guestComposeBlocked(true, room.currentConversation)}")

            room.switchConversation(MESH_CONVERSATION)
            assertEq(null, guestComposeBlocked(true, room.currentConversation))

            room.switchConversation("@someone")
            assertEq("Sign in to send direct messages", "Sign in to ${guestComposeBlocked(true, room.currentConversation)}")
        } finally {
            room.dispose()
        }
    }

    fun testASignedInUserCanPostAnywhere() {
        for (conversation in listOf("#general", "#random", MESH_CONVERSATION, "@someone")) {
            assertEq(null, guestComposeBlocked(isGuest = false, conversation))
        }
    }

    fun testAPhoneReachesTheMeshRoomThroughTheChannelsSheet() {
        val room = relay()
        try {
            room.switchConversation(MESH_CONVERSATION)
            assertEq(MESH_CONVERSATION, room.currentConversation)
            assertEq(MESH_CONVERSATION, conversationTitle(room.currentConversation, room.roster()))
        } finally {
            room.dispose()
        }
    }

    fun testDirectMessageTitlesComeFromTheRoster() {
        val roster = listOf(Member(id = "self", name = "me", self = true), Member(id = "node-a", name = "Ada"))
        assertEq("@Ada", conversationTitle("@node-a", roster))
        assertEq("@direct message", conversationTitle("@gone", roster))
        assertEq("#random", conversationTitle("#random", roster))
    }

    fun testAnArrivingMessageScrollsTheTranscriptUpToShowIt() {
        val follower = TranscriptFollower()
        val backlog = (0 until 60).map { message("remote-$it", "Remote", 1000L + it) }
        assertEq(TranscriptFollower.Action.JUMP_TO_END, follower.onMessages("#general", backlog) { false })
        assertEq(TranscriptFollower.Action.NONE, follower.onMessages("#general", backlog) { true })
        val more = backlog + message("remote-60", "Remote", 1060)
        assertEq(TranscriptFollower.Action.ANIMATE_TO_END, follower.onMessages("#general", more) { true })
        val readingBack = more + message("remote-61", "Remote", 1061)
        assertEq(TranscriptFollower.Action.NONE, follower.onMessages("#general", readingBack) { false })
        val own = readingBack + message("mine", "Me", 1062, self = true)
        assertEq(TranscriptFollower.Action.ANIMATE_TO_END, follower.onMessages("#general", own) { false })
        val next = own + message("remote-62", "Remote", 1063)
        assertEq(TranscriptFollower.Action.ANIMATE_TO_END, follower.onMessages("#general", next) { true })
        assertEq(TranscriptFollower.Action.NONE, follower.onMessages(MESH_CONVERSATION, emptyList()) { true })
    }

    fun testConsecutiveMessagesFromOneSenderGroupForFiveMinutes() {
        val first = message("a", "ada", 0)
        assertEq(false, continuesGroup(null, first))
        assertEq(true, continuesGroup(first, message("b", "ada", MESSAGE_GROUP_WINDOW_MS - 1)))
        assertEq(false, continuesGroup(first, message("c", "ada", MESSAGE_GROUP_WINDOW_MS)))
        assertEq(false, continuesGroup(first, message("d", "grace", 10)))
        assertEq(false, continuesGroup(first, message("e", "ada", 10, self = true)))
        assertEq("9:05 AM", messageTime(9 * 3_600_000L + 5 * 60_000L, ZoneOffset.UTC))
        assertEq("12:00 AM", messageTime(0L, ZoneOffset.UTC))
        assertEq("12:30 PM", messageTime(12 * 3_600_000L + 30 * 60_000L, ZoneOffset.UTC))
        assertEq("11:59 PM", messageTime(23 * 3_600_000L + 59 * 60_000L, ZoneOffset.UTC))
    }

    fun testDayDividersSplitTheTranscriptByLocalDay() {
        val hour = 3_600_000L
        val day = 24 * hour
        val lateNight = message("a", "ada", day - 60_000L)
        val afterMidnight = message("b", "ada", day + 60_000L)
        assertEq(true, startsNewDay(null, lateNight, ZoneOffset.UTC))
        assertEq(true, startsNewDay(lateNight, afterMidnight, ZoneOffset.UTC))
        assertEq(false, startsNewDay(lateNight, afterMidnight, ZoneOffset.ofHours(-5)))
        assertEq(false, continuesGroup(lateNight, afterMidnight, ZoneOffset.UTC))
        assertEq(true, continuesGroup(lateNight, afterMidnight, ZoneOffset.ofHours(-5)))
        val now = 1_791_200_000_000L
        assertEq("Today", dayDividerLabel(now - hour, now, ZoneOffset.UTC))
        assertEq("Yesterday", dayDividerLabel(now - day, now, ZoneOffset.UTC))
        assertEq("Friday, October 2", dayDividerLabel(now - 3 * day, now, ZoneOffset.UTC))
        assertEq("Sunday, October 5, 2025", dayDividerLabel(now - 365 * day, now, ZoneOffset.UTC))
    }

    fun testMeshBadgeUnreadBannerAndPlatformIcons() {
        assertEq("local only", meshBadge(networkActive = false, bluetoothActive = false))
        assertEq("local · this network", meshBadge(networkActive = true, bluetoothActive = false))
        assertEq("local · this network + bluetooth", meshBadge(networkActive = true, bluetoothActive = true))
        assertEq("1 unread message", unreadBannerLabel(1))
        assertEq("3 unread messages", unreadBannerLabel(3))
        assertEq("android", platformIconName("Android"))
        assertEq("laptop_mac", platformIconName("macos"))
        assertEq("devices_other", platformIconName(""))
        val groups = meshMemberGroups(listOf(Member(id = "me", name = "me", self = true), Member(id = "p", name = "peer")))
        assertEq(listOf("me", "peer"), groups.map { it.name })
        expect(groups[0].self && !groups[1].self)
    }

    fun testOpeningTheChatTabClearsTheBadgeButKeepsTheChannelList() {
        val shell = sourceFile("app/src/main/kotlin/com/forkmesh/nativeapp/shell/HomeShell.kt")
        val handler = shell.substring(shell.indexOf("fun select("), shell.indexOf("override fun Content("))
        expect(handler.contains("ChatFeature.markBadgeSeen()"), "HomeShell.select must clear the Chat tab's badge")
        expect(!handler.contains("ChatFeature.markAllRead()"), "opening the Chat tab must not wipe the channel list's unread marks")
        val feature = sourceFile("app/src/main/kotlin/com/forkmesh/nativeapp/feature/chat/ChatFeature.kt")
        val markAllRead = feature.substring(feature.indexOf("fun markAllRead("), feature.indexOf("fun markBadgeSeen("))
        expect(markAllRead.contains("markAllRead()"), "ChatFeature.markAllRead must call the relay's markAllRead")
        expect(!markAllRead.contains("markConversationRead"), "clearing only the selected conversation leaves a stuck badge")
        val markBadgeSeen = feature.substring(feature.indexOf("fun markBadgeSeen("), feature.indexOf("fun reconnect("))
        expect(markBadgeSeen.contains("room.markBadgeSeen()"), "ChatFeature.markBadgeSeen must call the relay's markBadgeSeen")
    }

    fun testSeeingTheBadgeKeepsEachConversationUnread() {
        val room = relay()
        try {
            room.switchConversation("#general")
            room.applyFrame(mapOf("type" to "chat", "id" to "b1", "senderId" to "peer", "sender" to "Peer", "ts" to 1000L, "channel" to "#random", "text" to "one"))
            room.applyFrame(mapOf("type" to "chat", "id" to "b2", "senderId" to "peer", "sender" to "Peer", "ts" to 2000L, "channel" to "#dev", "text" to "two"))
            assertEq(2, room.totalUnread)
            assertEq(2, room.badgeUnread)
            room.markBadgeSeen()
            assertEq(0, room.badgeUnread)
            assertEq(0, room.snapshot().badgeUnread)
            assertEq(true, room.hasUnread("#random"))
            assertEq(true, room.snapshot().hasUnread("#dev"))
            room.applyFrame(mapOf("type" to "chat", "id" to "b3", "senderId" to "peer", "sender" to "Peer", "ts" to 3000L, "channel" to "#dev", "text" to "three"))
            assertEq(3, room.totalUnread)
            assertEq(1, room.badgeUnread)
            room.switchConversation("#dev")
            assertEq(0, room.badgeUnread)
            assertEq(1, room.totalUnread)
            room.noteUnread("#office/crew")
            room.noteUnread("#dev")
            assertEq(true, room.hasUnread("#office/crew"))
            assertEq(false, room.hasUnread("#dev"))
            assertEq(1, room.badgeUnread)
        } finally {
            room.dispose()
        }
    }

    fun testMarkAllReadClearsEveryConversationAtOnceWithoutAConnection() {
        val room = relay()
        try {
            room.markAllRead()
            assertEq(0, room.totalUnread)
            assertEq(RelayConnectionState.OFFLINE, room.state)
            assertEq(listOf("#general", "#random"), room.channels)
        } finally {
            room.dispose()
        }
    }

    private fun sourceFile(relative: String): String {
        val candidates = listOf(File("../$relative"), File(relative), File("native/android/$relative"))
        val file = candidates.firstOrNull { it.isFile } ?: throw AssertionError("cannot find $relative from ${File(".").absolutePath}")
        return file.readText()
    }
}
