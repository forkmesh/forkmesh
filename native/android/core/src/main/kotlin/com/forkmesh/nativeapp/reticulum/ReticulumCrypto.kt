package com.forkmesh.nativeapp.reticulum

import com.forkmesh.nativeapp.crypto.Ed25519
import com.forkmesh.nativeapp.crypto.X25519
import java.io.ByteArrayOutputStream
import java.security.MessageDigest
import javax.crypto.Cipher
import javax.crypto.spec.IvParameterSpec
import javax.crypto.spec.SecretKeySpec

object RnsCrypto {
    fun x25519PublicFromPrivate(privateKey: ByteArray): ByteArray =
        if (privateKey.size != 32) ByteArray(0) else X25519.publicKey(privateKey)

    fun x25519Exchange(privateKey: ByteArray, peerPublicKey: ByteArray): ByteArray {
        if (privateKey.size != 32 || peerPublicKey.size != 32) return ByteArray(0)
        val shared = runCatching { X25519.sharedSecret(privateKey, peerPublicKey) }.getOrNull() ?: return ByteArray(0)
        return if (shared.all { it == 0.toByte() }) ByteArray(0) else shared
    }

    fun ed25519PublicFromPrivate(privateKey: ByteArray): ByteArray =
        if (privateKey.size != 32) ByteArray(0) else Ed25519.publicKey(privateKey)

    fun ed25519Sign(privateKey: ByteArray, data: ByteArray): ByteArray =
        if (privateKey.size != 32) ByteArray(0) else Ed25519.sign(privateKey, data)

    fun ed25519Verify(publicKey: ByteArray, signature: ByteArray, data: ByteArray): Boolean {
        if (publicKey.size != 32 || signature.size != Rns.SIGNATURE_LENGTH) return false
        return runCatching { Ed25519.verify(publicKey, data, signature) }.getOrDefault(false)
    }

    private fun aes(mode: Int, key: ByteArray, iv: ByteArray, input: ByteArray): ByteArray? = runCatching {
        val cipher = Cipher.getInstance("AES/CBC/PKCS5Padding")
        cipher.init(mode, SecretKeySpec(key, "AES"), IvParameterSpec(iv))
        cipher.doFinal(input)
    }.getOrNull()

    fun tokenEncrypt(key: ByteArray, plaintext: ByteArray, iv: ByteArray = ByteArray(0)): ByteArray {
        if (key.size != 64) return ByteArray(0)
        val vector = if (iv.size == 16) iv else Rns.randomBytes(16)
        val ciphertext = aes(Cipher.ENCRYPT_MODE, key.copyOfRange(32, 64), vector, plaintext) ?: return ByteArray(0)
        val signed = vector + ciphertext
        return signed + Rns.hmacSha256(key.copyOf(32), signed)
    }

    fun tokenDecrypt(key: ByteArray, token: ByteArray): ByteArray? {
        if (key.size != 64 || token.size < 16 + 16 + 32 || (token.size - 48) % 16 != 0) return null
        val mac = Rns.hmacSha256(key.copyOf(32), token.copyOf(token.size - 32))
        if (!MessageDigest.isEqual(mac, token.copyOfRange(token.size - 32, token.size))) return null
        return aes(Cipher.DECRYPT_MODE, key.copyOfRange(32, 64), token.copyOf(16), token.copyOfRange(16, token.size - 32))
    }

    fun buildProof(signingPrivateKey: ByteArray, packetHash: ByteArray, explicitProof: Boolean, destination: ByteArray, destinationType: Int): ByteArray {
        val signature = ed25519Sign(signingPrivateKey, packetHash)
        if (signature.isEmpty()) return ByteArray(0)
        return Rns.packPacket(
            Packet(
                packetType = Rns.PACKET_PROOF,
                destinationType = destinationType,
                destinationHash = destination,
                data = if (explicitProof) packetHash + signature else signature,
            ),
        )
    }

    fun validateProof(signingPublicKey: ByteArray, packetHash: ByteArray, proofData: ByteArray): Boolean = when (proofData.size) {
        Rns.FULL_HASH_LENGTH + Rns.SIGNATURE_LENGTH ->
            proofData.copyOf(Rns.FULL_HASH_LENGTH).contentEquals(packetHash) &&
                ed25519Verify(signingPublicKey, proofData.copyOfRange(Rns.FULL_HASH_LENGTH, proofData.size), packetHash)
        Rns.SIGNATURE_LENGTH -> ed25519Verify(signingPublicKey, proofData, packetHash)
        else -> false
    }

    fun msgPackArrayItems(bytes: ByteArray): List<IntArray>? {
        if (bytes.isEmpty()) return null
        val type = bytes[0].toInt() and 0xff
        val count: Int
        var offset = 1
        when {
            type and 0xf0 == 0x90 -> count = type and 0x0f
            type == 0xdc && bytes.size >= 3 -> {
                count = ((bytes[1].toInt() and 0xff) shl 8) or (bytes[2].toInt() and 0xff)
                offset = 3
            }
            else -> return null
        }
        if (count > 64) return null
        val whole = MsgPackReader(bytes)
        whole.read(0)
        if (!whole.ok || !whole.atEnd) return null
        val rest = bytes.copyOfRange(offset, bytes.size)
        val reader = MsgPackReader(rest)
        val items = ArrayList<IntArray>(count)
        repeat(count) {
            val start = reader.position
            reader.read(1)
            if (!reader.ok) return null
            items.add(intArrayOf(offset + start, offset + reader.position))
        }
        return items
    }
}

fun RnsIdentity.encrypt(plaintext: ByteArray, ratchetPublicKey: ByteArray = ByteArray(0)): ByteArray {
    if (publicKey.size != Rns.PUBLIC_KEY_LENGTH) return ByteArray(0)
    val secret = Rns.randomBytes(32)
    val ephemeralPublic = RnsCrypto.x25519PublicFromPrivate(secret)
    val target = if (ratchetPublicKey.size == 32) ratchetPublicKey else encryptionPublicKey
    val shared = RnsCrypto.x25519Exchange(secret, target)
    if (shared.isEmpty()) return ByteArray(0)
    val token = RnsCrypto.tokenEncrypt(Rns.hkdf(64, shared, hash), plaintext)
    if (token.isEmpty()) return ByteArray(0)
    return ephemeralPublic + token
}

fun RnsIdentity.decrypt(token: ByteArray): ByteArray? {
    if (!hasPrivateKey || token.size <= 32) return null
    val shared = RnsCrypto.x25519Exchange(privateKey.copyOf(32), token.copyOf(32))
    if (shared.isEmpty()) return null
    return RnsCrypto.tokenDecrypt(Rns.hkdf(64, shared, hash), token.copyOfRange(32, token.size))
}

class MsgPackWriter {
    private val out = ByteArrayOutputStream()

    fun bytes(): ByteArray = out.toByteArray()

    private fun header(small: Int, smallLimit: Int, base8: Int, base16: Int, base32: Int, size: Int) {
        when {
            size < smallLimit && small != 0 -> out.write(small or size)
            size <= 0xff && base8 != 0 -> {
                out.write(base8)
                out.write(size)
            }
            size <= 0xffff -> {
                out.write(base16)
                out.write(size ushr 8)
                out.write(size and 0xff)
            }
            else -> {
                out.write(base32)
                for (shift in 24 downTo 0 step 8) out.write((size ushr shift) and 0xff)
            }
        }
    }

    private fun big(type: Int, bits: Long, width: Int) {
        out.write(type)
        for (shift in (width - 1) * 8 downTo 0 step 8) out.write(((bits ushr shift) and 0xff).toInt())
    }

    fun nil() = apply { out.write(0xc0) }

    fun boolean(value: Boolean) = apply { out.write(if (value) 0xc3 else 0xc2) }

    fun integer(value: Long) = apply {
        if (value >= 0) {
            when {
                value <= 0x7f -> out.write(value.toInt())
                value <= 0xff -> big(0xcc, value, 1)
                value <= 0xffff -> big(0xcd, value, 2)
                value <= 0xffffffffL -> big(0xce, value, 4)
                else -> big(0xcf, value, 8)
            }
        } else {
            when {
                value >= -32 -> out.write(value.toInt() and 0xff)
                value >= -128 -> big(0xd0, value, 1)
                value >= -32768 -> big(0xd1, value, 2)
                value >= -2147483648L -> big(0xd2, value, 4)
                else -> big(0xd3, value, 8)
            }
        }
    }

    fun integer(value: Int) = integer(value.toLong())

    fun real(value: Double) = apply { big(0xcb, java.lang.Double.doubleToRawLongBits(value), 8) }

    fun bin(value: ByteArray) = apply {
        header(0, 0, 0xc4, 0xc5, 0xc6, value.size)
        out.write(value)
    }

    fun str(value: String) = apply {
        val utf8 = value.toByteArray(Charsets.UTF_8)
        header(0xa0, 32, 0xd9, 0xda, 0xdb, utf8.size)
        out.write(utf8)
    }

    fun array(count: Int) = apply { header(0x90, 16, 0, 0xdc, 0xdd, count) }

    fun map(count: Int) = apply { header(0x80, 16, 0, 0xde, 0xdf, count) }

    fun raw(bytes: ByteArray) = apply { out.write(bytes) }
}

class InterfaceAccessCode(networkName: String = "", passphrase: String = "", size: Int = 16) {
    val key: ByteArray
    private val signingKey: ByteArray
    val size: Int

    init {
        if ((networkName.isEmpty() && passphrase.isEmpty()) || size < 1 || size > 64) {
            key = ByteArray(0)
            signingKey = ByteArray(0)
            this.size = 0
        } else {
            var origin = ByteArray(0)
            if (networkName.isNotEmpty()) origin += Rns.fullHash(networkName.toByteArray(Charsets.UTF_8))
            if (passphrase.isNotEmpty()) origin += Rns.fullHash(passphrase.toByteArray(Charsets.UTF_8))
            key = Rns.hkdf(64, Rns.fullHash(origin), SALT)
            signingKey = key.copyOfRange(32, 64)
            this.size = size
        }
    }

    val enabled: Boolean get() = size > 0

    private val signingPublic: ByteArray by lazy { if (enabled) RnsCrypto.ed25519PublicFromPrivate(signingKey) else ByteArray(0) }

    private fun code(packet: ByteArray): ByteArray {
        val signature = Ed25519.sign(signingKey, packet, signingPublic)
        return signature.copyOfRange(signature.size - size, signature.size)
    }

    fun apply(raw: ByteArray): ByteArray {
        if (!enabled || raw.size < 2) return raw
        val code = code(raw)
        val framed = ByteArray(raw.size + size)
        framed[0] = (raw[0].toInt() or 0x80).toByte()
        framed[1] = raw[1]
        System.arraycopy(code, 0, framed, 2, size)
        System.arraycopy(raw, 2, framed, 2 + size, raw.size - 2)
        val mask = Rns.hkdf(framed.size, code, key)
        for (i in framed.indices) {
            if (i >= 2 && i < 2 + size) continue
            framed[i] = (framed[i].toInt() xor mask[i].toInt()).toByte()
        }
        framed[0] = (framed[0].toInt() or 0x80).toByte()
        return framed
    }

    fun remove(raw: ByteArray): ByteArray? {
        if (!enabled || raw.size <= 2 + size || raw[0].toInt() and 0x80 == 0) return null
        val code = raw.copyOfRange(2, 2 + size)
        val mask = Rns.hkdf(raw.size, code, key)
        val unmasked = raw.copyOf()
        for (i in unmasked.indices) {
            if (i >= 2 && i < 2 + size) continue
            unmasked[i] = (unmasked[i].toInt() xor mask[i].toInt()).toByte()
        }
        val packet = ByteArray(raw.size - size)
        packet[0] = (unmasked[0].toInt() and 0x7f).toByte()
        packet[1] = unmasked[1]
        System.arraycopy(unmasked, 2 + size, packet, 2, raw.size - 2 - size)
        return if (MessageDigest.isEqual(code(packet), code)) packet else null
    }

    private companion object {
        val SALT: ByteArray = Rns.fromHex("adf54d882c9a9b80771eb4995d702d4a3e733391b2a0f53f416d9f907e55cff8")!!
    }
}
