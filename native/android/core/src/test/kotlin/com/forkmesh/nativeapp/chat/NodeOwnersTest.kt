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

    fun testANoOwnerAnswerIsAskedAgainSoonAndAClaimIsRecheckedUntilConfirmed() {
        var clock = 1_000_000L
        val bound = HashMap<String, String>()
        val server = Server(bound)
        val timers = ArrayList<Pair<Long, () -> Unit>>()
        val owners = NodeOwners(server, now = { clock }, run = { it() }, later = { ms, task -> timers.add(ms to task) })
        var heard = 0
        owners.addListener { heard++ }
        assertEq("", owners.verified(alice, "alice"), "the device spoke before it was bound")
        owners.watch(alice, " Alice ")
        owners.watch(alice, "alice")
        expect(owners.isWatching(alice))
        assertEq(listOf(NodeOwners.RECHECK_MS), timers.map { it.first }, "one recheck per node")
        clock += NodeOwners.RETRY_MS
        owners.verified(alice, "alice")
        assertEq(2, server.asked.size, "a no-owner answer is kept a minute, not an hour")
        bound[alice] = "alice"
        val before = heard
        timers.removeAt(0).second()
        expect(heard > before, "the answer reaches the listeners that relabel the lines")
        assertEq("alice", owners.verified(alice, "alice"))
        expect(!owners.isWatching(alice))
        assertEq(0, timers.size, "no recheck after the claim is confirmed")
        owners.watch(alice, "alice")
        assertEq(0, timers.size, "a confirmed claim is not watched")

        owners.watch(mallory, "alice")
        val asked = server.asked.size
        repeat(NodeOwners.MAX_RECHECKS) { timers.removeAt(0).second() }
        assertEq(asked + NodeOwners.MAX_RECHECKS, server.asked.size)
        assertEq(0, timers.size, "rechecks stop after ten tries")
        expect(!owners.isWatching(mallory))
        assertEq("", owners.verified(mallory, "alice"), "an impostor stays a guest")

        owners.watch(mallory, "alice")
        owners.clear()
        val cleared = server.asked.size
        timers.removeAt(0).second()
        assertEq(cleared, server.asked.size, "clearing stops pending rechecks")
        owners.watch("node-alice-phone", "alice")
        assertEq(0, timers.size, "a node that is not a key is never watched")
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
