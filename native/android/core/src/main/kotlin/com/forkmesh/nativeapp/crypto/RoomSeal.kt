package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.security.SecretStore

class RoomSeal private constructor(
    private val roomName: String,
    private val storageScope: String,
    private val privateKey: ByteArray,
    private val publicKey: ByteArray,
    val keyId: String,
    private val store: SecretStore?,
) {
    private val peers = HashMap<String, ByteArray>()
    private val peerOrder = ArrayList<String>()
    private var lastAnnouncement = 0L

    private val pairwiseKeys = HashMap<String, ByteArray>()
    private val hkdfSalt = Sha256.hash(Utf8.encode("ForkMesh chat e2ee salt:$roomName"))

    val peerCount: Int
        @Synchronized get() = peers.size

    val peerKeyIds: List<String>
        @Synchronized get() = peerOrder.filter { peers.containsKey(it) }

    @Synchronized
    fun keyFrame(): Map<String, Any?> = linkedMapOf(
        "type" to KEY_TYPE,
        "v" to VERSION.toLong(),
        "kid" to keyId,
        "pub" to B64.encodeUrlNoPadding(publicKey),
    )

    private fun rememberPeer(id: String, publicBytes: ByteArray): Boolean {
        if (id == keyId || publicBytes.size != 32 || idFor(publicBytes) != id) return false
        val isNew = !peers.containsKey(id)
        peerOrder.remove(id)
        peerOrder.add(id)
        peers[id] = publicBytes.copyOf()
        while (peerOrder.size >= MAX_RECIPIENTS) peers.remove(peerOrder.removeAt(0))
        return isNew
    }

    @Synchronized
    fun acceptKeyFrame(frame: Map<String, Any?>): Boolean {
        if (!isKeyFrame(frame)) return false
        val id = frame["kid"] as String
        val publicBytes = publicBytes(frame["pub"] as String) ?: return false
        val isNew = rememberPeer(id, publicBytes)
        if (isNew) persistPeers()
        return isNew
    }

    private val scopeSuffix: String
        get() = B64.encodeUrlNoPadding(Utf8.encode("$storageScope\n$roomName"))

    private val peersStoreKey: String get() = "chat/e2ee/peersV2/$scopeSuffix"

    private val announcedStoreKey: String get() = "chat/e2ee/announcedV2/$scopeSuffix"

    private fun loadPeers() {
        val store = store ?: return
        try {
            val decoded = Json.parse(store.read(peersStoreKey) ?: "[]") as? List<*> ?: return
            val skip = if (decoded.size > MAX_RECIPIENTS - 1) decoded.size - (MAX_RECIPIENTS - 1) else 0
            for (value in decoded.drop(skip)) {
                if (value !is Map<*, *>) continue
                val id = jsonText(value["kid"])
                val publicBytes = publicBytes(jsonText(value["pub"]))
                if (publicBytes != null) rememberPeer(id, publicBytes)
            }
        } catch (_: Exception) {
        }
    }

    private fun persistPeers() {
        val store = store ?: return
        try {
            val values = ArrayList<Map<String, String>>()
            for (id in peerOrder) {
                val peer = peers[id]
                if (peer != null) values.add(linkedMapOf("kid" to id, "pub" to B64.encodeUrlNoPadding(peer)))
            }
            store.write(peersStoreKey, Json.stringify(values))
        } catch (_: Exception) {
        }
    }

    @Synchronized
    fun shouldPersistAdvertisement(now: Long = System.currentTimeMillis()): Boolean {
        val store = store
        var previous = lastAnnouncement
        if (store != null) {
            try {
                previous = dartIntTryParse(store.read(announcedStoreKey) ?: "") ?: 0L
            } catch (_: Exception) {
            }
        }
        if (now - previous < DURABLE_ANNOUNCEMENT_MS) return false
        lastAnnouncement = now
        if (store != null) {
            try {
                store.write(announcedStoreKey, "$now")
            } catch (_: Exception) {
            }
        }
        return true
    }

    private fun contentAad(senderKeyId: String): ByteArray =
        Utf8.encode("forkmesh-chat-e2ee-content-v$VERSION\n$roomName\n$senderKeyId")

    private fun wrapAad(senderKeyId: String, recipientKeyId: String, bodyNonce: ByteArray): ByteArray =
        Utf8.encode(
            "forkmesh-chat-e2ee-wrap-v$VERSION\n$roomName\n$senderKeyId\n" +
                "$recipientKeyId\n${B64.encodeUrlNoPadding(bodyNonce)}",
        )

    private fun pairwiseKey(peer: ByteArray, senderKeyId: String, recipientKeyId: String): ByteArray {
        val cacheKey = "$senderKeyId\n$recipientKeyId"
        pairwiseKeys[cacheKey]?.let { return it }
        if (pairwiseKeys.size >= MAX_RECIPIENTS * 4) pairwiseKeys.clear()
        val derived = derivePairwiseKey(peer, senderKeyId, recipientKeyId)
        pairwiseKeys[cacheKey] = derived
        return derived
    }

    private fun derivePairwiseKey(peer: ByteArray, senderKeyId: String, recipientKeyId: String): ByteArray {
        val shared = X25519.sharedSecret(privateKey, peer)
        if (Bytes.isAllZero(shared)) throw IllegalArgumentException("Invalid X25519 shared key.")
        return Hkdf.sha256(
            shared,
            hkdfSalt,
            Utf8.encode("forkmesh-chat-e2ee-kek-v$VERSION\n$roomName\n$senderKeyId\n$recipientKeyId"),
            32,
        )
    }

    @Synchronized
    fun seal(plain: Map<String, Any?>): Map<String, Any?> = sealFor(plain, peerOrder)

    @Synchronized
    fun sealFor(plain: Map<String, Any?>, keyIds: Collection<String>): Map<String, Any?> {
        val contentBytes = Bytes.random(32)
        val content = AesGcm.encrypt(Utf8.encode(Json.stringify(plain)), contentBytes, aad = contentAad(keyId))

        val recipients = LinkedHashMap<String, ByteArray>()
        for (id in keyIds) {
            val peer = peers[id]
            if (peer != null) recipients[id] = peer
        }
        recipients[keyId] = publicKey
        val wrappedKeys = ArrayList<Map<String, Any?>>()
        for (id in recipients.keys.sorted()) {
            val kek = pairwiseKey(recipients.getValue(id), keyId, id)
            val wrapped = AesGcm.encrypt(contentBytes, kek, aad = wrapAad(keyId, id, content.nonce))
            wrappedKeys.add(
                linkedMapOf(
                    "kid" to id,
                    "nonce" to B64.encode(wrapped.nonce),
                    "tag" to B64.encode(wrapped.mac),
                    "key" to B64.encode(wrapped.cipherText),
                ),
            )
        }
        Bytes.wipe(contentBytes)
        return linkedMapOf(
            "type" to SEALED_TYPE,
            "v" to VERSION.toLong(),
            "kid" to keyId,
            "pub" to B64.encodeUrlNoPadding(publicKey),
            "nonce" to B64.encode(content.nonce),
            "tag" to B64.encode(content.mac),
            "body" to B64.encode(content.cipherText),
            "keys" to wrappedKeys,
        )
    }

    @Synchronized
    fun unseal(frame: Map<String, Any?>): Map<String, Any?>? {
        if (!isSealedFrame(frame)) return null
        return try {
            val senderId = frame["kid"] as String
            val senderBytes = publicBytes(frame["pub"] as String)
            if (senderBytes == null || idFor(senderBytes) != senderId) return null
            var wrapped: Map<*, *>? = null
            for (value in frame["keys"] as List<*>) {
                if (value is Map<*, *> && value["kid"] == keyId) {
                    wrapped = value
                    break
                }
            }
            if (wrapped == null || wrapped.keys.any { it !is String }) return null
            val bodyNonce = B64.decode(frame["nonce"] as String)
            val bodyTag = B64.decode(frame["tag"] as String)
            val body = B64.decode(frame["body"] as String)
            val keyNonce = B64.decode(wrapped["nonce"] as String)
            val keyTag = B64.decode(wrapped["tag"] as String)
            val wrappedKey = B64.decode(wrapped["key"] as String)
            if (bodyNonce.size != NONCE_BYTES ||
                bodyTag.size != TAG_BYTES ||
                body.isEmpty() ||
                keyNonce.size != NONCE_BYTES ||
                keyTag.size != TAG_BYTES ||
                wrappedKey.size != 32
            ) {
                return null
            }
            val kek = pairwiseKey(senderBytes, senderId, keyId)
            val contentBytes = AesGcm.decrypt(wrappedKey, keyNonce, keyTag, kek, wrapAad(senderId, keyId, bodyNonce))
            if (contentBytes.size != 32) return null
            val clear = AesGcm.decrypt(body, bodyNonce, bodyTag, contentBytes, contentAad(senderId))
            @Suppress("UNCHECKED_CAST")
            val decoded = Json.parse(Utf8.decode(clear)) as? Map<String, Any?> ?: return null
            if (senderId != keyId && rememberPeer(senderId, senderBytes)) persistPeers()
            decoded
        } catch (_: Exception) {
            null
        }
    }

    companion object {
        private const val VERSION = 2
        private const val SEALED_TYPE = "sealed"
        private const val KEY_TYPE = "e2ee-key"
        private const val NONCE_BYTES = 12
        private const val TAG_BYTES = 16
        private const val KEY_ID_CHARS = 16
        private const val MAX_RECIPIENTS = 64

        const val PRIVATE_KEY_STORE_KEY = "chat/e2ee/x25519PrivateV2"
        private const val DURABLE_ANNOUNCEMENT_MS = 24L * 60 * 60 * 1000

        private val KEY_ID_PATTERN = Regex("[A-Za-z0-9_-]{16}")
        private val PUBLIC_KEY_PATTERN = Regex("[A-Za-z0-9_-]{43}")

        fun loadOrCreate(roomName: String, storageScope: String? = null, secureStore: SecretStore): RoomSeal {
            val room = dartTrim(roomName)
            if (room.isEmpty()) throw IllegalArgumentException("A chat room is required.")
            var privateKey: ByteArray? = null
            try {
                val encoded = secureStore.read(PRIVATE_KEY_STORE_KEY)
                if (!encoded.isNullOrEmpty()) {
                    val decoded = B64.decode(encoded)
                    if (decoded.size == 32) privateKey = decoded
                }
            } catch (_: Exception) {
            }
            if (privateKey == null) {
                privateKey = X25519.generatePrivateKey()
                secureStore.write(PRIVATE_KEY_STORE_KEY, B64.encode(privateKey))
            }
            val seal = fromPrivateKey(room, privateKey, storageScope, secureStore)
            synchronized(seal) { seal.loadPeers() }
            return seal
        }

        fun fromPrivateKey(
            roomName: String,
            privateKey: ByteArray,
            storageScope: String? = null,
            secureStore: SecretStore? = null,
        ): RoomSeal {
            val room = dartTrim(roomName)
            if (room.isEmpty() || privateKey.size != 32) {
                throw IllegalArgumentException("A room and 32-byte X25519 private key are required.")
            }
            val clamped = X25519.clampPrivateKey(privateKey)
            val publicKey = X25519.publicKey(clamped)
            val scope = storageScope?.let(::dartTrim)
            return RoomSeal(
                room,
                if (scope.isNullOrEmpty()) room else scope,
                clamped,
                publicKey,
                idFor(publicKey),
                secureStore,
            )
        }

        private fun idFor(publicKey: ByteArray): String =
            B64.encodeUrlNoPadding(Sha256.hash(publicKey)).substring(0, KEY_ID_CHARS)

        private fun publicBytes(value: String): ByteArray? {
            if (!PUBLIC_KEY_PATTERN.matches(value)) return null
            val bytes = B64.decodeOrNull("$value=") ?: return null
            return if (bytes.size == 32) bytes else null
        }

        fun isKeyFrame(frame: Map<String, Any?>): Boolean {
            val kid = frame["kid"]
            val pub = frame["pub"]
            return frame["type"] == KEY_TYPE &&
                dartNumEquals(frame["v"], VERSION.toLong()) &&
                kid is String && KEY_ID_PATTERN.matches(kid) &&
                pub is String && PUBLIC_KEY_PATTERN.matches(pub)
        }

        fun isSealedFrame(frame: Map<String, Any?>): Boolean {
            val kid = frame["kid"]
            val pub = frame["pub"]
            val keys = frame["keys"]
            return frame["type"] == SEALED_TYPE &&
                dartNumEquals(frame["v"], VERSION.toLong()) &&
                kid is String && KEY_ID_PATTERN.matches(kid) &&
                pub is String && PUBLIC_KEY_PATTERN.matches(pub) &&
                keys is List<*> && keys.isNotEmpty() && keys.size <= MAX_RECIPIENTS
        }

        fun frameKeyId(frame: Map<String, Any?>): String = if (isSealedFrame(frame)) frame["kid"] as String else ""

        fun wrappedKeyIds(frame: Map<String, Any?>): List<String> {
            if (!isSealedFrame(frame)) return emptyList()
            return (frame["keys"] as List<*>).mapNotNull { (it as? Map<*, *>)?.get("kid") as? String }
        }

        private fun dartIntTryParse(source: String): Long? {
            var text = dartTrim(source)
            if (text.isEmpty()) return null
            var negative = false
            if (text[0] == '+' || text[0] == '-') {
                negative = text[0] == '-'
                text = text.substring(1)
            }
            val radix = if (text.length > 2 && (text.startsWith("0x") || text.startsWith("0X"))) 16 else 10
            if (radix == 16) text = text.substring(2)
            if (text.isEmpty() || !text.all { Character.digit(it, radix) >= 0 && it.code < 128 }) return null
            return (if (negative) "-$text" else text).toLongOrNull(radix)
        }
    }
}
