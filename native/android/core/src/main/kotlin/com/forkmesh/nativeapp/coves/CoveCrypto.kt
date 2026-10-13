package com.forkmesh.nativeapp.coves

import com.forkmesh.nativeapp.crypto.AesGcm
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.crypto.Pbkdf2
import com.forkmesh.nativeapp.json.jsonText

data class CoveCipher(val nonce: String, val tag: String, val body: String) {
    fun toJson(): Map<String, Any?> = linkedMapOf("body" to body, "nonce" to nonce, "tag" to tag)

    companion object {
        fun fromJson(json: Map<*, *>?): CoveCipher? {
            if (json == null) return null
            val nonce = (json["nonce"] as? String).orEmpty()
            val tag = (json["tag"] as? String).orEmpty()
            val body = (json["body"] as? String).orEmpty()
            if (nonce.isEmpty() && tag.isEmpty() && body.isEmpty()) return null
            return CoveCipher(nonce, tag, body)
        }
    }
}

class CoveCryptoException(message: String) : Exception(message) {
    companion object {
        val passwordRequired get() = CoveCryptoException("A password is required.")
        val missingParameters get() = CoveCryptoException("The cove is missing its key-derivation parameters.")
        val badKeyLength get() = CoveCryptoException("A cove content key must be exactly 32 bytes.")
        val tooLarge get() = CoveCryptoException("The cove is larger than 4 MB.")
        val encryptFailed get() = CoveCryptoException("Could not encrypt the cove.")
    }
}

class CoveCrypto private constructor(private val key: ByteArray) {
    fun encrypt(plaintext: ByteArray, nonce: ByteArray? = null): CoveCipher {
        if (plaintext.size > MAX_PLAIN_BYTES) throw CoveCryptoException.tooLarge
        val box = try {
            AesGcm.encrypt(plaintext, key, nonce ?: Bytes.random(NONCE_BYTES))
        } catch (_: Exception) {
            throw CoveCryptoException.encryptFailed
        }
        return CoveCipher(CoveBase64.encode(box.nonce), CoveBase64.encode(box.mac), CoveBase64.encode(box.cipherText))
    }

    fun decrypt(cipher: CoveCipher): ByteArray? {
        val nonce = CoveBase64.decode(cipher.nonce)
        val tag = CoveBase64.decode(cipher.tag)
        val body = CoveBase64.decode(cipher.body)
        if (nonce.size != NONCE_BYTES || tag.size != TAG_BYTES || body.isEmpty() || body.size > MAX_PLAIN_BYTES) return null
        return AesGcm.decryptOrNull(body, nonce, tag, key)
    }

    companion object {
        const val DEFAULT_ROUNDS = 600_000
        const val KEY_BYTES = 32
        const val NONCE_BYTES = 12
        const val TAG_BYTES = 16
        const val SALT_BYTES = 16
        const val MAX_PLAIN_BYTES = 4 * 1024 * 1024

        fun withPassword(password: String, salt: ByteArray, rounds: Int): CoveCrypto {
            if (password.isEmpty()) throw CoveCryptoException.passwordRequired
            if (salt.isEmpty() || rounds <= 0) throw CoveCryptoException.missingParameters
            return CoveCrypto(deriveKey(password, salt, rounds))
        }

        fun withKey(key: ByteArray): CoveCrypto {
            if (key.size != KEY_BYTES) throw CoveCryptoException.badKeyLength
            return CoveCrypto(key.copyOf())
        }

        fun deriveKey(password: String, salt: ByteArray, rounds: Int): ByteArray =
            Pbkdf2.hmacSha256(password.toByteArray(Charsets.UTF_8), salt, rounds, KEY_BYTES)

        fun randomBytes(count: Int): ByteArray = Bytes.random(count)
        fun randomSalt(): ByteArray = randomBytes(SALT_BYTES)
        fun randomKey(): ByteArray = randomBytes(KEY_BYTES)
    }
}

object CoveBase64 {
    fun encode(bytes: ByteArray): String = java.util.Base64.getEncoder().encodeToString(bytes)

    fun decode(text: String): ByteArray {
        val cleaned = StringBuilder()
        for (c in text) {
            if (c.isWhitespace()) continue
            if (c.isLetterOrDigit() && c.code < 128 || c == '+' || c == '/') cleaned.append(c)
        }
        val remainder = cleaned.length % 4
        if (remainder == 1) return ByteArray(0)
        if (remainder != 0) repeat(4 - remainder) { cleaned.append('=') }
        return try {
            java.util.Base64.getDecoder().decode(cleaned.toString())
        } catch (_: IllegalArgumentException) {
            ByteArray(0)
        }
    }
}

object CoveJson {
    fun pretty(value: Any?, indent: String = "    "): String = StringBuilder().also { write(it, value, indent, 0) }.toString()

    private fun newline(out: StringBuilder, indent: String, depth: Int) {
        out.append('\n')
        repeat(depth) { out.append(indent) }
    }

    private fun write(out: StringBuilder, value: Any?, indent: String, depth: Int) {
        when (value) {
            is Map<*, *> -> {
                if (value.isEmpty()) {
                    out.append("{}")
                    return
                }
                out.append('{')
                val keys = value.keys.map { jsonText(it) }.sorted()
                keys.forEachIndexed { index, key ->
                    if (index > 0) out.append(',')
                    newline(out, indent, depth + 1)
                    out.append(com.forkmesh.nativeapp.json.Json.stringify(key))
                    out.append(": ")
                    write(out, value[key], indent, depth + 1)
                }
                newline(out, indent, depth)
                out.append('}')
            }
            is Iterable<*> -> {
                val items = value.toList()
                if (items.isEmpty()) {
                    out.append("[]")
                    return
                }
                out.append('[')
                items.forEachIndexed { index, item ->
                    if (index > 0) out.append(',')
                    newline(out, indent, depth + 1)
                    write(out, item, indent, depth + 1)
                }
                newline(out, indent, depth)
                out.append(']')
            }
            else -> out.append(com.forkmesh.nativeapp.json.Json.stringify(value))
        }
    }
}
