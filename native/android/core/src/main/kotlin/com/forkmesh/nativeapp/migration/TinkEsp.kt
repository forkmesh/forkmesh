package com.forkmesh.nativeapp.migration

import java.nio.ByteBuffer
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

object TinkEsp {
    const val KEY_KEYSET_PREF = "__androidx_security_crypto_encrypted_prefs_key_keyset__"
    const val VALUE_KEYSET_PREF = "__androidx_security_crypto_encrypted_prefs_value_keyset__"
    const val MASTER_KEY_ALIAS = "_androidx_security_master_key_"
    const val NULL_VALUE = "__NULL__"

    private const val AES_SIV_TYPE_URL = "type.googleapis.com/google.crypto.tink.AesSivKey"
    private const val AES_GCM_TYPE_URL = "type.googleapis.com/google.crypto.tink.AesGcmKey"

    class TinkKey(val keyId: Int, val typeUrl: String, val keyValue: ByteArray, val prefix: ByteArray, val enabled: Boolean)

    class Keyset(val primaryKeyId: Int, val keys: List<TinkKey>)

    class Entry(val key: String?, val value: Any?, val storedName: String)

    fun decryptAll(
        prefs: Map<String, Any?>,
        fileName: String,
        openKeyset: (sealed: ByteArray) -> ByteArray?,
    ): EspResult? {
        val keyKeysetHex = prefs[KEY_KEYSET_PREF] as? String ?: return null
        val valueKeysetHex = prefs[VALUE_KEYSET_PREF] as? String ?: return null
        val keyKeyset = readKeyset(hexToBytes(keyKeysetHex), openKeyset) ?: return EspResult(emptyList(), listOf("keyset"))
        val valueKeyset = readKeyset(hexToBytes(valueKeysetHex), openKeyset) ?: return EspResult(emptyList(), listOf("keyset"))
        val entries = ArrayList<Entry>()
        val failures = ArrayList<String>()
        for ((encryptedName, raw) in prefs) {
            if (encryptedName == KEY_KEYSET_PREF || encryptedName == VALUE_KEYSET_PREF) continue
            val sealedValue = raw as? String ?: continue
            try {
                val nameBytes = sivDecryptWithKeyset(keyKeyset, Base64s.decode(encryptedName), fileName.toByteArray(Charsets.UTF_8))
                val name = String(nameBytes, Charsets.UTF_8).let { if (it == NULL_VALUE) null else it }
                val payload = gcmDecryptWithKeyset(valueKeyset, Base64s.decode(sealedValue), encryptedName.toByteArray(Charsets.UTF_8))
                entries.add(Entry(name, decodePayload(payload), encryptedName))
            } catch (error: Exception) {
                failures.add(encryptedName)
            }
        }
        return EspResult(entries, failures)
    }

    class EspResult(val entries: List<Entry>, val failures: List<String>)

    fun readKeyset(bytes: ByteArray, openKeyset: (ByteArray) -> ByteArray?): Keyset? {
        val fields = Proto.parse(bytes)

        val sealed = fields.bytes(2)
        if (sealed != null && fields.varint(1) == null) {
            val plain = openKeyset(sealed) ?: return null
            return parseKeyset(plain)
        }
        return parseKeyset(bytes)
    }

    fun parseKeyset(bytes: ByteArray): Keyset {
        val fields = Proto.parse(bytes)
        val primary = fields.varint(1)?.toInt() ?: 0
        val keys = fields.allBytes(2).map { keyBytes ->
            val key = Proto.parse(keyBytes)
            val keyData = Proto.parse(key.bytes(1) ?: ByteArray(0))
            val typeUrl = keyData.bytes(1)?.let { String(it, Charsets.UTF_8) } ?: ""
            val value = Proto.parse(keyData.bytes(2) ?: ByteArray(0))

            val material = when (typeUrl) {
                AES_SIV_TYPE_URL -> value.bytes(2)
                AES_GCM_TYPE_URL -> value.bytes(3)
                else -> null
            } ?: ByteArray(0)
            val keyId = (key.varint(3) ?: 0L).toInt()
            val status = (key.varint(2) ?: 0L).toInt()
            val prefix = when ((key.varint(4) ?: 0L).toInt()) {
                1 -> byteArrayOf(1) + intBytes(keyId)
                2, 4 -> byteArrayOf(0) + intBytes(keyId)
                else -> ByteArray(0)
            }
            TinkKey(keyId, typeUrl, material, prefix, status == 1)
        }
        return Keyset(primary, keys)
    }

    private fun candidates(keyset: Keyset, ciphertext: ByteArray): List<Pair<TinkKey, ByteArray>> {
        val out = ArrayList<Pair<TinkKey, ByteArray>>()
        for (key in keyset.keys) {
            if (!key.enabled) continue
            if (key.prefix.isEmpty()) {
                out.add(key to ciphertext)
            } else if (ciphertext.size >= key.prefix.size && ciphertext.copyOfRange(0, key.prefix.size).contentEquals(key.prefix)) {
                out.add(key to ciphertext.copyOfRange(key.prefix.size, ciphertext.size))
            }
        }
        return out
    }

    fun sivDecryptWithKeyset(keyset: Keyset, ciphertext: ByteArray, ad: ByteArray): ByteArray {
        var last: Exception? = null
        for ((key, body) in candidates(keyset, ciphertext)) {
            try {
                return AesSiv.decrypt(key.keyValue, body, ad)
            } catch (error: Exception) {
                last = error
            }
        }
        throw last ?: IllegalArgumentException("no AES-SIV key matches")
    }

    fun gcmDecryptWithKeyset(keyset: Keyset, ciphertext: ByteArray, ad: ByteArray): ByteArray {
        var last: Exception? = null
        for ((key, body) in candidates(keyset, ciphertext)) {
            try {
                return aesGcmDecrypt(key.keyValue, body, ad)
            } catch (error: Exception) {
                last = error
            }
        }
        throw last ?: IllegalArgumentException("no AES-GCM key matches")
    }

    fun aesGcmDecrypt(key: ByteArray, ivAndCiphertext: ByteArray, ad: ByteArray): ByteArray {
        require(ivAndCiphertext.size >= 12 + 16) { "ciphertext too short" }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, ivAndCiphertext, 0, 12))
        if (ad.isNotEmpty()) cipher.updateAAD(ad)
        return cipher.doFinal(ivAndCiphertext, 12, ivAndCiphertext.size - 12)
    }

    fun decodePayload(payload: ByteArray): Any? {
        val buffer = ByteBuffer.wrap(payload)
        return when (val type = buffer.int) {
            0 -> {
                val length = buffer.int
                val text = String(payload, buffer.position(), length, Charsets.UTF_8)
                if (text == NULL_VALUE) null else text
            }
            1 -> {
                val set = LinkedHashSet<String>()
                while (buffer.hasRemaining()) {
                    val length = buffer.int
                    val text = String(payload, buffer.position(), length, Charsets.UTF_8)
                    buffer.position(buffer.position() + length)
                    set.add(text)
                }
                if (set.size == 1 && set.first() == NULL_VALUE) null else set
            }
            2 -> buffer.int
            3 -> buffer.long
            4 -> buffer.float
            5 -> buffer.get() != 0.toByte()
            else -> throw IllegalArgumentException("unknown ESP value type $type")
        }
    }

    private fun intBytes(value: Int): ByteArray = ByteBuffer.allocate(4).putInt(value).array()

    fun hexToBytes(hex: String): ByteArray {
        val clean = hex.trim()
        require(clean.length % 2 == 0) { "odd hex length" }
        return ByteArray(clean.length / 2) { clean.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
    }
}

object AesSiv {
    private val ZERO = ByteArray(16)

    fun decrypt(key: ByteArray, ciphertext: ByteArray, ad: ByteArray): ByteArray {
        require(key.size == 32 || key.size == 48 || key.size == 64) { "bad AES-SIV key size" }
        require(ciphertext.size >= 16) { "ciphertext too short" }
        val macKey = key.copyOfRange(0, key.size / 2)
        val ctrKey = key.copyOfRange(key.size / 2, key.size)
        val siv = ciphertext.copyOfRange(0, 16)
        val plaintext = ctr(ctrKey, siv, ciphertext.copyOfRange(16, ciphertext.size))
        val expected = s2v(macKey, listOf(ad), plaintext)
        if (!java.security.MessageDigest.isEqual(expected, siv)) throw javax.crypto.AEADBadTagException("AES-SIV tag mismatch")
        return plaintext
    }

    fun encrypt(key: ByteArray, plaintext: ByteArray, ad: ByteArray): ByteArray {
        val macKey = key.copyOfRange(0, key.size / 2)
        val ctrKey = key.copyOfRange(key.size / 2, key.size)
        val siv = s2v(macKey, listOf(ad), plaintext)
        return siv + ctr(ctrKey, siv, plaintext)
    }

    fun s2v(macKey: ByteArray, ads: List<ByteArray>, plaintext: ByteArray): ByteArray {
        var d = cmac(macKey, ZERO)
        for (ad in ads) d = xor(dbl(d), cmac(macKey, ad))
        val t = if (plaintext.size >= 16) {
            val copy = plaintext.copyOf()
            val offset = copy.size - 16
            for (i in 0 until 16) copy[offset + i] = (copy[offset + i].toInt() xor d[i].toInt()).toByte()
            copy
        } else {
            xor(dbl(d), pad(plaintext))
        }
        return cmac(macKey, t)
    }

    private fun ctr(key: ByteArray, siv: ByteArray, input: ByteArray): ByteArray {
        val iv = siv.copyOf()
        iv[8] = (iv[8].toInt() and 0x7f).toByte()
        iv[12] = (iv[12].toInt() and 0x7f).toByte()
        val cipher = Cipher.getInstance("AES/CTR/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), javax.crypto.spec.IvParameterSpec(iv))
        return cipher.doFinal(input)
    }

    fun cmac(key: ByteArray, message: ByteArray): ByteArray {
        val aes = Cipher.getInstance("AES/ECB/NoPadding")
        aes.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"))
        val l = aes.doFinal(ZERO)
        val k1 = dbl(l)
        val k2 = dbl(k1)
        val blocks = if (message.isEmpty()) 1 else (message.size + 15) / 16
        val complete = message.isNotEmpty() && message.size % 16 == 0
        val last = if (complete) {
            xor(message.copyOfRange((blocks - 1) * 16, blocks * 16), k1)
        } else {
            xor(pad(message.copyOfRange((blocks - 1) * 16, message.size)), k2)
        }
        var x = ByteArray(16)
        for (i in 0 until blocks - 1) x = aes.doFinal(xor(x, message.copyOfRange(i * 16, i * 16 + 16)))
        return aes.doFinal(xor(x, last))
    }

    private fun dbl(block: ByteArray): ByteArray {
        val out = ByteArray(16)
        var carry = 0
        for (i in 15 downTo 0) {
            val b = block[i].toInt() and 0xff
            out[i] = ((b shl 1) or carry).toByte()
            carry = b ushr 7
        }
        if ((block[0].toInt() and 0x80) != 0) out[15] = (out[15].toInt() xor 0x87).toByte()
        return out
    }

    private fun pad(partial: ByteArray): ByteArray {
        val out = ByteArray(16)
        partial.copyInto(out)
        out[partial.size] = 0x80.toByte()
        return out
    }

    private fun xor(a: ByteArray, b: ByteArray): ByteArray = ByteArray(16) { (a[it].toInt() xor b[it].toInt()).toByte() }
}

class Proto private constructor(private val fields: List<Pair<Int, Any>>) {
    fun bytes(number: Int): ByteArray? = fields.lastOrNull { it.first == number && it.second is ByteArray }?.second as ByteArray?

    fun allBytes(number: Int): List<ByteArray> = fields.filter { it.first == number && it.second is ByteArray }.map { it.second as ByteArray }

    fun varint(number: Int): Long? = fields.lastOrNull { it.first == number && it.second is Long }?.second as Long?

    companion object {
        fun parse(bytes: ByteArray): Proto {
            val out = ArrayList<Pair<Int, Any>>()
            var pos = 0
            fun readVarint(): Long {
                var result = 0L
                var shift = 0
                while (true) {
                    require(pos < bytes.size) { "truncated varint" }
                    val b = bytes[pos++].toInt() and 0xff
                    result = result or ((b and 0x7f).toLong() shl shift)
                    if (b and 0x80 == 0) return result
                    shift += 7
                    require(shift < 64) { "varint too long" }
                }
            }
            while (pos < bytes.size) {
                val tag = readVarint()
                val number = (tag ushr 3).toInt()
                when ((tag and 7).toInt()) {
                    0 -> out.add(number to readVarint())
                    1 -> pos += 8
                    2 -> {
                        val length = readVarint().toInt()
                        require(length >= 0 && pos + length <= bytes.size) { "truncated field" }
                        out.add(number to bytes.copyOfRange(pos, pos + length))
                        pos += length
                    }
                    5 -> pos += 4
                    else -> throw IllegalArgumentException("unsupported wire type")
                }
            }
            return Proto(out)
        }
    }
}

object Base64s {
    fun decode(value: String): ByteArray {
        val clean = value.filterNot { it == '\n' || it == '\r' || it == ' ' }
        return java.util.Base64.getDecoder().decode(clean)
    }

    fun encodeDefault(bytes: ByteArray): String {
        val plain = java.util.Base64.getEncoder().encodeToString(bytes)
        return plain.chunked(76).joinToString("") { "$it\n" }
    }
}
