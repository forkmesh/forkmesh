package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.bytes.dartTrim
import com.forkmesh.nativeapp.git.bytes.encodeLowerHex
import java.security.MessageDigest

const val catalogV2Prefix = "forkmesh-catalog-v2"

fun canonicalJson(value: Any?): String = StringBuilder().also { writeCanonical(it, value) }.toString()

fun catalogRecordHash(record: Map<String, Any?>): String =
    encodeLowerHex(MessageDigest.getInstance("SHA-256").digest(canonicalJson(record).toByteArray(Charsets.UTF_8)))

fun catalogV2Canonical(recordWithoutSignature: Map<String, Any?>): String =
    "$catalogV2Prefix\n${catalogRecordHash(recordWithoutSignature)}"

private fun writeCanonical(out: StringBuilder, value: Any?) {
    when (value) {
        is String -> writeCanonicalString(out, value)
        is Boolean -> out.append(if (value) "true" else "false")
        is Int, is Long, is Short, is Byte -> out.append(value.toString())
        is List<*> -> {
            out.append('[')
            value.forEachIndexed { index, item ->
                if (index > 0) out.append(',')
                writeCanonical(out, item)
            }
            out.append(']')
        }
        is Map<*, *> -> {
            val keys = value.keys.map { it as String }.sorted()
            out.append('{')
            keys.forEachIndexed { index, key ->
                if (index > 0) out.append(',')
                writeCanonicalString(out, key)
                out.append(':')
                writeCanonical(out, value[key])
            }
            out.append('}')
        }
        else -> throw IllegalArgumentException("Invalid argument (value): not canonicalizable JSON: $value")
    }
}

private fun writeCanonicalString(out: StringBuilder, value: String) {
    out.append('"')
    for (char in value) {
        when (val unit = char.code) {
            0x22 -> out.append("\\\"")
            0x5c -> out.append("\\\\")
            0x08 -> out.append("\\b")
            0x0c -> out.append("\\f")
            0x0a -> out.append("\\n")
            0x0d -> out.append("\\r")
            0x09 -> out.append("\\t")
            else -> if (unit < 0x20 || unit > 0x7e) {
                out.append("\\u").append(unit.toString(16).padStart(4, '0'))
            } else {
                out.append(char)
            }
        }
    }
    out.append('"')
}

fun cleanCatalogString(value: String, maxLength: Int): String {
    val kept = StringBuilder()
    var index = 0
    while (index < value.length) {
        val rune = value.codePointAt(index)
        index += Character.charCount(rune)
        if (rune < 0x20 || rune == 0x7f) continue
        kept.appendCodePoint(if (isExoticSpace(rune)) 0x20 else rune)
    }
    var text = kept.toString().dartTrim()
    if (text.codePointCount(0, text.length) > maxLength) {
        text = text.substring(0, text.offsetByCodePoints(0, maxLength)).dartTrim()
    }
    return text
}

private fun isExoticSpace(rune: Int): Boolean =
    rune == 0x85 ||
        rune == 0xa0 ||
        rune == 0x1680 ||
        rune in 0x2000..0x200a ||
        rune == 0x2028 ||
        rune == 0x2029 ||
        rune == 0x202f ||
        rune == 0x205f ||
        rune == 0x3000 ||
        rune == 0xfeff
