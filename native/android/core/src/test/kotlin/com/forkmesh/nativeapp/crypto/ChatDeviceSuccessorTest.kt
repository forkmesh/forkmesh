package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ChatDeviceSuccessorTest {
    private val room = "chat-direct:0123456789abcdef0123456789abcdef:v1"
    private val scope = "direct:0123456789abcdef0123456789abcdef"
    private val installedAt = 1_790_000_000_000L
    private val updatedAt = installedAt + 3_600_000L
    private val aliceNode = ChatNodeKey.generate()

    private fun keys(fill: Int): ChatDeviceKeys = ChatDeviceKeys.fromSeed(ByteArray(32) { fill.toByte() })

    private fun alice(fill: Int, nodeKey: ChatNodeKey? = aliceNode, now: Long = updatedAt, trust: ChatTrust? = null): RoomSeal =
        if (trust == null) {
            RoomSeal.create(room, scope, SealPolicy.STRICT, "alice", keys(fill), clock = { now }, nodeKey = nodeKey)
        } else {
            RoomSeal.create(room, scope, SealPolicy.STRICT, "alice", keys(fill), trust = trust, clock = { now }, nodeKey = nodeKey)
        }

    private fun bob(): RoomSeal =
        RoomSeal.create(room, scope, SealPolicy.STRICT, "bob", keys(9), clock = { installedAt }, nodeKey = ChatNodeKey.generate())

    private fun pairedAndVerified(): Pair<RoomSeal, RoomSeal> {
        val bob = bob()
        val old = alice(1, now = installedAt)
        bob.keyFrame(installedAt)
        assertEq(TrustState.APPROVED, bob.acceptKeyFrame(old.keyFrame(installedAt), installedAt)?.state)
        expect(bob.trust.markVerified())
        expect(bob.trust.verified())
        return bob to old
    }

    fun testASuccessorFromTheSameNodeKeepsApprovalAndVerified() {
        val (bob, old) = pairedAndVerified()
        val before = bob.safetyNumber().digits
        val revision = bob.trust.revision
        val fresh = alice(2)
        val bundle = fresh.keyFrame(updatedAt)
        expect(bundle["nsig"] != null, "the strict bundle carries nsig")
        val outcome = bob.acceptKeyFrame(bundle, updatedAt)
        assertEq(TrustState.APPROVED, outcome?.state)
        assertEq(true, outcome?.isNew)
        assertEq(null, bob.trust.record(old.keyId), "the old key is gone")
        val record = bob.trust.record(fresh.keyId)!!
        expect(record.verified, "the successor keeps verified")
        assertEq(updatedAt, record.firstSeen, "the successor is first seen now")
        expect(bob.trust.verified())
        expect(bob.trust.pending().isEmpty(), "nothing waits for approval")
        expect(bob.trust.revision > revision)
        assertEq(listOf(fresh.keyId), bob.peerKeyIds)
        val after = bob.safetyNumber().digits
        expect(after != before, "the number follows the new key")
        fresh.acceptKeyFrame(bob.keyFrame(updatedAt), updatedAt)
        assertEq(after, fresh.safetyNumber().digits, "both sides show the same number")
        val sealed = fresh.seal(mapOf("type" to "chat", "id" to "n1", "senderId" to aliceNode.node, "text" to "updated"), updatedAt)
        assertEq("updated", bob.unseal(sealed, updatedAt)?.get("text"))
    }

    fun testAReplayedOldBundleIsDroppedAfterTheTakeover() {
        val bob = bob()
        val old = alice(1, now = installedAt)
        bob.keyFrame(installedAt)
        val oldBundle = old.keyFrame(installedAt)
        bob.acceptKeyFrame(oldBundle, installedAt)
        val fresh = alice(2)
        assertEq(TrustState.APPROVED, bob.acceptKeyFrame(fresh.keyFrame(updatedAt), updatedAt)?.state)
        val later = updatedAt + 60_000L
        assertEq(null, bob.acceptKeyFrame(oldBundle, later), "the old key cannot flip back")
        assertEq(null, bob.trust.record(old.keyId))
        assertEq(listOf(fresh.keyId), bob.peerKeyIds)
        val sameTime = alice(3).keyFrame(updatedAt)
        assertEq(null, bob.acceptKeyFrame(sameTime, later), "a bundle no newer than the current key is dropped")
        assertEq(TrustState.APPROVED, bob.acceptKeyFrame(fresh.keyFrame(later), later)?.state, "the current key is still known")
    }

    fun testWithoutANodeSignatureTheNewKeyWaits() {
        val (bob, old) = pairedAndVerified()
        val bundle = LinkedHashMap(alice(2).keyFrame(updatedAt)).also { it.remove("nsig") }
        assertEq(TrustState.PENDING, bob.acceptKeyFrame(bundle, updatedAt)?.state)
        assertEq(TrustState.APPROVED, bob.trust.record(old.keyId)?.state, "the old key stays")
        expect(bob.trust.record(old.keyId)!!.verified)
    }

    fun testANodeSignatureFromAnotherKeyOrNodeWaits() {
        val (bob, old) = pairedAndVerified()
        val forged = alice(2)
        val bundle = LinkedHashMap(forged.keyFrame(updatedAt))
        bundle["nsig"] = ChatV3.b64u(ChatNodeKey.generate().sign(ChatV3.nodeMessage(room, forged.keyId, forged.keys.idkText, "alice")))
        assertEq(TrustState.PENDING, bob.acceptKeyFrame(bundle, updatedAt)?.state, "nsig by another key")
        val otherNode = alice(3, nodeKey = ChatNodeKey.generate())
        val signed = otherNode.keyFrame(updatedAt)
        expect(ChatV3.verifyNodeBinding(signed, room))
        assertEq(TrustState.PENDING, bob.acceptKeyFrame(signed, updatedAt)?.state, "a different node")
        assertEq(TrustState.APPROVED, bob.trust.record(old.keyId)?.state)
        assertEq(setOf(forged.keyId, otherNode.keyId), bob.trust.pending().map { it.kid }.toSet())
    }

    fun testASuccessorInheritsPendingAndBlocked() {
        val bob = bob()
        bob.keyFrame(installedAt)
        bob.acceptKeyFrame(alice(5, nodeKey = ChatNodeKey.generate(), now = installedAt).keyFrame(installedAt), installedAt)
        val waiting = alice(1, now = installedAt)
        assertEq(TrustState.PENDING, bob.acceptKeyFrame(waiting.keyFrame(installedAt), installedAt)?.state)
        val next = alice(2)
        assertEq(TrustState.PENDING, bob.acceptKeyFrame(next.keyFrame(updatedAt), updatedAt)?.state)
        assertEq(null, bob.trust.record(waiting.keyId))
        expect(bob.trust.block(next.keyId, "bob") != null)
        val thirdAt = updatedAt + 60_000L
        val third = alice(3, now = thirdAt)
        assertEq(TrustState.BLOCKED, bob.acceptKeyFrame(third.keyFrame(thirdAt), thirdAt)?.state)
        assertEq(null, bob.trust.record(next.keyId))
        assertEq(listOf(TrustState.BLOCKED), bob.trust.records().filter { it.node == aliceNode.node }.map { it.state })
    }

    fun testOwnNewKeyReplacesTheOldKeyOfThisInstall() {
        val trust = ChatTrust.inMemory(scope)
        val old = alice(1, now = installedAt, trust = trust)
        val oldBundle = old.keyFrame(installedAt)
        val bob = bob()
        old.acceptKeyFrame(bob.keyFrame(installedAt), installedAt)
        expect(trust.markVerified())
        val fresh = alice(2, trust = trust)
        fresh.keyFrame(updatedAt)
        assertEq(null, trust.record(old.keyId), "the old own key is gone")
        val own = trust.record(fresh.keyId)!!
        assertEq(TrustState.APPROVED, own.state)
        assertEq(updatedAt, own.firstSeen)
        expect(trust.verified(), "replacing our own key keeps verified")
        assertEq(setOf(fresh.keyId, bob.keyId), trust.approved().map { it.kid }.toSet())
        assertEq(null, fresh.acceptKeyFrame(oldBundle, updatedAt), "the updated app ignores its own older key")
        assertEq(null, trust.record(old.keyId))
        val earlier = alice(4, now = installedAt).keyFrame(installedAt)
        assertEq(null, trust.accept(earlier, SealPolicy.STRICT, fresh.keyId, updatedAt) { true })
        val newer = updatedAt + 60_000L
        val other = alice(5, now = newer)
        assertEq(TrustState.PENDING, fresh.acceptKeyFrame(other.keyFrame(newer), newer)?.state, "a newer key on this node is never our predecessor's heir")
        assertEq(TrustState.APPROVED, trust.record(fresh.keyId)?.state, "our own record is never taken over")
    }
}
