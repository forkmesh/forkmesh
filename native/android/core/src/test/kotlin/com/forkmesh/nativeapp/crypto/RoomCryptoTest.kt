package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.resourceText

class RoomCryptoTest {
    fun testReconnectsReuseTheDerivedRoomKeyInsteadOfRerunningPbkdf2() {
        val first = RoomCrypto.withPassphrase("lobby", "phrase")
        val again = RoomCrypto.withPassphrase(" lobby ", "phrase")
        val otherRoom = RoomCrypto.withPassphrase("other", "phrase")
        val otherPhrase = RoomCrypto.withPassphrase("lobby", "different")

        expect(first === again)
        expect(first !== otherRoom)
        expect(first !== otherPhrase)

        val envelope = first.encrypt(mapOf("type" to "presence", "id" to "p1"))
        assertEq(mapOf("type" to "presence", "id" to "p1"), again.decrypt(envelope))
        assertEq(null, otherRoom.decrypt(envelope))
        assertEq(null, otherPhrase.decrypt(envelope))
    }

    @Suppress("UNCHECKED_CAST")
    fun testDecryptsEnvelopesWrittenByTheWebClient() {
        val vectors = Json.parse(resourceText("crypto/web-client-vectors.json")) as Map<String, Any?>
        for (case in vectors["roomCrypto"] as List<Map<String, Any?>>) {
            val room = case["room"] as String
            val crypto = if (case["shared"] == true) RoomCrypto.shared(room) else RoomCrypto.withPassphrase(room, case["passphrase"] as String)
            assertEq(case["plain"], crypto.decrypt(case["envelope"] as Map<String, Any?>))
        }
    }

    fun testEnvelopeFormatAndMalformedInput() {
        val crypto = RoomCrypto.withPassphrase("lobby", "phrase")
        val envelope = crypto.encrypt(mapOf("type" to "chat", "text" to "é"))
        assertEq(listOf("kind", "v", "nonce", "tag", "body"), envelope.keys.toList())
        assertEq("cipher", envelope["kind"])
        assertEq(1L, envelope["v"])
        assertEq(12, java.util.Base64.getDecoder().decode(envelope["nonce"] as String).size)
        assertEq(16, java.util.Base64.getDecoder().decode(envelope["tag"] as String).size)
        expect(!(envelope["body"] as String).contains("é"))

        fun mutate(change: (MutableMap<String, Any?>) -> Unit): Map<String, Any?> = LinkedHashMap(envelope).also(change)
        assertEq(null, crypto.decrypt(mutate { it["kind"] = "plain" }))
        assertEq(null, crypto.decrypt(mutate { it["nonce"] = "AAAAAAAAAAAAAAAAAAAAAAAA" }))
        assertEq(null, crypto.decrypt(mutate { it["tag"] = B64.encode(ByteArray(15)) }))
        assertEq(null, crypto.decrypt(mutate { it["tag"] = (envelope["tag"] as String).trimEnd('=') }))
        assertEq(null, crypto.decrypt(mutate { it.remove("body") }))
        assertEq(null, crypto.decrypt(mutate { it["body"] = B64.encode(B64.decode(envelope["body"] as String).also { b -> b[0] = (b[0].toInt() xor 1).toByte() }) }))
        assertEq(mapOf("type" to "chat", "text" to "é"), crypto.decrypt(mutate { it["v"] = 9L }))
        assertEq(mapOf("type" to "chat", "text" to "é"), crypto.decrypt(mutate { it["body"] = (envelope["body"] as String).replace('+', '-').replace('/', '_') }))
    }

    fun testHexHelpers() {
        assertEq("00ff10", hex(byteArrayOf(0, -1, 16)))
        assertEq("00ff10", hex(bytesFromHex("00FF10")))
        assertEq(1, bytesFromHex("abc").size)
    }
}
