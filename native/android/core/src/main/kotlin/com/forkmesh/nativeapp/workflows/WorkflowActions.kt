package com.forkmesh.nativeapp.workflows

import java.math.BigDecimal
import java.math.RoundingMode
import java.time.Instant
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.ZoneOffset
import java.time.zone.ZoneRules

internal class WorkflowStop(val out: Map<String, List<Any?>>) : Exception(null, null, false, false)

internal object WorkflowActions {
    const val MAX_WAIT_SECONDS = 60
    const val MAX_REPEAT = 100
    const val MAX_WORKFLOW_DEPTH = 3
    private const val MAX_TEXT = 1000000
    private const val DATE_MIN = -62135596800000L
    private const val DATE_MAX = 253402300799999L
    private const val ASCII_SPACE = " \t\n\r\u000b\u000c"

    val TYPES = setOf(
        "core.code", "core.variable", "core.switch", "core.repeat", "core.wait", "core.runWorkflow", "core.stop", "core.comment",
        "core.textCase", "core.textReplace", "core.textMatch", "core.textSplit", "core.textCombine", "core.textEncode", "core.hash",
        "core.textExtract", "core.count", "core.math", "core.round", "core.formatNumber", "core.random", "core.statistics",
        "core.combineItems", "core.sort", "core.dedupe", "core.pick", "core.json", "core.url", "core.dateFormat", "core.dateAdjust",
        "core.dateDiff",
    )

    val VARIABLE_RE = Regex("^[A-Za-z_][A-Za-z0-9_]{0,63}$")
    private val OFFSET_RE = Regex("^([+-])([0-9]{2}):?([0-9]{2})$")
    private val DATE_RE = Regex(
        "^([0-9]{4})-([0-9]{2})-([0-9]{2})(?:[T ]([0-9]{2}):([0-9]{2})(?::([0-9]{2})(?:\\.([0-9]{1,9}))?)?)?[ ]*(Z|z|[+-][0-9]{2}:?[0-9]{2})?$",
    )
    private val NUMBER_TEXT_RE = Regex("^[+-]?(?:[0-9]+\\.?[0-9]*|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?$")
    private val URL_RE = Regex("https?://[^ \\t\\n\\r\\u000b\\u000c<>\"'`]+", RegexOption.IGNORE_CASE)
    private val EMAIL_RE = Regex("[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\\.[A-Za-z0-9-]+)*\\.[A-Za-z]{2,}")
    private val NUMBER_RE = Regex("-?[0-9]{1,3}(?:,[0-9]{3})+(?:\\.[0-9]+)?|-?[0-9]+(?:\\.[0-9]+)?")
    private val PHONE_RE = Regex("\\+?[0-9][0-9 ().-]{5,}[0-9]")
    private val HASHTAG_RE = Regex("(?<![A-Za-z0-9_&])#[A-Za-z0-9_]+")
    private val MENTION_RE = Regex("(?<![A-Za-z0-9._%+-])@[A-Za-z0-9_]+")
    private val URI_RE = Regex("^(?:([A-Za-z][A-Za-z0-9+.-]*):)?(?://([^/?#]*))?([^?#]*)(?:\\?([^#]*))?(?:#(.*))?$", RegexOption.DOT_MATCHES_ALL)
    private val TOKEN_RE = Regex("\\[[^\\]]*\\]|YYYY|YY|MMMM|MMM|MM|M|DD|D|dddd|ddd|HH|H|hh|h|mm|m|ss|s|SSS|A|a|ZZ|Z|X|x")
    private val ZONE_RE = Regex("^[A-Za-z][A-Za-z0-9_+-]*(?:/[A-Za-z0-9_+-]+)*$")
    private val HTML_ENTITY_RE = Regex("&(#[0-9]{1,7}|#[xX][0-9A-Fa-f]{1,6}|[A-Za-z]+);")
    private val HTML_NAMED = mapOf("amp" to "&", "lt" to "<", "gt" to ">", "quot" to "\"", "apos" to "'", "nbsp" to "\u00a0")
    private val SPACES_RE = Regex("[ \\t\\n\\r\\u000b\\u000c]+")
    private val LINES_RE = Regex("\\r\\n|\\n|\\r")
    private val MONTHS = listOf("January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December")
    private val WEEKDAYS = listOf("Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday")
    private val UNIT_MS = mapOf("seconds" to 1000L, "minutes" to 60000L, "hours" to 3600000L, "days" to 86400000L, "weeks" to 604800000L)

    fun fail(message: String): Nothing = throw WorkflowNodeError(message)

    fun stringify(value: Any?): String = WorkflowPy.stringify(value)

    fun numberOut(value: Any?): Any? {
        if (value is Double && value.isFinite() && value == Math.rint(value) && Math.abs(value) < 1e18) return value.toLong()
        return value
    }

    fun toNumber(value: Any?, label: String): Double {
        if (value is Boolean || value == null) fail("$label must be a number")
        val number = if (value is Number) {
            value.toDouble()
        } else {
            val text = WorkflowPy.str(value).trim()
            if (!NUMBER_TEXT_RE.matches(text)) fail("$label must be a number")
            text.toDouble()
        }
        if (number.isNaN() || number.isInfinite()) fail("$label must be a number")
        return number
    }

    fun copyJson(value: Any?): Any? = when (value) {
        is Map<*, *> -> LinkedHashMap<String, Any?>().also { out -> for ((k, v) in value) out[k.toString()] = copyJson(v) }
        is List<*> -> ArrayList(value.map { copyJson(it) })
        else -> value
    }

    @Suppress("UNCHECKED_CAST")
    fun setPath(target: MutableMap<String, Any?>, dotted: String, value: Any?) {
        val parts = dotted.split(".")
        var current = target
        for (part in parts.dropLast(1)) {
            val next = current[part]
            current = if (next is Map<*, *>) {
                LinkedHashMap(next as Map<String, Any?>).also { current[part] = it }
            } else {
                LinkedHashMap<String, Any?>().also { current[part] = it }
            }
        }
        current[parts.last()] = value
    }

    fun withField(item: Any?, field: String, value: Any?): Map<String, Any?> {
        val out = LinkedHashMap(WorkflowPy.map(item))
        setPath(out, field, value)
        return out
    }

    fun changeCase(text: String, mode: String): String = when (mode) {
        "upper" -> text.uppercase()
        "lower" -> text.lowercase()
        "capitalize" -> cpSub(text, 0, 1).uppercase() + cpSub(text, 1)
        else -> {
            val out = StringBuilder()
            var previousSpace = true
            for (ch in cpList(text)) {
                out.append(if (previousSpace) ch.uppercase() else ch)
                previousSpace = ch.length == 1 && ASCII_SPACE.indexOf(ch[0]) >= 0
            }
            out.toString()
        }
    }

    fun regex(pattern: String, ignoreCase: Boolean, flags: String = "g"): JsRegExp {
        val compiled = try {
            compileRegex(pattern, flags + if (ignoreCase) "i" else "")
        } catch (_: ScriptError) {
            fail("Invalid regular expression $pattern")
        }
        return JsRegExp(pattern, flags, compiled)
    }

    private fun escape(text: String): String {
        val out = StringBuilder()
        for (ch in text) {
            if ("\\^$.|?*+()[]{}/".indexOf(ch) >= 0) out.append('\\')
            out.append(ch)
        }
        return out.toString()
    }

    fun replaceText(text: String, find: String, replacement: String, useRegex: Boolean, ignoreCase: Boolean): String {
        if (find.isEmpty()) fail("Find is required")
        if (!useRegex && !ignoreCase) return text.replace(find, replacement)
        val pattern = regex(if (useRegex) find else escape(find), ignoreCase)
        val out = StringBuilder()
        var last = 0
        for (match in regexAll(pattern.compiled, text)) {
            val groups = match.groups.map { it ?: Undefined }
            val named: Map<String, Any?>? = if (pattern.names.isNotEmpty()) pattern.names.associateWith { match.named[it] ?: Undefined } else null
            out.append(cpSub(text, last, match.start))
            out.append(expandReplacement(if (useRegex) replacement else replacement.replace("$", "$$"), match.group0, match.start, text, groups, named))
            last = match.end
        }
        out.append(cpSub(text, last))
        val result = out.toString()
        if (cpLen(result) > MAX_TEXT) fail("The text is too long")
        return result
    }

    fun matchText(text: String, pattern: String, mode: String, ignoreCase: Boolean): Any? {
        val compiled = regex(pattern, ignoreCase)
        if (mode == "test") return regexSearch(compiled.compiled, text, 0) != null
        if (mode == "all") return ArrayList(regexAll(compiled.compiled, text).map { it.group0 })
        val match = regexSearch(compiled.compiled, text, 0)
        if (mode == "first") return match?.group0 ?: ""
        if (match == null) return ArrayList<Any?>()
        return ArrayList(match.groups.map { it ?: "" })
    }

    private fun pySplit(text: String, re: Regex): List<String> {
        val out = ArrayList<String>()
        var last = 0
        for (m in re.findAll(text)) {
            out.add(text.substring(last, m.range.first))
            last = m.range.last + 1
        }
        out.add(text.substring(last))
        return out
    }

    private fun plainSplit(text: String, sep: String): List<String> {
        val out = ArrayList<String>()
        var start = 0
        while (true) {
            val found = text.indexOf(sep, start)
            if (found < 0) break
            out.add(text.substring(start, found))
            start = found + sep.length
        }
        out.add(text.substring(start))
        return out
    }

    private fun asciiStrip(text: String): String = text.trim { ASCII_SPACE.indexOf(it) >= 0 }

    fun splitText(text: String, separator: String, custom: String, skipEmpty: Boolean): List<Any?> {
        var parts: List<String> = when (separator) {
            "newlines" -> pySplit(text, LINES_RE)
            "spaces" -> pySplit(text, SPACES_RE)
            "commas" -> plainSplit(text, ",").map { asciiStrip(it) }
            "characters" -> cpList(text)
            else -> {
                if (custom.isEmpty()) fail("Enter a custom separator")
                plainSplit(text, custom)
            }
        }
        if (skipEmpty) parts = parts.filter { it != "" }
        return ArrayList(parts)
    }

    fun combineText(value: Any?, separator: String, custom: String): String {
        val joiner = when (separator) {
            "newlines" -> "\n"
            "spaces" -> " "
            "commas" -> ", "
            "none" -> ""
            else -> custom
        }
        val items = if (value is List<*>) value else if (value == null || value == "") emptyList() else listOf(value)
        val result = items.joinToString(joiner) { stringify(it) }
        if (cpLen(result) > MAX_TEXT) fail("The text is too long")
        return result
    }

    fun base64Encode(text: String): String = java.util.Base64.getEncoder().encodeToString(text.toByteArray(Charsets.UTF_8))

    private const val B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

    fun base64Decode(text: String): String {
        val clean = text.filter { ASCII_SPACE.indexOf(it) < 0 }.replace('-', '+').replace('_', '/').trimEnd('=')
        if (clean.length % 4 == 1 || clean.any { B64.indexOf(it) < 0 }) fail("The text is not valid Base64")
        val out = java.io.ByteArrayOutputStream()
        var buffer = 0
        var bits = 0
        for (ch in clean) {
            buffer = (buffer shl 6) or B64.indexOf(ch)
            bits += 6
            if (bits >= 8) {
                bits -= 8
                out.write((buffer shr bits) and 0xff)
            }
        }
        return String(out.toByteArray(), Charsets.UTF_8)
    }

    fun urlEncode(text: String): String {
        val out = StringBuilder()
        for (byte in text.toByteArray(Charsets.UTF_8)) {
            val code = byte.toInt() and 0xff
            val ch = code.toChar()
            if (code < 128 && (ch.isLetterOrDigit() || ch in "_.-~")) out.append(ch) else out.append(String.format("%%%02X", code))
        }
        return out.toString()
    }

    fun percentDecode(text: String, plus: Boolean = false): String {
        val data = java.io.ByteArrayOutputStream()
        val out = StringBuilder()
        var index = 0
        while (index < text.length) {
            val ch = text[index]
            if (ch == '%' && index + 3 <= text.length && text.substring(index + 1, index + 3).all { it in "0123456789abcdefABCDEF" }) {
                data.write(text.substring(index + 1, index + 3).toInt(16))
                index += 3
                continue
            }
            if (data.size() > 0) {
                out.append(String(data.toByteArray(), Charsets.UTF_8))
                data.reset()
            }
            out.append(if (plus && ch == '+') ' ' else ch)
            index++
        }
        if (data.size() > 0) out.append(String(data.toByteArray(), Charsets.UTF_8))
        return out.toString()
    }

    fun htmlEscape(text: String): String =
        text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;").replace("'", "&#x27;")

    fun htmlUnescape(text: String): String = HTML_ENTITY_RE.replace(text) { match ->
        val body = match.groupValues[1]
        if (body[0] == '#') {
            val code = if (body[1] == 'x' || body[1] == 'X') body.substring(2).toLong(16) else body.substring(1).toLong()
            if (code == 0L || code > 0x10FFFF || code in 0xD800..0xDFFF) match.value else String(Character.toChars(code.toInt()))
        } else {
            HTML_NAMED[body] ?: match.value
        }
    }

    fun encodeText(text: String, method: String): String = when (method) {
        "base64Encode" -> base64Encode(text)
        "base64Decode" -> base64Decode(text)
        "urlEncode" -> urlEncode(text)
        "urlDecode" -> percentDecode(text)
        "htmlEscape" -> htmlEscape(text)
        else -> htmlUnescape(text)
    }

    private val HASHES = mapOf("md5" to ("MD5" to "HmacMD5"), "sha1" to ("SHA-1" to "HmacSHA1"), "sha256" to ("SHA-256" to "HmacSHA256"),
        "sha512" to ("SHA-512" to "HmacSHA512"))

    fun hashText(text: String, algorithm: String, key: String = "", encoding: String = "hex"): String {
        val names = HASHES[algorithm] ?: fail("Unknown hash algorithm $algorithm")
        val data = text.toByteArray(Charsets.UTF_8)
        val digest = if (key.isNotEmpty()) {
            val mac = javax.crypto.Mac.getInstance(names.second)
            mac.init(javax.crypto.spec.SecretKeySpec(key.toByteArray(Charsets.UTF_8), names.second))
            mac.doFinal(data)
        } else {
            java.security.MessageDigest.getInstance(names.first).digest(data)
        }
        if (encoding == "base64") return java.util.Base64.getEncoder().encodeToString(digest)
        return digest.joinToString("") { String.format("%02x", it.toInt() and 0xff) }
    }

    private fun trimUrl(raw: String): String {
        var url = raw.trimEnd { it in ".,;:!?'\"" }
        while (url.endsWith(")") && url.count { it == '(' } < url.count { it == ')' }) url = url.dropLast(1).trimEnd { it in ".,;:!?'\"" }
        return url
    }

    fun extract(text: String, kind: String): List<Any?> = when (kind) {
        "urls" -> URL_RE.findAll(text).map { trimUrl(it.value) }.toList()
        "emails" -> EMAIL_RE.findAll(text).map { it.value }.toList()
        "numbers" -> NUMBER_RE.findAll(text).map { numberOut(it.value.replace(",", "").toDouble()) }.toList()
        "phones" -> PHONE_RE.findAll(text).map { it.value }.filter { value -> value.count { it in '0'..'9' } in 7..15 }.toList()
        "hashtags" -> HASHTAG_RE.findAll(text).map { it.value }.toList()
        else -> MENTION_RE.findAll(text).map { it.value }.toList()
    }.let { ArrayList(it) }

    fun count(value: Any?, what: String): Long {
        if (what == "items") {
            if (value is List<*>) return value.size.toLong()
            if (value is Map<*, *>) return value.size.toLong()
            return if (value == null || value == "") 0 else 1
        }
        val text = stringify(value)
        if (what == "characters") return cpLen(text).toLong()
        if (what == "words") return pySplit(text, SPACES_RE).count { it.isNotEmpty() }.toLong()
        if (text.isEmpty()) return 0
        val lines = pySplit(text, LINES_RE).toMutableList()
        if (lines.isNotEmpty() && lines.last() == "") lines.removeAt(lines.size - 1)
        return lines.size.toLong()
    }

    private fun pyMod(a: Double, b: Double): Double {
        var result = a % b
        if (result != 0.0 && (b < 0) != (result < 0)) result += b
        return result
    }

    fun calculate(a: Double, operator: String, b: Double?): Double {
        if (operator == "sqrt") {
            if (a < 0) fail("Cannot take the square root of a negative number")
            return Math.sqrt(a)
        }
        if (operator == "abs") return Math.abs(a)
        if (b == null) fail("Second number must be a number")
        val result = when (operator) {
            "add" -> a + b
            "subtract" -> a - b
            "multiply" -> a * b
            "divide" -> {
                if (b == 0.0) fail("Cannot divide by zero")
                a / b
            }
            "modulo" -> {
                if (b == 0.0) fail("Cannot divide by zero")
                pyMod(a, b)
            }
            "power" -> {
                val r = StrictMath.pow(a, b)
                if (r.isNaN()) fail("The result is not a number")
                if (r.isInfinite()) fail("The result is too large")
                r
            }
            "min" -> minOf(a, b)
            else -> maxOf(a, b)
        }
        if (result.isInfinite()) fail("The result is too large")
        return result
    }

    private fun decimalOf(value: Double): BigDecimal = shortestDecimal(value)

    fun roundNumber(value: Double, decimals: Any?, mode: String): Double {
        val places = (WorkflowPy.toFloat(decimals) ?: 0.0).coerceIn(-15.0, 15.0).toInt()
        val rounding = when (mode) {
            "up" -> RoundingMode.CEILING
            "down" -> RoundingMode.FLOOR
            else -> RoundingMode.HALF_UP
        }
        val exact = decimalOf(value)
        val rounded = if (places >= 0) {
            exact.setScale(places, rounding)
        } else {
            exact.movePointLeft(-places).setScale(0, rounding).movePointRight(-places)
        }
        return rounded.toDouble() + 0.0
    }

    fun formatNumber(value: Double, decimals: Any?, grouping: Boolean): String {
        val places = (WorkflowPy.toFloat(decimals) ?: 0.0).coerceIn(0.0, 15.0).toInt()
        val exact = decimalOf(value).setScale(places, RoundingMode.HALF_UP)
        val negative = exact.signum() < 0
        val text = exact.abs().toPlainString()
        var whole = text.substringBefore('.')
        val fraction = if (text.contains('.')) text.substringAfter('.') else ""
        if (grouping) {
            val groups = ArrayList<String>()
            while (whole.length > 3) {
                groups.add(0, whole.takeLast(3))
                whole = whole.dropLast(3)
            }
            groups.add(0, whole)
            whole = groups.joinToString(",")
        }
        var result = whole + if (fraction.isNotEmpty()) ".$fraction" else ""
        if (negative && text.any { it in '1'..'9' }) result = "-$result"
        return result
    }

    fun statistics(values: List<Any?>, operation: String): Any {
        val numbers = ArrayList<Double>()
        for (value in values) {
            for (entry in if (value is List<*>) value else listOf(value)) {
                if (entry is Boolean || entry == null) continue
                if (entry is Number) {
                    numbers.add(entry.toDouble())
                } else if (entry is String && NUMBER_TEXT_RE.matches(entry.trim())) {
                    numbers.add(entry.trim().toDouble())
                }
            }
        }
        if (operation == "count") return numbers.size.toLong()
        var total = 0.0
        for (number in numbers) total += number
        if (operation == "sum") return total
        if (numbers.isEmpty()) fail("There are no numbers to calculate with")
        if (operation == "average") return total / numbers.size
        val ordered = numbers.sorted()
        when (operation) {
            "median" -> {
                val middle = ordered.size / 2
                return if (ordered.size % 2 == 1) ordered[middle] else (ordered[middle - 1] + ordered[middle]) / 2
            }
            "min" -> return ordered.first()
            "max" -> return ordered.last()
            "range" -> return ordered.last() - ordered.first()
        }
        if (numbers.size < 2) fail("A standard deviation needs at least two numbers")
        val mean = total / numbers.size
        var squares = 0.0
        for (number in numbers) squares += (number - mean) * (number - mean)
        return Math.sqrt(squares / (numbers.size - 1))
    }

    class SortKey(val kind: Int, val number: Double, val text: String) : Comparable<SortKey> {
        override fun compareTo(other: SortKey): Int {
            if (kind != other.kind) return kind.compareTo(other.kind)
            if (number != other.number) return number.compareTo(other.number)
            return cpCompare(text, other.text)
        }
    }

    fun sortKey(value: Any?): SortKey? {
        if (value == null) return null
        if (value is Number) return SortKey(0, value.toDouble(), "")
        return SortKey(1, 0.0, stringify(value))
    }

    fun sortItems(items: List<Any?>, keys: List<SortKey?>, order: String, random: () -> Double): List<Any?> {
        if (order == "reverse") return items.reversed()
        if (order == "shuffle") {
            val out = ArrayList(items)
            for (index in out.size - 1 downTo 1) {
                val other = Math.floor(random() * (index + 1)).toInt()
                val swap = out[index]
                out[index] = out[other]
                out[other] = swap
            }
            return out
        }
        val present = items.indices.filter { keys[it] != null }
        val missing = items.indices.filter { keys[it] == null }.map { items[it] }
        val sorted = if (order == "descending") present.sortedWith { a, b -> keys[b]!!.compareTo(keys[a]!!) } else present.sortedWith { a, b -> keys[a]!!.compareTo(keys[b]!!) }
        return sorted.map { items[it] } + missing
    }

    private fun parseOffset(text: String): Int? {
        val match = OFFSET_RE.find(text) ?: return null
        val hours = match.groupValues[2].toInt()
        val minutes = match.groupValues[3].toInt()
        if (hours > 23 || minutes > 59) return null
        val total = hours * 60 + minutes
        return if (match.groupValues[1] == "+") total else -total
    }

    fun checkRange(ms: Long): Long {
        if (ms < DATE_MIN || ms > DATE_MAX) fail("The date is out of range")
        return ms
    }

    class Zone(raw: String) {
        var fixed: Int? = null
        private var rules: ZoneRules? = null

        init {
            val text = raw.trim()
            if (text.isEmpty() || text.uppercase() in setOf("UTC", "Z", "GMT")) {
                fixed = 0
            } else {
                val offset = parseOffset(text)
                if (offset != null) {
                    fixed = offset
                } else {
                    if (!ZONE_RE.matches(text) || text !in ZoneId.getAvailableZoneIds()) fail("Unknown time zone $text")
                    rules = try {
                        ZoneId.of(text).rules
                    } catch (_: Exception) {
                        fail("Unknown time zone $text")
                    }
                }
            }
        }

        fun offsetMinutes(ms: Long): Int {
            fixed?.let { return it }
            checkRange(ms)
            val seconds = rules!!.getOffset(Instant.ofEpochSecond(Math.floorDiv(ms, 1000L))).totalSeconds
            return Math.floorDiv(seconds, 60)
        }

        fun localToUtc(localMs: Long): Long {
            fixed?.let { return localMs - it * 60000L }
            checkRange(localMs)
            val local = LocalDateTime.ofEpochSecond(Math.floorDiv(localMs, 1000L), 0, ZoneOffset.UTC)
            val zoneRules = rules!!
            val transition = zoneRules.getTransition(local)
            val offset = transition?.offsetBefore ?: zoneRules.getValidOffsets(local).first()
            return localMs - offset.totalSeconds * 1000L
        }
    }

    fun parseDate(value: Any?, zone: Zone): Long {
        if (value is Boolean || value == null || value == "") fail("Cannot read the date " + stringify(value))
        val number: Double
        if (value is Number) {
            number = value.toDouble()
        } else {
            val text = WorkflowPy.str(value).trim()
            if (NUMBER_TEXT_RE.matches(text)) {
                number = text.toDouble()
            } else {
                val match = DATE_RE.find(text) ?: fail("Cannot read the date " + text.take(80))
                val g = match.groupValues
                val year = g[1].toLong()
                val month = g[2].toInt()
                val day = g[3].toInt()
                val hours = g[4].ifEmpty { "0" }.toInt()
                val minutes = g[5].ifEmpty { "0" }.toInt()
                val seconds = g[6].ifEmpty { "0" }.toInt()
                val millis = g[7].ifEmpty { "0" }.take(3).padEnd(3, '0').toInt()
                if (!(month in 1..12 && day >= 1 && day <= monthDays(year, month) && hours <= 23 && minutes <= 59 && seconds <= 59)) {
                    fail("Cannot read the date " + text.take(80))
                }
                val local = dateFromParts(year, (month - 1).toLong(), day.toLong(), hours.toLong(), minutes.toLong(), seconds.toLong(), millis.toLong())
                val offset = g[8]
                if (offset == "Z" || offset == "z") return checkRange(local)
                if (offset.isNotEmpty()) {
                    val minutesOffset = parseOffset(offset) ?: fail("Cannot read the date " + text.take(80))
                    return checkRange(local - minutesOffset * 60000L)
                }
                return checkRange(zone.localToUtc(local))
            }
        }
        if (number.isNaN() || number.isInfinite()) fail("Cannot read the date " + stringify(value))
        val ms = if (Math.abs(number) >= 1e11) number else number * 1000
        val floor = Math.floor(ms)
        if (floor < DATE_MIN || floor > DATE_MAX) fail("The date is out of range")
        return checkRange(floor.toLong())
    }

    private fun localParts(ms: Long, zone: Zone): Pair<Civil, Int> {
        val offset = zone.offsetMinutes(ms)
        return civilFromMs(ms + offset * 60000L) to offset
    }

    private fun offsetText(offset: Int, colon: Boolean = true): String {
        val sign = if (offset >= 0) "+" else "-"
        val hours = Math.abs(offset) / 60
        val minutes = Math.abs(offset) % 60
        return String.format("%s%02d%s%02d", sign, hours, if (colon) ":" else "", minutes)
    }

    fun isoDate(ms: Long, zone: Zone): String {
        val (c, offset) = localParts(ms, zone)
        var text = String.format("%04d-%02d-%02dT%02d:%02d:%02d", c.year, c.month, c.day, c.hours, c.minutes, c.seconds)
        if (c.millis != 0) text += String.format(".%03d", c.millis)
        return text + if (zone.fixed == 0) "Z" else offsetText(offset)
    }

    private fun formatPattern(ms: Long, zone: Zone, pattern: String): String {
        val (c, offset) = localParts(ms, zone)
        val twelve = if (c.hours % 12 == 0) 12 else c.hours % 12
        return TOKEN_RE.replace(pattern) { match ->
            val text = match.value
            if (text.startsWith("[")) {
                text.substring(1, text.length - 1)
            } else {
                when (text) {
                    "YYYY" -> String.format("%04d", c.year)
                    "YY" -> String.format("%02d", Math.floorMod(c.year, 100L))
                    "MMMM" -> MONTHS[c.month - 1]
                    "MMM" -> MONTHS[c.month - 1].take(3)
                    "MM" -> String.format("%02d", c.month)
                    "M" -> c.month.toString()
                    "DD" -> String.format("%02d", c.day)
                    "D" -> c.day.toString()
                    "dddd" -> WEEKDAYS[c.weekday]
                    "ddd" -> WEEKDAYS[c.weekday].take(3)
                    "HH" -> String.format("%02d", c.hours)
                    "H" -> c.hours.toString()
                    "hh" -> String.format("%02d", twelve)
                    "h" -> twelve.toString()
                    "mm" -> String.format("%02d", c.minutes)
                    "m" -> c.minutes.toString()
                    "ss" -> String.format("%02d", c.seconds)
                    "s" -> c.seconds.toString()
                    "SSS" -> String.format("%03d", c.millis)
                    "A" -> if (c.hours < 12) "AM" else "PM"
                    "a" -> if (c.hours < 12) "am" else "pm"
                    "ZZ" -> offsetText(offset, false)
                    "Z" -> offsetText(offset)
                    "X" -> Math.floorDiv(ms, 1000L).toString()
                    else -> ms.toString()
                }
            }
        }
    }

    fun formatDate(value: Any?, format: String, pattern: String = "", zoneName: String = ""): Any {
        val zone = Zone(zoneName)
        val ms = parseDate(value, zone)
        return when (format) {
            "iso" -> isoDate(ms, zone)
            "unix" -> Math.floorDiv(ms, 1000L)
            "unixMs" -> ms
            "custom" -> {
                if (pattern.isEmpty()) fail("Enter a custom pattern")
                formatPattern(ms, zone, pattern)
            }
            "date" -> formatPattern(ms, zone, "YYYY-MM-DD")
            "time" -> formatPattern(ms, zone, "HH:mm")
            "datetime" -> formatPattern(ms, zone, "YYYY-MM-DD HH:mm")
            else -> formatPattern(ms, zone, "dddd, D MMMM YYYY")
        }
    }

    private fun addMonths(ms: Long, months: Long, zone: Zone): Long {
        val (c, _) = localParts(ms, zone)
        val total = c.year * 12 + (c.month - 1) + months
        val year = Math.floorDiv(total, 12L)
        val month = Math.floorMod(total, 12L)
        val day = minOf(c.day, monthDays(year, (month + 1).toInt()))
        val local = dateFromParts(year, month, day.toLong(), c.hours.toLong(), c.minutes.toLong(), c.seconds.toLong(), c.millis.toLong())
        return zone.localToUtc(local)
    }

    private fun addDays(ms: Long, days: Long, zone: Zone): Long {
        val (c, _) = localParts(ms, zone)
        val local = dateFromParts(c.year, (c.month - 1).toLong(), c.day + days, c.hours.toLong(), c.minutes.toLong(), c.seconds.toLong(), c.millis.toLong())
        return zone.localToUtc(local)
    }

    private fun startOf(ms: Long, unit: String, zone: Zone): Long {
        val (c, _) = localParts(ms, zone)
        val m = (c.month - 1).toLong()
        val local = when (unit) {
            "seconds" -> dateFromParts(c.year, m, c.day.toLong(), c.hours.toLong(), c.minutes.toLong(), c.seconds.toLong())
            "minutes" -> dateFromParts(c.year, m, c.day.toLong(), c.hours.toLong(), c.minutes.toLong())
            "hours" -> dateFromParts(c.year, m, c.day.toLong(), c.hours.toLong())
            "days" -> dateFromParts(c.year, m, c.day.toLong())
            "weeks" -> dateFromParts(c.year, m, (c.day - (c.weekday + 6) % 7).toLong())
            "months" -> dateFromParts(c.year, m, 1)
            else -> dateFromParts(c.year, 0, 1)
        }
        return zone.localToUtc(local)
    }

    private fun shift(ms: Long, amount: Double, unit: String, zone: Zone): Long {
        UNIT_MS[unit]?.let { size ->
            if (unit == "seconds" || unit == "minutes" || unit == "hours") {
                val delta = amount * size
                return ms + (if (delta < 0) Math.ceil(delta) else Math.floor(delta)).toLong()
            }
        }
        if (amount != Math.rint(amount)) fail("Use a whole number of $unit")
        val whole = amount.toLong()
        return when (unit) {
            "days" -> addDays(ms, whole, zone)
            "weeks" -> addDays(ms, whole * 7, zone)
            "months" -> addMonths(ms, whole, zone)
            else -> addMonths(ms, whole * 12, zone)
        }
    }

    fun adjustDate(value: Any?, operation: String, amount: Any?, unit: String, zoneName: String = ""): String {
        val zone = Zone(zoneName)
        val ms = parseDate(value, zone)
        val result = if (operation == "startOf" || operation == "endOf") {
            val start = startOf(ms, unit, zone)
            if (operation == "endOf") shift(start, 1.0, unit, zone) - 1000 else start
        } else {
            if (amount == null) fail("Amount is required")
            val number = (amount as Number).toDouble()
            shift(ms, if (operation == "add") number else -number, unit, zone)
        }
        return isoDate(checkRange(result), zone)
    }

    private fun monthsBetween(a: Long, b: Long): Long {
        if (b < a) return -monthsBetween(b, a)
        val utc = Zone("")
        val ca = civilFromMs(a)
        val cb = civilFromMs(b)
        var months = (cb.year - ca.year) * 12 + (cb.month - ca.month)
        while (months > 0 && addMonths(a, months, utc) > b) months--
        return months
    }

    fun dateDifference(start: Any?, end: Any?, unit: String): Long {
        val utc = Zone("")
        val a = parseDate(start, utc)
        val b = parseDate(end, utc)
        UNIT_MS[unit]?.let { return ((b - a).toDouble() / it).toLong() }
        val months = monthsBetween(a, b)
        if (unit == "months") return months
        return (months.toDouble() / 12).toLong()
    }

    fun urlParts(raw: String): Map<String, Any?> {
        val match = URI_RE.find(raw.trim())!!
        val scheme = match.groups[1]?.value
        var authority = match.groups[2]?.value
        val path = match.groups[3]?.value ?: ""
        val query = match.groups[4]?.value
        val fragment = match.groups[5]?.value
        var host = ""
        var port: Long? = null
        var user = ""
        fun asciiDigits(text: String) = text.isNotEmpty() && text.all { it in '0'..'9' }
        if (authority != null) {
            if (authority.contains('@')) {
                user = authority.substringBeforeLast('@')
                authority = authority.substringAfterLast('@')
            }
            if (authority.startsWith("[")) {
                val end = authority.indexOf(']')
                host = if (end > 0) authority.substring(0, end + 1) else authority
                val rest = if (end > 0) authority.substring(end + 1) else ""
                if (rest.startsWith(":") && asciiDigits(rest.substring(1))) port = rest.substring(1).toBigInteger().toLong()
            } else {
                val colon = authority.lastIndexOf(':')
                if (colon >= 0) {
                    val head = authority.substring(0, colon)
                    val tail = authority.substring(colon + 1)
                    if (asciiDigits(tail)) {
                        host = head
                        port = tail.toBigInteger().toLong()
                    } else if (tail.isEmpty()) {
                        host = head
                    } else {
                        host = authority
                    }
                } else {
                    host = authority
                }
            }
        }
        val params = LinkedHashMap<String, Any?>()
        for (piece in (query ?: "").split("&")) {
            if (piece.isEmpty()) continue
            val name = piece.substringBefore('=')
            val value = if (piece.contains('=')) piece.substringAfter('=') else ""
            params[percentDecode(name, true)] = percentDecode(value, true)
        }
        val out = linkedMapOf<String, Any?>(
            "scheme" to (scheme ?: "").lowercase(), "host" to host.lowercase(), "port" to port, "path" to path, "query" to params,
            "fragment" to (fragment ?: ""),
        )
        out["origin"] = if (!scheme.isNullOrEmpty() && host.isNotEmpty()) {
            out["scheme"].toString() + "://" + out["host"] + (if (port != null) ":$port" else "")
        } else {
            ""
        }
        if (user.isNotEmpty()) out["user"] = percentDecode(user.substringBefore(':'))
        return out
    }

    fun utilsObject(interp: JsInterpreter): JsObject {
        fun text(args: List<Any?>, index: Int, default: String = ""): String {
            val value = if (index < args.size) args[index] else Undefined
            if (value === Undefined || value == null) return default
            return jsToString(value)
        }
        fun guard(fn: (List<Any?>) -> Any?): NativeFn = { _, _, args ->
            try {
                fn(args)
            } catch (error: WorkflowNodeError) {
                throw Thrown(errorObject("Error", error.message ?: ""))
            }
        }
        fun dateValue(value: Any?): Any? {
            if (value is JsDate) {
                if (value.ms.isNaN()) fail("Cannot read the date Invalid Date")
                return value.ms.toLong()
            }
            if (value is Double) return value
            return jsToString(value)
        }
        val named = setOf("iso", "date", "time", "datetime", "long", "unix", "unixMs")
        val natives = linkedMapOf<String, (List<Any?>) -> Any?>(
            "hash" to { args -> hashText(text(args, 0), text(args, 1, "sha256"), "", text(args, 2, "hex")) },
            "hmac" to { args -> hashText(text(args, 0), text(args, 2, "sha256"), text(args, 1), text(args, 3, "hex")) },
            "base64Encode" to { args -> base64Encode(text(args, 0)) },
            "base64Decode" to { args -> base64Decode(text(args, 0)) },
            "urlEncode" to { args -> urlEncode(text(args, 0)) },
            "urlDecode" to { args -> percentDecode(text(args, 0)) },
            "formatDate" to { args ->
                val fmt = text(args, 1, "iso")
                var value = if (args.isNotEmpty()) args[0] else Undefined
                if (value === Undefined) value = interp.nowMs
                val result = if (fmt in named) formatDate(dateValue(value), fmt, "", text(args, 2)) else formatDate(dateValue(value), "custom", fmt, text(args, 2))
                if (result is Long) result.toDouble() else result
            },
            "formatNumber" to { args ->
                val value = if (args.isNotEmpty()) jsToNumber(args[0]) else Double.NaN
                if (value.isNaN() || value.isInfinite()) {
                    jsToString(value)
                } else {
                    val decimals = if (args.size > 1 && args[1] !== Undefined) toInteger(args[1]) else 2.0
                    val grouping = if (args.size < 3) true else jsTruthy(args[2])
                    formatNumber(value, decimals, grouping)
                }
            },
        )
        val out = JsObject()
        for ((name, fn) in natives) out[name] = Native(name, fn = guard(fn))
        return out
    }

    fun pyDumpsIndent(value: Any?, indent: String = "  ", current: String = ""): String = when (value) {
        is Map<*, *> -> if (value.isEmpty()) {
            "{}"
        } else {
            val inner = current + indent
            "{\n" + value.entries.joinToString(",\n") { inner + WorkflowPy.quoteJson(it.key.toString()) + ": " + pyDumpsIndent(it.value, indent, inner) } + "\n" + current + "}"
        }
        is List<*> -> if (value.isEmpty()) {
            "[]"
        } else {
            val inner = current + indent
            "[\n" + value.joinToString(",\n") { inner + pyDumpsIndent(it, indent, inner) } + "\n" + current + "]"
        }
        else -> WorkflowPy.dumps(value)
    }
}
