package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.testing.assertEq
import java.util.Base64

class GroupCallsTest {
    private val ana = "a".repeat(32)
    private val ben = "b".repeat(32)

    private fun frame(type: String, peer: String, sender: String, vararg extra: Pair<String, Any?>): Map<String, Any?> =
        mapOf("type" to type, "peer" to peer, "sender" to sender) + extra

    fun testPresenceTracksJoinsLeavesAndExpiry() {
        var clock = 1_000_000L
        val roster = GroupCallRoster { clock }
        assertEq(
            GroupCallFrameEvent.Joined(ana, true),
            roster.handle(frame(GroupCallFrames.PRESENCE_TYPE, ana, "Ana", "call" to "in"), "#team", ben),
        )
        assertEq(
            GroupCallFrameEvent.Joined(ana, false),
            roster.handle(frame(GroupCallFrames.PRESENCE_TYPE, ana, "ana", "call" to "in"), "#team", ben),
        )
        assertEq(listOf(GroupCallMember(ana, "ana")), roster.roster("#team"))
        assertEq(emptyList<GroupCallMember>(), roster.roster("#other"))
        clock += GroupCallFrames.PRESENCE_TTL_MS + 1
        assertEq(emptyList<GroupCallMember>(), roster.roster("#team"))
        roster.handle(frame(GroupCallFrames.PRESENCE_TYPE, ana, "ana", "call" to "in"), "#team", ben)
        assertEq(
            GroupCallFrameEvent.Left(ana),
            roster.handle(frame(GroupCallFrames.PRESENCE_TYPE, ana, "ana", "call" to "out"), "#team", ben),
        )
        assertEq(emptyList<GroupCallMember>(), roster.roster("#team"))
    }

    fun testKeysAreValidatedAndBoundToTheAnnouncedSender() {
        val roster = GroupCallRoster()
        val raw = ByteArray(16) { it.toByte() }
        roster.handle(frame(GroupCallFrames.PRESENCE_TYPE, ana, "ana", "call" to "in"), "#team", ben)
        val good = frame(GroupCallFrames.KEY_TYPE, ana, "ana") + GroupCallFrames.key(ana, 4, raw)
        assertEq(GroupCallFrameEvent.Key(ana, GroupCallKey(4, raw, "ana")), roster.handle(good, "#team", ben))
        assertEq(null, roster.handle(good + ("sender" to "mallory"), "#team", ben))
        assertEq(null, roster.handle(good + ("key" to Base64.getEncoder().encodeToString(byteArrayOf(1, 2, 3))), "#team", ben))
        assertEq(null, roster.handle(good + ("keyId" to 256), "#team", ben))
        assertEq(null, roster.handle(good + ("keyId" to 1.5), "#team", ben))
        assertEq(null, roster.handle(good + ("peer" to "../x"), "#team", ben))
        assertEq(null, roster.handle(good, "#team", ana))
        assertEq(4, roster.keys("#team")[ana]?.keyId)
    }

    fun testAPeerIdStaysWithTheFirstSenderThatClaimedIt() {
        var clock = 1_000_000L
        val roster = GroupCallRoster { clock }
        val cat = "c".repeat(32)
        fun presence(peer: String, sender: String, state: String = "in") =
            roster.handle(frame(GroupCallFrames.PRESENCE_TYPE, peer, sender, "call" to state), "#team", ben)
        fun key(peer: String, sender: String, keyId: Int) =
            roster.handle(frame(GroupCallFrames.KEY_TYPE, peer, sender) + GroupCallFrames.key(peer, keyId, ByteArray(16) { keyId.toByte() }), "#team", ben)
        assertEq(GroupCallFrameEvent.Joined(ana, true), presence(ana, "Ana"))
        assertEq(null, presence(ana, "mallory", "out"))
        assertEq(null, presence(ana, "mallory"))
        assertEq(null, key(ana, "mallory", 1))
        assertEq(listOf(GroupCallMember(ana, "ana")), roster.roster("#team"))
        assertEq(null, roster.keys("#team")[ana])
        assertEq(true, key(ana, "ana", 2) != null)
        assertEq(null, presence(cat, ""))
        assertEq(null, presence(cat, "  "))
        assertEq(null, key(cat, "", 3))
        assertEq(true, key(cat, "cat", 3) != null)
        assertEq(null, presence(cat, "mallory"))
        assertEq(GroupCallFrameEvent.Joined(cat, true), presence(cat, "cat"))
        assertEq(GroupCallFrameEvent.Left(ana), presence(ana, "ana", "out"))
        assertEq(null, roster.keys("#team")[ana])
        assertEq(GroupCallFrameEvent.Joined(ana, true), presence(ana, "mallory"))
        clock += GroupCallFrames.PRESENCE_TTL_MS + 1
        assertEq(GroupCallFrameEvent.Joined(cat, false), presence(cat, "dan"))
        assertEq(listOf(GroupCallMember(cat, "dan")), roster.roster("#team"))
    }

    fun testTicketsNeedASecureUrlAndAPeerId() {
        assertEq(true, GroupCallTicket.from(mapOf("ticket" to "t", "url" to "wss://n2.next.forkmesh.com/calls/ws", "peer" to ana)) != null)
        assertEq(null, GroupCallTicket.from(mapOf("ticket" to "t", "url" to "ws://x/calls/ws", "peer" to ana)))
        assertEq(null, GroupCallTicket.from(mapOf("ticket" to "", "url" to "wss://x", "peer" to ana)))
        assertEq(null, GroupCallTicket.from(mapOf("ticket" to "t", "url" to "wss://x", "peer" to "nope")))
    }

    fun testTheSendKeyRotatesItsIdAndBytes() {
        var counter = 0
        val key = GroupCallSendKey { counter += 1; ByteArray(16) { counter.toByte() } }
        assertEq(0, key.keyId)
        assertEq(1, key.raw[0].toInt())
        key.rotate()
        assertEq(1, key.keyId)
        assertEq(2, key.raw[0].toInt())
        repeat(255) { key.rotate() }
        assertEq(0, key.keyId)
        assertEq(16, GroupCallSendKey.randomKey().size)
    }
}
