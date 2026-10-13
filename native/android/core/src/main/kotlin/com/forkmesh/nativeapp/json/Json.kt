package com.forkmesh.nativeapp.json

import java.math.BigDecimal
import java.math.BigInteger

object Json {
    class JsonException(message: String) : RuntimeException(message)

    fun interface Writable {
        fun toJson(): Any?
    }

    fun parse(text: String): Any? {
        val parser = Parser(text)
        parser.skipWhitespace()
        val value = parser.readValue()
        parser.skipWhitespace()
        if (parser.pos != text.length) throw JsonException("trailing characters at ${parser.pos}")
        return value
    }

    @Suppress("UNCHECKED_CAST")
    fun parseObject(text: String): MutableMap<String, Any?> {
        val value = try {
            parse(text)
        } catch (_: JsonException) {
            null
        }
        return value as? MutableMap<String, Any?> ?: LinkedHashMap()
    }

    fun stringify(value: Any?): String = StringBuilder().also { write(it, value) }.toString()

    private fun write(out: StringBuilder, value: Any?) {
        when (value) {
            null -> out.append("null")
            is String -> quote(out, value)
            is Boolean -> out.append(if (value) "true" else "false")
            is Double, is Float -> {
                val number = (value as Number).toDouble()
                if (number.isNaN() || number.isInfinite()) throw JsonException("cannot encode $number")
                out.append(formatDouble(number))
            }
            is BigDecimal -> out.append(value.toPlainString())
            is Number -> out.append(value.toString())
            is Map<*, *> -> {
                out.append('{')
                var first = true
                for ((key, item) in value) {
                    if (!first) out.append(',')
                    first = false
                    quote(out, key.toString())
                    out.append(':')
                    write(out, item)
                }
                out.append('}')
            }
            is Iterable<*> -> {
                out.append('[')
                var first = true
                for (item in value) {
                    if (!first) out.append(',')
                    first = false
                    write(out, item)
                }
                out.append(']')
            }
            is Array<*> -> write(out, value.asList())
            is Writable -> write(out, value.toJson())
            else -> quote(out, value.toString())
        }
    }

    fun formatDouble(number: Double): String {
        if (number == Math.rint(number) && Math.abs(number) < 1e21) {
            return BigDecimal(number).toBigInteger().toString() + ".0"
        }
        val text = number.toString()
        return if (text.contains('E')) BigDecimal(number).stripTrailingZeros().toPlainString() else text
    }

    private fun quote(out: StringBuilder, text: String) {
        out.append('"')
        for (c in text) {
            when (c) {
                '"' -> out.append("\\\"")
                '\\' -> out.append("\\\\")
                '\b' -> out.append("\\b")
                '' -> out.append("\\f")
                '\n' -> out.append("\\n")
                '\r' -> out.append("\\r")
                '\t' -> out.append("\\t")
                else -> if (c.code < 0x20) out.append(String.format("\\u%04x", c.code)) else out.append(c)
            }
        }
        out.append('"')
    }

    private class Parser(val text: String) {
        var pos = 0

        fun skipWhitespace() {
            while (pos < text.length) {
                val c = text[pos]
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') pos++ else break
            }
        }

        fun peek(): Char {
            if (pos >= text.length) throw JsonException("unexpected end of input")
            return text[pos]
        }

        fun readValue(): Any? {
            return when (val c = peek()) {
                '{' -> readObject()
                '[' -> readArray()
                '"' -> readString()
                't' -> { expectWord("true"); true }
                'f' -> { expectWord("false"); false }
                'n' -> { expectWord("null"); null }
                else -> if (c == '-' || c in '0'..'9') readNumber()
                else throw JsonException("unexpected '$c' at $pos")
            }
        }

        fun expectWord(word: String) {
            if (!text.startsWith(word, pos)) throw JsonException("expected $word at $pos")
            pos += word.length
        }

        fun readObject(): MutableMap<String, Any?> {
            val map = LinkedHashMap<String, Any?>()
            pos++
            skipWhitespace()
            if (peek() == '}') {
                pos++
                return map
            }
            while (true) {
                skipWhitespace()
                if (peek() != '"') throw JsonException("expected key at $pos")
                val key = readString()
                skipWhitespace()
                if (peek() != ':') throw JsonException("expected ':' at $pos")
                pos++
                skipWhitespace()
                map[key] = readValue()
                skipWhitespace()
                val c = peek()
                pos++
                if (c == '}') return map
                if (c != ',') throw JsonException("expected ',' or '}' at ${pos - 1}")
            }
        }

        fun readArray(): MutableList<Any?> {
            val list = ArrayList<Any?>()
            pos++
            skipWhitespace()
            if (peek() == ']') {
                pos++
                return list
            }
            while (true) {
                skipWhitespace()
                list.add(readValue())
                skipWhitespace()
                val c = peek()
                pos++
                if (c == ']') return list
                if (c != ',') throw JsonException("expected ',' or ']' at ${pos - 1}")
            }
        }

        fun readString(): String {
            pos++
            var out: StringBuilder? = null
            var start = pos
            while (true) {
                if (pos >= text.length) throw JsonException("unterminated string")
                val c = text[pos]
                if (c == '"') {
                    val result = out?.append(text, start, pos)?.toString() ?: text.substring(start, pos)
                    pos++
                    return result
                }
                if (c == '\\') {
                    val builder = out ?: StringBuilder().also { out = it }
                    builder.append(text, start, pos)
                    pos++
                    if (pos >= text.length) throw JsonException("unterminated escape")
                    when (val e = text[pos++]) {
                        '"' -> builder.append('"')
                        '\\' -> builder.append('\\')
                        '/' -> builder.append('/')
                        'b' -> builder.append('\b')
                        'f' -> builder.append('')
                        'n' -> builder.append('\n')
                        'r' -> builder.append('\r')
                        't' -> builder.append('\t')
                        'u' -> {
                            if (pos + 4 > text.length) throw JsonException("bad unicode escape")
                            val code = text.substring(pos, pos + 4).toIntOrNull(16)
                                ?: throw JsonException("bad unicode escape at $pos")
                            builder.append(code.toChar())
                            pos += 4
                        }
                        else -> throw JsonException("bad escape \\$e")
                    }
                    start = pos
                    continue
                }
                if (c.code < 0x20) throw JsonException("control character in string at $pos")
                pos++
            }
        }

        fun readNumber(): Any {
            val start = pos
            if (text[pos] == '-') pos++
            var fractional = false
            while (pos < text.length) {
                val c = text[pos]
                if (c in '0'..'9') {
                    pos++
                } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                    fractional = true
                    pos++
                } else {
                    break
                }
            }
            val number = text.substring(start, pos)
            if (number == "-") throw JsonException("bad number at $start")
            return try {
                if (!fractional) {
                    val big = BigInteger(number)
                    if (big.bitLength() < 64) big.toLong() else big.toDouble()
                } else {
                    number.toDouble()
                }
            } catch (_: NumberFormatException) {
                throw JsonException("bad number '$number'")
            }
        }
    }
}
