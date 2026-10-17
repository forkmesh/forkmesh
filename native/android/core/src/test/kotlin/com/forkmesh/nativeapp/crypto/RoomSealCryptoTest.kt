package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.account.projects.ScopedSecretStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class RoomSealCryptoTest {
    private val room = "world-general"
    private val now = 1_790_000_000_000L

    private fun device(acct: String, policy: SealPolicy = SealPolicy.OPEN): RoomSeal =
        RoomSeal.loadOrCreate(room, scope = room, policy = policy, acct = acct, secureStore = MemorySecretStore(), clock = { now })

    private fun pair(): Pair<RoomSeal, RoomSeal> {
        val alice = device("alice-node")
        val bob = device("bob-node")
        expect(alice.acceptKeyFrame(bob.keyFrame()) != null)
        expect(bob.acceptKeyFrame(alice.keyFrame()) != null)
        return alice to bob
    }

    fun testFrameShapesAndLegacyFramesAreRecognised() {
        val (alice, _) = pair()
        val bundle = alice.keyFrame()
        expect(RoomSeal.isKeyFrame(bundle))
        expect(!RoomSeal.isLegacyFrame(bundle))
        val frame = alice.seal(mapOf("type" to "chat", "senderId" to "alice-node"))
        expect(RoomSeal.isSealedFrame(frame))
        assertEq(alice.keyId, RoomSeal.frameKeyId(frame))
        expect(RoomSeal.isLegacyFrame(mapOf("type" to "sealed", "v" to 2L, "kid" to "x")))
        expect(RoomSeal.isLegacyFrame(mapOf("type" to "e2ee-key", "v" to 2L)))
        expect(!RoomSeal.isSealedFrame(mapOf("type" to "sealed", "v" to 2L, "kid" to alice.keyId, "keys" to listOf(1))))
        expect(!RoomSeal.isSealedFrame(LinkedHashMap(frame).also { it["keys"] = emptyList<Any>() }))
        expect(!RoomSeal.isSealedFrame(LinkedHashMap(frame).also { it["v"] = 3.0 }), "v must be an integer")
        assertEq("", RoomSeal.frameKeyId(mapOf("type" to "chat")))
    }

    fun testDevicesDiscoverEachOtherAndBothCanOpen() {
        val (alice, bob) = pair()
        assertEq(1, alice.peerCount)
        assertEq(1, bob.peerCount)
        val plain = mapOf("type" to "chat", "senderId" to "alice-node", "text" to "no setup required")
        val frame = alice.seal(plain)
        expect(!Json.stringify(frame).contains("no setup required"))
        assertEq(listOf(alice.keyId, bob.keyId).sorted(), RoomSeal.wrappedKeyIds(frame))
        assertEq("no setup required", bob.unseal(frame)?.get("text"))
        assertEq("no setup required", alice.unseal(frame)?.get("text"))
        val relabelled = LinkedHashMap(frame).also { it["kid"] = bob.keyId }
        expect(bob.open(relabelled) is SealOpen.Invalid)
    }

    fun testUnknownSendersAreHeldUntilTheirBundleArrives() {
        val alice = device("alice-node")
        val bob = device("bob-node")
        alice.acceptKeyFrame(bob.keyFrame())
        val frame = alice.seal(mapOf("type" to "chat", "senderId" to "alice-node", "text" to "early"))
        val held = bob.open(frame)
        expect(held is SealOpen.Held && !held.blocked)
        assertEq(1, bob.trust.heldCount(room))
        val outcome = bob.acceptKeyFrame(alice.keyFrame())
        assertEq(TrustState.APPROVED, outcome?.state)
        val released = bob.takeHeld(alice.keyId)
        assertEq(1, released.size)
        assertEq("early", bob.unseal(released.single().frame)?.get("text"))
        assertEq(0, bob.trust.heldCount(room))
    }

    fun testHeldFramesNeedAValidSignatureAndAnEntryForThisDevice() {
        val alice = device("alice-node")
        val bob = device("bob-node")
        val carol = device("carol-node")
        alice.acceptKeyFrame(bob.keyFrame())
        val forged = alice.seal(mapOf("type" to "chat")).let { LinkedHashMap(it).also { copy -> copy["ctr"] = 99L } }
        expect(bob.open(forged) is SealOpen.Held)
        assertEq(0, bob.trust.heldCount(room), "a frame whose signature fails is not kept")
        expect(carol.open(alice.seal(mapOf("type" to "chat"))) is SealOpen.Held)
        assertEq(0, carol.trust.heldCount(room), "a frame with no entry for this device is not kept")
        expect(bob.open(alice.seal(mapOf("type" to "chat"))) is SealOpen.Held)
        assertEq(1, bob.trust.heldCount(room))
    }

    fun testMalformedFramesAreInvalid() {
        val (alice, bob) = pair()
        val frame = alice.seal(mapOf("type" to "chat", "senderId" to "alice-node", "text" to "hi bob"))
        fun mutate(change: (MutableMap<String, Any?>) -> Unit): Map<String, Any?> = LinkedHashMap(frame).also(change)
        @Suppress("UNCHECKED_CAST")
        fun mutateWrap(change: (MutableMap<String, Any?>) -> Unit): Map<String, Any?> = mutate { copy ->
            copy["keys"] = (frame["keys"] as List<*>).map { entry -> LinkedHashMap(entry as Map<String, Any?>).also(change) }
        }
        for (bad in listOf(
            mutate { it["nonce"] = "AAAA" },
            mutate { it["tag"] = "not base64!" },
            mutate { it["ctr"] = 0L },
            mutate { it["ctr"] = 1.0 },
            mutate { it["ts"] = "now" },
            mutate { it["idk"] = ChatV3.b64u(ByteArray(32)) },
            mutate { it["sig"] = ChatV3.b64u(ByteArray(64)) },
            mutate { it["body"] = B64.encode(B64.decode(frame["body"] as String).also { b -> b[0] = (b[0].toInt() xor 1).toByte() }) },
            mutate { it["keys"] = (frame["keys"] as List<*>).reversed() },
            mutate { it["keys"] = (frame["keys"] as List<*>) + (frame["keys"] as List<*>).first() },
            mutateWrap { it["key"] = B64.encode(ByteArray(31)) },
            mutateWrap { it["ct"] = B64.encode(ByteArray(1087)) },
            mutateWrap { it["nonce"] = 12 },
            mutate { it["keys"] = listOf("not a map") },
        )) {
            expect(bob.open(bad) is SealOpen.Invalid, "malformed frame must be invalid")
        }
        expect(bob.open(frame) is SealOpen.Opened, "the untouched frame still opens")
    }

    fun testStoredIdentityIsSharedByEveryRoomOfTheStore() {
        val store = MemorySecretStore()
        val first = RoomSeal.loadOrCreate(room, scope = room, policy = SealPolicy.OPEN, acct = "me", secureStore = store, clock = { now })
        val other = RoomSeal.loadOrCreate("chat-channel:42:v1", scope = "chat-channel:42:v1", policy = SealPolicy.OPEN, acct = "me", secureStore = store, clock = { now })
        assertEq(first.keyId, other.keyId)
        val idk = ChatV3.unb64(store.read(ChatDeviceKeys.IDENTITY_STORE_KEY))!!
        assertEq(first.keyId, ChatV3.id16(Ed25519.publicKey(idk)))
        expect(store.read("chat/e2ee/x25519PrivateV2") == null)
        assertFails<IllegalArgumentException> { RoomSeal.loadOrCreate("   ", scope = "", policy = SealPolicy.OPEN, acct = "me", secureStore = store) }
    }

    fun testScopedStoresWithTheSameNamespaceShareOneDeviceKey() {
        val base = MemorySecretStore()
        val a = ChatDeviceKeys.forStore(ScopedSecretStore(base, "p/x/"))
        val b = ChatDeviceKeys.forStore(ScopedSecretStore(base, "p/x/"))
        val c = ChatDeviceKeys.forStore(ScopedSecretStore(base, "p/y/"))
        expect(a === b, "one in-memory key set per namespace, so prekeys never diverge")
        expect(a.kid != c.kid)
    }

    fun testAdvertisementIsPersistedDailyAndOnRotation() {
        val store = MemorySecretStore()
        var clock = now
        val seal = RoomSeal.loadOrCreate(room, scope = room, policy = SealPolicy.OPEN, acct = "me", secureStore = store, clock = { clock })
        expect(seal.shouldPersistAdvertisement(now))
        expect(!seal.shouldPersistAdvertisement(now + 1000))
        seal.keyFrame()
        expect(!seal.needsAdvertisement())
        clock = now + ChatV3.DAY_MS
        seal.seal(mapOf("type" to "chat"))
        expect(seal.needsAdvertisement(), "a new prekey means a new bundle")
        expect(seal.shouldPersistAdvertisement(clock))
    }

    fun testRelayDirectMessagesCanBeSealedToChosenDevices() {
        val alice = device("alice-node")
        val bob = device("bob-node")
        val carol = device("carol-node")
        for (peer in listOf(bob, carol)) {
            alice.acceptKeyFrame(peer.keyFrame())
            peer.acceptKeyFrame(alice.keyFrame())
        }
        val toBob = alice.seal(mapOf("type" to "dm", "senderId" to "alice-node", "to" to "bob-node"), only = listOf(bob.keyId))
        assertEq(listOf(alice.keyId, bob.keyId).sorted(), RoomSeal.wrappedKeyIds(toBob))
        expect(bob.open(toBob) is SealOpen.Opened)
        expect(carol.open(toBob) is SealOpen.NotForMe)
    }
}
