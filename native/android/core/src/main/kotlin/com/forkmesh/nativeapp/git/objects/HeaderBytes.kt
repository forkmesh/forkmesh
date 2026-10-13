package com.forkmesh.nativeapp.git.objects

import com.forkmesh.nativeapp.git.bytes.DartFormatException
import com.forkmesh.nativeapp.git.bytes.utf8Decode
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException

internal fun findHeaderBodySeparator(bytes: ByteArray): Int {
    var index = 0
    while (index + 1 < bytes.size) {
        if (bytes[index] == 0x0a.toByte() && bytes[index + 1] == 0x0a.toByte()) return index
        index += 1
    }
    return -1
}

internal fun decodeAsciiHeaders(bytes: ByteArray): String {
    for (byte in bytes) {
        val value = byte.toInt() and 0xff
        if (value == 0 || value > 0x7f || value == 0x0d) {
            throw GitException(GitErrorCode.invalidObject, "object headers are not ASCII")
        }
    }
    return String(bytes, Charsets.ISO_8859_1)
}

internal fun decodeUtf8Headers(bytes: ByteArray): String {
    for (byte in bytes) {
        if (byte == 0.toByte() || byte == 0x0d.toByte()) {
            throw GitException(GitErrorCode.invalidObject, "object headers contain forbidden control bytes")
        }
    }
    return try {
        utf8Decode(bytes)
    } catch (error: DartFormatException) {
        throw GitException(GitErrorCode.invalidObject, "object headers are not valid UTF-8")
    }
}
