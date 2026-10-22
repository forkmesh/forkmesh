package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.resourceText
import java.io.File

class ChatNodeBindingTest {
    private val vectors: Map<String, Any?> = Json.parseObject(load())
    private val room = vectors["room"] as String
    private val acct = vectors["acct"] as String
    private val now = 1_790_000_000_000L

    private fun load(): String {
        val root = runCatching { CryptoInterop.repoRoot }.getOrNull()
        val canonical = root?.let { File(it, "conformance/chat/v3/node-binding.json") }
        if (canonical != null && canonical.isFile) return canonical.readText(Charsets.UTF_8)
        return resourceText("chat/v3/node-binding.json")
    }

    private fun text(key: String): String = vectors[key] as String

    private fun identityKeys(): ChatDeviceKeys = ChatDeviceKeys.fromSeed(ChatV3.unb64(text("identitySeed"))!!)

    private fun nodeKey(): ChatNodeKey = ChatNodeKey.fromSeed(ChatV3.unb64(text("nodeSeed"))!!)

    private fun binding(node: String, nsig: String): Map<String, Any?> {
        val keys = identityKeys()
        return linkedMapOf("kid" to keys.kid, "idk" to keys.idkText, "acct" to acct, "node" to node, "nsig" to nsig)
    }

    fun testTheNodeKeyAndMessageMatchTheVectors() {
        val keys = identityKeys()
        assertEq(text("idk"), keys.idkText)
        assertEq(text("node"), nodeKey().node)
        assertEq(text("message"), String(ChatV3.nodeMessage(room, keys.kid, keys.idkText, acct), Charsets.UTF_8))
        assertEq(text("nsig"), ChatV3.b64u(nodeKey().sign(ChatV3.nodeMessage(room, keys.kid, keys.idkText, acct))))
    }

    fun testAnOpenRoomBundleCarriesTheVectorSignature() {
        val seal = RoomSeal.create(room, room, SealPolicy.OPEN, acct, identityKeys(), clock = { now }, nodeKey = nodeKey())
        val bundle = seal.keyFrame(now)
        assertEq(text("node"), bundle["node"])
        assertEq(text("nsig"), bundle["nsig"])
        expect(ChatV3.verifyBundle(bundle, room, now))
        expect(ChatV3.verifyNodeBinding(bundle, room))
        val strict = RoomSeal.create(room, "direct:1", SealPolicy.STRICT, acct, identityKeys(), clock = { now }, nodeKey = nodeKey())
        expect(strict.keyFrame(now)["nsig"] == null, "strict rooms keep the v3 bundle")
    }

    fun testTheVectorBindingVerifiesAndEveryRejectFails() {
        expect(ChatV3.verifyNodeBinding(binding(text("node"), text("nsig")), room))
        for (raw in vectors["rejects"] as List<*>) {
            val reject = raw as Map<*, *>
            val bundle = binding(reject["node"] as String, reject["nsig"] as String)
            expect(!ChatV3.verifyNodeBinding(bundle, reject["room"] as String), reject["why"] as String)
        }
    }

    fun testOpenRoomsDropBundlesWithoutAValidNodeSignature() {
        val receiver = RoomSeal.create(room, room, SealPolicy.OPEN, "bob", ChatDeviceKeys.fromSeed(ByteArray(32) { 3 }), clock = { now }, nodeKey = ChatNodeKey.generate())
        val alice = RoomSeal.create(room, room, SealPolicy.OPEN, acct, identityKeys(), clock = { now }, nodeKey = nodeKey())
        val genuine = alice.keyFrame(now)
        val unsigned = LinkedHashMap(genuine).also { it.remove("nsig") }
        val otherKey = LinkedHashMap(genuine).also { it["nsig"] = ChatV3.b64u(ChatNodeKey.generate().sign(ChatV3.nodeMessage(room, alice.keyId, alice.keys.idkText, acct))) }
        assertEq(null, receiver.acceptKeyFrame(unsigned, now), "no nsig")
        assertEq(null, receiver.acceptKeyFrame(otherKey, now), "nsig by another key")
        val named = RoomSeal.create(room, room, SealPolicy.OPEN, acct, ChatDeviceKeys.fromSeed(ByteArray(32) { 5 }), clock = { now }, node = "node-alice-phone")
        assertEq(null, receiver.acceptKeyFrame(named.keyFrame(now), now), "a node that is not an Ed25519 key")
        assertEq(TrustState.APPROVED, receiver.acceptKeyFrame(genuine, now)?.state)
        assertEq(text("node"), receiver.trust.record(alice.keyId)?.node)
        val strict = RoomSeal.create(room, "direct:1", SealPolicy.STRICT, "bob", ChatDeviceKeys.fromSeed(ByteArray(32) { 4 }), clock = { now })
        expect(strict.acceptKeyFrame(unsigned, now) != null, "strict rooms keep the v3 rules")
    }

    fun testADeviceIdentityIsItsOwnNodeKey() {
        val identity = Identity.fromSeed(ChatV3.unb64(text("nodeSeed"))!!)
        val bound = ChatNodeKey.of(identity)
        assertEq(text("node"), bound.node)
        assertEq(identity.nodeId, bound.node)
        val keys = identityKeys()
        assertEq(text("nsig"), ChatV3.b64u(bound.sign(ChatV3.nodeMessage(room, keys.kid, keys.idkText, acct))))
    }
}
