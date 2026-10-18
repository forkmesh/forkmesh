package com.forkmesh.nativeapp.workflows

import java.math.BigDecimal
import java.math.BigInteger
import java.util.IdentityHashMap
import java.util.regex.Matcher
import java.util.regex.Pattern
import java.util.regex.PatternSyntaxException

internal object JsLimits {
    const val MAX_STEPS = 100000
    const val MAX_DEPTH = 40
    const val MAX_NESTING = 50
    const val MAX_STRING = 1000000
    const val MAX_ARRAY = 100000
    const val MAX_LOGS = 100
    const val MAX_LOG_CHARS = 1000
    const val MAX_INT = 1e18
}

internal const val JS_SPACE =
    "\t\n\u000b\u000c\r                  　﻿"
internal const val LINE_ENDS = "\n\r  "
private val KEYWORDS = setOf(
    "var", "let", "const", "if", "else", "for", "while", "do", "break", "continue", "return", "function", "true", "false",
    "null", "typeof", "instanceof", "new", "throw", "try", "catch", "finally", "switch", "case", "default", "delete", "this",
    "void", "in",
)
private val UNSUPPORTED = setOf("class", "async", "await", "yield", "import", "export", "with", "debugger", "super", "enum", "extends", "static")
private val PUNCTUATORS = listOf(
    ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "??=", "=>", "==", "!=", "<=", ">=", "&&", "||", "??",
    "?.", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "**", "<<", ">>", "{", "}", "(", ")", "[", "]", ";", ",",
    "<", ">", "+", "-", "*", "/", "%", "&", "|", "^", "!", "~", "?", ":", "=", ".",
)
private val REGEX_AFTER_KEYWORDS = setOf("return", "typeof", "case", "do", "else", "in", "instanceof", "new", "delete", "void", "throw")
private val BINARY = mapOf(
    "||" to 1, "??" to 1, "&&" to 2, "|" to 3, "^" to 4, "&" to 5, "==" to 6, "!=" to 6, "===" to 6, "!==" to 6, "<" to 7,
    ">" to 7, "<=" to 7, ">=" to 7, "in" to 7, "instanceof" to 7, "<<" to 8, ">>" to 8, ">>>" to 8, "+" to 9, "-" to 9,
    "*" to 10, "/" to 10, "%" to 10, "**" to 11,
)
private val ASSIGN = setOf("=", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=", "|=", "^=", "&&=", "||=", "??=")
internal val DECIMAL_RE = Regex("[+-]?(?:[0-9]+\\.?[0-9]*|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?")
internal val FLOAT_PREFIX_RE = Regex("^[+-]?(?:Infinity|(?:[0-9]+\\.?[0-9]*|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?)")
private val INDEX_RE = Regex("0|[1-9][0-9]*")
private val ISO_DATE_RE = Regex(
    "([+-][0-9]{6}|[0-9]{4})(?:-([0-9]{2})(?:-([0-9]{2}))?)?" +
        "(?:T([0-9]{2}):([0-9]{2})(?::([0-9]{2})(?:\\.([0-9]{1,9}))?)?(Z|[+-][0-9]{2}:[0-9]{2})?)?",
)
private const val URI_UNRESERVED = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.!~*'()"
private val DAY_NAMES = listOf("Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat")
private val MONTH_NAMES = listOf("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
private const val WORD = "[A-Za-z0-9_]"

internal class ScriptError(val line: Int, val text: String) : Exception("Line $line: $text")
internal class ScriptLimit(message: String) : Exception(message)
internal class Thrown(val value: Any?, var line: Int = 0) : Exception(null, null, false, false)
internal object ShortCircuit : Exception(null, null, false, false) {
    private fun readResolve(): Any = ShortCircuit
}

internal object Undefined {
    override fun toString() = "undefined"
}

internal object BreakSignal
internal object ContinueSignal
internal class ReturnSignal(val value: Any?)

internal class N(
    val k: String,
    val line: Int,
    val a: Any? = null,
    val b: Any? = null,
    val c: Any? = null,
    val d: Any? = null,
    val e: Any? = null,
    val f: Any? = null,
)

internal class JsFunction(
    var name: String,
    val params: List<Pair<N, N?>>,
    val rest: N?,
    val body: Any?,
    val scope: Scope,
    val arrow: Boolean,
    val expression: Boolean,
)

internal class Native(
    val name: String,
    val props: MutableMap<String, Any?> = LinkedHashMap(),
    val construct: ((JsInterpreter, List<Any?>) -> Any?)? = null,
    val fn: (JsInterpreter, Any?, List<Any?>) -> Any?,
)

internal class JsDate(ms: Double) {
    var ms: Double = timeClip(ms)
}

internal class CompiledRegex(val pattern: Pattern, val names: List<String>, val javaNames: List<String>)

internal class JsRegExp(val source: String, val flags: String, val compiled: CompiledRegex) {
    var lastIndex: Int = 0
    val names: List<String> get() = compiled.names
}

internal class JsArray(items: Collection<Any?> = emptyList()) : ArrayList<Any?>(items) {
    var isMatch = false
    var index: Any? = Undefined
    var input: Any? = Undefined
    var groups: Any? = Undefined

    override fun equals(other: Any?): Boolean = this === other
    override fun hashCode(): Int = System.identityHashCode(this)
}

internal class JsObject : LinkedHashMap<String, Any?>() {
    override fun equals(other: Any?): Boolean = this === other
    override fun hashCode(): Int = System.identityHashCode(this)
}

internal class JsCollection(val kind: String) {
    var keys = ArrayList<Any?>()
    var values = ArrayList<Any?>()
    var index = HashMap<Any, Int>()

    fun find(key: Any?): Int? = index[collectionKey(key)]

    fun put(key: Any?, value: Any?) {
        val marker = collectionKey(key)
        val position = index[marker]
        if (position == null) {
            if (keys.size >= JsLimits.MAX_ARRAY) throw rangeError("Invalid array length")
            index[marker] = keys.size
            keys.add(if (key is Double && key == 0.0) 0.0 else key)
            values.add(value)
        } else {
            values[position] = value
        }
    }

    fun remove(key: Any?): Boolean {
        val position = find(key) ?: return false
        keys.removeAt(position)
        values.removeAt(position)
        index = HashMap()
        for ((i, k) in keys.withIndex()) index[collectionKey(k)] = i
        return true
    }

    fun entries(): JsArray = if (kind == "Set") JsArray(keys) else JsArray(keys.indices.map { JsArray(listOf(keys[it], values[it])) })
}

private class IdentityKey(val value: Any) {
    override fun equals(other: Any?): Boolean = other is IdentityKey && other.value === value
    override fun hashCode(): Int = System.identityHashCode(value)
}

private fun collectionKey(value: Any?): Any = when (value) {
    is Double -> if (value.isNaN()) "nan" else Pair("n", value + 0.0)
    is String -> Pair("s", value)
    is Boolean -> Pair("b", value)
    null -> "null"
    Undefined -> "undefined"
    else -> IdentityKey(value)
}

internal fun timeClip(ms: Double): Double {
    if (ms.isNaN() || ms.isInfinite() || Math.abs(ms) > 8.64e15) return Double.NaN
    return Math.floor(Math.abs(ms)).let { if (ms < 0) -it else it } + 0.0
}

internal class Scope(val parent: Scope? = null) {
    var vars = HashMap<String, Any?>()
    var consts = HashSet<String>()

    fun declare(name: String, value: Any?, kind: String) {
        if ((kind == "let" || kind == "const") && vars.containsKey(name)) {
            throw Thrown(errorObject("SyntaxError", "Identifier '$name' has already been declared"))
        }
        vars[name] = value
        if (kind == "const") consts.add(name) else consts.remove(name)
    }

    fun lookup(name: String): Scope? {
        var scope: Scope? = this
        while (scope != null) {
            if (scope.vars.containsKey(name)) return scope
            scope = scope.parent
        }
        return null
    }
}

internal fun errorObject(name: String, message: String): JsObject = JsObject().apply {
    put("name", name)
    put("message", message)
}

internal fun typeError(message: String) = Thrown(errorObject("TypeError", message))
internal fun rangeError(message: String) = Thrown(errorObject("RangeError", message))

internal fun cpLen(text: String): Int = text.codePointCount(0, text.length)

internal fun hasSurrogates(text: String): Boolean {
    for (c in text) if (c in '\ud800'..'\udfff') return true
    return false
}

internal fun cpList(text: String): List<String> {
    if (!hasSurrogates(text)) return text.map { it.toString() }
    val out = ArrayList<String>()
    var i = 0
    while (i < text.length) {
        val cp = text.codePointAt(i)
        val n = Character.charCount(cp)
        out.add(text.substring(i, i + n))
        i += n
    }
    return out
}

internal fun cpOffset(text: String, index: Int): Int {
    if (!hasSurrogates(text)) return index.coerceIn(0, text.length)
    val total = cpLen(text)
    return text.offsetByCodePoints(0, index.coerceIn(0, total))
}

internal fun cpSub(text: String, start: Int, end: Int = Int.MAX_VALUE): String {
    if (!hasSurrogates(text)) {
        val s = start.coerceIn(0, text.length)
        val e = end.coerceIn(s, text.length)
        return text.substring(s, e)
    }
    val total = cpLen(text)
    val s = start.coerceIn(0, total)
    val e = end.coerceIn(s, total)
    val a = text.offsetByCodePoints(0, s)
    return text.substring(a, text.offsetByCodePoints(a, e - s))
}

internal fun cpAt(text: String, index: Int): String = cpSub(text, index, index + 1)

internal fun cpToIndex(text: String, utf16: Int): Int = text.codePointCount(0, utf16)

internal fun cpFind(text: String, needle: String, start: Int): Int {
    val found = text.indexOf(needle, cpOffset(text, start))
    return if (found < 0) -1 else cpToIndex(text, found)
}

internal fun cpCompare(a: String, b: String): Int {
    var i = 0
    var j = 0
    while (i < a.length && j < b.length) {
        val x = a.codePointAt(i)
        val y = b.codePointAt(j)
        if (x != y) return if (x < y) -1 else 1
        i += Character.charCount(x)
        j += Character.charCount(y)
    }
    return when {
        i < a.length -> 1
        j < b.length -> -1
        else -> 0
    }
}

internal fun jsStrip(text: String, start: Boolean = true, end: Boolean = true): String {
    var s = 0
    var e = text.length
    if (start) while (s < e && JS_SPACE.indexOf(text[s]) >= 0) s++
    if (end) while (e > s && JS_SPACE.indexOf(text[e - 1]) >= 0) e--
    return text.substring(s, e)
}

private fun isIdStart(cp: Int): Boolean = cp == '_'.code || cp == '$'.code || Character.isLetter(cp)

private fun isNumberType(cp: Int): Boolean {
    val type = Character.getType(cp)
    return type == Character.DECIMAL_DIGIT_NUMBER.toInt() || type == Character.LETTER_NUMBER.toInt() || type == Character.OTHER_NUMBER.toInt()
}

private fun isIdPart(cp: Int): Boolean = cp == '_'.code || cp == '$'.code || Character.isLetter(cp) || isNumberType(cp)

private fun isDigit(c: Char?): Boolean = c != null && c in '0'..'9'

private fun isHex(c: Char): Boolean = c in '0'..'9' || c in 'a'..'f' || c in 'A'..'F'

internal class Token(val kind: String, val value: Any?, val line: Int, val newline: Boolean, val extra: Any? = null)

internal class Lexer(val src: String, var line: Int = 1) {
    var pos = 0
    var tokens = ArrayList<Token>()

    private fun error(message: String): Nothing = throw ScriptError(line, message)

    private fun at(i: Int): Char? = if (i in src.indices) src[i] else null

    fun skipSpace(start: Boolean): Boolean {
        var newline = start
        while (pos < src.length) {
            val ch = src[pos]
            if (LINE_ENDS.indexOf(ch) >= 0) {
                if (!(ch == '\r' && at(pos + 1) == '\n')) line++
                newline = true
                pos++
            } else if (JS_SPACE.indexOf(ch) >= 0) {
                pos++
            } else if (src.startsWith("//", pos)) {
                while (pos < src.length && LINE_ENDS.indexOf(src[pos]) < 0) pos++
            } else if (src.startsWith("/*", pos)) {
                val end = src.indexOf("*/", pos + 2)
                if (end < 0) error("Unterminated comment")
                for (index in pos until end) {
                    val c = src[index]
                    if (LINE_ENDS.indexOf(c) >= 0 && !(c == '\r' && at(index + 1) == '\n')) {
                        line++
                        newline = true
                    }
                }
                pos = end + 2
            } else {
                break
            }
        }
        return newline
    }

    private fun regexAllowed(): Boolean {
        if (tokens.isEmpty()) return true
        val last = tokens.last()
        if (last.kind == "punct") return last.value !in setOf(")", "]", "}")
        if (last.kind == "kw") return last.value in REGEX_AFTER_KEYWORDS
        return false
    }

    private fun number(): Double {
        val start = pos
        val value: Double
        val second = at(start + 1)
        if (src[start] == '0' && second != null && second in "xXoObB") {
            val base = when (second.lowercaseChar()) {
                'x' -> 16
                'o' -> 8
                else -> 2
            }
            pos += 2
            val allowed = "0123456789abcdef".substring(0, base)
            val digits = StringBuilder()
            while (pos < src.length && (src[pos] == '_' || allowed.indexOf(src[pos].lowercaseChar()) >= 0)) {
                digits.append(src[pos])
                pos++
            }
            val clean = digits.toString().replace("_", "")
            if (clean.isEmpty()) error("Invalid number")
            value = BigInteger(clean, base).toDouble()
        } else {
            while (pos < src.length && src[pos] in "0123456789_") pos++
            if (at(pos) == '.') {
                pos++
                while (pos < src.length && src[pos] in "0123456789_") pos++
            }
            if (at(pos) == 'e' || at(pos) == 'E') {
                val mark = pos
                pos++
                if (at(pos) == '+' || at(pos) == '-') pos++
                if (isDigit(at(pos))) {
                    while (pos < src.length && src[pos] in '0'..'9') pos++
                } else {
                    pos = mark
                }
            }
            val text = src.substring(start, pos).replace("_", "")
            value = text.toDouble()
        }
        if (pos < src.length && (isIdStart(src.codePointAt(pos)) || src[pos] in '0'..'9')) error("Invalid number")
        return value
    }

    private fun escape(): String {
        pos++
        if (pos >= src.length) error("Unterminated string")
        val cp = src.codePointAt(pos)
        val ch = String(Character.toChars(cp))
        pos += Character.charCount(cp)
        when (ch) {
            "n" -> return "\n"
            "t" -> return "\t"
            "r" -> return "\r"
            "b" -> return "\b"
            "f" -> return "\u000c"
            "v" -> return "\u000b"
        }
        if (ch == "0" && !isDigit(at(pos))) return "\u0000"
        if (ch == "x") {
            val digits = src.substring(pos, minOf(pos + 2, src.length))
            if (digits.length != 2 || !digits.all(::isHex)) error("Invalid escape")
            pos += 2
            return digits.toInt(16).toChar().toString()
        }
        if (ch == "u") {
            if (at(pos) == '{') {
                val end = src.indexOf("}", pos)
                val digits = if (end > 0) src.substring(pos + 1, end) else ""
                if (digits.isEmpty() || !digits.all(::isHex) || BigInteger(digits, 16) > BigInteger.valueOf(0x10FFFF)) error("Invalid escape")
                pos = end + 1
                return String(Character.toChars(digits.toInt(16)))
            }
            val digits = src.substring(pos, minOf(pos + 4, src.length))
            if (digits.length != 4 || !digits.all(::isHex)) error("Invalid escape")
            pos += 4
            return digits.toInt(16).toChar().toString()
        }
        if (LINE_ENDS.contains(ch)) {
            if (ch == "\r" && at(pos) == '\n') pos++
            line++
            return ""
        }
        return ch
    }

    private fun string(quote: Char): String {
        pos++
        val out = StringBuilder()
        while (true) {
            if (pos >= src.length || src[pos] == '\n' || src[pos] == '\r') error("Unterminated string")
            val ch = src[pos]
            if (ch == quote) {
                pos++
                return out.toString()
            }
            if (ch == '\\') {
                out.append(escape())
            } else {
                out.append(ch)
                pos++
            }
        }
    }

    private fun template(): Pair<List<String>, List<List<Token>>> {
        pos++
        val strings = ArrayList<String>()
        val parts = ArrayList<List<Token>>()
        var out = StringBuilder()
        while (true) {
            if (pos >= src.length) error("Unterminated template")
            var ch = src[pos]
            if (ch == '`') {
                pos++
                strings.add(out.toString())
                return strings to parts
            }
            if (ch == '\\') {
                out.append(escape())
                continue
            }
            if (ch == '$' && at(pos + 1) == '{') {
                strings.add(out.toString())
                out = StringBuilder()
                pos += 2
                val inner = Lexer(src, line)
                inner.pos = pos
                var depth = 0
                var newline = false
                while (true) {
                    newline = inner.skipSpace(newline)
                    if (inner.pos >= src.length) error("Unterminated template")
                    if (src[inner.pos] == '}' && depth == 0) break
                    inner.stepOne(newline)
                    newline = false
                    val token = inner.tokens.last()
                    if (token.kind == "punct" && token.value == "{") depth++ else if (token.kind == "punct" && token.value == "}") depth--
                }
                inner.tokens.add(Token("eof", null, inner.line, true))
                parts.add(inner.tokens)
                pos = inner.pos + 1
                line = inner.line
                continue
            }
            if (LINE_ENDS.indexOf(ch) >= 0) {
                if (!(ch == '\r' && at(pos + 1) == '\n')) line++
                if (ch == '\r') {
                    if (at(pos + 1) == '\n') pos++
                    ch = '\n'
                }
            }
            out.append(ch)
            pos++
        }
    }

    fun stepOne(newline: Boolean) {
        val ch = src[pos]
        val tokenLine = line
        val token: Token
        if (ch in '0'..'9' || (ch == '.' && isDigit(at(pos + 1)))) {
            token = Token("num", number(), tokenLine, newline)
        } else if (ch == '"' || ch == '\'') {
            token = Token("str", string(ch), tokenLine, newline)
        } else if (ch == '`') {
            val (strings, parts) = template()
            token = Token("tmpl", strings, tokenLine, newline, parts)
        } else if (isIdStart(src.codePointAt(pos))) {
            val name = identifier()
            if (name in UNSUPPORTED) error("$name is not supported in scripts")
            token = Token(if (name in KEYWORDS) "kw" else "ident", name, tokenLine, newline)
        } else if (ch == '/' && regexAllowed()) {
            val (source, flags) = regex()
            token = Token("regex", source, tokenLine, newline, flags)
        } else {
            var found: Token? = null
            for (punct in PUNCTUATORS) {
                if (src.startsWith(punct, pos)) {
                    if (punct == "?." && isDigit(at(pos + 2))) continue
                    pos += punct.length
                    found = Token("punct", punct, tokenLine, newline)
                    break
                }
            }
            token = found ?: error("Unexpected character " + String(Character.toChars(src.codePointAt(pos))))
        }
        tokens.add(token)
    }

    private fun identifier(): String {
        val start = pos
        while (pos < src.length) {
            val cp = src.codePointAt(pos)
            if (!isIdPart(cp)) break
            pos += Character.charCount(cp)
        }
        return src.substring(start, pos)
    }

    private fun regex(): Pair<String, String> {
        pos++
        val start = pos
        var inClass = false
        while (true) {
            if (pos >= src.length || LINE_ENDS.indexOf(src[pos]) >= 0) error("Unterminated regular expression")
            val ch = src[pos]
            if (ch == '\\') {
                pos += 2
                continue
            }
            if (ch == '[') inClass = true else if (ch == ']') inClass = false else if (ch == '/' && !inClass) break
            pos++
        }
        val source = src.substring(start, pos)
        pos++
        val flagStart = pos
        while (pos < src.length && isIdPart(src.codePointAt(pos))) pos += Character.charCount(src.codePointAt(pos))
        return source to src.substring(flagStart, pos)
    }
}

internal fun lexAll(source: String): List<Token> {
    val lexer = Lexer(source)
    var newline = false
    while (true) {
        newline = lexer.skipSpace(newline)
        if (lexer.pos >= source.length) {
            lexer.tokens.add(Token("eof", null, lexer.line, true))
            return lexer.tokens
        }
        lexer.stepOne(newline)
        newline = false
    }
}

@Suppress("UNCHECKED_CAST")
internal class Parser(val tokens: List<Token>) {
    var index = 0
    var nesting = 0
    var loops = 0
    var switches = 0
    var functions = 0
    var shorthand: IdentityHashMap<N, Int> = IdentityHashMap()

    val tok: Token get() = tokens[index]

    private fun peek(offset: Int = 1): Token = tokens[minOf(index + offset, tokens.size - 1)]

    fun error(token: Token = tok): Nothing {
        if (token.kind == "eof") throw ScriptError(token.line, "Unexpected end of script")
        val text = when (token.kind) {
            "str" -> "string"
            "num" -> "number"
            "tmpl" -> "template"
            "regex" -> "regular expression"
            else -> token.value.toString()
        }
        throw ScriptError(token.line, "Unexpected token $text")
    }

    private fun isPunct(value: String, token: Token = tok) = token.kind == "punct" && token.value == value
    private fun isKw(value: String, token: Token = tok) = token.kind == "kw" && token.value == value

    private fun advance(): Token {
        val token = tokens[index]
        if (index < tokens.size - 1) index++
        return token
    }

    private fun expect(value: String): Token {
        if (!isPunct(value)) error()
        return advance()
    }

    private fun eat(value: String): Boolean {
        if (isPunct(value)) {
            advance()
            return true
        }
        return false
    }

    private fun semicolon() {
        if (eat(";")) return
        if (isPunct("}") || tok.kind == "eof" || tok.newline) return
        error()
    }

    private fun enter() {
        nesting++
        if (nesting > JsLimits.MAX_NESTING) throw ScriptError(tok.line, "The script nests too deeply")
    }

    private fun leave() {
        nesting--
    }

    fun program(): List<N> {
        val body = ArrayList<N>()
        while (tok.kind != "eof") body.add(statement())
        return body
    }

    private fun statement(): N {
        enter()
        try {
            return statementInner()
        } finally {
            leave()
        }
    }

    private fun statementInner(): N {
        val t = tok
        val line = t.line
        if (t.kind == "punct") {
            if (t.value == "{") return N("block", line, block())
            if (t.value == ";") {
                advance()
                return N("empty", line)
            }
        }
        if (t.kind == "kw") {
            when (val word = t.value as String) {
                "var", "let", "const" -> {
                    val node = declaration()
                    semicolon()
                    return node
                }
                "function" -> {
                    advance()
                    val fn = functionRest(line, true)
                    return N("funcdecl", line, fn.a, fn)
                }
                "if" -> {
                    advance()
                    expect("(")
                    val test = expression()
                    expect(")")
                    val then = statement()
                    var other: N? = null
                    if (isKw("else")) {
                        advance()
                        other = statement()
                    }
                    return N("if", line, test, then, other)
                }
                "for" -> return forStatement()
                "while" -> {
                    advance()
                    expect("(")
                    val test = expression()
                    expect(")")
                    return N("while", line, test, loopBody())
                }
                "do" -> {
                    advance()
                    val body = loopBody()
                    if (!isKw("while")) error()
                    advance()
                    expect("(")
                    val test = expression()
                    expect(")")
                    eat(";")
                    return N("dowhile", line, body, test)
                }
                "return" -> {
                    advance()
                    var value: N? = null
                    if (!(isPunct(";") || isPunct("}") || tok.kind == "eof" || tok.newline)) value = expression()
                    semicolon()
                    return N("return", line, value)
                }
                "break", "continue" -> {
                    advance()
                    if (word == "break" && loops == 0 && switches == 0) throw ScriptError(line, "Illegal break statement")
                    if (word == "continue" && loops == 0) throw ScriptError(line, "Illegal continue statement")
                    semicolon()
                    return N(word, line)
                }
                "throw" -> {
                    advance()
                    if (tok.newline) throw ScriptError(line, "Illegal newline after throw")
                    val value = expression()
                    semicolon()
                    return N("throw", line, value)
                }
                "try" -> return tryStatement()
                "switch" -> return switchStatement()
            }
        }
        val expr = expression()
        semicolon()
        return N("expr", line, expr)
    }

    private fun loopBody(): N {
        loops++
        try {
            return statement()
        } finally {
            loops--
        }
    }

    private fun block(): List<N> {
        expect("{")
        val body = ArrayList<N>()
        while (!isPunct("}")) {
            if (tok.kind == "eof") error()
            body.add(statement())
        }
        advance()
        return body
    }

    private fun declaration(inFor: Boolean = false): N {
        val line = tok.line
        val kind = advance().value as String
        val decls = ArrayList<Pair<N, N?>>()
        while (true) {
            val target = bindingTarget()
            var init: N? = null
            if (eat("=")) {
                init = assignment(inFor)
            } else if (kind == "const" && !(inFor && (isKw("in") || isOf()))) {
                throw ScriptError(tok.line, "Missing initializer in const declaration")
            } else if (target.k != "pid" && !(inFor && (isKw("in") || isOf()))) {
                throw ScriptError(tok.line, "Missing initializer in destructuring declaration")
            }
            decls.add(target to init)
            if (!eat(",")) break
        }
        return N("var", line, kind, decls)
    }

    private fun isOf() = tok.kind == "ident" && tok.value == "of"

    private fun forStatement(): N {
        val line = advance().line
        expect("(")
        var init: N? = null
        if (tok.kind == "kw" && tok.value in setOf("var", "let", "const")) {
            val decl = declaration(true)
            val decls = decl.b as List<Pair<N, N?>>
            if (decls.size == 1 && decls[0].second == null && (isOf() || isKw("in"))) {
                val kind = if (isOf()) "forof" else "forin"
                advance()
                val subject = if (kind == "forof") assignment() else expression()
                expect(")")
                val body = loopBody()
                return N(kind, line, decl.a, decls[0].first, subject, body)
            }
            for ((target, value) in decls) {
                if (value == null && (decl.a == "const" || target.k != "pid")) throw ScriptError(line, "Missing initializer in declaration")
            }
            init = decl
        } else if (!isPunct(";")) {
            val expr = expression(true)
            if (isOf() || isKw("in")) {
                val kind = if (isOf()) "forof" else "forin"
                advance()
                val target = toPattern(expr)
                val subject = if (kind == "forof") assignment() else expression()
                expect(")")
                val body = loopBody()
                return N(kind, line, null, target, subject, body)
            }
            init = N("expr", line, expr)
        }
        expect(";")
        val test = if (isPunct(";")) null else expression()
        expect(";")
        val update = if (isPunct(")")) null else expression()
        expect(")")
        val body = loopBody()
        return N("for", line, init, test, update, body)
    }

    private fun tryStatement(): N {
        val line = advance().line
        val body = block()
        var param: N? = null
        var handler: List<N>? = null
        var final: List<N>? = null
        if (isKw("catch")) {
            advance()
            if (eat("(")) {
                param = bindingTarget()
                expect(")")
            }
            handler = block()
        }
        if (isKw("finally")) {
            advance()
            final = block()
        }
        if (handler == null && final == null) throw ScriptError(line, "Missing catch or finally after try")
        return N("try", line, body, param, handler, final)
    }

    private fun switchStatement(): N {
        val line = advance().line
        expect("(")
        val subject = expression()
        expect(")")
        expect("{")
        val cases = ArrayList<Pair<N?, List<N>>>()
        var seenDefault = false
        switches++
        try {
            while (!isPunct("}")) {
                val test: N?
                if (isKw("case")) {
                    advance()
                    test = expression()
                } else if (isKw("default")) {
                    if (seenDefault) throw ScriptError(tok.line, "More than one default clause in switch")
                    seenDefault = true
                    advance()
                    test = null
                } else {
                    error()
                }
                expect(":")
                val body = ArrayList<N>()
                while (!(isKw("case") || isKw("default") || isPunct("}"))) {
                    if (tok.kind == "eof") error()
                    body.add(statement())
                }
                cases.add(test to body)
            }
        } finally {
            switches--
        }
        advance()
        return N("switch", line, subject, cases)
    }

    private fun bindingTarget(): N {
        val t = tok
        if (t.kind == "ident") {
            advance()
            return N("pid", t.line, t.value)
        }
        if (isPunct("[")) {
            advance()
            val elements = ArrayList<Pair<N, N?>?>()
            var rest: N? = null
            while (!isPunct("]")) {
                if (isPunct(",")) {
                    advance()
                    elements.add(null)
                    continue
                }
                if (eat("...")) {
                    rest = bindingTarget()
                    break
                }
                val target = bindingTarget()
                val default = if (eat("=")) assignment() else null
                elements.add(target to default)
                if (!isPunct("]")) expect(",")
            }
            expect("]")
            return N("parr", t.line, elements, rest)
        }
        if (isPunct("{")) {
            advance()
            val props = ArrayList<Triple<String, N, N?>>()
            var rest: N? = null
            while (!isPunct("}")) {
                if (eat("...")) {
                    rest = bindingTarget()
                    break
                }
                val keyTok = tok
                val key = when (keyTok.kind) {
                    "ident", "kw", "str" -> keyTok.value as String
                    "num" -> numberToString(keyTok.value as Double)
                    else -> error()
                }
                advance()
                val target = if (eat(":")) {
                    bindingTarget()
                } else {
                    if (keyTok.kind != "ident") error(keyTok)
                    N("pid", keyTok.line, key)
                }
                val default = if (eat("=")) assignment() else null
                props.add(Triple(key, target, default))
                if (!isPunct("}")) expect(",")
            }
            expect("}")
            return N("pobj", t.line, props, rest)
        }
        error()
    }

    private fun optionalFlag(node: N): Boolean = when (node.k) {
        "member", "index", "call" -> node.c == true
        else -> false
    }

    private fun toPattern(expr: N): N {
        when (expr.k) {
            "pid", "parr", "pobj", "pexpr" -> return expr
            "ident" -> return N("pid", expr.line, expr.a)
            "member", "index" -> {
                if (optionalFlag(expr)) throw ScriptError(expr.line, "Invalid assignment target")
                return N("pexpr", expr.line, expr)
            }
            "array" -> {
                val items = expr.a as List<N?>
                val elements = ArrayList<Pair<N, N?>?>()
                var rest: N? = null
                for ((i, element) in items.withIndex()) {
                    if (element == null) {
                        elements.add(null)
                    } else if (element.k == "spread") {
                        if (i != items.size - 1) throw ScriptError(expr.line, "Invalid assignment target")
                        rest = toPattern(element.a as N)
                    } else if (element.k == "assign" && element.a == "=") {
                        elements.add(toPattern(element.b as N) to element.c as N?)
                    } else {
                        elements.add(toPattern(element) to null)
                    }
                }
                return N("parr", expr.line, elements, rest)
            }
            "object" -> {
                shorthand.remove(expr)
                val props = ArrayList<Triple<String, N, N?>>()
                var rest: N? = null
                for (prop in expr.a as List<N>) {
                    if (prop.k == "spread") {
                        rest = toPattern(prop.a as N)
                        continue
                    }
                    if (prop.c == true) throw ScriptError(expr.line, "Invalid assignment target")
                    val key = prop.a as String
                    val value = prop.b as N
                    val shorthandDefault = prop.d as N?
                    if (shorthandDefault != null) {
                        props.add(Triple(key, N("pid", expr.line, key), shorthandDefault))
                    } else if (value.k == "assign" && value.a == "=") {
                        props.add(Triple(key, toPattern(value.b as N), value.c as N?))
                    } else {
                        props.add(Triple(key, toPattern(value), null))
                    }
                }
                return N("pobj", expr.line, props, rest)
            }
        }
        throw ScriptError(expr.line, "Invalid assignment target")
    }

    fun expression(noIn: Boolean = false): N {
        val line = tok.line
        val expr = assignment(noIn)
        if (isPunct(",")) {
            val items = arrayListOf(expr)
            while (eat(",")) items.add(assignment(noIn))
            return N("seq", line, items)
        }
        return expr
    }

    private fun arrowAhead(): Boolean {
        if (tok.kind == "ident" && isPunct("=>", peek())) return true
        if (!isPunct("(")) return false
        var depth = 0
        var i = index
        while (i < tokens.size) {
            val token = tokens[i]
            if (token.kind == "eof") return false
            if (token.kind == "punct") {
                if (token.value in setOf("(", "[", "{")) {
                    depth++
                } else if (token.value in setOf(")", "]", "}")) {
                    depth--
                    if (depth == 0) {
                        val following = tokens[minOf(i + 1, tokens.size - 1)]
                        return following.kind == "punct" && following.value == "=>" && !following.newline
                    }
                }
            }
            i++
        }
        return false
    }

    private fun assignment(noIn: Boolean = false): N {
        enter()
        try {
            return assignmentInner(noIn)
        } finally {
            leave()
        }
    }

    private fun assignmentInner(noIn: Boolean): N {
        if (arrowAhead()) return arrow()
        val line = tok.line
        val left = conditional(noIn)
        val t = tok
        if (t.kind == "punct" && t.value in ASSIGN) {
            val op = t.value as String
            val target = if (op == "=") {
                toPattern(left)
            } else if (left.k in setOf("ident", "member", "index") && !(left.k != "ident" && optionalFlag(left))) {
                left
            } else {
                throw ScriptError(line, "Invalid assignment target")
            }
            advance()
            val value = assignment(noIn)
            return N("assign", line, op, target, value)
        }
        return left
    }

    private fun arrow(): N {
        val line = tok.line
        val params: List<Pair<N, N?>>
        var rest: N? = null
        if (tok.kind == "ident") {
            params = listOf(N("pid", line, advance().value) to null)
        } else {
            val parsed = params()
            params = parsed.first
            rest = parsed.second
        }
        expect("=>")
        return functionBody(line, "", params, rest, true)
    }

    private fun params(): Pair<List<Pair<N, N?>>, N?> {
        expect("(")
        val params = ArrayList<Pair<N, N?>>()
        var rest: N? = null
        while (!isPunct(")")) {
            if (eat("...")) {
                rest = bindingTarget()
                break
            }
            val target = bindingTarget()
            val default = if (eat("=")) assignment() else null
            params.add(target to default)
            if (!isPunct(")")) expect(",")
        }
        expect(")")
        return params to rest
    }

    private fun functionBody(line: Int, name: String, params: List<Pair<N, N?>>, rest: N?, arrow: Boolean): N {
        val savedLoops = loops
        val savedSwitches = switches
        loops = 0
        switches = 0
        functions++
        try {
            if (arrow && !isPunct("{")) {
                val body = assignment()
                return N("func", line, name, params, rest, body, true, true)
            }
            val body = block()
            return N("func", line, name, params, rest, body, arrow, false)
        } finally {
            loops = savedLoops
            switches = savedSwitches
            functions--
        }
    }

    private fun functionRest(line: Int, declaration: Boolean): N {
        var name = ""
        if (tok.kind == "ident") name = advance().value as String else if (declaration) error()
        val (params, rest) = params()
        return functionBody(line, name, params, rest, false)
    }

    private fun conditional(noIn: Boolean): N {
        val line = tok.line
        val test = binary(0, noIn)
        if (eat("?")) {
            val then = assignment()
            expect(":")
            val other = assignment(noIn)
            return N("cond", line, test, then, other)
        }
        return test
    }

    private fun binary(minPrec: Int, noIn: Boolean): N {
        var left = unary()
        while (true) {
            val t = tok
            val op = if (t.kind == "punct" || (t.kind == "kw" && (t.value == "in" || t.value == "instanceof"))) t.value as String else break
            if (op == "in" && noIn) break
            val prec = BINARY[op]
            if (prec == null || prec <= minPrec) break
            val line = t.line
            advance()
            enter()
            val right = try {
                binary(if (op == "**") prec - 1 else prec, noIn)
            } finally {
                leave()
            }
            left = if (op == "&&" || op == "||" || op == "??") N("logic", line, op, left, right) else N("bin", line, op, left, right)
        }
        return left
    }

    private fun updateTarget(target: N): Boolean = target.k in setOf("ident", "member", "index") && !(target.k != "ident" && optionalFlag(target))

    private fun unary(): N {
        val t = tok
        val line = t.line
        if (t.kind == "punct" && t.value in setOf("!", "-", "+", "~")) {
            advance()
            enter()
            try {
                return N("unary", line, t.value, unary())
            } finally {
                leave()
            }
        }
        if (t.kind == "punct" && (t.value == "++" || t.value == "--")) {
            advance()
            val target = unary()
            if (!updateTarget(target)) throw ScriptError(line, "Invalid update target")
            return N("update", line, t.value, true, target)
        }
        if (t.kind == "kw" && t.value in setOf("typeof", "delete", "void")) {
            advance()
            enter()
            try {
                return N("unary", line, t.value, unary())
            } finally {
                leave()
            }
        }
        val expr = postfix()
        val next = tok
        if (next.kind == "punct" && (next.value == "++" || next.value == "--") && !next.newline) {
            if (!updateTarget(expr)) throw ScriptError(line, "Invalid update target")
            advance()
            return N("update", line, next.value, false, expr)
        }
        return expr
    }

    private fun postfix(): N {
        val line = tok.line
        var expr = if (isKw("new")) newExpression() else primary()
        var optional = false
        while (true) {
            val t = tok
            if (t.kind != "punct") break
            when (t.value) {
                "." -> {
                    advance()
                    expr = N("member", t.line, expr, propertyName(), false)
                }
                "?." -> {
                    optional = true
                    advance()
                    expr = if (isPunct("(")) {
                        N("call", t.line, expr, arguments(), true)
                    } else if (eat("[")) {
                        val key = expression()
                        expect("]")
                        N("index", t.line, expr, key, true)
                    } else {
                        N("member", t.line, expr, propertyName(), true)
                    }
                }
                "[" -> {
                    advance()
                    val key = expression()
                    expect("]")
                    expr = N("index", t.line, expr, key, false)
                }
                "(" -> expr = N("call", t.line, expr, arguments(), false)
                else -> break
            }
        }
        if (optional) return N("chain", line, expr)
        return expr
    }

    private fun propertyName(): String {
        val t = tok
        if (t.kind != "ident" && t.kind != "kw") error()
        advance()
        return t.value as String
    }

    private fun newExpression(): N {
        val line = advance().line
        var callee = if (isKw("new")) newExpression() else primary()
        while (true) {
            if (isPunct(".")) {
                advance()
                callee = N("member", line, callee, propertyName(), false)
            } else if (isPunct("[")) {
                advance()
                val key = expression()
                expect("]")
                callee = N("index", line, callee, key, false)
            } else {
                break
            }
        }
        val args = if (isPunct("(")) arguments() else emptyList()
        return N("new", line, callee, args)
    }

    private fun arguments(): List<N> {
        expect("(")
        val args = ArrayList<N>()
        while (!isPunct(")")) {
            if (eat("...")) args.add(N("spread", tok.line, assignment())) else args.add(assignment())
            if (!isPunct(")")) expect(",")
        }
        expect(")")
        return args
    }

    private fun primary(): N {
        val t = tok
        val line = t.line
        when (t.kind) {
            "num", "str" -> {
                advance()
                return N("lit", line, t.value)
            }
            "tmpl" -> {
                advance()
                val parts = ArrayList<N>()
                for (tokens in t.extra as List<List<Token>>) {
                    val sub = Parser(tokens)
                    sub.nesting = nesting
                    sub.loops = loops
                    sub.switches = switches
                    sub.shorthand = shorthand
                    val expr = sub.expression()
                    if (sub.tok.kind != "eof") sub.error()
                    parts.add(expr)
                }
                return N("tmpl", line, t.value, parts)
            }
            "regex" -> {
                advance()
                return N("regex", line, t.value, t.extra, compileRegex(t.value as String, t.extra as String, line))
            }
            "ident" -> {
                advance()
                return N("ident", line, t.value)
            }
            "kw" -> when (t.value) {
                "true", "false" -> {
                    advance()
                    return N("lit", line, t.value == "true")
                }
                "null" -> {
                    advance()
                    return N("lit", line, null)
                }
                "this" -> {
                    advance()
                    return N("this", line)
                }
                "function" -> {
                    advance()
                    return functionRest(line, false)
                }
            }
            "punct" -> when (t.value) {
                "(" -> {
                    advance()
                    val expr = expression()
                    expect(")")
                    return expr
                }
                "[" -> return arrayLiteral()
                "{" -> return objectLiteral()
            }
        }
        error()
    }

    private fun arrayLiteral(): N {
        val line = advance().line
        val elements = ArrayList<N?>()
        while (!isPunct("]")) {
            if (isPunct(",")) {
                advance()
                elements.add(null)
                continue
            }
            if (eat("...")) elements.add(N("spread", tok.line, assignment())) else elements.add(assignment())
            if (!isPunct("]")) expect(",")
        }
        advance()
        return N("array", line, elements)
    }

    private fun objectLiteral(): N {
        val line = advance().line
        val props = ArrayList<N>()
        while (!isPunct("}")) {
            if (eat("...")) {
                props.add(N("spread", 0, assignment()))
            } else {
                val keyTok = tok
                var computed = false
                val key: Any?
                if (keyTok.kind in setOf("ident", "kw", "str")) {
                    key = keyTok.value
                    advance()
                } else if (keyTok.kind == "num") {
                    key = numberToString(keyTok.value as Double)
                    advance()
                } else if (isPunct("[")) {
                    advance()
                    key = assignment()
                    expect("]")
                    computed = true
                } else {
                    error()
                }
                if (eat(":")) {
                    props.add(N("prop", 0, key, assignment(), computed, null))
                } else if (isPunct("(")) {
                    val (params, rest) = params()
                    val fn = functionBody(keyTok.line, if (!computed) key as String else "", params, rest, false)
                    props.add(N("prop", 0, key, fn, computed, null))
                } else if (keyTok.kind == "ident" && !computed) {
                    var default: N? = null
                    if (isPunct("=")) {
                        advance()
                        default = assignment()
                    }
                    props.add(N("prop", 0, key, N("ident", keyTok.line, key), false, default))
                } else {
                    error()
                }
            }
            if (!isPunct("}")) expect(",")
        }
        advance()
        val node = N("object", line, props)
        if (props.any { it.k == "prop" && it.d != null }) shorthand[node] = line
        return node
    }
}

internal fun parseScript(source: String): List<N> {
    val parser = Parser(lexAll(source))
    val program = parser.program()
    if (parser.shorthand.isNotEmpty()) throw ScriptError(parser.shorthand.values.min(), "Invalid shorthand property initializer")
    return program
}

private fun badRegex(line: Int, source: String): Nothing = throw ScriptError(line, "Invalid regular expression /$source/")

private val SPACE_CLASS: String = JS_SPACE.map { String.format("\\u%04x", it.code) }.joinToString("")
private val QUANTIFIER_RE = Regex("^\\{[0-9]+(,[0-9]*)?\\}")
private val LOOSE_QUANTIFIER_RE = Regex("^\\{,([0-9]+)\\}")

private fun translateRegex(source: String, flags: String, line: Int): Triple<String, List<String>, List<String>> {
    val out = StringBuilder()
    val names = ArrayList<String>()
    val javaNames = ArrayList<String>()
    var index = 0
    var inClass = false
    val dotall = flags.contains('s')
    val multiline = flags.contains('m')
    val cps = cpList(source)
    val length = cps.size
    fun at(i: Int): String = if (i in 0 until length) cps[i] else ""
    fun rest(i: Int): String = cps.subList(minOf(i, length), length).joinToString("")
    fun find(target: String, from: Int): Int {
        for (i in from until length) if (cps[i] == target) return i
        return -1
    }
    fun javaName(name: String): String {
        val position = names.indexOf(name)
        return if (position >= 0) javaNames[position] else "g${names.size + 1000}q"
    }
    while (index < length) {
        val ch = cps[index]
        if (ch == "\\") {
            if (index + 1 >= length) badRegex(line, source)
            val nxt = cps[index + 1]
            index += 2
            when {
                nxt == "d" -> out.append(if (inClass) "0-9" else "[0-9]")
                nxt == "D" -> {
                    if (inClass) badRegex(line, source)
                    out.append("[^0-9]")
                }
                nxt == "w" -> out.append(if (inClass) "A-Za-z0-9_" else WORD)
                nxt == "W" -> {
                    if (inClass) badRegex(line, source)
                    out.append("[^A-Za-z0-9_]")
                }
                nxt == "s" -> out.append(if (inClass) SPACE_CLASS else "[$SPACE_CLASS]")
                nxt == "S" -> {
                    if (inClass) badRegex(line, source)
                    out.append("[^$SPACE_CLASS]")
                }
                nxt == "b" && !inClass -> out.append("(?:(?<=$WORD)(?!$WORD)|(?<!$WORD)(?=$WORD))")
                nxt == "B" && !inClass -> out.append("(?:(?<=$WORD)(?=$WORD)|(?<!$WORD)(?!$WORD))")
                nxt == "b" -> out.append("\\x08")
                nxt == "k" && at(index) == "<" -> {
                    val end = find(">", index)
                    if (end < 0) badRegex(line, source)
                    val name = cps.subList(index + 1, end).joinToString("")
                    if (name !in names) badRegex(line, source)
                    out.append("\\k<" + javaName(name) + ">")
                    index = end + 1
                }
                nxt == "u" && at(index) == "{" -> {
                    val end = find("}", index)
                    if (end < 0) badRegex(line, source)
                    val digits = cps.subList(index + 1, end).joinToString("")
                    if (digits.isEmpty() || !digits.all(::isHex) || BigInteger(digits, 16) > BigInteger.valueOf(0x10FFFF)) badRegex(line, source)
                    val value = digits.toLong(16)
                    out.append("\\x{" + java.lang.Long.toHexString(value) + "}")
                    index = end + 1
                }
                nxt == "c" && index < length && at(index).length == 1 && Character.isLetter(at(index)[0]) -> {
                    out.append(String.format("\\x%02x", at(index)[0].code % 32))
                    index++
                }
                nxt == "0" && !(at(index).length == 1 && Character.isDigit(at(index)[0])) -> out.append("\\x00")
                nxt == "/" -> out.append("/")
                nxt == "v" -> out.append("\\x0b")
                nxt == "Z" -> out.append("\\z")
                nxt == "U" -> {
                    val digits = cps.subList(minOf(index, length), minOf(index + 8, length)).joinToString("")
                    if (digits.length != 8 || !digits.all(::isHex) || digits.toLong(16) > 0x10FFFF) badRegex(line, source)
                    out.append("\\x{" + java.lang.Long.toHexString(digits.toLong(16)) + "}")
                    index += 8
                }
                nxt.length == 1 && nxt[0] in 'a'..'z' || nxt.length == 1 && nxt[0] in 'A'..'Z' -> {
                    if (nxt[0] !in "afnrtxuNA") badRegex(line, source)
                    out.append("\\" + nxt)
                }
                else -> out.append("\\" + nxt)
            }
            continue
        }
        if (inClass) {
            if (ch == "]") {
                inClass = false
            } else if (ch == "[") {
                out.append("\\[")
                index++
                continue
            } else if (ch == "&") {
                out.append("\\&")
                index++
                continue
            }
            out.append(ch)
            index++
            continue
        }
        if (ch == "[") {
            inClass = true
            if (rest(index).startsWith("[^]")) {
                out.append("[\\s\\S]")
                index += 3
                inClass = false
                continue
            }
            if (rest(index).startsWith("[]")) {
                out.append("(?!)")
                index += 2
                inClass = false
                continue
            }
            out.append("[")
            index++
            if (at(index) == "^") {
                out.append("^")
                index++
            }
            if (at(index) == "]") {
                out.append("\\]")
                index++
            }
            continue
        }
        if (ch == "(" && rest(index).startsWith("(?<") && at(index + 3) != "=" && at(index + 3) != "!") {
            val end = find(">", index)
            if (end < 0) badRegex(line, source)
            val name = cps.subList(index + 3, end).joinToString("")
            if (name.isEmpty() || !Character.isJavaIdentifierStart(name[0]) || !name.all { Character.isJavaIdentifierPart(it) } ||
                name in names
            ) {
                badRegex(line, source)
            }
            names.add(name)
            javaNames.add("g${names.size}q")
            out.append("(?<" + javaNames.last() + ">")
            index = end + 1
            continue
        }
        if (ch == "(" && rest(index).startsWith("(?P")) badRegex(line, source)
        when (ch) {
            "." -> out.append(if (dotall) "[\\s\\S]" else "[^\\n\\r\\u2028\\u2029]")
            "$" -> out.append(if (multiline) "(?=[\\n\\r\\u2028\\u2029]|\\z)" else "\\z")
            "^" -> out.append(if (multiline) "(?:(?<=[\\n\\r\\u2028\\u2029])|\\A)" else "\\A")
            "{" -> {
                val tail = rest(index)
                val loose = LOOSE_QUANTIFIER_RE.find(tail)
                if (QUANTIFIER_RE.containsMatchIn(tail)) {
                    out.append("{")
                } else if (loose != null) {
                    out.append("{0," + loose.groupValues[1] + "}")
                    index += cpLen(loose.value)
                    continue
                } else {
                    out.append("\\{")
                }
            }
            else -> out.append(ch)
        }
        index++
    }
    return Triple(out.toString(), names, javaNames)
}

internal fun compileRegex(source: String, flags: String, line: Int = 0): CompiledRegex {
    if (flags.any { it !in "gimsuyd" } || flags.toSet().size != flags.length) throw ScriptError(line, "Invalid regular expression flags")
    val (pattern, names, javaNames) = translateRegex(source, flags, line)
    val options = if (flags.contains('i')) Pattern.CASE_INSENSITIVE or Pattern.UNICODE_CASE else 0
    try {
        return CompiledRegex(Pattern.compile(pattern, options), names, javaNames)
    } catch (_: PatternSyntaxException) {
        badRegex(line, source)
    } catch (_: IllegalArgumentException) {
        badRegex(line, source)
    } catch (_: StackOverflowError) {
        badRegex(line, source)
    }
}

internal class RegexMatch(val start: Int, val end: Int, val group0: String, val groups: List<String?>, val named: Map<String, String?>)

internal fun regexSearch(regex: CompiledRegex, subject: String, position: Int, anchored: Boolean = false): RegexMatch? {
    val total = cpLen(subject)
    if (position > total) return null
    val matcher: Matcher = regex.pattern.matcher(subject)
    val offset = cpOffset(subject, position)
    val found = if (anchored) {
        matcher.region(offset, subject.length)
        matcher.useTransparentBounds(true)
        matcher.useAnchoringBounds(false)
        matcher.lookingAt()
    } else {
        matcher.find(offset)
    }
    if (!found) return null
    val groups = (1..matcher.groupCount()).map { matcher.group(it) }
    val named = LinkedHashMap<String, String?>()
    for ((i, name) in regex.names.withIndex()) named[name] = matcher.group(regex.javaNames[i])
    return RegexMatch(cpToIndex(subject, matcher.start()), cpToIndex(subject, matcher.end()), matcher.group(), groups, named)
}

internal fun shortestDecimal(value: Double): BigDecimal {
    val exact = BigDecimal(value)
    for (precision in 1..17) {
        val rounded = exact.round(java.math.MathContext(precision, java.math.RoundingMode.HALF_EVEN))
        if (rounded.toDouble() == value) return rounded
    }
    return exact.round(java.math.MathContext(17, java.math.RoundingMode.HALF_EVEN))
}

internal fun numberToString(value: Double): String {
    if (value.isNaN()) return "NaN"
    if (value == Double.POSITIVE_INFINITY) return "Infinity"
    if (value == Double.NEGATIVE_INFINITY) return "-Infinity"
    if (value == 0.0) return "0"
    val sign = if (value < 0) "-" else ""
    val decimal = shortestDecimal(Math.abs(value)).stripTrailingZeros()
    val digits = decimal.unscaledValue().toString()
    val k = digits.length
    val n = k - decimal.scale()
    if (n in k..21) return sign + digits + "0".repeat(n - k)
    if (n in 1..21) return sign + digits.substring(0, n) + "." + digits.substring(n)
    if (n > -6 && n <= 0) return sign + "0." + "0".repeat(-n) + digits
    val e = n - 1
    val exp = (if (e >= 0) "+" else "-") + Math.abs(e)
    if (k == 1) return sign + digits + "e" + exp
    return sign + digits[0] + "." + digits.substring(1) + "e" + exp
}

internal fun stringToNumber(raw: String): Double {
    val text = jsStrip(raw)
    if (text.isEmpty()) return 0.0
    if (text == "Infinity" || text == "+Infinity") return Double.POSITIVE_INFINITY
    if (text == "-Infinity") return Double.NEGATIVE_INFINITY
    if (text.length > 2 && text[0] == '0' && text[1] in "xXoObB") {
        val base = when (text[1].lowercaseChar()) {
            'x' -> 16
            'o' -> 8
            else -> 2
        }
        if (text.contains('_')) return Double.NaN
        var body = text.substring(2).trim()
        var negative = false
        if (body.startsWith("+") || body.startsWith("-")) {
            negative = body[0] == '-'
            body = body.substring(1)
        }
        val digits = "0123456789abcdefghijklmnopqrstuvwxyz".substring(0, base)
        if (body.isEmpty() || !body.all { digits.indexOf(it.lowercaseChar()) >= 0 }) return Double.NaN
        val number = BigInteger(body, base).toDouble()
        return if (negative) -number else number
    }
    if (DECIMAL_RE.matches(text)) return text.toDouble()
    return Double.NaN
}

internal fun pyFloorDiv(a: Long, b: Long): Long = Math.floorDiv(a, b)
internal fun pyMod(a: Long, b: Long): Long = Math.floorMod(a, b)

private fun truncLong(value: Double): Long = value.toLong()

internal fun dateFromParts(
    year: Double, month: Double, day: Double = 1.0, hours: Double = 0.0, minutes: Double = 0.0, seconds: Double = 0.0, ms: Double = 0.0,
): Double {
    val values = listOf(year, month, day, hours, minutes, seconds, ms)
    if (values.any { it.isNaN() || it.isInfinite() }) return Double.NaN
    val y = truncLong(year)
    val m = truncLong(month)
    val yearTotal = y + pyFloorDiv(m, 12)
    val monthIndex = pyMod(m, 12)
    val days = daysFromCivil(yearTotal, monthIndex + 1, 1) + truncLong(day) - 1
    return days.toDouble() * 86400000.0 + truncLong(hours).toDouble() * 3600000.0 + truncLong(minutes).toDouble() * 60000.0 +
        truncLong(seconds).toDouble() * 1000.0 + truncLong(ms).toDouble()
}

internal fun dateFromParts(year: Long, month: Long, day: Long = 1, hours: Long = 0, minutes: Long = 0, seconds: Long = 0, ms: Long = 0): Long {
    val yearTotal = year + pyFloorDiv(month, 12)
    val monthIndex = pyMod(month, 12)
    val days = daysFromCivil(yearTotal, monthIndex + 1, 1) + day - 1
    return days * 86400000L + hours * 3600000L + minutes * 60000L + seconds * 1000L + ms
}

internal fun daysFromCivil(yearIn: Long, month: Long, day: Long): Long {
    val year = yearIn - if (month <= 2) 1 else 0
    val era = pyFloorDiv(if (year >= 0) year else year - 399, 400)
    val yoe = year - era * 400
    val doy = pyFloorDiv(153 * (month + (if (month > 2) -3 else 9)) + 2, 5) + day - 1
    val doe = yoe * 365 + pyFloorDiv(yoe, 4) - pyFloorDiv(yoe, 100) + doy
    return era * 146097 + doe - 719468
}

internal data class Civil(
    val year: Long, val month: Int, val day: Int, val hours: Int, val minutes: Int, val seconds: Int, val millis: Int, val weekday: Int,
)

internal fun civilFromMs(msIn: Long): Civil {
    val days = Math.floorDiv(msIn, 86400000L)
    val rem = Math.floorMod(msIn, 86400000L)
    val z = days + 719468
    val era = pyFloorDiv(if (z >= 0) z else z - 146096, 146097)
    val doe = z - era * 146097
    val yoe = pyFloorDiv(doe - pyFloorDiv(doe, 1460) + pyFloorDiv(doe, 36524) - pyFloorDiv(doe, 146096), 365)
    var year = yoe + era * 400
    val doy = doe - (365 * yoe + pyFloorDiv(yoe, 4) - pyFloorDiv(yoe, 100))
    val mp = pyFloorDiv(5 * doy + 2, 153)
    val day = doy - pyFloorDiv(153 * mp + 2, 5) + 1
    val month = mp + if (mp < 10) 3 else -9
    if (month <= 2) year += 1
    val weekday = Math.floorMod(days + 4, 7L)
    return Civil(
        year, month.toInt(), day.toInt(), (rem / 3600000).toInt(), (rem / 60000 % 60).toInt(), (rem / 1000 % 60).toInt(),
        (rem % 1000).toInt(), weekday.toInt(),
    )
}

internal fun monthDays(year: Long, month: Int): Int {
    if (month == 2) return if ((year % 4 == 0L && year % 100 != 0L) || year % 400 == 0L) 29 else 28
    return if (month in setOf(4, 6, 9, 11)) 30 else 31
}

internal fun parseDateString(raw: String): Double {
    val text = jsStrip(raw)
    val match = ISO_DATE_RE.matchEntire(text) ?: return Double.NaN
    val g = match.groupValues
    val yearText = g[1]
    if (yearText == "-000000") return Double.NaN
    val year = yearText.toLong()
    val month = g[2].ifEmpty { "1" }.toInt()
    val day = g[3].ifEmpty { "1" }.toInt()
    val hours = g[4].ifEmpty { "0" }.toInt()
    val minutes = g[5].ifEmpty { "0" }.toInt()
    val seconds = g[6].ifEmpty { "0" }.toInt()
    val ms = g[7].ifEmpty { "0" }.take(3).padEnd(3, '0').toInt()
    if (!(month in 1..12 && day in 1..31 && hours <= 24 && minutes <= 59 && seconds <= 59)) return Double.NaN
    if (hours == 24 && (minutes != 0 || seconds != 0 || ms != 0)) return Double.NaN
    if (day > monthDays(year, month)) return Double.NaN
    var value = dateFromParts(year.toDouble(), (month - 1).toDouble(), day.toDouble(), hours.toDouble(), minutes.toDouble(), seconds.toDouble(), ms.toDouble())
    val offset = g[8]
    if (offset.isNotEmpty() && offset != "Z") {
        val sign = if (offset[0] == '+') 1 else -1
        value -= sign * (offset.substring(1, 3).toInt() * 60 + offset.substring(4, 6).toInt()) * 60000.0
    }
    return timeClip(value)
}

internal fun isoString(ms: Double): String {
    val c = civilFromMs(ms.toLong())
    val yearText = if (c.year in 0..9999) String.format("%04d", c.year) else (if (c.year > 0) "+" else "-") + String.format("%06d", Math.abs(c.year))
    return String.format("%s-%02d-%02dT%02d:%02d:%02d.%03dZ", yearText, c.month, c.day, c.hours, c.minutes, c.seconds, c.millis)
}

internal fun dateToString(ms: Double): String {
    if (ms.isNaN()) return "Invalid Date"
    val c = civilFromMs(ms.toLong())
    val yearText = if (c.year < 0) String.format("-%06d", -c.year) else String.format("%04d", c.year)
    return String.format(
        "%s %s %02d %s %02d:%02d:%02d GMT+0000 (Coordinated Universal Time)", DAY_NAMES[c.weekday], MONTH_NAMES[c.month - 1], c.day,
        yearText, c.hours, c.minutes, c.seconds,
    )
}

internal fun isIndexKey(key: String): Boolean = INDEX_RE.matches(key) && (key.length < 10 || (key.length == 10 && key.toLong() < 4294967295L))

internal fun ownKeys(obj: Map<String, Any?>): List<String> {
    val numeric = ArrayList<String>()
    val other = ArrayList<String>()
    for (key in obj.keys) if (isIndexKey(key)) numeric.add(key) else other.add(key)
    if (numeric.isEmpty()) return other
    numeric.sortBy { it.toLong() }
    return numeric + other
}

internal fun fromJson(value: Any?): Any? = when (value) {
    null, is Boolean, is String -> value
    is Number -> value.toDouble()
    is Map<*, *> -> JsObject().also { out -> for ((k, v) in value) out[k.toString()] = fromJson(v) }
    is List<*> -> JsArray(value.map { fromJson(it) })
    else -> value.toString()
}

internal fun toJsonValue(value: Any?, seen: IdentityHashMap<Any, Boolean> = IdentityHashMap()): Any? {
    when (value) {
        Undefined, null -> return null
        is Boolean, is String -> return value
        is Double -> {
            if (value.isNaN() || value.isInfinite()) return null
            if (value == Math.rint(value) && Math.abs(value) < JsLimits.MAX_INT) return value.toLong()
            return value
        }
        is JsDate -> return if (value.ms.isNaN()) null else isoString(value.ms)
        is JsFunction, is Native -> return null
        is JsRegExp, is JsCollection -> return LinkedHashMap<String, Any?>()
    }
    if (value !is Map<*, *> && value !is List<*>) return null
    if (seen.containsKey(value)) throw ScriptLimit("The script returned a value that contains itself")
    seen[value] = true
    try {
        if (value is Map<*, *>) {
            @Suppress("UNCHECKED_CAST")
            val obj = value as Map<String, Any?>
            val out = LinkedHashMap<String, Any?>()
            for (key in ownKeys(obj)) {
                val item = obj[key]
                if (item === Undefined || item is JsFunction || item is Native) continue
                out[key] = toJsonValue(item, seen)
            }
            return out
        }
        return (value as List<*>).map { toJsonValue(it, seen) }
    } finally {
        seen.remove(value)
    }
}

internal fun jsJsonQuote(text: String): String {
    val out = StringBuilder("\"")
    var i = 0
    while (i < text.length) {
        val code = text.codePointAt(i)
        val n = Character.charCount(code)
        when {
            code == '"'.code -> out.append("\\\"")
            code == '\\'.code -> out.append("\\\\")
            code == '\n'.code -> out.append("\\n")
            code == '\r'.code -> out.append("\\r")
            code == '\t'.code -> out.append("\\t")
            code == 8 -> out.append("\\b")
            code == 12 -> out.append("\\f")
            code < 0x20 || code in 0xD800..0xDFFF -> out.append(String.format("\\u%04x", code))
            else -> out.appendCodePoint(code)
        }
        i += n
    }
    return out.append('"').toString()
}

internal object PyJson {
    class Bad : Exception(null, null, false, false)

    private val NUMBER = Pattern.compile("-?(?:0|[1-9][0-9]*)(\\.[0-9]+)?([eE][-+]?[0-9]+)?")

    fun loads(text: String): Any? {
        val parser = Reader(text)
        parser.ws()
        val value = parser.value(0)
        parser.ws()
        if (parser.pos != text.length) throw Bad()
        return value
    }

    private class Reader(val s: String) {
        var pos = 0

        fun ws() {
            while (pos < s.length && s[pos] in " \t\n\r") pos++
        }

        fun value(depth: Int): Any? {
            if (depth > 900) throw Bad()
            if (pos >= s.length) throw Bad()
            when (s[pos]) {
                '{' -> {
                    pos++
                    val out = LinkedHashMap<String, Any?>()
                    ws()
                    if (pos < s.length && s[pos] == '}') {
                        pos++
                        return out
                    }
                    while (true) {
                        ws()
                        if (pos >= s.length || s[pos] != '"') throw Bad()
                        val key = string()
                        ws()
                        if (pos >= s.length || s[pos] != ':') throw Bad()
                        pos++
                        ws()
                        val item = value(depth + 1)
                        out.remove(key)
                        out[key] = item
                        ws()
                        if (pos < s.length && s[pos] == ',') {
                            pos++
                            continue
                        }
                        if (pos < s.length && s[pos] == '}') {
                            pos++
                            return out
                        }
                        throw Bad()
                    }
                }
                '[' -> {
                    pos++
                    val out = ArrayList<Any?>()
                    ws()
                    if (pos < s.length && s[pos] == ']') {
                        pos++
                        return out
                    }
                    while (true) {
                        ws()
                        out.add(value(depth + 1))
                        ws()
                        if (pos < s.length && s[pos] == ',') {
                            pos++
                            continue
                        }
                        if (pos < s.length && s[pos] == ']') {
                            pos++
                            return out
                        }
                        throw Bad()
                    }
                }
                '"' -> return string()
                't' -> return word("true", true)
                'f' -> return word("false", false)
                'n' -> return word("null", null)
                else -> return number()
            }
        }

        fun word(text: String, value: Any?): Any? {
            if (!s.startsWith(text, pos)) throw Bad()
            pos += text.length
            return value
        }

        fun number(): Any {
            val matcher = NUMBER.matcher(s)
            matcher.region(pos, s.length)
            if (!matcher.lookingAt()) throw Bad()
            val text = matcher.group()
            pos += text.length
            if (matcher.group(1) == null && matcher.group(2) == null) {
                val big = BigInteger(text)
                return if (big.bitLength() < 64) big.toLong() else big.toDouble()
            }
            return text.toDouble()
        }

        fun string(): String {
            pos++
            val out = StringBuilder()
            while (true) {
                if (pos >= s.length) throw Bad()
                val c = s[pos]
                when {
                    c == '"' -> {
                        pos++
                        return out.toString()
                    }
                    c == '\\' -> {
                        if (pos + 1 >= s.length) throw Bad()
                        val e = s[pos + 1]
                        pos += 2
                        when (e) {
                            '"' -> out.append('"')
                            '\\' -> out.append('\\')
                            '/' -> out.append('/')
                            'b' -> out.append('\b')
                            'f' -> out.append('\u000c')
                            'n' -> out.append('\n')
                            'r' -> out.append('\r')
                            't' -> out.append('\t')
                            'u' -> {
                                val hex = s.substring(pos, minOf(pos + 4, s.length))
                                if (hex.length != 4 || !hex.all(::isHex)) throw Bad()
                                out.append(hex.toInt(16).toChar())
                                pos += 4
                            }
                            else -> throw Bad()
                        }
                    }
                    c.code < 0x20 -> throw Bad()
                    else -> {
                        out.append(c)
                        pos++
                    }
                }
            }
        }
    }
}

internal fun uriEncode(text: String): String {
    val out = StringBuilder()
    var i = 0
    while (i < text.length) {
        val cp = text.codePointAt(i)
        val n = Character.charCount(cp)
        if (cp < 128 && URI_UNRESERVED.indexOf(cp.toChar()) >= 0) {
            out.append(cp.toChar())
        } else {
            if (cp in 0xD800..0xDFFF) throw Thrown(errorObject("URIError", "URI malformed"))
            for (byte in String(Character.toChars(cp)).toByteArray(Charsets.UTF_8)) out.append(String.format("%%%02X", byte.toInt() and 0xff))
        }
        i += n
    }
    return out.toString()
}

private fun uriByte(text: String, index: Int): Int {
    val pair = if (index + 3 <= text.length) text.substring(index + 1, index + 3) else ""
    if (index >= text.length || text[index] != '%' || pair.length != 2 || !pair.all(::isHex)) throw Thrown(errorObject("URIError", "URI malformed"))
    return pair.toInt(16)
}

internal fun uriDecode(text: String): String {
    val out = StringBuilder()
    var index = 0
    while (index < text.length) {
        val ch = text[index]
        if (ch != '%') {
            out.append(ch)
            index++
            continue
        }
        val first = uriByte(text, index)
        index += 3
        if (first < 0x80) {
            out.append(first.toChar())
            continue
        }
        val count = when {
            first and 0xE0 == 0xC0 -> 1
            first and 0xF0 == 0xE0 -> 2
            first and 0xF8 == 0xF0 -> 3
            else -> throw Thrown(errorObject("URIError", "URI malformed"))
        }
        val data = java.io.ByteArrayOutputStream()
        data.write(first)
        repeat(count) {
            val byte = uriByte(text, index)
            if (byte and 0xC0 != 0x80) throw Thrown(errorObject("URIError", "URI malformed"))
            data.write(byte)
            index += 3
        }
        out.append(strictUtf8(data.toByteArray()) ?: throw Thrown(errorObject("URIError", "URI malformed")))
    }
    return out.toString()
}

internal fun strictUtf8(bytes: ByteArray): String? = try {
    Charsets.UTF_8.newDecoder().onMalformedInput(java.nio.charset.CodingErrorAction.REPORT)
        .onUnmappableCharacter(java.nio.charset.CodingErrorAction.REPORT).decode(java.nio.ByteBuffer.wrap(bytes)).toString()
} catch (_: java.nio.charset.CharacterCodingException) {
    null
}

internal fun expandReplacement(
    template: String, matched: String, position: Int, subject: String, groups: List<Any?>, named: Map<String, Any?>?,
): String {
    val t = cpList(template)
    val out = StringBuilder()
    var index = 0
    val length = t.size
    while (index < length) {
        val ch = t[index]
        if (ch != "$" || index + 1 >= length) {
            out.append(ch)
            index++
            continue
        }
        val nxt = t[index + 1]
        when {
            nxt == "$" -> {
                out.append("$")
                index += 2
            }
            nxt == "&" -> {
                out.append(matched)
                index += 2
            }
            nxt == "`" -> {
                out.append(cpSub(subject, 0, position))
                index += 2
            }
            nxt == "'" -> {
                out.append(cpSub(subject, position + cpLen(matched)))
                index += 2
            }
            nxt.length == 1 && nxt[0] in '0'..'9' -> {
                val second = if (index + 2 < length) t[index + 2] else ""
                val two = nxt + second
                if (second.length == 1 && second[0] in '0'..'9' && two.toInt() in 1..groups.size) {
                    val value = groups[two.toInt() - 1]
                    out.append(if (value === Undefined) "" else value as String)
                    index += 3
                } else if (nxt.toInt() in 1..groups.size) {
                    val value = groups[nxt.toInt() - 1]
                    out.append(if (value === Undefined) "" else value as String)
                    index += 2
                } else {
                    out.append("$").append(nxt)
                    index += 2
                }
            }
            nxt == "<" && named != null -> {
                var end = -1
                for (i in index until length) if (t[i] == ">") {
                    end = i
                    break
                }
                if (end < 0) {
                    out.append("$<")
                    index += 2
                } else {
                    val name = t.subList(index + 2, end).joinToString("")
                    val value = if (named.containsKey(name)) named[name] else Undefined
                    out.append(if (value === Undefined) "" else jsToString(value))
                    index = end + 1
                }
            }
            else -> {
                out.append("$")
                index++
            }
        }
    }
    return out.toString()
}

internal fun matchArray(regex: JsRegExp, match: RegexMatch, subject: String): JsArray {
    val result = JsArray(listOf(match.group0) + match.groups.map { it ?: Undefined })
    result.isMatch = true
    result.index = match.start.toDouble()
    result.input = subject
    result.groups = if (regex.names.isNotEmpty()) {
        JsObject().also { out -> for (name in regex.names) out[name] = match.named[name] ?: Undefined }
    } else {
        Undefined
    }
    return result
}

internal fun regexExec(regex: JsRegExp, subject: String): JsArray? {
    val sticky = regex.flags.contains('y')
    val useLast = regex.flags.contains('g') || sticky
    val start = if (useLast) regex.lastIndex else 0
    if (start > cpLen(subject)) {
        regex.lastIndex = 0
        return null
    }
    val match = regexSearch(regex.compiled, subject, start, sticky)
    if (match == null) {
        if (useLast) regex.lastIndex = 0
        return null
    }
    if (useLast) regex.lastIndex = match.end
    return matchArray(regex, match, subject)
}

internal fun regexAll(regex: CompiledRegex, subject: String): List<RegexMatch> {
    val matches = ArrayList<RegexMatch>()
    var position = 0
    val total = cpLen(subject)
    while (position <= total) {
        val match = regexSearch(regex, subject, position) ?: break
        matches.add(match)
        if (matches.size > JsLimits.MAX_ARRAY) throw rangeError("Invalid array length")
        position = if (match.end > match.start) match.end else match.end + 1
    }
    return matches
}

internal fun toFixed(value: Double, digits: Int): String {
    if (value.isNaN()) return "NaN"
    if (Math.abs(value) >= 1e21 || value.isInfinite()) return numberToString(value)
    val negative = value < 0
    val exact = BigDecimal(Math.abs(value)).setScale(digits, java.math.RoundingMode.HALF_UP)
    return (if (negative) "-" else "") + exact.toPlainString()
}
