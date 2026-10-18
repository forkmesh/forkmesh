package com.forkmesh.nativeapp.codeinsight

import java.util.regex.Matcher
import java.util.regex.Pattern

internal object QText {
    fun isSpace(c: Char): Boolean {
        val code = c.code
        if (code < 0x80) return code == 0x20 || code in 0x09..0x0d
        if (code < 0x100) return code == 0x85 || code == 0xa0
        return Character.isSpaceChar(c)
    }

    fun isLetterOrNumber(c: Char): Boolean = when (Character.getType(c).toByte()) {
        Character.UPPERCASE_LETTER, Character.LOWERCASE_LETTER, Character.TITLECASE_LETTER,
        Character.MODIFIER_LETTER, Character.OTHER_LETTER, Character.DECIMAL_DIGIT_NUMBER,
        Character.LETTER_NUMBER, Character.OTHER_NUMBER -> true
        else -> false
    }

    fun trimmed(s: String): String {
        var start = 0
        var end = s.length
        while (start < end && isSpace(s[start])) start++
        while (end > start && isSpace(s[end - 1])) end--
        return if (start == 0 && end == s.length) s else s.substring(start, end)
    }

    fun isBlank(s: CharSequence, start: Int, end: Int): Boolean {
        for (i in start until end) if (!isSpace(s[i])) return false
        return true
    }

    fun simplified(s: String, out: StringBuilder): String {
        out.setLength(0)
        var pendingSpace = false
        for (c in s) {
            if (isSpace(c)) {
                if (out.isNotEmpty()) pendingSpace = true
            } else {
                if (pendingSpace) out.append(' ')
                pendingSpace = false
                out.append(c)
            }
        }
        return out.toString()
    }

    fun mid(s: String, pos: Int): String = if (pos >= s.length) "" else if (pos <= 0) s else s.substring(pos)

    fun mid(s: String, pos: Int, count: Int): String {
        val start = pos.coerceIn(0, s.length)
        val end = (pos + count).coerceIn(start, s.length)
        return s.substring(start, end)
    }

    fun left(s: String, n: Int): String = if (n >= s.length) s else if (n <= 0) "" else s.substring(0, n)

    fun section(s: String, sep: String, startIndex: Int, endIndex: Int = -1): String {
        val sections = s.split(sep)
        val size = sections.size
        var start = startIndex
        var end = endIndex
        if (start < 0) start += size
        if (end < 0) end += size
        if (start >= size || end < 0 || start > end) return ""
        val out = StringBuilder()
        var x = 0
        var i = 0
        while (x <= end && i < size) {
            if (x >= start) {
                if (x > start && i > 0) out.append(sep)
                out.append(sections[i])
            }
            x++
            i++
        }
        return out.toString()
    }

    fun firstToken(s: String): String {
        for (i in s.indices) {
            val c = s[i]
            if (c == ' ' || c == '\t' || c == '\n' || c == '\u000B' || c == '\u000C' || c == '\r') return s.substring(0, i)
        }
        return s
    }

    fun cleanPath(path: String): String {
        if (path.isEmpty()) return path
        val absolute = path.startsWith('/')
        val parts = ArrayList<String>()
        for (segment in path.split('/')) {
            when (segment) {
                "", "." -> Unit
                ".." -> {
                    if (parts.isNotEmpty() && parts.last() != "..") {
                        parts.removeAt(parts.size - 1)
                    } else if (!absolute) {
                        parts.add("..")
                    }
                }
                else -> parts.add(segment)
            }
        }
        val joined = parts.joinToString("/")
        return if (absolute) "/$joined" else joined.ifEmpty { "." }
    }
}

internal object QRegex {
    private const val WORD = "A-Za-z0-9_"
    private const val SPACE = "\\t\\n\\x0B\\f\\r "
    private const val BOUNDARY = "(?:(?<=[$WORD])(?![$WORD])|(?<![$WORD])(?=[$WORD]))"

    fun compile(pattern: String, multiline: Boolean = false, caseless: Boolean = false): Pattern {
        var flags = Pattern.UNIX_LINES
        if (multiline) flags = flags or Pattern.MULTILINE
        if (caseless) flags = flags or Pattern.CASE_INSENSITIVE or Pattern.UNICODE_CASE
        return Pattern.compile(translate(pattern), flags)
    }

    private fun isWordChar(c: Char): Boolean = c in 'A'..'Z' || c in 'a'..'z' || c in '0'..'9' || c == '_'

    private fun escaped(pattern: String, at: Int): Boolean {
        var slashes = 0
        var k = at - 1
        while (k >= 0 && pattern[k] == '\\') {
            slashes++
            k--
        }
        return slashes % 2 == 1
    }

    private fun optionalAfter(pattern: String, at: Int): Boolean =
        at < pattern.length && (pattern[at] == '?' || pattern[at] == '*' || (pattern[at] == '{' && pattern.startsWith("{0", at)))

    private fun wordClass(content: String): Boolean =
        content.isNotEmpty() && !content.startsWith('^') && content.all { isWordChar(it) || it == '-' }

    private fun alternatives(pattern: String, open: Int, close: Int): List<String> {
        var body = pattern.substring(open + 1, close)
        if (body.startsWith("?:")) body = body.substring(2)
        if (body.startsWith("?")) return emptyList()
        val parts = ArrayList<String>()
        var depth = 0
        var start = 0
        var k = 0
        while (k < body.length) {
            val c = body[k]
            if (c == '\\') {
                k += 2
                continue
            }
            if (c == '(' || c == '[') depth++
            if (c == ')' || c == ']') depth--
            if (c == '|' && depth == 0) {
                parts.add(body.substring(start, k))
                start = k + 1
            }
            k++
        }
        parts.add(body.substring(start))
        return parts
    }

    private fun matchingClose(pattern: String, open: Int): Int {
        var depth = 0
        var k = open
        while (k < pattern.length) {
            val c = pattern[k]
            if (c == '\\') {
                k += 2
                continue
            }
            if (c == '(') depth++
            if (c == ')') {
                depth--
                if (depth == 0) return k
            }
            k++
        }
        return -1
    }

    private fun matchingOpen(pattern: String, close: Int): Int {
        var depth = 0
        var k = close
        while (k >= 0) {
            val c = pattern[k]
            if (!escaped(pattern, k)) {
                if (c == ')') depth++
                if (c == '(') {
                    depth--
                    if (depth == 0) return k
                }
            }
            k--
        }
        return -1
    }

    private fun startsWithWord(pattern: String, at: Int): Boolean {
        if (at >= pattern.length) return false
        val c = pattern[at]
        if (isWordChar(c)) return !optionalAfter(pattern, at + 1)
        if (c == '[') {
            val end = pattern.indexOf(']', at + 1)
            return end > at && wordClass(pattern.substring(at + 1, end)) && !optionalAfter(pattern, end + 1)
        }
        if (c == '(') {
            val close = matchingClose(pattern, at)
            if (close < 0 || optionalAfter(pattern, close + 1)) return false
            val parts = alternatives(pattern, at, close)
            return parts.isNotEmpty() && parts.all { it.isNotEmpty() && isWordChar(it[0]) && !optionalAfter(it, 1) }
        }
        return false
    }

    private fun endsWithWord(pattern: String, at: Int): Boolean {
        if (at < 0 || escaped(pattern, at)) return false
        val c = pattern[at]
        if (isWordChar(c)) return true
        if (c == ')') {
            val open = matchingOpen(pattern, at)
            if (open < 0) return false
            val parts = alternatives(pattern, open, at)
            return parts.isNotEmpty() && parts.all { it.isNotEmpty() && isWordChar(it.last()) && !escaped(it, it.length - 1) }
        }
        if (c == '}' || c == '+') {
            val atomEnd = if (c == '+') at - 1 else pattern.lastIndexOf('{', at) - 1
            if (c == '}' && pattern.startsWith("{0", atomEnd + 1)) return false
            if (atomEnd < 0) return false
            if (pattern[atomEnd] == ']') {
                val open = pattern.lastIndexOf('[', atomEnd)
                return open >= 0 && wordClass(pattern.substring(open + 1, atomEnd))
            }
            return isWordChar(pattern[atomEnd]) && !escaped(pattern, atomEnd)
        }
        return false
    }

    private fun translate(pattern: String): String {
        val out = StringBuilder(pattern.length * 2)
        var inClass = false
        var i = 0
        val n = pattern.length
        while (i < n) {
            val c = pattern[i]
            if (c == '\\' && i + 1 < n) {
                val e = pattern[i + 1]
                if (inClass) {
                    when (e) {
                        'w' -> out.append(WORD)
                        's' -> out.append(SPACE)
                        'd' -> out.append("0-9")
                        else -> out.append(c).append(e)
                    }
                } else {
                    when (e) {
                        'w' -> out.append("[$WORD]")
                        'W' -> out.append("[^$WORD]")
                        's' -> out.append("[$SPACE]")
                        'S' -> out.append("[^$SPACE]")
                        'd' -> out.append("[0-9]")
                        'b' -> out.append(
                            when {
                                startsWithWord(pattern, i + 2) -> "(?<![$WORD])"
                                endsWithWord(pattern, i - 1) -> "(?![$WORD])"
                                else -> BOUNDARY
                            },
                        )
                        else -> out.append(c).append(e)
                    }
                }
                i += 2
                continue
            }
            if (!inClass && c == '[') {
                inClass = true
                out.append(c)
                i++
                if (i < n && pattern[i] == '^') {
                    out.append('^')
                    i++
                }
                if (i < n && pattern[i] == ']') {
                    out.append("\\]")
                    i++
                }
                continue
            }
            if (inClass && c == ']') inClass = false
            out.append(c)
            i++
        }
        return out.toString()
    }
}

internal fun Pattern.found(text: CharSequence): Boolean = matcher(text).find()

internal fun Pattern.firstMatch(text: CharSequence): Matcher? = matcher(text).let { if (it.find()) it else null }

internal fun Pattern.countIn(text: CharSequence): Int {
    val m = matcher(text)
    var count = 0
    while (m.find()) count++
    return count
}

internal fun Matcher.groupOrEmpty(group: Int): String = group(group) ?: ""
