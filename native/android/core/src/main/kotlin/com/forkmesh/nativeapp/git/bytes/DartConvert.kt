package com.forkmesh.nativeapp.git.bytes

import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.CodingErrorAction
import java.util.zip.Deflater

internal class DartFormatException(message: String) : IllegalArgumentException(message)

internal fun utf8Decode(bytes: ByteArray, offset: Int = 0, length: Int = bytes.size - offset): String {
    var ascii = true
    for (index in offset until offset + length) {
        if (bytes[index] < 0) {
            ascii = false
            break
        }
    }
    if (ascii) return String(bytes, offset, length, Charsets.ISO_8859_1)
    val decoder = Charsets.UTF_8.newDecoder()
        .onMalformedInput(CodingErrorAction.REPORT)
        .onUnmappableCharacter(CodingErrorAction.REPORT)
    try {
        return decoder.decode(ByteBuffer.wrap(bytes, offset, length)).toString()
    } catch (error: CharacterCodingException) {
        throw DartFormatException("Invalid UTF-8 byte sequence")
    }
}

internal fun utf8Encode(value: String): ByteArray {
    var clean = true
    var index = 0
    while (index < value.length) {
        val char = value[index]
        if (Character.isHighSurrogate(char)) {
            if (index + 1 < value.length && Character.isLowSurrogate(value[index + 1])) {
                index += 2
                continue
            }
            clean = false
            break
        }
        if (Character.isLowSurrogate(char)) {
            clean = false
            break
        }
        index += 1
    }
    if (clean) return value.toByteArray(Charsets.UTF_8)
    val builder = StringBuilder(value.length)
    index = 0
    while (index < value.length) {
        val char = value[index]
        if (Character.isHighSurrogate(char) && index + 1 < value.length && Character.isLowSurrogate(value[index + 1])) {
            builder.append(char).append(value[index + 1])
            index += 2
            continue
        }
        builder.append(if (Character.isSurrogate(char)) '\uFFFD' else char)
        index += 1
    }
    return builder.toString().toByteArray(Charsets.UTF_8)
}

internal fun asciiDecode(bytes: ByteArray, start: Int = 0, end: Int = bytes.size): String {
    for (index in start until end) {
        if (bytes[index] < 0) throw DartFormatException("Invalid value in input: ${bytes[index].toInt() and 0xff}")
    }
    return String(bytes, start, end - start, Charsets.ISO_8859_1)
}

internal fun asciiDecodeAllowInvalid(bytes: ByteArray, start: Int = 0, end: Int = bytes.size): String {
    val output = CharArray(end - start)
    for (index in start until end) {
        val byte = bytes[index].toInt()
        output[index - start] = if (byte < 0) '\uFFFD' else byte.toChar()
    }
    return String(output)
}

internal fun asciiEncode(value: String): ByteArray {
    val output = ByteArray(value.length)
    for (index in value.indices) {
        val char = value[index]
        if (char.code > 0x7f) throw IllegalArgumentException("String contains invalid characters.")
        output[index] = char.code.toByte()
    }
    return output
}

internal fun latin1Decode(bytes: ByteArray): String = String(bytes, Charsets.ISO_8859_1)

internal fun isDartWhitespace(char: Char): Boolean {
    val code = char.code
    return code in 0x09..0x0d || code == 0x20 || code == 0x85 || code == 0xa0 || code == 0x1680 ||
        code in 0x2000..0x200a || code == 0x2028 || code == 0x2029 || code == 0x202f ||
        code == 0x205f || code == 0x3000 || code == 0xfeff
}

internal fun String.dartTrim(): String {
    var start = 0
    var end = length
    while (start < end && isDartWhitespace(this[start])) start += 1
    while (end > start && isDartWhitespace(this[end - 1])) end -= 1
    return substring(start, end)
}

internal fun String.dartTrimLeft(): String {
    var start = 0
    while (start < length && isDartWhitespace(this[start])) start += 1
    return substring(start)
}

internal fun dartSplitLines(text: String): List<String> {
    val lines = ArrayList<String>()
    var start = 0
    var index = 0
    while (index < text.length) {
        val char = text[index]
        if (char == '\n' || char == '\r') {
            lines.add(text.substring(start, index))
            if (char == '\r' && index + 1 < text.length && text[index + 1] == '\n') index += 1
            start = index + 1
        }
        index += 1
    }
    if (start < text.length) lines.add(text.substring(start))
    return lines
}

internal fun zlibEncode(bytes: ByteArray): ByteArray {
    val deflater = Deflater(Deflater.DEFAULT_COMPRESSION)
    try {
        deflater.setInput(bytes)
        deflater.finish()
        val output = GitBytesBuilder(maxOf(64, bytes.size / 2 + 64))
        val buffer = ByteArray(64 * 1024)
        while (!deflater.finished()) {
            val count = deflater.deflate(buffer)
            output.add(buffer, 0, count)
        }
        return output.takeBytes()
    } finally {
        deflater.end()
    }
}

internal class GitBytesBuilder(initialCapacity: Int = 64) {
    private var buffer = ByteArray(maxOf(16, initialCapacity))

    var length: Int = 0
        private set

    val isEmpty: Boolean get() = length == 0

    fun add(bytes: ByteArray) = add(bytes, 0, bytes.size)

    fun add(bytes: ByteArray, offset: Int, count: Int) {
        ensureCapacity(count)
        System.arraycopy(bytes, offset, buffer, length, count)
        length += count
    }

    fun addByte(value: Int) {
        ensureCapacity(1)
        buffer[length++] = value.toByte()
    }

    operator fun get(index: Int): Int = buffer[index].toInt() and 0xff

    fun takeBytes(): ByteArray {
        val result = buffer.copyOf(length)
        buffer = ByteArray(16)
        length = 0
        return result
    }

    fun toBytes(): ByteArray = buffer.copyOf(length)

    private fun ensureCapacity(extra: Int) {
        val needed = length.toLong() + extra
        if (needed > Int.MAX_VALUE - 8) throw OutOfMemoryError("byte builder exceeds the array size limit")
        if (needed <= buffer.size) return
        var capacity = buffer.size.toLong() * 2
        if (capacity < needed) capacity = needed
        if (capacity > Int.MAX_VALUE - 8) capacity = (Int.MAX_VALUE - 8).toLong()
        buffer = buffer.copyOf(capacity.toInt())
    }
}

internal fun ByteArray.indexOfByte(value: Int, start: Int = 0): Int {
    val needle = value.toByte()
    for (index in maxOf(0, start) until size) {
        if (this[index] == needle) return index
    }
    return -1
}

internal fun constantTimeEquals(left: ByteArray, right: ByteArray): Boolean {
    if (left.size != right.size) return false
    var different = 0
    for (index in left.indices) {
        different = different or (left[index].toInt() xor right[index].toInt())
    }
    return different == 0
}

internal fun ByteArray.readUint32(offset: Int): Long =
    ((this[offset].toLong() and 0xff) shl 24) or
        ((this[offset + 1].toLong() and 0xff) shl 16) or
        ((this[offset + 2].toLong() and 0xff) shl 8) or
        (this[offset + 3].toLong() and 0xff)

internal fun ByteArray.readUint16(offset: Int): Int =
    ((this[offset].toInt() and 0xff) shl 8) or (this[offset + 1].toInt() and 0xff)

internal fun uint32Bytes(value: Long): ByteArray =
    byteArrayOf((value shr 24).toByte(), (value shr 16).toByte(), (value shr 8).toByte(), value.toByte())

internal fun uint16Bytes(value: Int): ByteArray = byteArrayOf((value shr 8).toByte(), value.toByte())
