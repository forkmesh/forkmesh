package com.forkmesh.nativeapp.security

import com.forkmesh.nativeapp.crypto.AesGcm
import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.crypto.Pbkdf2
import com.forkmesh.nativeapp.crypto.Utf8
import com.forkmesh.nativeapp.crypto.dartNumEquals
import com.forkmesh.nativeapp.json.Json

object PortableIdentity {
    const val TYPE = "forkmesh.portable-identity"
    const val SCHEMA_VERSION = 1
    private const val AAD = "forkmesh.portable-identity/v1"
    private const val ITERATIONS = 600000
    private const val MAX_PLAINTEXT_BYTES = 128 * 1024

    class FormatException(message: String) : RuntimeException(message)

    private val B64URL_TEXT = Regex("[A-Za-z0-9_-]+")

    private fun b64url(bytes: ByteArray): String = B64.encodeUrlNoPadding(bytes)

    private fun decodeB64url(value: Any?): ByteArray {
        if (value !is String || !B64URL_TEXT.matches(value)) throw FormatException("Invalid base64url.")
        val lastBlock = (value.length - 1) % 4 + 1
        if (lastBlock == 1) throw FormatException("Invalid base64 encoding length ")
        val decoded = try {
            B64.decode(value + "=".repeat(4 - lastBlock))
        } catch (error: IllegalArgumentException) {
            throw FormatException(error.message ?: "Invalid base64url.")
        }
        if (b64url(decoded) != value) throw FormatException("Non-canonical base64url.")
        return decoded
    }

    private fun checkPassphrase(passphrase: String) {
        if (passphrase.length < 12) {
            throw IllegalArgumentException("Use a recovery passphrase of at least 12 characters.")
        }
    }

    private fun derive(passphrase: String, salt: ByteArray): ByteArray =
        Pbkdf2.hmacSha256(Utf8.encode(passphrase), salt, ITERATIONS, 32)

    fun export(identity: Map<String, Any?>, passphrase: String, createdAt: Long? = null): Map<String, Any?> {
        checkPassphrase(passphrase)
        val plaintext = Utf8.encode(Json.stringify(identity))
        if (plaintext.isEmpty() || plaintext.size > MAX_PLAINTEXT_BYTES) {
            throw IllegalArgumentException("Identity payload is empty or too large.")
        }
        val salt = Bytes.random(16)
        val nonce = Bytes.random(12)
        val key = derive(passphrase, salt)
        val box = AesGcm.encrypt(plaintext, key, nonce, Utf8.encode(AAD))
        Bytes.wipe(key, plaintext)
        val ciphertext = Bytes.concat(box.cipherText, box.mac)
        return linkedMapOf(
            "type" to TYPE,
            "schemaVersion" to SCHEMA_VERSION.toLong(),
            "createdAt" to (createdAt ?: System.currentTimeMillis()),
            "kdf" to linkedMapOf(
                "name" to "PBKDF2-HMAC-SHA-256",
                "iterations" to ITERATIONS.toLong(),
                "salt" to b64url(salt),
            ),
            "encryption" to linkedMapOf(
                "name" to "AES-256-GCM",
                "iv" to b64url(nonce),
                "ciphertext" to b64url(ciphertext),
            ),
        )
    }

    fun import(container: Map<String, Any?>, passphrase: String): Map<String, Any?> {
        checkPassphrase(passphrase)
        val kdf = container["kdf"]
        val encryption = container["encryption"]
        if (container["type"] != TYPE ||
            !dartNumEquals(container["schemaVersion"], SCHEMA_VERSION.toLong()) ||
            kdf !is Map<*, *> ||
            encryption !is Map<*, *> ||
            kdf["name"] != "PBKDF2-HMAC-SHA-256" ||
            !dartNumEquals(kdf["iterations"], ITERATIONS.toLong()) ||
            encryption["name"] != "AES-256-GCM"
        ) {
            throw FormatException("Unsupported portable identity format.")
        }
        val salt = decodeB64url(kdf["salt"])
        val nonce = decodeB64url(encryption["iv"])
        val combined = decodeB64url(encryption["ciphertext"])
        if (salt.size != 16 || nonce.size != 12 || combined.size < 17 || combined.size > MAX_PLAINTEXT_BYTES + 16) {
            throw FormatException("Invalid portable identity encryption parameters.")
        }
        try {
            val key = derive(passphrase, salt)
            val clear = AesGcm.decrypt(
                combined.copyOfRange(0, combined.size - 16),
                nonce,
                combined.copyOfRange(combined.size - 16, combined.size),
                key,
                Utf8.encode(AAD),
            )
            Bytes.wipe(key)
            val result = Json.parse(Utf8.decode(clear))
            if (result !is Map<*, *>) throw FormatException("Invalid portable identity payload.")
            if (result.keys.any { it !is String }) throw FormatException("Invalid portable identity payload.")
            @Suppress("UNCHECKED_CAST")
            return LinkedHashMap(result as Map<String, Any?>)
        } catch (_: Exception) {
            throw FormatException("Recovery passphrase is incorrect or the file was changed.")
        }
    }
}
