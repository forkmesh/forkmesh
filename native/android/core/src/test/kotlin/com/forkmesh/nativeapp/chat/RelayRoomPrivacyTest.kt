package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class RelayRoomPrivacyTest {

    private fun relay(): RelayRoom {
        val settings = AccountSettings.create(MemoryPrefStore(), MemorySecretStore())
        return RelayRoom(settings, Identity.fromSeed(Bytes.random(32)), MemorySecretStore(), loop = EventLoop("privacy", virtualTime = true))
    }

    fun testHelloKeepsHiddenChannelsToThisDevice() {
        val room = relay()
        try {
            val hub = ChatHub(RelayRoomChat(room))
            room.addChannel("#alpha-hidden")
            hub.extras.removeChannel("#alpha-hidden")
            room.addChannel("#office/team")
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

    fun testOnlyHellosAndUnaddressedFramesGoToTheWholeRoom() {
        assertEq("bob-node", RelayRoom.directRecipient(mapOf("type" to "reaction", "to" to "bob-node")))
        assertEq("bob-node", RelayRoom.directRecipient(mapOf("type" to "typing", "to" to "bob-node")))
        assertEq(null, RelayRoom.directRecipient(mapOf("type" to "hello", "to" to "bob-node")))
        assertEq(null, RelayRoom.directRecipient(mapOf("type" to "reaction", "channel" to "#general")))
        assertEq(null, RelayRoom.directRecipient(mapOf("type" to "edit", "to" to "")))
    }

    fun testAPrivateFrameAppliedLocallyIsDropped() {
        val room = relay()
        try {
            room.applyFrame(peerFrame("dm", mapOf("to" to room.selfId, "text" to "dm"), id = "l-1"))
            room.applyFrame(peerFrame("chat", mapOf("channel" to "#plans", "private" to true, "text" to "private"), id = "l-2"))
            room.applyFrame(peerFrame("chat", mapOf("conversation" to "@peer-1", "channel" to "#x", "text" to "at"), id = "l-3"))
            expect(room.historySnapshot().isEmpty(), "nothing private is filed, whichever path it came in on")
            expect("#plans" !in room.channels && "#x" !in room.channels)
        } finally {
            room.dispose()
        }
    }
}
