package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.json.jsonText
import java.math.BigDecimal
import java.math.RoundingMode
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.ZoneOffset

fun dartIntTryParse(text: String): Long? {
    var s = text.trim()
    if (s.isEmpty()) return null
    var negative = false
    if (s[0] == '+' || s[0] == '-') {
        negative = s[0] == '-'
        s = s.substring(1)
    }
    var radix = 10
    if (s.length > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        radix = 16
        s = s.substring(2)
    }
    if (s.isEmpty()) return null
    for (c in s) {
        val ok = c in '0'..'9' || (radix == 16 && (c in 'a'..'f' || c in 'A'..'F'))
        if (!ok) return null
    }
    return try {
        java.lang.Long.parseLong((if (negative) "-" else "") + s, radix)
    } catch (_: NumberFormatException) {
        null
    }
}

private val DART_DOUBLE = Regex("[+-]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][+-]?\\d+)?")

fun dartDoubleTryParse(text: String): Double? {
    val s = text.trim()
    if (s.isEmpty()) return null
    when (s) {
        "NaN" -> return Double.NaN
        "Infinity", "+Infinity" -> return Double.POSITIVE_INFINITY
        "-Infinity" -> return Double.NEGATIVE_INFINITY
    }
    if (!DART_DOUBLE.matches(s)) return null
    return s.toDoubleOrNull()
}

private fun dartInterpolate(value: Any?): String = if (value == null) "null" else jsonText(value)

fun modelInt(value: Any?): Long = when (value) {
    is Long -> value
    is Int -> value.toLong()
    is Short -> value.toLong()
    is Byte -> value.toLong()
    is Number -> value.toDouble().let { if (it.isNaN()) 0L else it.toLong() }
    else -> dartIntTryParse(dartInterpolate(value)) ?: 0L
}

fun modelInt(value: Any?, fallback: Long): Long = when (value) {
    null -> fallback
    is Number -> modelInt(value)
    else -> dartIntTryParse(dartInterpolate(value)) ?: fallback
}

fun modelDouble(value: Any?): Double = when (value) {
    is Number -> value.toDouble()
    else -> dartDoubleTryParse(dartInterpolate(value)) ?: 0.0
}

fun modelBool(value: Any?): Boolean {
    if (value is Boolean) return value
    val text = dartInterpolate(value).trim().lowercase()
    return text == "true" || text == "1" || text == "yes"
}

fun modelString(value: Any?): String = if (value == null) "" else jsonText(value)

fun firstModelString(json: Map<String, Any?>, keys: List<String>): String {
    for (key in keys) {
        val value = modelString(json[key]).trim()
        if (value.isNotEmpty()) return value
    }
    return ""
}

fun modelEpochMs(value: Any?): Long {
    val n = modelInt(value)
    if (n <= 0) return 0
    if (n < 1_000_000_000_000L) return n * 1000
    return n
}

fun modelTitleCase(value: String): String {
    val clean = value.replace('_', ' ').replace('-', ' ').trim()
    if (clean.isEmpty()) return ""
    return clean.split(Regex("\\s+")).joinToString(" ") { part ->
        if (part.isEmpty()) part else part.substring(0, 1).uppercase() + part.substring(1).lowercase()
    }
}

fun modelTruncate(value: String, maxLength: Int): String {
    if (maxLength <= 0 || value.length <= maxLength) return value
    if (maxLength <= 3) return "..."
    return value.substring(0, maxLength - 3) + "..."
}

fun modelCompactDuration(durationMs: Long): String {
    val seconds = (durationMs / 1000).coerceIn(0L, 10L * 24 * 60 * 60)
    val days = seconds / 86_400
    val hours = (seconds / 3_600) % 24
    val minutes = (seconds / 60) % 60
    val secs = seconds % 60
    if (days > 0) return "${days}d ${hours}h"
    if (hours > 0) return "${hours}h ${minutes}m"
    if (minutes > 0) return "${minutes}m ${secs}s"
    return "${secs}s"
}

fun dartDoubleToString(value: Double): String {
    if (value.isNaN()) return "NaN"
    if (value.isInfinite()) return if (value > 0) "Infinity" else "-Infinity"
    if (value == 0.0) return if (1.0 / value < 0) "-0.0" else "0.0"
    val magnitude = Math.abs(value)
    val decimal = BigDecimal(value.toString()).stripTrailingZeros()
    if (magnitude >= 1e-6 && magnitude < 1e21) {
        val plain = decimal.toPlainString()
        return if (plain.contains('.')) plain else "$plain.0"
    }
    val digits = decimal.unscaledValue().abs().toString()
    val exponent = digits.length - 1 - decimal.scale()
    val mantissa = if (digits.length == 1) digits else digits[0] + "." + digits.substring(1)
    val sign = if (value < 0) "-" else ""
    return "$sign${mantissa}e${if (exponent >= 0) "+" else "-"}${Math.abs(exponent)}"
}

fun dartToStringAsFixed(value: Double, digits: Int): String =
    BigDecimal(value).setScale(digits, RoundingMode.HALF_UP).toPlainString()

fun modelTrimDouble(value: Double): String {
    val text = dartDoubleToString(value)
    return if (text.contains('.')) text.replaceFirst(Regex("\\.?0+$"), "") else text
}

fun modelLocalDate(epochMs: Long): String {
    val date = java.time.Instant.ofEpochMilli(epochMs).atZone(ZoneId.systemDefault()).toLocalDate()
    return "${date.year}-${date.monthValue.toString().padStart(2, '0')}-${date.dayOfMonth.toString().padStart(2, '0')}"
}

private val DART_DATE_TIME = Regex(
    "^([+-]?\\d{4,6})-?(\\d\\d)-?(\\d\\d)" +
        "(?:[ T](\\d\\d)(?::?(\\d\\d)(?::?(\\d\\d)(?:[.,](\\d+))?)?)?" +
        "( ?[zZ]| ?([-+])(\\d\\d)(?::?(\\d\\d))?)?)?$",
)

fun dartDateTimeTryParseMs(text: String): Long? {
    val match = DART_DATE_TIME.find(text) ?: return null
    val g = match.groupValues
    fun num(index: Int): Long = if (g[index].isEmpty()) 0L else g[index].toLong()
    val year = num(1)
    val month = if (g[2].isEmpty()) 1L else num(2)
    val day = if (g[3].isEmpty()) 1L else num(3)
    val fraction = g[7]
    val micros = if (fraction.isEmpty()) 0L else fraction.take(6).padEnd(6, '0').toLong()
    val base = LocalDateTime.of(year.toInt(), 1, 1, 0, 0)
        .plusMonths(month - 1)
        .plusDays(day - 1)
        .plusHours(num(4))
        .plusMinutes(num(5))
        .plusSeconds(num(6))
        .plusNanos(micros * 1000)
    return try {
        if (g[8].isEmpty()) {
            base.atZone(ZoneId.systemDefault()).toInstant().toEpochMilli()
        } else {
            var utc = base.toInstant(ZoneOffset.UTC).toEpochMilli()
            if (g[9].isNotEmpty()) {
                val offsetMinutes = num(10) * 60 + num(11)
                val sign = if (g[9] == "-") 1 else -1
                utc += sign * offsetMinutes * 60_000
            }
            utc
        }
    } catch (_: Exception) {
        null
    }
}
