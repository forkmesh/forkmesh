package com.forkmesh.nativeapp.json

typealias JsonMap = Map<String, Any?>

fun jsonText(value: Any?): String = when (value) {
    null -> ""
    is String -> value
    is Double -> Json.formatDouble(value)
    is Map<*, *>, is List<*> -> Json.stringify(value)
    else -> value.toString()
}

fun JsonMap?.str(key: String): String = jsonText(this?.get(key))

fun JsonMap?.str(key: String, fallback: String): String {
    val value = this?.get(key) ?: return fallback
    return jsonText(value)
}

fun JsonMap?.firstStr(vararg keys: String): String {
    if (this == null) return ""
    for (key in keys) {
        val value = this[key]
        if (value != null) return jsonText(value)
    }
    return ""
}

fun jsonLong(value: Any?, fallback: Long = 0L): Long = when (value) {
    is Number -> value.toLong()
    is String -> value.trim().toLongOrNull() ?: value.trim().toDoubleOrNull()?.toLong() ?: fallback
    else -> fallback
}

fun JsonMap?.long(key: String, fallback: Long = 0L): Long = jsonLong(this?.get(key), fallback)

fun JsonMap?.int(key: String, fallback: Int = 0): Int = jsonLong(this?.get(key), fallback.toLong()).toInt()

fun JsonMap?.double(key: String, fallback: Double = 0.0): Double = when (val value = this?.get(key)) {
    is Number -> value.toDouble()
    is String -> value.trim().toDoubleOrNull() ?: fallback
    else -> fallback
}

fun JsonMap?.bool(key: String): Boolean = this?.get(key) == true

fun JsonMap?.optBool(key: String): Boolean? = this?.get(key) as? Boolean

@Suppress("UNCHECKED_CAST")
fun Any?.asJsonMap(): Map<String, Any?> = this as? Map<String, Any?> ?: emptyMap()

fun JsonMap?.obj(key: String): Map<String, Any?> = this?.get(key).asJsonMap()

fun JsonMap?.list(key: String): List<Any?> = this?.get(key) as? List<Any?> ?: emptyList()

@Suppress("UNCHECKED_CAST")
fun JsonMap?.maps(key: String): List<Map<String, Any?>> =
    list(key).filterIsInstance<Map<*, *>>().map { it as Map<String, Any?> }

@Suppress("UNCHECKED_CAST")
fun jsonMaps(value: Any?): List<Map<String, Any?>> =
    (value as? List<Any?>).orEmpty().filterIsInstance<Map<*, *>>().map { it as Map<String, Any?> }

fun JsonMap?.strings(key: String): List<String> = list(key).map(::jsonText)

fun jsonOf(vararg pairs: Pair<String, Any?>): LinkedHashMap<String, Any?> = linkedMapOf(*pairs)
