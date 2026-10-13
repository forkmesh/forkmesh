package com.forkmesh.nativeapp.security

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.resourceText
import java.util.Base64

@Suppress("UNCHECKED_CAST")
class PortableIdentityCryptoTest {
    private val passphrase = "correct horse battery staple"

    fun testImportsAKitExportedByTheWebClient() {
        val vectors = Json.parse(resourceText("crypto/web-client-vectors.json")) as Map<String, Any?>
        val case = vectors["portableIdentity"] as Map<String, Any?>
        val restored = PortableIdentity.import(case["container"] as Map<String, Any?>, case["passphrase"] as String)
        assertEq(case["identity"], restored)
    }

    fun testExportMatchesTheDocumentedEnvelopeAndRoundTrips() {
        val identity = linkedMapOf<String, Any?>("v" to 1L, "ed25519Seed" to "nWGxne_9WmC6hEr0kuwsxERJxWl7MmkZcDusAxyuf2A", "label" to "Pixel ü")
        val kit = PortableIdentity.export(identity, passphrase, createdAt = 1783000000000)
        assertEq(listOf("type", "schemaVersion", "createdAt", "kdf", "encryption"), kit.keys.toList())
        assertEq("forkmesh.portable-identity", kit["type"])
        assertEq(1L, kit["schemaVersion"])
        assertEq(1783000000000L, kit["createdAt"])
        val kdf = kit["kdf"] as Map<String, Any?>
        assertEq(listOf("name", "iterations", "salt"), kdf.keys.toList())
        assertEq("PBKDF2-HMAC-SHA-256", kdf["name"])
        assertEq(600000L, kdf["iterations"])
        assertEq(16, Base64.getUrlDecoder().decode(kdf["salt"] as String).size)
        val encryption = kit["encryption"] as Map<String, Any?>
        assertEq(listOf("name", "iv", "ciphertext"), encryption.keys.toList())
        assertEq("AES-256-GCM", encryption["name"])
        assertEq(12, Base64.getUrlDecoder().decode(encryption["iv"] as String).size)
        assertEq(Json.stringify(identity).toByteArray().size + 16, Base64.getUrlDecoder().decode(encryption["ciphertext"] as String).size)
        expect(!Json.stringify(kit).contains("="), "every binary field is unpadded base64url")

        assertEq(identity, PortableIdentity.import(Json.parse(Json.stringify(kit)) as Map<String, Any?>, passphrase))
        expect(System.currentTimeMillis() - (PortableIdentity.export(identity, passphrase)["createdAt"] as Long) < 60_000)
    }

    fun testWrongPassphrasesAndTamperingFailClosed() {
        val kit = PortableIdentity.export(mapOf("seed" to "x"), passphrase, createdAt = 1)
        val wrong = assertFails<PortableIdentity.FormatException> { PortableIdentity.import(kit, "not the same passphrase") }
        assertEq("Recovery passphrase is incorrect or the file was changed.", wrong.message)

        val encryption = LinkedHashMap(kit["encryption"] as Map<String, Any?>)
        val bytes = Base64.getUrlDecoder().decode(encryption["ciphertext"] as String)
        bytes[0] = (bytes[0].toInt() xor 1).toByte()
        encryption["ciphertext"] = Base64.getUrlEncoder().withoutPadding().encodeToString(bytes)
        val tampered = LinkedHashMap(kit).also { it["encryption"] = encryption }
        assertEq("Recovery passphrase is incorrect or the file was changed.", assertFails<PortableIdentity.FormatException> { PortableIdentity.import(tampered, passphrase) }.message)

        assertFails<IllegalArgumentException> { PortableIdentity.import(kit, "short") }
        assertFails<IllegalArgumentException> { PortableIdentity.export(mapOf("a" to 1L), "short") }
        assertFails<IllegalArgumentException> { PortableIdentity.export(mapOf("blob" to "x".repeat(128 * 1024)), passphrase) }
    }

    fun testRejectsMalformedContainersBeforeDerivingAKey() {
        val kit = PortableIdentity.export(mapOf("seed" to "x"), passphrase, createdAt = 1)
        fun with(section: String?, key: String, value: Any?): Map<String, Any?> {
            val copy = LinkedHashMap(kit)
            if (section == null) {
                copy[key] = value
            } else {
                copy[section] = LinkedHashMap(kit[section] as Map<String, Any?>).also { it[key] = value }
            }
            return copy
        }
        fun message(container: Map<String, Any?>): String? =
            assertFails<PortableIdentity.FormatException> { PortableIdentity.import(container, passphrase) }.message

        assertEq("Unsupported portable identity format.", message(with(null, "type", "forkmesh.other")))
        assertEq("Unsupported portable identity format.", message(with(null, "schemaVersion", 2L)))
        assertEq("Unsupported portable identity format.", message(with("kdf", "iterations", 100000L)))
        assertEq("Unsupported portable identity format.", message(with("encryption", "name", "AES-128-GCM")))
        assertEq("Unsupported portable identity format.", message(with(null, "kdf", "pbkdf2")))
        val salt = (kit["kdf"] as Map<String, Any?>)["salt"] as String
        assertEq("Invalid base64url.", message(with("kdf", "salt", "$salt==")))
        assertEq("Invalid base64url.", message(with("kdf", "salt", "+" + salt.drop(1))))
        assertEq("Invalid encoding before padding", message(with("kdf", "salt", salt.dropLast(1) + nonCanonicalLast(salt.last()))))
        assertEq("Invalid portable identity encryption parameters.", message(with("kdf", "salt", Base64.getUrlEncoder().withoutPadding().encodeToString(ByteArray(15)))))
        assertEq("Invalid portable identity encryption parameters.", message(with("encryption", "ciphertext", Base64.getUrlEncoder().withoutPadding().encodeToString(ByteArray(16)))))
        expect(message(with("encryption", "iv", "A")) != null, "a one-character block is not base64")
    }

    private fun nonCanonicalLast(c: Char): Char {
        val alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
        return alphabet[alphabet.indexOf(c) or 1]
    }
}
