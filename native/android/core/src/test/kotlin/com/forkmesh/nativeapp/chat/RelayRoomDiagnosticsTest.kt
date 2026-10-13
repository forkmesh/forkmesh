package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.insights.NodeDiagnosticFinding
import com.forkmesh.nativeapp.insights.NodeDiagnosticSeverity
import com.forkmesh.nativeapp.insights.NodeRow
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class RelayRoomDiagnosticsTest {
    private fun relay(): RelayRoom {
        val settings = AccountSettings.create(MemoryPrefStore(), MemorySecretStore())
        return RelayRoom(settings, Identity.fromSeed(Bytes.random(32)), MemorySecretStore(), loop = EventLoop("diag", virtualTime = true))
    }

    private fun hello(id: String, extra: Map<String, Any?>) =
        mapOf<String, Any?>("type" to "hello", "id" to id, "senderId" to "peer-1", "sender" to "desk", "to" to "x", "ts" to System.currentTimeMillis()) + extra

    fun testHelloDiagnosticsReachTheRosterAndTheNodeRow() {
        val room = relay()
        try {
            val dg = listOf(mapOf("i" to "disk", "s" to 2L, "m" to "Disk almost full"), mapOf("i" to "fd", "s" to 1L, "m" to "Many open files"))
            room.applyFrame(hello("h1", mapOf("dg" to dg)))
            val member = room.roster().first { it.id == "peer-1" }
            assertEq(2, member.diagnostics?.size)
            val row = NodeRow.fromMember(member, NodeDiagnosticFinding.parse(member.diagnostics))
            assertEq(NodeDiagnosticSeverity.Warning, row.worst)
            assertEq("Disk almost full", row.diagnostics.first().message)
        } finally {
            room.dispose()
        }
    }

    fun testAFrameWithoutDiagnosticsKeepsTheLastOnesAndAnEmptyListClearsThem() {
        val room = relay()
        try {
            room.applyFrame(hello("h1", mapOf("dg" to listOf(mapOf("i" to "disk", "s" to 3L, "m" to "Disk full")))))
            room.applyFrame(mapOf("type" to "presence", "id" to "p1", "senderId" to "peer-1", "sender" to "desk", "ts" to System.currentTimeMillis()))
            assertEq(1, room.roster().first { it.id == "peer-1" }.diagnostics?.size)
            room.applyFrame(hello("h2", mapOf("dg" to emptyList<Any?>())))
            assertEq(0, room.roster().first { it.id == "peer-1" }.diagnostics?.size)
        } finally {
            room.dispose()
        }
    }

    fun testOversizedDiagnosticsAreCappedAndSelfHasNone() {
        val room = relay()
        try {
            val many = (1..100).map { mapOf("i" to "k$it", "s" to 1L, "m" to "m$it") }
            room.applyFrame(hello("h1", mapOf("dg" to many)))
            assertEq(RelayRoom.MAX_PEER_DIAGNOSTICS, room.roster().first { it.id == "peer-1" }.diagnostics?.size)
            expect(room.roster().first { it.self }.diagnostics == null)
        } finally {
            room.dispose()
        }
    }
}
