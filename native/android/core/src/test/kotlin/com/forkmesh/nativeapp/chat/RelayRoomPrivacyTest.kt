package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.inbox.LocalPingLog
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.crypto.RoomSeal
import com.forkmesh.nativeapp.crypto.SealOpen
import com.forkmesh.nativeapp.crypto.SealPolicy
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class RelayRoomPrivacyTest {
    private val roomName = "world-general"

    private fun relay(): RelayRoom {
        val settings = AccountSettings.create(MemoryPrefStore(), MemorySecretStore())
        return RelayRoom(settings, Identity.fromSeed(Bytes.random(32)), MemorySecretStore(), loop = EventLoop("privacy", virtualTime = true))
    }

    private fun device(acct: String, node: String): RoomSeal =
        RoomSeal.loadOrCreate(roomName, scope = roomName, policy = SealPolicy.OPEN, acct = acct, secureStore = MemorySecretStore(), node = node)

    fun testHelloKeepsPrivateAndHiddenChannelsToThisDevice() {
        val room = relay()
        try {
            val hub = ChatHub(RelayRoomChat(room))
            val secret = hub.extras.createPrivateChannel("alpha-private")!!
            room.addChannel("#alpha-hidden")
            hub.extras.removeChannel("#alpha-hidden")
            room.addChannel("#office/team")
            expect(secret in room.channels)
            expect("#alpha-hidden" !in room.channels)

            room.applyFrame(
                peerFrame(
                    "hello",
                    mapOf("channels" to listOf("#peer-room", "#alpha-hidden", "#office/other", "plain", "#peer-room", "")),
                    id = "h-1",
                ),
            )
            assertEq(1, room.channels.count { it == "#peer-room" })
            expect("#alpha-hidden" !in room.channels, "a hidden channel is not adopted back")
            expect("#office/other" !in room.channels)
            expect("plain" !in room.channels)

            val advertised = room.advertisedChannels
            expect(secret !in advertised, "a private channel name never leaves the device")
            expect("#office/team" !in advertised)
            expect("#alpha-hidden" !in advertised)
            expect("#general" in advertised && "#peer-room" in advertised)
        } finally {
            room.dispose()
        }
    }

    fun testChatAndRestoredHistoryDoNotBringBackAHiddenChannel() {
        val room = relay()
        try {
            val hub = ChatHub(RelayRoomChat(room))
            room.addChannel("#gone")
            hub.extras.removeChannel("#gone")
            room.applyFrame(peerFrame("chat", mapOf("channel" to "#gone", "text" to "still here?"), id = "c-1"))
            expect("#gone" !in room.channels)
            room.restoreHistory(listOf(ChatMessage(id = "r-1", conversation = "#gone", senderId = "peer-1", senderName = "Pat", text = "old", timestamp = 1, self = false)))
            expect("#gone" !in room.channels)
            room.applyFrame(peerFrame("chat", mapOf("channel" to "#fresh", "text" to "hi"), id = "c-2"))
            expect("#fresh" in room.channels)
        } finally {
            room.dispose()
        }
    }

    private fun ownHub(room: RelayRoom): ChatHub {
        room.accountProvider = { "Mobile Tester" }
        val hub = ChatHub(RelayRoomChat(room))
        room.frameObserver = { hub.extras.apply(it) }
        return hub
    }

    fun testAStrangersPrivateChannelNeverReachesThisDevice() {
        val room = relay()
        try {
            val hub = ownHub(room)
            val mine = hub.extras.createPrivateChannel("team-x")!!
            room.applyFrame(peerFrame("chat", mapOf("channel" to "#their-secret", "private" to true, "ownerUser" to "mallory", "text" to "hidden"), id = "p-1"))
            room.applyFrame(peerFrame("chat", mapOf("channel" to mine, "private" to true, "ownerUser" to "pat", "text" to "for the team"), id = "p-2"))
            room.applyFrame(peerFrame("chat", mapOf("channel" to "#own-other-device", "private" to true, "ownerUser" to " MOBILE tester ", "text" to "mine"), id = "p-3"))
            room.applyFrame(peerFrame("thread-reply", mapOf("channel" to "#their-secret", "private" to true, "ownerUser" to "mallory", "rootId" to "root-x", "text" to "reply"), id = "p-4"))
            room.applyFrame(peerFrame("reaction", mapOf("channel" to "#their-secret", "conversation" to "#their-secret", "private" to true, "ownerUser" to "mallory", "target" to "p-1", "emoji" to "👍", "added" to true), id = "p-5"))

            expect(room.messages(mine).isNotEmpty(), "a private channel this device is in still works")
            expect(room.messages("#own-other-device").isNotEmpty(), "the own account's other device may open a private channel")
            expect(room.messages("#their-secret").isEmpty())
            expect("#their-secret" !in room.channels)
            expect(!hub.extras.isPrivate("#their-secret"))
            expect(hub.extras.replies("root-x").isEmpty(), "a stranger's private thread reply never reaches the thread store")
            expect(hub.extras.reactions("p-1").isEmpty())
            expect(hub.extras.isPrivate("#own-other-device"))
        } finally {
            room.dispose()
        }
    }

    fun testThePrivateFirstMessageOfOwnChannelIsNotPinged() {
        val room = relay()
        try {
            val hub = ownHub(room)
            val pings = LocalPingLog(MemoryPrefStore(), System::currentTimeMillis)
            hub.pings = pings
            room.applyFrame(peerFrame("chat", mapOf("channel" to "#random", "text" to "public news"), id = "q-0"))
            room.applyFrame(peerFrame("chat", mapOf("channel" to "#own-plans", "private" to true, "ownerUser" to "Mobile Tester", "text" to "private plan"), id = "q-1"))
            expect(room.messages("#own-plans").isNotEmpty())
            expect(pings.pings.any { it.message.contains("public news") }, "the ping log is wired")
            expect(pings.pings.none { it.message.contains("private plan") }, "a private first message is not filed as a ping")
        } finally {
            room.dispose()
        }
    }

    fun testAnEditInADirectConversationIsSealedOnlyToThePeer() {
        val me = device("me-acct", "me-node")
        val myTablet = device("me-acct", "me-tablet")
        val bob = device("bob-acct", "bob-node")
        val carol = device("carol-acct", "carol-node")
        for (peer in listOf(myTablet, bob, carol)) {
            me.acceptKeyFrame(peer.keyFrame())
            peer.acceptKeyFrame(me.keyFrame())
        }

        val (hub, relay) = hubWith(FakeChatRelay(selfId = "me-node"))
        assertEq(ChatHubSend.Sent, hub.sendText("@bob-node", "hi"))
        val mine = relay.messages("@bob-node").single()
        expect(hub.edit(mine, "hi, edited"))
        val edit = relay.sentOfType("edit").single()
        assertEq("bob-node", edit["to"])

        val only = RelayRoom.sealRecipients(me, edit, "me-acct")
        expect(only != null, "an addressed frame is not sealed to the whole room")
        val sealed = me.seal(edit, only = only)
        expect(bob.open(sealed) is SealOpen.Opened)
        expect(myTablet.open(sealed) is SealOpen.Opened)
        expect(carol.open(sealed) is SealOpen.NotForMe)
    }

    fun testOnlyHellosAndUnaddressedFramesGoToTheWholeRoom() {
        assertEq("bob-node", RelayRoom.directRecipient(mapOf("type" to "reaction", "to" to "bob-node")))
        assertEq("bob-node", RelayRoom.directRecipient(mapOf("type" to "typing", "to" to "bob-node")))
        assertEq("bob-node", RelayRoom.directRecipient(mapOf("type" to "invite", "to" to "bob-node")))
        assertEq("bob-node", RelayRoom.directRecipient(mapOf("type" to "dm", "to" to "bob-node")))
        assertEq(null, RelayRoom.directRecipient(mapOf("type" to "hello", "to" to "bob-node")))
        assertEq(null, RelayRoom.directRecipient(mapOf("type" to "reaction", "channel" to "#general")))
        assertEq(null, RelayRoom.directRecipient(mapOf("type" to "edit", "to" to "")))
    }
}
