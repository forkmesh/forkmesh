package com.forkmesh.nativeapp.migration

import com.forkmesh.nativeapp.json.Json
import java.io.ByteArrayInputStream
import java.io.ObjectInputStream
import java.io.ObjectStreamClass

sealed class PreferenceValue {
    data class Bool(val value: Boolean) : PreferenceValue()

    data class Int64(val value: Long) : PreferenceValue()

    data class Real(val value: Double) : PreferenceValue()

    data class Text(val value: String) : PreferenceValue()

    data class TextList(val value: List<String>) : PreferenceValue()
}

object FlutterPreferences {
    const val FILE = "FlutterSharedPreferences"
    const val PREFIX = "flutter."

    const val LIST_PREFIX = "VGhpcyBpcyB0aGUgcHJlZml4IGZvciBhIGxpc3Qu"

    const val JSON_LIST_PREFIX = "$LIST_PREFIX!"
    const val BIG_INTEGER_PREFIX = "VGhpcyBpcyB0aGUgcHJlZml4IGZvciBCaWdJbnRlZ2Vy"
    const val DOUBLE_PREFIX = "VGhpcyBpcyB0aGUgcHJlZml4IGZvciBEb3VibGUu"

    fun decode(all: Map<String, Any?>, unreadable: MutableList<String>? = null): Map<String, PreferenceValue> {
        val out = LinkedHashMap<String, PreferenceValue>()
        for ((stored, raw) in all) {
            if (!stored.startsWith(PREFIX) || raw == null) continue
            val key = stored.substring(PREFIX.length)
            val value = runCatching { decodeValue(raw) }.getOrNull()
            if (value == null) unreadable?.add(key) else out[key] = value
        }
        return out
    }

    fun decodeValue(raw: Any): PreferenceValue? = when (raw) {
        is Boolean -> PreferenceValue.Bool(raw)
        is Long -> PreferenceValue.Int64(raw)
        is Int -> PreferenceValue.Int64(raw.toLong())
        is Float -> PreferenceValue.Real(raw.toDouble())
        is Double -> PreferenceValue.Real(raw)
        is Set<*> -> PreferenceValue.TextList(raw.map { it.toString() })
        is String -> when {
            raw.startsWith(JSON_LIST_PREFIX) -> {
                val parsed = Json.parse(raw.substring(JSON_LIST_PREFIX.length)) as? List<*>
                parsed?.let { list -> PreferenceValue.TextList(list.map { it?.toString() ?: "" }) }
            }
            raw.startsWith(LIST_PREFIX) -> PreferenceValue.TextList(decodeSerializedList(raw.substring(LIST_PREFIX.length)))
            raw.startsWith(BIG_INTEGER_PREFIX) -> {
                val big = java.math.BigInteger(raw.substring(BIG_INTEGER_PREFIX.length), Character.MAX_RADIX)
                PreferenceValue.Int64(big.toLong())
            }
            raw.startsWith(DOUBLE_PREFIX) -> PreferenceValue.Real(raw.substring(DOUBLE_PREFIX.length).toDouble())
            else -> PreferenceValue.Text(raw)
        }
        else -> null
    }

    fun decodeSerializedList(encoded: String): List<String> {
        val bytes = Base64s.decode(encoded)
        val allowed = setOf(
            "java.util.ArrayList",
            "java.util.List",
            "java.util.Arrays\$ArrayList",
            "java.util.Collections\$UnmodifiableRandomAccessList",
            "java.util.Collections\$UnmodifiableList",
            "java.util.Collections\$UnmodifiableCollection",
            "java.lang.String",
            "java.lang.Object",
            "java.util.AbstractList",
            "java.util.AbstractCollection",
            "[Ljava.lang.String;",
            "[Ljava.lang.Object;",
        )
        val stream = object : ObjectInputStream(ByteArrayInputStream(bytes)) {
            override fun resolveClass(desc: ObjectStreamClass): Class<*> {
                if (desc.name !in allowed) throw java.io.InvalidClassException("refused", desc.name)
                return super.resolveClass(desc)
            }
        }
        return stream.use { (it.readObject() as List<*>).map { item -> item.toString() } }
    }
}
