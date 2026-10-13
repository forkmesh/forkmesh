package com.forkmesh.nativeapp.git.bytes

import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException

class CheckedBytesReader(bytes: ByteArray) {
    private val bytes: ByteArray = bytes.copyOf()

    var offset: Int = 0
        private set

    val remaining: Int get() = bytes.size - offset

    val isAtEnd: Boolean get() = remaining == 0

    fun readByte(): Int {
        if (remaining == 0) {
            throw GitException(GitErrorCode.invalidObject, "unexpected end of input")
        }
        return bytes[offset++].toInt() and 0xff
    }

    fun readBytes(length: Int): ByteArray {
        if (length < 0 || length > remaining) {
            throw GitException(
                GitErrorCode.resourceLimitExceeded,
                "declared byte range exceeds remaining input",
            )
        }
        val result = bytes.copyOfRange(offset, offset + length)
        offset += length
        return result
    }

    fun readUint32(): Long {
        val bytes = readBytes(4)
        return ((bytes[0].toLong() and 0xff) shl 24) or
            ((bytes[1].toLong() and 0xff) shl 16) or
            ((bytes[2].toLong() and 0xff) shl 8) or
            (bytes[3].toLong() and 0xff)
    }
}

class CheckedBytesWriter {
    private val builder = GitBytesBuilder()

    val length: Int get() = builder.length

    fun writeByte(value: Int) {
        if (value < 0 || value > 0xff) {
            throw IllegalArgumentException("Invalid argument (value): must be a byte: $value")
        }
        builder.addByte(value)
    }

    fun writeBytes(values: ByteArray) = builder.add(values)

    fun writeUint32(value: Long) {
        if (value < 0 || value > 0xffffffffL) {
            throw IllegalArgumentException("Invalid argument (value): must be an unsigned 32-bit integer: $value")
        }
        builder.addByte((value shr 24).toInt())
        builder.addByte((value shr 16).toInt())
        builder.addByte((value shr 8).toInt())
        builder.addByte(value.toInt())
    }

    fun takeBytes(): ByteArray = builder.takeBytes()
}
