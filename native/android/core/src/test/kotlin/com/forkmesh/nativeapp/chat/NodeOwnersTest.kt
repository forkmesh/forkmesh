package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.crypto.ChatNodeKey
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class NodeOwnersTest {
    private val alice = ChatNodeKey.generate().node
    private val mallory = ChatNodeKey.generate().node

    private class Server(val bound: Map<String, String>) : ChatAccountCalling {
        val asked = ArrayList<String>()

        override fun accountApiCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
            assertEq("GET", method)
            expect(path.startsWith(NodeOwners.PATH + "?node="), path)
            val node = path.substringAfter("?node=")
            asked.add(node)
            return mapOf("account" to bound[node].orEmpty())
        }
    }

    fun testAClaimCountsOnlyWhenTheServerBindsTheNodeToThatAccount() {
        var clock = 1_000_000L
        val server = Server(mapOf(alice to "alice"))
        val owners = NodeOwners(server, now = { clock }, run = { it() })
        assertEq("alice", owners.verified(alice, " Alice "))
        assertEq("", owners.verified(mallory, "alice"), "an unbound node claiming an account is a guest")
        assertEq("", owners.verified(alice, "bob"), "a claim that does not match is a guest")
        assertEq("", owners.verified("node-alice-phone", "alice"), "a node that is not a key is never asked about")
        assertEq(listOf(alice, mallory), server.asked)
        clock += NodeOwners.CACHE_MS - 1
        owners.verified(alice, "alice")
        assertEq(2, server.asked.size, "answers are kept for an hour")
        clock += 2
        owners.verified(alice, "alice")
        assertEq(3, server.asked.size)
    }

    fun testTheRosterShowsOnlyVerifiedAccounts() {
        val settings = AccountSettings.create(MemoryPrefStore(), MemorySecretStore())
        val room = RelayRoom(settings, Identity.fromSeed(Bytes.random(32)), MemorySecretStore(), loop = EventLoop("owners", virtualTime = true))
        try {
            room.owners = NodeOwners(Server(mapOf(alice to "alice")), run = { it() })
            room.receive(emptyMap(), mapOf("type" to "hello", "id" to "h-1", "senderId" to alice, "sender" to "Alice", "ownerUser" to "alice"), alice)
            room.receive(emptyMap(), mapOf("type" to "hello", "id" to "h-2", "senderId" to mallory, "sender" to "Alice", "ownerUser" to "alice"), mallory)
            room.loop.awaitIdle()
            val roster = room.roster().associateBy { it.id }
            assertEq("alice", roster[alice]?.owner)
            assertEq("", roster[mallory]?.owner, "the impostor is shown as a guest")
            val hub = ChatHub(RelayRoomChat(room))
            assertEq("alice", hub.accountRecipient("@$alice"))
            assertEq(null, hub.accountRecipient("@$mallory"), "a guest has no direct conversation")
        } finally {
            room.dispose()
        }
    }
}
