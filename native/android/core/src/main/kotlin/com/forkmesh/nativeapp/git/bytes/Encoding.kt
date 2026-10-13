package com.forkmesh.nativeapp.git.bytes

import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import java.util.Base64

private val lowerHexDigits = "0123456789abcdef".toCharArray()

fun decodeLowerHex(value: String): ByteArray {
    if (value.length % 2 != 0 || !isLowerHex(value)) {
        throw GitException(GitErrorCode.invalidObject, "expected canonical lowercase hexadecimal")
    }
    val output = ByteArray(value.length / 2)
    for (index in output.indices) {
        output[index] = ((hexValue(value[2 * index]) shl 4) or hexValue(value[2 * index + 1])).toByte()
    }
    return output
}

fun encodeLowerHex(value: ByteArray): String {
    val output = CharArray(value.size * 2)
    for (index in value.indices) {
        val byte = value[index].toInt() and 0xff
        output[2 * index] = lowerHexDigits[byte ushr 4]
        output[2 * index + 1] = lowerHexDigits[byte and 0x0f]
    }
    return String(output)
}

fun encodeCanonicalBase64Url(value: ByteArray): String =
    Base64.getUrlEncoder().encodeToString(value).replace("=", "")

fun decodeCanonicalBase64Url(value: String): ByteArray {
    if (value.contains('=') || !base64UrlAlphabet.matches(value)) {
        throw GitException(GitErrorCode.invalidObject, "expected unpadded canonical base64url")
    }
    val padding = "=".repeat((4 - value.length % 4) % 4)
    val decoded = try {
        Base64.getUrlDecoder().decode("$value$padding")
    } catch (error: IllegalArgumentException) {
        throw DartFormatException("Invalid base64url length or padding")
    }
    if (encodeCanonicalBase64Url(decoded) != value) {
        throw GitException(GitErrorCode.invalidObject, "base64url spelling is not canonical")
    }
    return decoded
}

private val base64UrlAlphabet = Regex("[A-Za-z0-9_-]*")

private fun isLowerHex(value: String): Boolean {
    for (char in value) {
        if (!(char in '0'..'9' || char in 'a'..'f')) return false
    }
    return true
}

private fun hexValue(char: Char): Int = if (char <= '9') char - '0' else char - 'a' + 10
