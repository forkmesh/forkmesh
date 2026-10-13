package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class RoomSealCryptoTest {
    private val vectorRoom = "world-general"
    private val senderPrivate = "UE5ZIyTdbSCtuDSqkI3g5TnXBMw1nx4Oz1BfmvE6CGc"
    private val receiverPrivate = "cJMcO0LKgE1B6DRDn8z2mCcgt-cKyuWEtafYL2hFjHk"
    private val senderKeyId = "SQAm1SbhWWBxNiSt"
    private val receiverKeyId = "PA22MdT_F-KVxa5L"
    private val vectorPlain = mapOf("type" to "chat", "id" to "seal-vector-2", "sender" to "ada", "text" to "automatic beyond the relay")

    private fun vectorFrame(): Map<String, Any?> = linkedMapOf(
        "type" to "sealed",
        "v" to 2L,
        "kid" to senderKeyId,
        "pub" to "SJXRaOihp8_2nTX69yDoRqkDhapmBc30VI8lARlbQTw",
        "nonce" to "43+zI9tUg9t5SdVG",
        "tag" to "D9qgU0oVQpoOY05RaI3xDg==",
        "body" to "Jn79ws7IdrSi5kFmmLHAB7yUUuARxKFeLZKdzMQBoHbghdPyQgEq8OZanyL0hTh/" +
            "TYbavVieT2DhVLLZq2F0kH1yeAP7JdwPDqPysTZCETMtZYDTKBLr",
        "keys" to listOf(
            linkedMapOf(
                "kid" to receiverKeyId,
                "nonce" to "dIYd3AXET2IQhRjH",
                "tag" to "wga2/Tmek1r/0u9oXq4ckw==",
                "key" to "pNMivvZuaR6ZEu3V/GkZOVYDlSP4Lr4MtsG0mIwtOMs=",
            ),
            linkedMapOf(
                "kid" to senderKeyId,
                "nonce" to "5vZE4C5wbgSbNENA",
                "tag" to "eiRSoFEo1m5rBwQ0jSgiEw==",
                "key" to "K7BObyf/R3vByKsOWMJQWiRDBb02WFDP3fy4cGaJ5O8=",
            ),
        ),
    )

    private fun b64url(value: String): ByteArray = B64.decode("$value=")

    private fun urlNoPad(bytes: ByteArray): String = java.util.Base64.getUrlEncoder().withoutPadding().encodeToString(bytes)

    fun testMobileOpensTheBrowserV2RecipientEnvelope() {
        val receiver = RoomSeal.fromPrivateKey(vectorRoom, b64url(receiverPrivate))
        assertEq(receiverKeyId, receiver.keyId)
        expect(RoomSeal.isSealedFrame(vectorFrame()))
        assertEq(senderKeyId, RoomSeal.frameKeyId(vectorFrame()))
        assertEq(vectorPlain, receiver.unseal(vectorFrame()))

        val sender = RoomSeal.fromPrivateKey(vectorRoom, b64url(senderPrivate))
        assertEq(senderKeyId, sender.keyId)
        assertEq(vectorPlain, sender.unseal(vectorFrame()))

        val stranger = RoomSeal.fromPrivateKey(vectorRoom, ByteArray(32) { (it + 1).toByte() })
        assertEq(null, stranger.unseal(vectorFrame()))
        val wrongRoom = RoomSeal.fromPrivateKey("chat-channel:42:v1", b64url(receiverPrivate))
        assertEq(null, wrongRoom.unseal(vectorFrame()))
        assertEq(vectorPlain, RoomSeal.fromPrivateKey("  $vectorRoom\n", b64url(receiverPrivate)).unseal(vectorFrame()))
        @Suppress("UNCHECKED_CAST")
        assertEq(vectorPlain, receiver.unseal(Json.parse(Json.stringify(vectorFrame())) as Map<String, Any?>))
    }

    fun testDevicesDiscoverEachOtherAndWrapWithoutAPhrase() {
        val alice = RoomSeal.fromPrivateKey(vectorRoom, b64url(senderPrivate))
        val bob = RoomSeal.fromPrivateKey(vectorRoom, b64url(receiverPrivate))
        expect(RoomSeal.isKeyFrame(alice.keyFrame()))
        expect(alice.acceptKeyFrame(bob.keyFrame()))
        expect(bob.acceptKeyFrame(alice.keyFrame()))
        expect(!bob.acceptKeyFrame(alice.keyFrame()), "a repeated advertisement is not new")
        expect(!alice.acceptKeyFrame(alice.keyFrame()), "a device never lists itself")
        assertEq(1, alice.peerCount)
        assertEq(1, bob.peerCount)

        val frame = alice.seal(mapOf("type" to "chat", "text" to "no setup required"))
        expect(!Json.stringify(frame).contains("no setup required"))
        val keys = frame["keys"] as List<*>
        assertEq(2, keys.size)
        assertEq(listOf(receiverKeyId, senderKeyId).sorted(), keys.map { (it as Map<*, *>)["kid"] })
        assertEq("no setup required", bob.unseal(frame)?.get("text"))
        assertEq("no setup required", alice.unseal(frame)?.get("text"))

        val relabelled = LinkedHashMap(frame).also { it["kid"] = receiverKeyId }
        assertEq(null, bob.unseal(relabelled))
        expect(!RoomSeal.isSealedFrame(mapOf("type" to "chat", "text" to "plaintext")))
        expect(!RoomSeal.isSealedFrame(mapOf("type" to "sealed", "v" to 2L, "kid" to "short")))
        expect(RoomSeal.isSealedFrame(LinkedHashMap(frame).also { it["v"] = 2.0 }), "Dart num equality accepts 2.0")
        expect(!RoomSeal.isSealedFrame(LinkedHashMap(frame).also { it["keys"] = emptyList<Any>() }))
        assertEq("", RoomSeal.frameKeyId(mapOf("type" to "chat")))
    }

    fun testTheDeviceKeyIsGeneratedAndRetainedAutomatically() {
        val store = MemorySecretStore()
        val first = RoomSeal.loadOrCreate(vectorRoom, storageScope = "wss://relay-one.example", secureStore = store)
        val peer = RoomSeal.fromPrivateKey(vectorRoom, b64url(senderPrivate))
        first.acceptKeyFrame(peer.keyFrame())
        val second = RoomSeal.loadOrCreate(vectorRoom, storageScope = "wss://relay-one.example", secureStore = store)
        val otherController = RoomSeal.loadOrCreate(vectorRoom, storageScope = "wss://relay-two.example", secureStore = store)
        expect(first.keyId.isNotEmpty())
        assertEq(first.keyId, second.keyId)
        assertEq(1, second.peerCount)
        assertEq(first.keyId, otherController.keyId)
        assertEq(0, otherController.peerCount)
        expect(first.shouldPersistAdvertisement(now = 100000000))
        expect(!second.shouldPersistAdvertisement(now = 100000001))
        expect(otherController.shouldPersistAdvertisement(now = 100000001))
        assertFails<IllegalArgumentException> { RoomSeal.loadOrCreate("   ", secureStore = store) }
        assertFails<IllegalArgumentException> { RoomSeal.fromPrivateKey(vectorRoom, ByteArray(31)) }
    }

    fun testStorageMatchesFlutter() {
        val store = MemorySecretStore()
        val seal = RoomSeal.loadOrCreate(vectorRoom, storageScope = " wss://relay-one.example ", secureStore = store)
        val storedPrivate = store.read(RoomSeal.PRIVATE_KEY_STORE_KEY)!!
        assertEq(44, storedPrivate.length)
        val privateKey = java.util.Base64.getDecoder().decode(storedPrivate)
        expect((privateKey[0].toInt() and 7) == 0 && (privateKey[31].toInt() and 0xC0) == 0x40, "stored clamped, like Dart")
        assertEq(urlNoPad(Sha256.hash(X25519.publicKey(privateKey))).substring(0, 16), seal.keyId)

        val peer = RoomSeal.fromPrivateKey(vectorRoom, b64url(senderPrivate))
        expect(seal.acceptKeyFrame(peer.keyFrame()))
        val suffix = urlNoPad("wss://relay-one.example\n$vectorRoom".toByteArray())
        assertEq(
            """[{"kid":"$senderKeyId","pub":"SJXRaOihp8_2nTX69yDoRqkDhapmBc30VI8lARlbQTw"}]""",
            store.read("chat/e2ee/peersV2/$suffix"),
        )
        expect(seal.shouldPersistAdvertisement(now = 1783000000000))
        assertEq("1783000000000", store.read("chat/e2ee/announcedV2/$suffix"))

        val flutterStore = MemorySecretStore(
            mapOf(
                RoomSeal.PRIVATE_KEY_STORE_KEY to java.util.Base64.getEncoder().encodeToString(b64url(receiverPrivate)),
                "chat/e2ee/peersV2/${urlNoPad("$vectorRoom\n$vectorRoom".toByteArray())}" to
                    """[{"kid":"$senderKeyId","pub":"SJXRaOihp8_2nTX69yDoRqkDhapmBc30VI8lARlbQTw"},{"kid":"bogus","pub":"x"},7]""",
                "chat/e2ee/announcedV2/${urlNoPad("$vectorRoom\n$vectorRoom".toByteArray())}" to " 100 ",
            ),
        )
        val restored = RoomSeal.loadOrCreate(vectorRoom, secureStore = flutterStore)
        assertEq(receiverKeyId, restored.keyId)
        assertEq(1, restored.peerCount)
        expect(!restored.shouldPersistAdvertisement(now = 100 + 1000), "Dart int.tryParse trims whitespace")
        assertEq(vectorPlain, restored.unseal(vectorFrame()))
    }

    fun testPeerListIsCappedAndEvictsTheOldest() {
        val store = MemorySecretStore()
        val seal = RoomSeal.loadOrCreate(vectorRoom, secureStore = store)
        val peers = List(70) { RoomSeal.fromPrivateKey(vectorRoom, X25519.generatePrivateKey()) }
        peers.forEach { expect(seal.acceptKeyFrame(it.keyFrame())) }
        assertEq(63, seal.peerCount)
        val frame = seal.seal(mapOf("type" to "chat", "text" to "to many"))
        assertEq(64, (frame["keys"] as List<*>).size)
        assertEq(null, peers.first().unseal(frame), "the oldest peer was evicted")
        assertEq("to many", peers.last().unseal(frame)?.get("text"))
        assertEq(63, RoomSeal.loadOrCreate(vectorRoom, secureStore = store).peerCount)
    }

    fun testUnsealRemembersSendersAndRejectsMalformedFrames() {
        val alice = RoomSeal.fromPrivateKey(vectorRoom, b64url(senderPrivate))
        val bob = RoomSeal.fromPrivateKey(vectorRoom, b64url(receiverPrivate))
        alice.acceptKeyFrame(bob.keyFrame())
        val frame = alice.seal(mapOf("type" to "dm", "text" to "hi bob"))
        assertEq(0, bob.peerCount)
        assertEq("hi bob", bob.unseal(frame)?.get("text"))
        assertEq(1, bob.peerCount, "a verified sender becomes a peer")

        fun mutate(change: (MutableMap<String, Any?>) -> Unit): Map<String, Any?> = LinkedHashMap(frame).also(change)
        fun mutateWrap(change: (MutableMap<String, Any?>) -> Unit): Map<String, Any?> = mutate { copy ->
            copy["keys"] = (frame["keys"] as List<*>).map { entry -> LinkedHashMap(entry as Map<String, Any?>).also(change) }
        }
        for (bad in listOf(
            mutate { it["nonce"] = "AAAA" },
            mutate { it["tag"] = "not base64!" },
            mutate { it["body"] = "" },
            mutate { it["pub"] = "SJXRaOihp8_2nTX69yDoRqkDhapmBc30VI8lARlbQTA" },
            mutate { it["body"] = B64.encode(B64.decode(frame["body"] as String).also { b -> b[0] = (b[0].toInt() xor 1).toByte() }) },
            mutateWrap { it["key"] = B64.encode(ByteArray(31)) },
            mutateWrap { it["tag"] = B64.encode(ByteArray(16)) },
            mutateWrap { it["nonce"] = 12 },
            mutate { it["keys"] = listOf("not a map") },
        )) {
            assertEq(null, bob.unseal(bad))
        }
    }
}
