package com.forkmesh.nativeapp.workflows

import java.math.BigDecimal

internal object WorkflowPy {
    private val numberRe = Regex("^[+-]?([0-9]+\\.?[0-9]*|\\.[0-9]+)([eE][+-]?[0-9]+)?$")
    private val specialRe = Regex("^[+-]?(inf|infinity|nan)$", RegexOption.IGNORE_CASE)

    fun isMap(value: Any?): Boolean = value is Map<*, *>

    @Suppress("UNCHECKED_CAST")
    fun map(value: Any?): Map<String, Any?> = value as? Map<String, Any?> ?: emptyMap()

    @Suppress("UNCHECKED_CAST")
    fun list(value: Any?): List<Any?> = value as? List<Any?> ?: emptyList()

    fun truthy(value: Any?): Boolean = when (value) {
        null -> false
        is Boolean -> value
        is Long -> value != 0L
        is Int -> value != 0
        is Double -> value != 0.0 && !value.isNaN()
        is Number -> value.toDouble() != 0.0
        is String -> value.isNotEmpty()
        is Collection<*> -> value.isNotEmpty()
        is Map<*, *> -> value.isNotEmpty()
        else -> true
    }

    fun <T> or(value: Any?, fallback: T): Any? = if (truthy(value)) value else fallback

    fun get(value: Any?, key: String): Any? = (value as? Map<*, *>)?.get(key)

    fun floatRepr(value: Double): String {
        if (value.isNaN()) return "NaN"
        if (value == Double.POSITIVE_INFINITY) return "Infinity"
        if (value == Double.NEGATIVE_INFINITY) return "-Infinity"
        if (value == 0.0) return if (1.0 / value < 0) "-0.0" else "0.0"
        val decimal = BigDecimal(value.toString()).stripTrailingZeros()
        val digits = decimal.unscaledValue().abs().toString()
        val exponent = digits.length - 1 - decimal.scale()
        val sign = if (value < 0) "-" else ""
        if (exponent < -4 || exponent >= 16) {
            val mantissa = if (digits.length == 1) digits else digits[0] + "." + digits.substring(1)
            val power = if (exponent < 0) "-" + (-exponent).toString().padStart(2, '0') else "+" + exponent.toString().padStart(2, '0')
            return "$sign${mantissa}e$power"
        }
        val plain = decimal.abs().toPlainString()
        return sign + if (plain.contains('.')) plain else "$plain.0"
    }

    fun number(value: Number): String = when (value) {
        is Double -> if (value.isFinite() && value == Math.rint(value)) {
            BigDecimal(value).toBigInteger().toString()
        } else {
            floatRepr(value)
        }
        is Float -> number(value.toDouble())
        is BigDecimal -> value.toPlainString()
        else -> value.toString()
    }

    fun quoteJson(text: String): String {
        val out = StringBuilder("\"")
        for (c in text) {
            when {
                c == '"' -> out.append("\\\"")
                c == '\\' -> out.append("\\\\")
                c == '\n' -> out.append("\\n")
                c == '\r' -> out.append("\\r")
                c == '\t' -> out.append("\\t")
                c == '\b' -> out.append("\\b")
                c == '\u000c' -> out.append("\\f")
                c.code < 0x20 || c.code >= 0x7f -> out.append(String.format("\\u%04x", c.code))
                else -> out.append(c)
            }
        }
        return out.append('"').toString()
    }

    fun dumps(value: Any?): String = when (value) {
        null -> "null"
        is Boolean -> if (value) "true" else "false"
        is Double -> floatRepr(value)
        is Float -> floatRepr(value.toDouble())
        is Number -> value.toString()
        is String -> quoteJson(value)
        is Map<*, *> -> value.entries.joinToString(", ", "{", "}") { quoteJson(it.key.toString()) + ": " + dumps(it.value) }
        is Collection<*> -> value.joinToString(", ", "[", "]") { dumps(it) }
        else -> quoteJson(value.toString())
    }

    fun str(value: Any?): String = when (value) {
        null -> "None"
        is Boolean -> if (value) "True" else "False"
        is Double -> floatRepr(value)
        is Number -> value.toString()
        is String -> value
        else -> dumps(value)
    }

    fun stringify(value: Any?): String = when (value) {
        null -> ""
        is Boolean -> if (value) "true" else "false"
        is Map<*, *>, is Collection<*> -> dumps(value)
        is Double -> number(value)
        is Number -> value.toString()
        else -> str(value)
    }

    fun toFloat(value: Any?): Double? = when (value) {
        is Boolean -> if (value) 1.0 else 0.0
        is Number -> value.toDouble()
        is String -> {
            val text = value.trim().replace("_", "")
            when {
                numberRe.matches(text) -> text.toDoubleOrNull()
                specialRe.matches(text) -> when {
                    text.contains("nan", ignoreCase = true) -> Double.NaN
                    text.startsWith("-") -> Double.NEGATIVE_INFINITY
                    else -> Double.POSITIVE_INFINITY
                }
                else -> null
            }
        }
        else -> null
    }

    fun numberValue(value: Double): Any = if (value.isFinite() && value == Math.rint(value) && Math.abs(value) < 9.2e18) value.toLong() else value

    fun percent(text: String, safe: String, plus: Boolean): String {
        val out = StringBuilder()
        for (byte in text.toByteArray(Charsets.UTF_8)) {
            val code = byte.toInt() and 0xff
            val ch = code.toChar()
            when {
                ch in '0'..'9' || ch in 'A'..'Z' || ch in 'a'..'z' || ch == '_' || ch == '.' || ch == '-' || ch == '~' ||
                    (code < 0x80 && safe.indexOf(ch) >= 0) -> out.append(ch)
                plus && ch == ' ' -> out.append('+')
                else -> out.append('%').append(String.format("%02X", code))
            }
        }
        return out.toString()
    }

    fun quote(text: String, safe: String = ""): String = percent(text, safe, false)

    fun urlencode(pairs: List<Pair<Any?, Any?>>): String =
        pairs.joinToString("&") { percent(str(it.first), "", true) + "=" + percent(str(it.second), "", true) }

    fun base64(text: String): String = java.util.Base64.getEncoder().encodeToString(text.toByteArray(Charsets.UTF_8))

    fun copy(value: Map<String, Any?>): LinkedHashMap<String, Any?> = LinkedHashMap(value)
}
