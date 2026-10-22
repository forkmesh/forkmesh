package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.account.guestChatName
import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.crypto.ChatNodeKey
import com.forkmesh.nativeapp.inbox.LocalPingLog
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class BotLabelTest {
    private val alice = ChatNodeKey.generate().node
    private val guest = ChatNodeKey.generate().node

    private fun botLine(id: String, node: String, type: String = "chat", extra: Map<String, Any?> = emptyMap()): Map<String, Any?> =
        linkedMapOf<String, Any?>(
            "type" to type, "id" to id, "senderId" to node, "sender" to "Alice", "ownerUser" to "alice", "bot" to "forkbot",
            "ts" to System.currentTimeMillis(), "channel" to "#general", "text" to "I am ForkBot",
        ).also { it.putAll(extra) }

    fun testABotLineNamesWhoPostedIt() {
        val settings = AccountSettings.create(MemoryPrefStore(), MemorySecretStore())
        val room = RelayRoom(settings, Identity.fromSeed(Bytes.random(32)), MemorySecretStore(), loop = EventLoop("bot-label", virtualTime = true))
        try {
            val pending = ArrayList<() -> Unit>()
            val api = ChatAccountCalling { _, path, _ -> mapOf("account" to if (path.endsWith(alice)) "alice" else "") }
            val owners = NodeOwners(api, run = { pending.add(it) })
            room.owners = owners
            val hub = ChatHub(RelayRoomChat(room))
            room.frameObserver = { hub.extras.apply(it) }
            owners.addListener { hub.extras.relabelBots() }
            val pings = LocalPingLog(MemoryPrefStore(), System::currentTimeMillis)
            hub.pings = pings
            room.receive(emptyMap(), botLine("g-1", guest), guest)
            room.receive(emptyMap(), botLine("a-1", alice), alice)
            room.receive(emptyMap(), botLine("a-2", alice, "thread-reply", mapOf("rootId" to "a-1")), alice)
            room.loop.awaitIdle()
            val names = room.messages("#general").associate { it.id to it.senderName }
            assertEq("forkbot · via ${guestChatName(guest)}", names["g-1"], "a guest's bot line says who sent it")
            assertEq("forkbot · via ${guestChatName(alice)}", names["a-1"], "an unverified claim is a guest until the server answers")
            expect(pings.pings.none { it.title.startsWith("forkbot in") }, "no ping names the bot alone")
            for (task in ArrayList(pending)) task()
            room.loop.awaitIdle()
            assertEq("forkbot · via alice", room.messages("#general").single { it.id == "a-1" }.senderName)
            assertEq("forkbot · via alice", hub.extras.replies("a-1").single().senderName)
            assertEq("forkbot · via ${guestChatName(guest)}", room.messages("#general").single { it.id == "g-1" }.senderName)
            expect(hub.listPreview("#general")!!.line.startsWith("forkbot \u00b7 via "), "the list preview names the sender too")
        } finally {
            room.dispose()
        }
    }

    fun testOfficeBotLinesNameTheSenderToo() {
        val plain = botLine("o-1", guest)
        assertEq("forkbot · via ${guestChatName(guest)}", OfficeFrames.chatMessage(plain, "#office/a", "me")?.senderName)
        assertEq("forkbot · via bob", OfficeFrames.chatMessage(plain, "#office/a", "me") { "bob" }?.senderName)
        assertEq("Alice", OfficeFrames.chatMessage(plain - "bot", "#office/a", "me")?.senderName)
    }
}
