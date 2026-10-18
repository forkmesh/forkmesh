package com.forkmesh.nativeapp.securedrive

import java.io.ByteArrayInputStream
import java.math.BigDecimal
import java.math.MathContext
import java.util.Locale
import java.util.regex.Pattern
import javax.xml.parsers.DocumentBuilderFactory
import org.w3c.dom.Element

class Sheet private constructor(private val cells: MutableList<MutableList<String>>) {
    constructor() : this(mutableListOf())

    internal class Value(
        val number: Double = 0.0,
        val text: String = "",
        val error: String = "",
        val isText: Boolean = false,
        val empty: Boolean = false,
    )

    val rows: Int get() = cells.size

    val columns: Int get() = cells.maxOfOrNull { it.size } ?: 0

    fun raw(row: Int, column: Int): String {
        if (row < 0 || row >= rows || column < 0 || column >= cells[row].size) return ""
        return cells[row][column]
    }

    fun setRaw(row: Int, column: Int, text: String) {
        if (row < 0 || column < 0 || row >= MAX_ROWS || column >= MAX_COLUMNS) return
        if (row >= rows) {
            if (text.isEmpty()) return
            while (cells.size <= row) cells.add(mutableListOf())
        }
        val line = cells[row]
        if (column >= line.size) {
            if (text.isEmpty()) return
            while (line.size <= column) line.add("")
        }
        line[column] = text
    }

    fun display(row: Int, column: Int): String {
        val text = raw(row, column)
        if (!text.startsWith("=")) return text
        val value = cellValue(row, column, HashSet())
        if (value.error.isNotEmpty()) return value.error
        if (value.isText) return value.text
        return formatNumber(if (value.empty) 0.0 else value.number)
    }

    fun toCsv(): ByteArray {
        var lastRow = -1
        for (r in 0 until rows) if (cells[r].any { it.isNotEmpty() }) lastRow = r
        val lines = mutableListOf<String>()
        for (r in 0..lastRow) {
            val row = cells[r].toMutableList()
            while (row.isNotEmpty() && row.last().isEmpty()) row.removeAt(row.size - 1)
            lines += row.joinToString(",") { cell ->
                if (cell.contains(',') || cell.contains('"') || cell.contains('\n') || cell.contains('\r') || cell != cell.trim()) {
                    "\"" + cell.replace("\"", "\"\"") + "\""
                } else {
                    cell
                }
            }
        }
        return if (lines.isEmpty()) ByteArray(0) else (lines.joinToString("\r\n") + "\r\n").toByteArray(Charsets.UTF_8)
    }

    internal fun cellValue(row: Int, column: Int, visiting: MutableSet<Long>): Value {
        val text = raw(row, column).trim()
        if (text.isEmpty()) return Value(empty = true)
        if (text.startsWith("=")) {
            val key = (row.toLong() shl 20) or column.toLong()
            if (visiting.contains(key) || visiting.size > 4096) return Value(error = "#CYCLE!")
            visiting.add(key)
            val value = FormulaParser(this, text.substring(1), visiting).run()
            visiting.remove(key)
            return value
        }
        val number = parseNumber(text)
        if (number != null) return Value(number = number)
        return Value(text = text, isText = true)
    }

    companion object {
        const val MAX_ROWS = 10000
        const val MAX_COLUMNS = 702
        private val NUMBER = Regex("^[+-]?([0-9]+(\\.[0-9]*)?|\\.[0-9]+)([eE][+-]?[0-9]+)?$")
        private val CELL = Regex("^([A-Za-z]{1,2})([0-9]{1,5})$")

        internal fun parseNumber(text: String): Double? = if (NUMBER.matches(text)) text.toDoubleOrNull() else null

        fun fromCsv(csv: ByteArray): Sheet {
            val sheet = Sheet()
            var text = String(csv, Charsets.UTF_8)
            if (text.startsWith("﻿")) text = text.substring(1)
            var row = mutableListOf<String>()
            val field = StringBuilder()
            var quoted = false
            var any = false
            var i = 0
            fun push(line: MutableList<String>) {
                if (sheet.cells.size < MAX_ROWS) sheet.cells.add(line.take(MAX_COLUMNS).toMutableList())
            }
            while (i < text.length) {
                val c = text[i]
                if (quoted) {
                    if (c == '"') {
                        if (i + 1 < text.length && text[i + 1] == '"') {
                            field.append(c)
                            i++
                        } else {
                            quoted = false
                        }
                    } else {
                        field.append(c)
                    }
                    i++
                    continue
                }
                when {
                    c == '"' && field.isEmpty() -> {
                        quoted = true
                        any = true
                    }
                    c == ',' -> {
                        row.add(field.toString())
                        field.setLength(0)
                        any = true
                    }
                    c == '\n' || c == '\r' -> {
                        if (c == '\r' && i + 1 < text.length && text[i + 1] == '\n') i++
                        row.add(field.toString())
                        field.setLength(0)
                        push(row)
                        row = mutableListOf()
                        any = false
                    }
                    else -> {
                        field.append(c)
                        any = true
                    }
                }
                i++
            }
            if (any || field.isNotEmpty() || row.isNotEmpty()) {
                row.add(field.toString())
                push(row)
            }
            return sheet
        }

        fun columnName(column: Int): String {
            val name = StringBuilder()
            var n = column + 1
            while (n > 0) {
                val rem = (n - 1) % 26
                name.insert(0, ('A' + rem))
                n = (n - 1) / 26
            }
            return name.toString()
        }

        fun parseCell(reference: String): Pair<Int, Int>? {
            val match = CELL.matchEntire(reference) ?: return null
            var c = 0
            for (ch in match.groupValues[1].uppercase()) c = c * 26 + (ch - 'A' + 1)
            val r = match.groupValues[2].toInt()
            if (r < 1 || r > MAX_ROWS || c < 1 || c > MAX_COLUMNS) return null
            return Pair(r - 1, c - 1)
        }

        fun formatNumber(value: Double): String {
            if (value.isNaN() || value.isInfinite()) return "#NUM!"
            if (value == Math.floor(value) && Math.abs(value) < 1e15) return value.toLong().toString()
            return formatG(value, 12)
        }

        internal fun formatG(value: Double, precision: Int): String {
            if (value == 0.0) return "0"
            val rounded = BigDecimal(value).round(MathContext(precision))
            val exponent = rounded.precision() - rounded.scale() - 1
            if (exponent < -4 || exponent >= precision) {
                val digits = rounded.unscaledValue().abs().toString().trimEnd('0').ifEmpty { "0" }
                val mantissa = if (digits.length > 1) digits[0] + "." + digits.substring(1) else digits
                val sign = if (rounded.signum() < 0) "-" else ""
                val expSign = if (exponent < 0) "-" else "+"
                val expDigits = Math.abs(exponent).toString().padStart(2, '0')
                return "$sign${mantissa}e$expSign$expDigits"
            }
            return rounded.stripTrailingZeros().toPlainString()
        }
    }
}

internal class FormulaParser(private val sheet: Sheet, private val text: String, private val visiting: MutableSet<Long>) {
    private var pos = 0

    fun run(): Sheet.Value {
        val value = comparison()
        skip()
        if (value.error.isEmpty() && pos < text.length) return error("#ERROR!")
        return value
    }

    private fun error(code: String) = Sheet.Value(error = code)

    private fun number(n: Double) = Sheet.Value(number = n)

    private fun skip() {
        while (pos < text.length && text[pos].isWhitespace()) pos++
    }

    private fun take(token: String): Boolean {
        skip()
        if (text.regionMatches(pos, token, 0, token.length, ignoreCase = true)) {
            pos += token.length
            return true
        }
        return false
    }

    private fun numeric(v: Sheet.Value): Double? {
        if (v.error.isNotEmpty() || v.isText) return null
        return if (v.empty) 0.0 else v.number
    }

    private fun comparison(): Sheet.Value {
        val left = additive()
        for (op in listOf("<=", ">=", "<>", "<", ">", "=")) {
            if (!take(op)) continue
            val right = additive()
            if (left.error.isNotEmpty()) return left
            if (right.error.isNotEmpty()) return right
            val result = if (left.isText || right.isText) {
                val order = left.text.compareTo(right.text, ignoreCase = true)
                when (op) {
                    "=" -> order == 0
                    "<>" -> order != 0
                    "<" -> order < 0
                    ">" -> order > 0
                    "<=" -> order <= 0
                    else -> order >= 0
                }
            } else {
                val a = numeric(left) ?: 0.0
                val b = numeric(right) ?: 0.0
                when (op) {
                    "=" -> a == b
                    "<>" -> a != b
                    "<" -> a < b
                    ">" -> a > b
                    "<=" -> a <= b
                    else -> a >= b
                }
            }
            return number(if (result) 1.0 else 0.0)
        }
        return left
    }

    private fun additive(): Sheet.Value {
        var left = multiplicative()
        while (true) {
            val plus = take("+")
            if (!plus && !take("-")) return left
            val right = multiplicative()
            if (left.error.isNotEmpty()) return left
            if (right.error.isNotEmpty()) return right
            val a = numeric(left) ?: return error("#VALUE!")
            val b = numeric(right) ?: return error("#VALUE!")
            left = number(if (plus) a + b else a - b)
        }
    }

    private fun multiplicative(): Sheet.Value {
        var left = power()
        while (true) {
            val times = take("*")
            if (!times && !take("/")) return left
            val right = power()
            if (left.error.isNotEmpty()) return left
            if (right.error.isNotEmpty()) return right
            val a = numeric(left) ?: return error("#VALUE!")
            val b = numeric(right) ?: return error("#VALUE!")
            if (!times && b == 0.0) return error("#DIV/0!")
            left = number(if (times) a * b else a / b)
        }
    }

    private fun power(): Sheet.Value {
        val base = unary()
        if (!take("^")) return base
        val exponent = power()
        if (base.error.isNotEmpty()) return base
        if (exponent.error.isNotEmpty()) return exponent
        val a = numeric(base) ?: return error("#VALUE!")
        val b = numeric(exponent) ?: return error("#VALUE!")
        return number(Math.pow(a, b))
    }

    private fun unary(): Sheet.Value {
        if (take("-")) {
            val inner = unary()
            if (inner.error.isNotEmpty()) return inner
            val a = numeric(inner) ?: return error("#VALUE!")
            return number(-a)
        }
        if (take("+")) return unary()
        return primary()
    }

    private fun lookingAt(pattern: Pattern): String? {
        val matcher = pattern.matcher(text)
        matcher.region(pos, text.length)
        matcher.useTransparentBounds(true)
        matcher.useAnchoringBounds(false)
        return if (matcher.lookingAt()) matcher.group() else null
    }

    private fun reference(): Pair<Int, Int>? {
        skip()
        val matcher = REFERENCE.matcher(text)
        matcher.region(pos, text.length)
        if (!matcher.lookingAt()) return null
        val cell = Sheet.parseCell(matcher.group(1) + matcher.group(2)) ?: return null
        pos = matcher.end()
        return cell
    }

    private fun range(values: MutableList<Sheet.Value>): Boolean {
        val start = pos
        val first = reference() ?: return false
        if (!take(":")) {
            values.add(sheet.cellValue(first.first, first.second, visiting))
            return true
        }
        val second = reference()
        if (second == null) {
            pos = start
            return false
        }
        for (r in minOf(first.first, second.first)..maxOf(first.first, second.first)) {
            for (c in minOf(first.second, second.second)..maxOf(first.second, second.second)) {
                values.add(sheet.cellValue(r, c, visiting))
            }
        }
        return true
    }

    private fun function(name: String): Sheet.Value {
        val values = mutableListOf<Sheet.Value>()
        val arguments = mutableListOf<List<Sheet.Value>>()
        if (!take(")")) {
            while (true) {
                val argument = mutableListOf<Sheet.Value>()
                val start = pos
                if (range(argument)) {
                    skip()
                    if (pos < text.length && text[pos] != ',' && text[pos] != ')') {
                        pos = start
                        argument.clear()
                        argument.add(comparison())
                    }
                } else {
                    argument.add(comparison())
                }
                arguments.add(argument)
                values.addAll(argument)
                if (take(",")) continue
                if (take(")")) break
                return error("#ERROR!")
            }
        }
        for (v in values) if (v.error.isNotEmpty()) return v
        val upper = name.uppercase(Locale.ROOT)
        val numbers = values.filter { !it.isText && !it.empty }.map { it.number }
        when (upper) {
            "SUM" -> return number(numbers.sum())
            "AVERAGE" -> return if (numbers.isEmpty()) error("#DIV/0!") else number(numbers.sum() / numbers.size)
            "MIN" -> return number(numbers.minOrNull() ?: 0.0)
            "MAX" -> return number(numbers.maxOrNull() ?: 0.0)
            "COUNT" -> return number(numbers.size.toDouble())
        }
        if (upper == "ABS" && arguments.size == 1 && arguments[0].size == 1) {
            val a = numeric(arguments[0][0])
            if (a != null) return number(Math.abs(a))
        }
        if (upper == "ROUND" && (arguments.size == 1 || arguments.size == 2)) {
            val a = numeric(arguments[0].firstOrNull() ?: Sheet.Value()) ?: return error("#VALUE!")
            val b = if (arguments.size == 2) numeric(arguments[1].firstOrNull() ?: Sheet.Value()) ?: return error("#VALUE!") else 0.0
            val scale = Math.pow(10.0, roundHalfAway(b))
            return number(roundHalfAway(a * scale) / scale)
        }
        if (upper == "IF" && (arguments.size == 2 || arguments.size == 3)) {
            val test = numeric(arguments[0].firstOrNull() ?: Sheet.Value()) ?: return error("#VALUE!")
            if (test != 0.0) return arguments[1].firstOrNull() ?: Sheet.Value()
            return if (arguments.size == 3) arguments[2].firstOrNull() ?: Sheet.Value() else number(0.0)
        }
        return error("#NAME?")
    }

    private fun roundHalfAway(x: Double): Double = if (x < 0) -Math.floor(-x + 0.5) else Math.floor(x + 0.5)

    private fun primary(): Sheet.Value {
        skip()
        if (pos >= text.length) return error("#ERROR!")
        if (take("(")) {
            val inner = comparison()
            if (!take(")")) return error("#ERROR!")
            return inner
        }
        if (text[pos] == '"') {
            val out = StringBuilder()
            pos++
            while (pos < text.length) {
                val c = text[pos++]
                if (c == '"') {
                    if (pos < text.length && text[pos] == '"') {
                        out.append(c)
                        pos++
                        continue
                    }
                    return Sheet.Value(text = out.toString(), isText = true)
                }
                out.append(c)
            }
            return error("#ERROR!")
        }
        lookingAt(NUMBER)?.let {
            pos += it.length
            return number(it.toDouble())
        }
        lookingAt(NAME)?.let {
            pos += it.length
            take("(")
            return function(it)
        }
        val cell = reference() ?: return error("#NAME?")
        return sheet.cellValue(cell.first, cell.second, visiting)
    }

    companion object {
        private val REFERENCE = Pattern.compile("\\$?([A-Za-z]{1,2})\\$?([0-9]{1,5})")
        private val NUMBER = Pattern.compile("[0-9]+(\\.[0-9]*)?([eE][-+]?[0-9]+)?|\\.[0-9]+([eE][-+]?[0-9]+)?")
        private val NAME = Pattern.compile("[A-Za-z]+(?=\\s*\\()")
    }
}

data class Point(val x: Double, val y: Double)

class Stroke(var color: String = "#1f2328", var width: Double = 3.0, val points: MutableList<Point> = mutableListOf())

class Drawing(
    var width: Int = 1600,
    var height: Int = 1000,
    var background: String = "#ffffff",
    val strokes: MutableList<Stroke> = mutableListOf(),
) {
    fun toSvg(): ByteArray {
        val out = StringBuilder()
        out.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
        out.append("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"$width\" height=\"$height\" viewBox=\"0 0 $width $height\" data-forkmesh-drawing=\"1\">\n")
        out.append("    <rect width=\"$width\" height=\"$height\" fill=\"${if (validColor(background)) background else "#ffffff"}\"/>\n")
        for (stroke in strokes) {
            if (stroke.points.isEmpty()) continue
            val d = mutableListOf<String>()
            stroke.points.forEachIndexed { i, p -> d += "${if (i == 0) "M" else "L"}${fixed(p.x)} ${fixed(p.y)}" }
            if (stroke.points.size == 1) d += "L${fixed(stroke.points[0].x)} ${fixed(stroke.points[0].y)}"
            val color = if (validColor(stroke.color)) stroke.color else "#1f2328"
            out.append("    <path d=\"${d.joinToString(" ")}\" fill=\"none\" stroke=\"$color\" stroke-width=\"${fixed(stroke.width)}\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>\n")
        }
        out.append("</svg>\n")
        return out.toString().toByteArray(Charsets.UTF_8)
    }

    companion object {
        private const val MAX_STROKES = 20000
        private const val MAX_POINTS = 200000
        private val COLOR = Regex("^#[0-9a-fA-F]{6}$")
        private val COMMAND = Regex("([ML])\\s*(-?[0-9]+(?:\\.[0-9]+)?)[\\s,]+(-?[0-9]+(?:\\.[0-9]+)?)")

        fun validColor(color: String): Boolean = COLOR.matches(color)

        private fun fixed(value: Double): String = String.format(Locale.ROOT, "%.1f", value)

        fun fromSvg(svg: ByteArray): Drawing? {
            val text = String(svg, Charsets.UTF_8)
            if (text.contains("<!DOCTYPE", ignoreCase = true) || text.contains("<!ENTITY", ignoreCase = true)) return null
            val document = try {
                val factory = DocumentBuilderFactory.newInstance()
                factory.isNamespaceAware = true
                factory.isExpandEntityReferences = false
                runCatching { factory.setFeature("http://apache.org/xml/features/disallow-doctype-decl", true) }
                val builder = factory.newDocumentBuilder()
                builder.setErrorHandler(object : org.xml.sax.ErrorHandler {
                    override fun warning(exception: org.xml.sax.SAXParseException) {}
                    override fun error(exception: org.xml.sax.SAXParseException) = throw exception
                    override fun fatalError(exception: org.xml.sax.SAXParseException) = throw exception
                })
                builder.parse(ByteArrayInputStream(svg))
            } catch (_: Exception) {
                return null
            }
            val parsed = Drawing()
            var marked = false
            var background = false
            var points = 0
            val elements = document.getElementsByTagName("*")
            for (i in 0 until elements.length) {
                val element = elements.item(i) as? Element ?: continue
                val name = element.localName ?: element.tagName
                when (name) {
                    "svg" -> {
                        marked = element.getAttribute("data-forkmesh-drawing") == "1"
                        parsed.width = (element.getAttribute("width").trim().toIntOrNull() ?: 0).coerceIn(1, 20000)
                        parsed.height = (element.getAttribute("height").trim().toIntOrNull() ?: 0).coerceIn(1, 20000)
                    }
                    "rect" -> if (!background) {
                        background = true
                        val fill = element.getAttribute("fill")
                        if (validColor(fill)) parsed.background = fill
                    }
                    "path" -> {
                        if (parsed.strokes.size >= MAX_STROKES) return null
                        val stroke = Stroke()
                        val color = element.getAttribute("stroke")
                        if (validColor(color)) stroke.color = color
                        element.getAttribute("stroke-width").trim().toDoubleOrNull()?.let { stroke.width = it.coerceIn(0.5, 200.0) }
                        for (match in COMMAND.findAll(element.getAttribute("d"))) {
                            stroke.points.add(Point(match.groupValues[2].toDouble(), match.groupValues[3].toDouble()))
                            if (++points > MAX_POINTS) return null
                        }
                        if (stroke.points.size == 2 && stroke.points[0] == stroke.points[1]) stroke.points.removeAt(1)
                        if (stroke.points.isNotEmpty()) parsed.strokes.add(stroke)
                    }
                }
            }
            return if (marked) parsed else null
        }
    }
}
