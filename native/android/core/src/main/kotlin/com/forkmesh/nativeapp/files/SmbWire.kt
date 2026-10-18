package com.forkmesh.nativeapp.files

import java.io.ByteArrayOutputStream

class ByteWriter {
    private val out = ByteArrayOutputStream()

    val size: Int get() = out.size()

    fun u8(value: Int) = apply { out.write(value and 0xff) }

    fun u16(value: Int) = apply {
        out.write(value and 0xff)
        out.write((value ushr 8) and 0xff)
    }

    fun u32(value: Long) = apply {
        for (i in 0 until 4) out.write(((value ushr (8 * i)) and 0xff).toInt())
    }

    fun u32(value: Int) = u32(value.toLong() and 0xffffffffL)

    fun u64(value: Long) = apply {
        for (i in 0 until 8) out.write(((value ushr (8 * i)) and 0xff).toInt())
    }

    fun bytes(value: ByteArray) = apply { out.write(value) }

    fun zeros(count: Int) = apply { repeat(count) { out.write(0) } }

    fun align(boundary: Int) = apply { while (out.size() % boundary != 0) out.write(0) }

    fun toByteArray(): ByteArray = out.toByteArray()
}

class ByteReader(val data: ByteArray, var position: Int = 0) {
    val remaining: Int get() = data.size - position

    private fun need(count: Int, at: Int) {
        if (at < 0 || count < 0 || at + count > data.size) throw SmbException(SmbStatus.PROTOCOL_ERROR, SmbStatus.message(SmbStatus.PROTOCOL_ERROR))
    }

    fun u8(at: Int): Int {
        need(1, at)
        return data[at].toInt() and 0xff
    }

    fun u16(at: Int): Int {
        need(2, at)
        return (data[at].toInt() and 0xff) or ((data[at + 1].toInt() and 0xff) shl 8)
    }

    fun u32(at: Int): Long {
        need(4, at)
        var value = 0L
        for (i in 0 until 4) value = value or ((data[at + i].toLong() and 0xff) shl (8 * i))
        return value
    }

    fun u64(at: Int): Long {
        need(8, at)
        var value = 0L
        for (i in 0 until 8) value = value or ((data[at + i].toLong() and 0xff) shl (8 * i))
        return value
    }

    fun bytes(at: Int, count: Int): ByteArray {
        need(count, at)
        return data.copyOfRange(at, at + count)
    }

    fun u8(): Int = u8(position).also { position += 1 }

    fun u16(): Int = u16(position).also { position += 2 }

    fun u32(): Long = u32(position).also { position += 4 }

    fun u64(): Long = u64(position).also { position += 8 }

    fun bytes(count: Int): ByteArray = bytes(position, count).also { position += count }

    fun align(boundary: Int) {
        while (position % boundary != 0) position++
    }
}

fun utf16(text: String): ByteArray = text.toByteArray(Charsets.UTF_16LE)

fun fromUtf16(bytes: ByteArray): String = String(bytes, Charsets.UTF_16LE)

object SmbFraming {
    fun frame(message: ByteArray): ByteArray {
        val length = message.size
        return byteArrayOf(0, (length ushr 16).toByte(), (length ushr 8).toByte(), length.toByte()) + message
    }

    fun length(header: ByteArray): Int =
        ((header[1].toInt() and 0xff) shl 16) or ((header[2].toInt() and 0xff) shl 8) or (header[3].toInt() and 0xff)
}

object SmbStatus {
    const val SUCCESS = 0L
    const val PENDING = 0x00000103L
    const val BUFFER_OVERFLOW = 0x80000005L
    const val NO_MORE_FILES = 0x80000006L
    const val MORE_PROCESSING_REQUIRED = 0xC0000016L
    const val NO_SUCH_FILE = 0xC000000FL
    const val END_OF_FILE = 0xC0000011L
    const val ACCESS_DENIED = 0xC0000022L
    const val OBJECT_NAME_NOT_FOUND = 0xC0000034L
    const val OBJECT_NAME_COLLISION = 0xC0000035L
    const val OBJECT_PATH_NOT_FOUND = 0xC000003AL
    const val SHARING_VIOLATION = 0xC0000043L
    const val LOGON_FAILURE = 0xC000006DL
    const val DISK_FULL = 0xC000007FL
    const val MEDIA_WRITE_PROTECTED = 0xC00000A2L
    const val NOT_SUPPORTED = 0xC00000BBL
    const val BAD_NETWORK_NAME = 0xC00000CCL
    const val DIRECTORY_NOT_EMPTY = 0xC0000101L
    const val USER_SESSION_DELETED = 0xC0000203L
    const val NETWORK_SESSION_EXPIRED = 0xC000035CL
    const val UNREACHABLE = -1L
    const val NOT_LOCAL = -2L
    const val NO_DIALECT = -3L
    const val PROTOCOL_ERROR = -4L
    const val TOO_LARGE = -5L
    const val CANCELLED = -6L

    fun message(status: Long, host: String = "", share: String = ""): String = when (status) {
        LOGON_FAILURE -> "The user name or password is wrong."
        ACCESS_DENIED -> "Access denied."
        BAD_NETWORK_NAME -> "There is no share named $share on $host."
        OBJECT_NAME_NOT_FOUND, OBJECT_PATH_NOT_FOUND, NO_SUCH_FILE -> "That file or folder no longer exists."
        OBJECT_NAME_COLLISION -> "Something with that name is already there."
        DIRECTORY_NOT_EMPTY -> "The folder is not empty."
        SHARING_VIOLATION -> "The file is open somewhere else."
        DISK_FULL -> "The disk is full."
        MEDIA_WRITE_PROTECTED -> "The share is read only."
        UNREACHABLE -> "Could not reach $host. Is file sharing on?"
        NOT_LOCAL -> "Network shares must be on your local network."
        NO_DIALECT -> "$host does not speak SMB 2.1."
        PROTOCOL_ERROR -> "The file server sent an answer ForkMesh does not understand."
        TOO_LARGE -> "The file is too large to open here. Download it instead."
        CANCELLED -> "Cancelled."
        else -> "The file server answered 0x%08X.".format(status)
    }
}

class SmbException(val status: Long, message: String) : Exception(message)
