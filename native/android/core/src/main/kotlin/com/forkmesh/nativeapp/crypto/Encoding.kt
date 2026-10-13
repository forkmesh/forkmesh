package com.forkmesh.nativeapp.crypto

import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.CodingErrorAction
import java.security.SecureRandom

object B64 {
    private const val STANDARD = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
    private const val URL_SAFE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
    private const val PADDING = -2
    private const val INVALID = -1

    private val INVERSE = IntArray(128) { INVALID }.also { table ->
        for (i in STANDARD.indices) table[STANDARD[i].code] = i
        table['-'.code] = 62
        table['_'.code] = 63
        table['='.code] = PADDING
        table['%'.code] = PADDING
    }

    fun encode(bytes: ByteArray): String = encodeWith(bytes, STANDARD, pad = true)

    fun encodeUrl(bytes: ByteArray): String = encodeWith(bytes, URL_SAFE, pad = true)

    fun encodeUrlNoPadding(bytes: ByteArray): String = encodeWith(bytes, URL_SAFE, pad = false)

    fun decode(text: String): ByteArray {
        val end = text.length
        val out = ByteArray(end / 4 * 3 + 3)
        var size = 0
        var bits = 0
        var count = 0
        var i = 0
        while (i < end) {
            val char = text[i].code
            val code = if (char < 128) INVERSE[char] else INVALID
            if (code >= 0) {
                bits = ((bits shl 6) or code) and 0xFFFFFF
                count = (count + 1) and 3
                if (count == 0) {
                    out[size++] = (bits shr 16).toByte()
                    out[size++] = (bits shr 8).toByte()
                    out[size++] = bits.toByte()
                    bits = 0
                }
                i++
                continue
            }
            if (code == PADDING && count > 1) {
                if (count == 3) {
                    if ((bits and 0x03) != 0) throw IllegalArgumentException("Invalid encoding before padding")
                    out[size++] = (bits shr 10).toByte()
                    out[size++] = (bits shr 2).toByte()
                } else {
                    if ((bits and 0x0F) != 0) throw IllegalArgumentException("Invalid encoding before padding")
                    out[size++] = (bits shr 4).toByte()
                }
                var expected = (3 - count) * 3
                if (char == '%'.code) expected += 2
                if (checkPadding(text, i + 1, end, expected) != 0) {
                    throw IllegalArgumentException("Missing padding character")
                }
                return out.copyOf(size)
            }
            throw IllegalArgumentException("Invalid character")
        }
        if (count != 0) throw IllegalArgumentException("Invalid length, must be multiple of four")
        return out.copyOf(size)
    }

    fun decodeOrNull(text: String): ByteArray? = try {
        decode(text)
    } catch (_: IllegalArgumentException) {
        null
    }

    private fun checkPadding(input: String, from: Int, end: Int, expectedPadding: Int): Int {
        var start = from
        var expected = expectedPadding
        if (start == end) return expected
        while (expected > 0) {
            var char = input[start].code
            if (expected == 3) {
                if (char == '='.code) {
                    expected -= 3
                    start++
                    break
                }
                if (char == '%'.code) {
                    expected--
                    start++
                    if (start == end) break
                    char = input[start].code
                } else {
                    break
                }
            }
            var partial = expected
            if (partial > 3) partial -= 3
            if (partial == 2) {
                if (char != '3'.code) break
                start++
                expected--
                if (start == end) break
                char = input[start].code
            }
            if ((char or 0x20) != 'd'.code) break
            start++
            expected--
            if (start == end) break
        }
        if (start != end) throw IllegalArgumentException("Invalid padding character")
        return expected
    }

    private fun encodeWith(bytes: ByteArray, alphabet: String, pad: Boolean): String {
        val out = StringBuilder((bytes.size + 2) / 3 * 4)
        var i = 0
        while (i + 3 <= bytes.size) {
            val n = ((bytes[i].toInt() and 0xff) shl 16) or
                ((bytes[i + 1].toInt() and 0xff) shl 8) or
                (bytes[i + 2].toInt() and 0xff)
            out.append(alphabet[n ushr 18]).append(alphabet[(n ushr 12) and 63])
                .append(alphabet[(n ushr 6) and 63]).append(alphabet[n and 63])
            i += 3
        }
        when (bytes.size - i) {
            1 -> {
                val n = (bytes[i].toInt() and 0xff) shl 16
                out.append(alphabet[n ushr 18]).append(alphabet[(n ushr 12) and 63])
                if (pad) out.append("==")
            }
            2 -> {
                val n = ((bytes[i].toInt() and 0xff) shl 16) or ((bytes[i + 1].toInt() and 0xff) shl 8)
                out.append(alphabet[n ushr 18]).append(alphabet[(n ushr 12) and 63])
                    .append(alphabet[(n ushr 6) and 63])
                if (pad) out.append('=')
            }
        }
        return out.toString()
    }
}

object Utf8 {
    fun encode(text: String): ByteArray {
        var ascii = true
        for (c in text) {
            if (c.code >= 0x80) {
                ascii = false
                break
            }
        }
        if (ascii) return ByteArray(text.length) { text[it].code.toByte() }
        var out = ByteArray(text.length * 3)
        var size = 0
        var i = 0
        while (i < text.length) {
            var c = text[i].code
            if (c in 0xD800..0xDBFF && i + 1 < text.length && text[i + 1].code in 0xDC00..0xDFFF) {
                c = 0x10000 + ((c - 0xD800) shl 10) + (text[i + 1].code - 0xDC00)
                i++
            } else if (c in 0xD800..0xDFFF) {
                c = 0xFFFD
            }
            if (size + 4 > out.size) out = out.copyOf(out.size * 2 + 4)
            when {
                c < 0x80 -> out[size++] = c.toByte()
                c < 0x800 -> {
                    out[size++] = (0xC0 or (c shr 6)).toByte()
                    out[size++] = (0x80 or (c and 0x3F)).toByte()
                }
                c < 0x10000 -> {
                    out[size++] = (0xE0 or (c shr 12)).toByte()
                    out[size++] = (0x80 or ((c shr 6) and 0x3F)).toByte()
                    out[size++] = (0x80 or (c and 0x3F)).toByte()
                }
                else -> {
                    out[size++] = (0xF0 or (c shr 18)).toByte()
                    out[size++] = (0x80 or ((c shr 12) and 0x3F)).toByte()
                    out[size++] = (0x80 or ((c shr 6) and 0x3F)).toByte()
                    out[size++] = (0x80 or (c and 0x3F)).toByte()
                }
            }
            i++
        }
        return out.copyOf(size)
    }

    fun decode(bytes: ByteArray): String = try {
        Charsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(bytes))
            .toString()
    } catch (error: CharacterCodingException) {
        throw IllegalArgumentException("Invalid UTF-8", error)
    }
}

object Bytes {
    private val secureRandom = SecureRandom()

    fun random(count: Int): ByteArray = ByteArray(count).also { secureRandom.nextBytes(it) }

    fun concat(vararg parts: ByteArray): ByteArray {
        val out = ByteArray(parts.sumOf { it.size })
        var offset = 0
        for (part in parts) {
            System.arraycopy(part, 0, out, offset, part.size)
            offset += part.size
        }
        return out
    }

    fun constantTimeEquals(a: ByteArray, b: ByteArray): Boolean {
        if (a.size != b.size) return false
        var diff = 0
        for (i in a.indices) diff = diff or (a[i].toInt() xor b[i].toInt())
        return diff == 0
    }

    fun isAllZero(bytes: ByteArray): Boolean {
        var acc = 0
        for (b in bytes) acc = acc or b.toInt()
        return acc == 0
    }

    fun wipe(vararg arrays: ByteArray) {
        for (array in arrays) array.fill(0)
    }

    private const val HEX = "0123456789abcdef"

    fun hex(bytes: ByteArray): String {
        val out = CharArray(bytes.size * 2)
        for (i in bytes.indices) {
            val v = bytes[i].toInt() and 0xff
            out[2 * i] = HEX[v ushr 4]
            out[2 * i + 1] = HEX[v and 15]
        }
        return String(out)
    }

    fun fromHex(text: String): ByteArray = ByteArray(text.length / 2) {
        text.substring(2 * it, 2 * it + 2).toInt(16).toByte()
    }
}

fun dartTrim(text: String): String {
    var start = 0
    var end = text.length
    while (start < end && isDartWhitespace(text[start].code)) start++
    while (end > start && isDartWhitespace(text[end - 1].code)) end--
    return text.substring(start, end)
}

private fun isDartWhitespace(c: Int): Boolean =
    c in 0x09..0x0D || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 || c in 0x2000..0x200A ||
        c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF

internal fun dartNumEquals(value: Any?, expected: Long): Boolean = when (value) {
    is Long -> value == expected
    is Int -> value.toLong() == expected
    is Short -> value.toLong() == expected
    is Byte -> value.toLong() == expected
    is Double -> value == expected.toDouble()
    is Float -> value.toDouble() == expected.toDouble()
    is java.math.BigInteger -> value == java.math.BigInteger.valueOf(expected)
    is java.math.BigDecimal -> value.compareTo(java.math.BigDecimal.valueOf(expected)) == 0
    else -> false
}
