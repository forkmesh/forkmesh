package com.forkmesh.nativeapp.crypto

import java.security.GeneralSecurityException
import java.security.MessageDigest
import javax.crypto.Mac
import javax.crypto.SecretKeyFactory
import javax.crypto.spec.PBEKeySpec
import javax.crypto.spec.SecretKeySpec

object Sha256 {
    const val BYTES = 32

    fun hash(vararg parts: ByteArray): ByteArray {
        val digest = MessageDigest.getInstance("SHA-256")
        for (part in parts) digest.update(part)
        return digest.digest()
    }

    fun hex(vararg parts: ByteArray): String = Bytes.hex(hash(*parts))
}

object Sha512 {
    const val BYTES = 64

    private val local = ThreadLocal.withInitial { MessageDigest.getInstance("SHA-512") }

    internal fun digest(): MessageDigest = local.get().also { it.reset() }

    fun hash(vararg parts: ByteArray): ByteArray {
        val digest = digest()
        for (part in parts) digest.update(part)
        return digest.digest()
    }
}

object HmacSha256 {
    const val BYTES = 32

    internal fun newMac(key: ByteArray): Mac = Mac.getInstance("HmacSHA256").also {
        it.init(SecretKeySpec(if (key.isEmpty()) ByteArray(1) else key, "HmacSHA256"))
    }

    fun mac(key: ByteArray, vararg data: ByteArray): ByteArray {
        val mac = newMac(key)
        for (part in data) mac.update(part)
        return mac.doFinal()
    }
}

object Hkdf {
    fun sha256(ikm: ByteArray, salt: ByteArray, info: ByteArray, length: Int): ByteArray {
        require(length in 1..255 * HmacSha256.BYTES) { "HKDF output length out of range" }
        val prk = HmacSha256.mac(salt, ikm)
        val mac = HmacSha256.newMac(prk)
        val out = ByteArray(length)
        var previous = ByteArray(0)
        var offset = 0
        var counter = 1
        while (offset < length) {
            mac.update(previous)
            mac.update(info)
            mac.update(counter.toByte())
            previous = mac.doFinal()
            val take = minOf(previous.size, length - offset)
            System.arraycopy(previous, 0, out, offset, take)
            offset += take
            counter++
        }
        Bytes.wipe(prk, previous)
        return out
    }
}

object Pbkdf2 {
    fun hmacSha256(password: ByteArray, salt: ByteArray, iterations: Int, length: Int): ByteArray {
        require(iterations > 0 && length > 0) { "PBKDF2 needs positive iterations and length" }
        val asciiPassword = password.isNotEmpty() && password.all { it in 1..127 }
        if (asciiPassword && salt.isNotEmpty()) {
            val chars = CharArray(password.size) { password[it].toInt().toChar() }
            val spec = PBEKeySpec(chars, salt, iterations, length * 8)
            try {
                return SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256").generateSecret(spec).encoded
            } catch (_: GeneralSecurityException) {
            } finally {
                spec.clearPassword()
                chars.fill('\u0000')
            }
        }
        return hmacSha256Portable(password, salt, iterations, length)
    }

    internal fun hmacSha256Portable(password: ByteArray, salt: ByteArray, iterations: Int, length: Int): ByteArray {
        val mac = HmacSha256.newMac(password)
        val out = ByteArray(length)
        val u = ByteArray(HmacSha256.BYTES)
        val block = ByteArray(HmacSha256.BYTES)
        var offset = 0
        var index = 1
        while (offset < length) {
            mac.update(salt)
            mac.update((index ushr 24).toByte())
            mac.update((index ushr 16).toByte())
            mac.update((index ushr 8).toByte())
            mac.update(index.toByte())
            mac.doFinal(u, 0)
            System.arraycopy(u, 0, block, 0, u.size)
            for (round in 1 until iterations) {
                mac.update(u)
                mac.doFinal(u, 0)
                for (i in block.indices) block[i] = (block[i].toInt() xor u[i].toInt()).toByte()
            }
            val take = minOf(block.size, length - offset)
            System.arraycopy(block, 0, out, offset, take)
            offset += take
            index++
        }
        Bytes.wipe(u, block)
        return out
    }
}
