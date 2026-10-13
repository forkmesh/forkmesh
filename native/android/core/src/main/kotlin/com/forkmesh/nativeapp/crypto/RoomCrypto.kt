package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.json.Json
import java.util.concurrent.ConcurrentHashMap

class RoomCrypto private constructor(private val key: ByteArray) {
    fun encrypt(plain: Map<String, Any?>): Map<String, Any?> {
        val box = AesGcm.encrypt(Utf8.encode(Json.stringify(plain)), key)
        return linkedMapOf(
            "kind" to "cipher",
            "v" to 1L,
            "nonce" to B64.encode(box.nonce),
            "tag" to B64.encode(box.mac),
            "body" to B64.encode(box.cipherText),
        )
    }

    fun decrypt(envelope: Map<String, Any?>): Map<String, Any?>? {
        if (envelope["kind"] != "cipher") return null
        return try {
            val nonce = B64.decode(envelope["nonce"] as String)
            val tag = B64.decode(envelope["tag"] as String)
            val body = B64.decode(envelope["body"] as String)
            if (nonce.size != NONCE_BYTES || tag.size != TAG_BYTES) return null
            val clear = AesGcm.decrypt(body, nonce, tag, key)
            @Suppress("UNCHECKED_CAST")
            Json.parse(Utf8.decode(clear)) as? Map<String, Any?>
        } catch (_: Exception) {
            null
        }
    }

    companion object {
        private const val APP_ROOM_KEY = "forkmesh-shared-room-key-v1"
        private const val ROUNDS = 210000
        private const val NONCE_BYTES = 12
        private const val TAG_BYTES = 16

        private val derived = ConcurrentHashMap<String, Lazy<RoomCrypto>>()

        fun shared(roomName: String): RoomCrypto = derive(APP_ROOM_KEY, roomName)

        fun withPassphrase(roomName: String, passphrase: String): RoomCrypto = derive(passphrase, roomName)

        private fun derive(secret: String, roomName: String): RoomCrypto {
            val room = dartTrim(roomName)
            val cacheKey = hex(Sha256.hash(Utf8.encode("$room\n$secret")))
            val entry = derived.computeIfAbsent(cacheKey) { lazy { deriveUncached(secret, room) } }
            return try {
                entry.value
            } catch (error: Throwable) {
                derived.remove(cacheKey, entry)
                throw error
            }
        }

        private fun deriveUncached(secret: String, room: String): RoomCrypto =
            RoomCrypto(Pbkdf2.hmacSha256(Utf8.encode(secret), saltForRoom(room), ROUNDS, 32))

        private fun saltForRoom(roomName: String): ByteArray =
            Sha256.hash(Utf8.encode("ForkMesh room:$roomName")).copyOf(16)
    }
}

fun hex(bytes: ByteArray): String = Bytes.hex(bytes)

fun bytesFromHex(s: String): ByteArray = ByteArray(s.length / 2) { s.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
