package com.forkmesh.nativeapp.securedrive

import com.forkmesh.nativeapp.crypto.AesGcm
import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.crypto.Ed25519
import com.forkmesh.nativeapp.crypto.HmacSha256
import com.forkmesh.nativeapp.crypto.Hkdf
import com.forkmesh.nativeapp.crypto.Keccak
import com.forkmesh.nativeapp.crypto.MlKem768
import com.forkmesh.nativeapp.crypto.Sha256
import com.forkmesh.nativeapp.crypto.X25519
import com.forkmesh.nativeapp.vault.Argon2id
import java.text.Normalizer

class SecureDriveException(val code: String) : Exception(code)

internal fun fail(code: String): Nothing = throw SecureDriveException(code)

object SdCrypto {
    const val SUITE = "fmsd1"
    const val KEY_BYTES = 32
    const val MLKEM_PUBLIC_BYTES = 1184
    const val MLKEM_CIPHERTEXT_BYTES = 1088
    private const val PREFIX = "forkmesh-secure-drive/fmsd1/"
    private val XWING_LABEL = byteArrayOf(0x5c, 0x2e, 0x2f, 0x2f, 0x5e, 0x5c)
    private const val BASE32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

    fun utf8(text: String): ByteArray = text.toByteArray(Charsets.UTF_8)

    fun label(name: String): ByteArray = utf8(PREFIX + name)

    fun label(name: ByteArray): ByteArray = Bytes.concat(utf8(PREFIX), name)

    fun toB64(bytes: ByteArray): String = B64.encodeUrlNoPadding(bytes)

    fun fromB64(text: Any?): ByteArray? {
        val value = text as? String ?: return null
        for (c in value) {
            val ok = c in 'A'..'Z' || c in 'a'..'z' || c in '0'..'9' || c == '-' || c == '_'
            if (!ok) return null
        }
        if (value.length % 4 == 1) return null
        val padded = value + "=".repeat((4 - value.length % 4) % 4)
        return B64.decodeOrNull(padded.replace('-', '+').replace('_', '/'))
    }

    fun random(count: Int): ByteArray = Bytes.random(count)

    fun randomId(): String = Bytes.hex(random(16))

    fun wipe(bytes: ByteArray?) {
        bytes?.fill(0)
    }

    fun sha256(vararg parts: ByteArray): ByteArray = Sha256.hash(*parts)

    fun hex(bytes: ByteArray): String = Bytes.hex(bytes)

    fun hkdf(secret: ByteArray, info: String, length: Int = KEY_BYTES): ByteArray {
        if (secret.isEmpty()) fail("kdf_failed")
        return Hkdf.sha256(secret, ByteArray(32), label(info), length)
    }

    fun hkdf(secret: ByteArray, info: ByteArray, length: Int = KEY_BYTES): ByteArray {
        if (secret.isEmpty()) fail("kdf_failed")
        return Hkdf.sha256(secret, ByteArray(32), label(info), length)
    }

    fun hmac(key: ByteArray, message: ByteArray): ByteArray = HmacSha256.mac(key, message)

    fun constantTimeEqual(a: ByteArray, b: ByteArray): Boolean =
        a.isNotEmpty() && a.size == b.size && Bytes.constantTimeEquals(a, b)

    fun aeadSeal(key: ByteArray, nonce: ByteArray, aad: ByteArray, plain: ByteArray): ByteArray {
        if (key.size != KEY_BYTES || nonce.size != 12) fail("encryption_failed")
        val box = AesGcm.encrypt(plain, key, nonce, aad)
        return Bytes.concat(box.cipherText, box.mac)
    }

    fun aeadOpen(key: ByteArray, nonce: ByteArray, aad: ByteArray, sealed: ByteArray): ByteArray? {
        if (key.size != KEY_BYTES || nonce.size != 12 || sealed.size < 16) return null
        return AesGcm.decryptOrNull(
            sealed.copyOfRange(0, sealed.size - 16),
            nonce,
            sealed.copyOfRange(sealed.size - 16, sealed.size),
            key,
            aad,
        )
    }

    fun stretch(passphrase: String, kdf: Kdf): ByteArray {
        Kdf.fromJson(kdf.toJson(), kdf.floorMemoryKiB, kdf.floorPasses)
        val secret = utf8(Normalizer.normalize(passphrase, Normalizer.Form.NFKC))
        try {
            return Argon2id.deriveKey(
                password = secret,
                salt = kdf.salt,
                parallelism = kdf.parallelism,
                memoryKiB = kdf.memoryKiB,
                iterations = kdf.passes,
                hashLength = KEY_BYTES,
            )
        } catch (_: IllegalArgumentException) {
            fail("kdf_failed")
        } finally {
            wipe(secret)
        }
    }

    fun sign(seed: ByteArray, message: ByteArray): ByteArray {
        if (seed.size != 32) fail("signing_failed")
        return Ed25519.sign(seed, message)
    }

    fun verify(publicKey: ByteArray, message: ByteArray, signature: ByteArray): Boolean =
        publicKey.size == 32 && signature.size == 64 &&
            runCatching { Ed25519.verify(publicKey, message, signature) }.getOrDefault(false)

    fun validMlkemPublic(bytes: ByteArray): Boolean {
        if (bytes.size != MLKEM_PUBLIC_BYTES) return false
        var i = 0
        while (i + 3 <= 1152) {
            val b0 = bytes[i].toInt() and 0xff
            val b1 = bytes[i + 1].toInt() and 0xff
            val b2 = bytes[i + 2].toInt() and 0xff
            if ((b0 or ((b1 and 0x0f) shl 8)) >= 3329) return false
            if (((b1 ushr 4) or (b2 shl 4)) >= 3329) return false
            i += 3
        }
        return true
    }

    fun sealToDevice(recipient: PublicKeys, label: ByteArray, plain: ByteArray): ByteArray {
        if (!recipient.isValid()) fail("invalid_public_keys")
        val ephemeral = random(32)
        val ctX = X25519.publicKey(ephemeral)
        val ssX = X25519.sharedSecret(ephemeral, recipient.x25519)
        wipe(ephemeral)
        if (Bytes.isAllZero(ssX)) fail("invalid_public_keys")
        val encapsulation = try {
            MlKem768.encapsulate(recipient.mlkem768)
        } catch (_: IllegalArgumentException) {
            fail("invalid_public_keys")
        }
        val ctM = encapsulation.ciphertext
        val ssM = encapsulation.sharedSecret
        val shared = Keccak.sha3_256(ssM, ssX, ctX, recipient.x25519, XWING_LABEL)
        val key = hkdf(shared, Bytes.concat(utf8("seal/"), label))
        val nonce = random(12)
        val aad = Bytes.concat(label("seal/"), label, ctX, ctM)
        val body = aeadSeal(key, nonce, aad, plain)
        wipe(ssM)
        wipe(ssX)
        wipe(shared)
        wipe(key)
        return Bytes.concat(ctX, ctM, nonce, body)
    }

    fun openFromDevice(recipient: DeviceKeys, label: ByteArray, sealed: ByteArray): ByteArray? {
        if (!recipient.isValid()) return null
        if (sealed.size < 32 + MLKEM_CIPHERTEXT_BYTES + 12 + 16) return null
        val ctX = sealed.copyOfRange(0, 32)
        val ctM = sealed.copyOfRange(32, 32 + MLKEM_CIPHERTEXT_BYTES)
        val nonce = sealed.copyOfRange(32 + MLKEM_CIPHERTEXT_BYTES, 32 + MLKEM_CIPHERTEXT_BYTES + 12)
        val body = sealed.copyOfRange(32 + MLKEM_CIPHERTEXT_BYTES + 12, sealed.size)
        val myX = X25519.publicKey(recipient.x25519)
        val ssX = X25519.sharedSecret(recipient.x25519, ctX)
        if (Bytes.isAllZero(ssX)) return null
        val ssM = try {
            MlKem768.decapsulate(recipient.mlkemSecret(), ctM)
        } catch (_: IllegalArgumentException) {
            return null
        }
        val shared = Keccak.sha3_256(ssM, ssX, ctX, myX, XWING_LABEL)
        val key = hkdf(shared, Bytes.concat(utf8("seal/"), label))
        wipe(ssM)
        wipe(ssX)
        wipe(shared)
        val aad = Bytes.concat(label("seal/"), label, ctX, ctM)
        val plain = aeadOpen(key, nonce, aad, body)
        wipe(key)
        return plain
    }

    fun safetyNumber(context: ByteArray, keys: PublicKeys): String {
        val digest = sha256(label("safety"), context, keys.ed25519, keys.x25519, keys.mlkem768)
        return (0 until 4).joinToString(" ") { i ->
            var value = 0L
            for (j in 0 until 4) value = (value shl 8) or (digest[i * 4 + j].toLong() and 0xff)
            String.format(java.util.Locale.ROOT, "%05d", value % 100000)
        }
    }

    private fun base32(bytes: ByteArray): String {
        val out = StringBuilder()
        var buffer = 0L
        var bits = 0
        for (byte in bytes) {
            buffer = (buffer shl 8) or (byte.toLong() and 0xff)
            bits += 8
            while (bits >= 5) {
                out.append(BASE32[((buffer ushr (bits - 5)) and 31).toInt()])
                bits -= 5
            }
        }
        if (bits > 0) out.append(BASE32[((buffer shl (5 - bits)) and 31).toInt()])
        return out.toString()
    }

    class InviteCode(val code: String, val secret: ByteArray)

    fun newInviteCode(): InviteCode {
        val secret = random(16)
        val check = sha256(label("invite-check"), secret).copyOfRange(0, 1)
        val text = base32(Bytes.concat(secret, check)).substring(0, 27)
        return InviteCode(text.chunked(3).joinToString("-"), secret)
    }

    fun parseInviteCode(code: String): ByteArray? {
        val cleaned = StringBuilder()
        for (c in code.uppercase()) {
            when {
                c == '-' || c.isWhitespace() -> continue
                c == 'O' -> cleaned.append('0')
                c == 'I' || c == 'L' -> cleaned.append('1')
                else -> cleaned.append(c)
            }
        }
        if (cleaned.length != 27) return null
        var buffer = 0L
        var bits = 0
        val bytes = java.io.ByteArrayOutputStream()
        for (c in cleaned) {
            val index = BASE32.indexOf(c)
            if (index < 0) return null
            buffer = (buffer shl 5) or index.toLong()
            bits += 5
            if (bits >= 8) {
                bytes.write(((buffer ushr (bits - 8)) and 0xff).toInt())
                bits -= 8
            }
        }
        val secret = bytes.toByteArray()
        if (secret.size != 16) return null
        val check = sha256(label("invite-check"), secret)[0].toInt() and 0xff
        val expected = check ushr (8 - bits)
        if ((buffer and ((1L shl bits) - 1)).toInt() != expected) return null
        return secret
    }

    fun paddedSize(length: Long): Long {
        val floor = 4096L
        if (length <= floor) return floor
        var exponent = 0
        while ((1L shl (exponent + 1)) <= length) exponent++
        var bitsOfExponent = 0
        while ((1 shl bitsOfExponent) <= exponent) bitsOfExponent++
        val lastBits = exponent - bitsOfExponent
        val mask = (1L shl lastBits) - 1
        return (length + mask) and mask.inv()
    }
}

class KdfProfile(val memoryKiB: Int, val passes: Int) {
    companion object {
        val STANDARD = KdfProfile(65536, 3)
        val TEST_ONLY_WEAK = KdfProfile(64, 1)
    }
}

class Kdf(
    val memoryKiB: Int = 65536,
    val passes: Int = 3,
    val parallelism: Int = 1,
    val salt: ByteArray,
    internal val floorMemoryKiB: Int = FLOOR_MEMORY_KIB,
    internal val floorPasses: Int = FLOOR_PASSES,
) {
    fun toJson(): Map<String, Any?> = linkedMapOf(
        "algorithm" to "argon2id",
        "version" to 19L,
        "memoryKiB" to memoryKiB.toLong(),
        "passes" to passes.toLong(),
        "parallelism" to parallelism.toLong(),
        "salt" to SdCrypto.toB64(salt),
    )

    companion object {
        const val FLOOR_MEMORY_KIB = 19456
        const val FLOOR_PASSES = 2

        fun fresh(profile: KdfProfile): Kdf = Kdf(
            profile.memoryKiB,
            profile.passes,
            1,
            SdCrypto.random(16),
            minOf(FLOOR_MEMORY_KIB, profile.memoryKiB),
            minOf(FLOOR_PASSES, profile.passes),
        )

        fun fromJson(json: Any?, floorMemoryKiB: Int = FLOOR_MEMORY_KIB, floorPasses: Int = FLOOR_PASSES): Kdf {
            val map = json as? Map<*, *> ?: fail("unsafe_kdf_parameters")
            fun whole(key: String): Int? = when (val v = map[key]) {
                is Long -> if (v in Int.MIN_VALUE..Int.MAX_VALUE) v.toInt() else null
                is Int -> v
                is Double -> if (v == Math.rint(v) && Math.abs(v) < 2e9) v.toInt() else null
                else -> null
            }
            val memory = whole("memoryKiB")
            val passes = whole("passes")
            val parallelism = whole("parallelism")
            val salt = SdCrypto.fromB64(map["salt"])
            val version = map["version"]
            val versionOk = (version is Long && version == 19L) || (version is Double && version == 19.0)
            if (map["algorithm"] != "argon2id" || !versionOk || memory == null || passes == null ||
                parallelism == null || salt == null || salt.size < 16 || salt.size > 64 ||
                memory < floorMemoryKiB || memory > 1048576 || passes < floorPasses || passes > 16 ||
                parallelism < 1 || parallelism > 16
            ) {
                fail("unsafe_kdf_parameters")
            }
            return Kdf(memory, passes, parallelism, salt, floorMemoryKiB, floorPasses)
        }
    }
}

class PublicKeys(val x25519: ByteArray, val mlkem768: ByteArray, val ed25519: ByteArray) {
    fun isValid(): Boolean = x25519.size == 32 && mlkem768.size == SdCrypto.MLKEM_PUBLIC_BYTES && ed25519.size == 32

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "x25519" to SdCrypto.toB64(x25519),
        "mlkem768" to SdCrypto.toB64(mlkem768),
        "ed25519" to SdCrypto.toB64(ed25519),
    )

    companion object {
        val EMPTY = PublicKeys(ByteArray(0), ByteArray(0), ByteArray(0))

        fun fromJson(json: Any?): PublicKeys? {
            val map = json as? Map<*, *> ?: return null
            val x = SdCrypto.fromB64(map["x25519"]) ?: return null
            val m = SdCrypto.fromB64(map["mlkem768"]) ?: return null
            val e = SdCrypto.fromB64(map["ed25519"]) ?: return null
            val keys = PublicKeys(x, m, e)
            if (!keys.isValid() || !SdCrypto.validMlkemPublic(m)) return null
            return keys
        }
    }
}

class DeviceKeys(val x25519: ByteArray, val mlkemSeed: ByteArray, val ed25519: ByteArray) {
    @Volatile
    private var secret: ByteArray? = null

    fun isValid(): Boolean = x25519.size == 32 && mlkemSeed.size == 64 && ed25519.size == 32

    internal fun mlkemSecret(): ByteArray =
        secret ?: MlKem768.generateKeyPair(mlkemSeed).secretKey.also { secret = it }

    fun publicKeys(): PublicKeys {
        if (!isValid()) fail("invalid_device_keys")
        val pair = MlKem768.generateKeyPair(mlkemSeed)
        secret = pair.secretKey
        return PublicKeys(X25519.publicKey(x25519), pair.publicKey, Ed25519.publicKey(ed25519))
    }

    fun clear() {
        SdCrypto.wipe(x25519)
        SdCrypto.wipe(mlkemSeed)
        SdCrypto.wipe(ed25519)
        SdCrypto.wipe(secret)
        secret = null
    }

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "x25519" to SdCrypto.toB64(x25519),
        "mlkemSeed" to SdCrypto.toB64(mlkemSeed),
        "ed25519" to SdCrypto.toB64(ed25519),
    )

    companion object {
        val EMPTY = DeviceKeys(ByteArray(0), ByteArray(0), ByteArray(0))

        fun generate(): DeviceKeys = DeviceKeys(SdCrypto.random(32), SdCrypto.random(64), SdCrypto.random(32))

        fun fromJson(json: Any?): DeviceKeys {
            val map = json as? Map<*, *> ?: fail("invalid_device_keys")
            val keys = DeviceKeys(
                SdCrypto.fromB64(map["x25519"]) ?: fail("invalid_device_keys"),
                SdCrypto.fromB64(map["mlkemSeed"]) ?: fail("invalid_device_keys"),
                SdCrypto.fromB64(map["ed25519"]) ?: fail("invalid_device_keys"),
            )
            if (!keys.isValid()) fail("invalid_device_keys")
            return keys
        }
    }
}
