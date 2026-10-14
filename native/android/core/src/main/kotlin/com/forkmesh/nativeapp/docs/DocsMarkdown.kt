package com.forkmesh.nativeapp.docs

enum class DocsAlign { Start, Center, End }

sealed class DocsBlock {
    data class Heading(val level: Int, val text: String, val anchor: String) : DocsBlock()
    data class Paragraph(val text: String) : DocsBlock()
    data class ListItem(val depth: Int, val marker: String, val text: String, val checked: Boolean? = null) : DocsBlock() {
        val ordered: Boolean get() = marker.firstOrNull()?.isDigit() == true
        val continuation: Boolean get() = marker.isEmpty()
    }
    data class Quote(val blocks: List<DocsBlock>) : DocsBlock()
    data class Code(val language: String, val text: String) : DocsBlock()
    data class Table(val header: List<String>, val rows: List<List<String>>, val align: List<DocsAlign>) : DocsBlock()
    data class Anchor(val id: String) : DocsBlock()
    data object Rule : DocsBlock()
}

data class DocsDocument(val blocks: List<DocsBlock>, val references: Map<String, String>) {
    fun anchorIndex(fragment: String): Int = DocsMarkdown.anchorIndex(blocks, fragment)
}

data class DocsSpan(
    val text: String,
    val bold: Boolean = false,
    val italic: Boolean = false,
    val code: Boolean = false,
    val strike: Boolean = false,
    val href: String? = null,
)

object DocsMarkdown {
    private val HEADING = Regex("^(#{1,6})(?:\\s+(.*?))?\\s*$")
    private val CLOSING_HASHES = Regex("\\s+#+$")
    private val RULE = Regex("^([-*_])(\\s*\\1){2,}$")
    private val ITEM = Regex("^(\\s*)([-*+]|\\d{1,9}[.)])(?:\\s+(.*))?$")
    private val TASK = Regex("^\\[([ xX])]\\s+(.*)$")
    private val TABLE_SEPARATOR = Regex("^\\|?\\s*:?-+:?\\s*(\\|\\s*:?-+:?\\s*)*\\|?$")
    private val HTML_ANCHOR = Regex("^<a\\s+(?:id|name)\\s*=\\s*\"([^\"]+)\"\\s*/?>\\s*(?:</a>)?$", RegexOption.IGNORE_CASE)
    private val REFERENCE = Regex("^\\s{0,3}\\[([^\\]]+)]:\\s*<?([^\\s>]+)>?(?:\\s+.*)?$")
    private val QUOTE_PREFIX = Regex("^\\s*>\\s?")
    private val TAG = Regex("^</?([A-Za-z][A-Za-z0-9-]*)(?:\\s[^<>]*)?/?>")
    private val HTML_LINE = Regex("</?([A-Za-z][A-Za-z0-9-]*)(?:\\s[^<>]*)?/?>\\s*")
    private val HTML_TAGS = setOf(
        "a", "abbr", "b", "br", "button", "center", "code", "dd", "del", "details", "div", "dl", "dt", "em", "figure",
        "figcaption", "font", "h1", "h2", "h3", "h4", "h5", "h6", "hr", "i", "img", "input", "ins", "kbd", "li", "mark",
        "ol", "p", "picture", "pre", "q", "s", "samp", "section", "small", "source", "span", "strike", "strong", "sub",
        "summary", "sup", "table", "tbody", "td", "th", "thead", "tr", "u", "ul", "var", "video", "wbr",
    )
    private val AUTOLINK = Regex("^<((?:https?://|mailto:)[^\\s<>]+)>")
    private val ENTITY = Regex("^&(#\\d{1,6}|#x[0-9a-fA-F]{1,6}|[a-zA-Z]{2,8});")
    private val ENTITIES = mapOf("amp" to "&", "lt" to "<", "gt" to ">", "quot" to "\"", "apos" to "'", "nbsp" to "\u00a0", "mdash" to "\u2014", "ndash" to "\u2013", "hellip" to "\u2026", "copy" to "\u00a9", "rarr" to "\u2192", "larr" to "\u2190")
    private const val PUNCTUATION = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"

    fun blocks(markdown: String): List<DocsBlock> = parse(markdown).blocks

    fun parse(markdown: String): DocsDocument {
        val references = LinkedHashMap<String, String>()
        val lines = clean(markdown, references)
        val anchors = HashMap<String, Int>()
        return DocsDocument(BlockParser(lines, anchors).run(), references)
    }

    fun headingAnchor(text: String): String {
        val out = StringBuilder()
        for (c in text.trim().lowercase()) {
            when {
                c.isLetterOrDigit() || c == '_' || c == '-' -> out.append(c)
                c.isWhitespace() -> out.append('-')
            }
        }
        return out.toString()
    }

    fun anchorIndex(blocks: List<DocsBlock>, fragment: String): Int {
        if (fragment.isEmpty()) return -1
        val exact = blocks.indexOfFirst { (it is DocsBlock.Heading && it.anchor == fragment) || (it is DocsBlock.Anchor && it.id == fragment) }
        if (exact >= 0) return exact
        val lowered = fragment.lowercase()
        return blocks.indexOfFirst {
            (it is DocsBlock.Heading && it.anchor == lowered) || (it is DocsBlock.Anchor && it.id.lowercase() == lowered) ||
                (it is DocsBlock.Heading && it.anchor == headingAnchor(fragment))
        }
    }

    fun inline(text: String, references: Map<String, String> = emptyMap()): List<DocsSpan> {
        val spans = ArrayList<DocsSpan>()
        InlineParser(references, spans).parse(text, Style())
        return merge(spans)
    }

    fun plain(text: String, references: Map<String, String> = emptyMap()): String =
        DocsLibrary.collapse(inline(text, references).joinToString("") { it.text })

    private fun clean(markdown: String, references: MutableMap<String, String>): List<String> {
        val out = ArrayList<String>()
        var fence: String? = null
        var comment = false
        for (raw in markdown.replace("\r\n", "\n").replace('\r', '\n').split('\n')) {
            val line = raw.replace("\t", "    ")
            if (fence != null) {
                out += line
                if (closesFence(line.trim(), fence)) fence = null
                continue
            }
            var text = line
            if (comment) {
                val end = text.indexOf("-->")
                if (end < 0) continue
                text = text.substring(end + 3)
                comment = false
                if (text.isBlank()) continue
            }
            while (true) {
                val start = text.indexOf("<!--")
                if (start < 0) break
                val end = text.indexOf("-->", start + 4)
                if (end < 0) {
                    text = text.substring(0, start)
                    comment = true
                    break
                }
                text = text.substring(0, start) + text.substring(end + 3)
            }
            if (comment && text.isBlank()) continue
            if (text.isBlank() && line.contains("<!--")) continue
            val reference = REFERENCE.matchEntire(text)
            if (reference != null) {
                references.putIfAbsent(reference.groupValues[1].trim().lowercase(), reference.groupValues[2])
                continue
            }
            out += text
            openingFence(text.trim())?.let { fence = it }
        }
        return out
    }

    private fun openingFence(trimmed: String): String? {
        val c = trimmed.firstOrNull() ?: return null
        if (c != '`' && c != '~') return null
        val run = trimmed.takeWhile { it == c }
        if (run.length < 3) return null
        if (c == '`' && trimmed.substring(run.length).contains('`')) return null
        return run
    }

    private fun closesFence(trimmed: String, fence: String): Boolean {
        val c = fence[0]
        val run = trimmed.takeWhile { it == c }
        return run.length >= fence.length && trimmed.substring(run.length).isBlank()
    }

    private fun htmlOnly(trimmed: String): Boolean {
        if (!trimmed.startsWith("<") || !trimmed.endsWith(">")) return false
        val tags = HTML_LINE.findAll(trimmed).toList()
        if (tags.isEmpty() || tags.sumOf { it.value.length } != trimmed.length) return false
        return tags.all { it.groupValues[1].lowercase() in HTML_TAGS }
    }

    private fun indentOf(line: String): Int = line.length - line.trimStart().length

    private class BlockParser(val lines: List<String>, val anchors: MutableMap<String, Int>) {
        val out = ArrayList<DocsBlock>()
        val paragraph = ArrayList<String>()
        val listIndents = ArrayList<Int>()
        var item: DocsBlock.ListItem? = null
        val itemLines = ArrayList<String>()
        var blankBefore = false

        fun run(): List<DocsBlock> {
            var i = 0
            while (i < lines.size) {
                val line = lines[i]
                val trimmed = line.trim()
                val indent = indentOf(line)
                if (trimmed.isEmpty()) {
                    flushParagraph()
                    flushItem()
                    blankBefore = true
                    i++
                    continue
                }
                val fence = openingFence(trimmed)
                if (fence != null) {
                    flushParagraph()
                    flushItem()
                    if (indent == 0 && listIndents.isNotEmpty() && blankBefore) listIndents.clear()
                    val language = trimmed.substring(fence.length).trim().split(' ').firstOrNull().orEmpty()
                    val body = ArrayList<String>()
                    i++
                    while (i < lines.size && !closesFence(lines[i].trim(), fence)) {
                        val content = lines[i]
                        val strip = minOf(indent, indentOf(content))
                        body += content.substring(strip)
                        i++
                    }
                    i++
                    out += DocsBlock.Code(language, body.joinToString("\n"))
                    blankBefore = false
                    continue
                }
                val heading = if (indent < 4) HEADING.matchEntire(trimmed) else null
                if (heading != null) {
                    endBlocks()
                    val level = heading.groupValues[1].length
                    val text = CLOSING_HASHES.replace(heading.groupValues[2], "").let { if (it.all { c -> c == '#' }) "" else it }
                    out += DocsBlock.Heading(level, text, anchorFor(plain(text)))
                    i++
                    continue
                }
                if (indent < 4 && RULE.matches(trimmed)) {
                    endBlocks()
                    out += DocsBlock.Rule
                    i++
                    continue
                }
                val htmlAnchor = HTML_ANCHOR.matchEntire(trimmed)
                if (htmlAnchor != null) {
                    flushParagraph()
                    flushItem()
                    out += DocsBlock.Anchor(htmlAnchor.groupValues[1])
                    i++
                    continue
                }
                if (htmlOnly(trimmed)) {
                    flushParagraph()
                    flushItem()
                    i++
                    continue
                }
                if (trimmed.startsWith(">")) {
                    endBlocks()
                    val quoted = ArrayList<String>()
                    while (i < lines.size && lines[i].trim().startsWith(">")) {
                        quoted += QUOTE_PREFIX.replaceFirst(lines[i], "")
                        i++
                    }
                    out += DocsBlock.Quote(BlockParser(quoted, anchors).run())
                    continue
                }
                if (trimmed.contains('|') && i + 1 < lines.size && TABLE_SEPARATOR.matches(lines[i + 1].trim()) &&
                    lines[i + 1].contains('|')
                ) {
                    endBlocks()
                    val header = cells(trimmed)
                    val align = cells(lines[i + 1].trim()).map {
                        val cell = it.trim()
                        when {
                            cell.startsWith(":") && cell.endsWith(":") -> DocsAlign.Center
                            cell.endsWith(":") -> DocsAlign.End
                            else -> DocsAlign.Start
                        }
                    }
                    i += 2
                    val rows = ArrayList<List<String>>()
                    while (i < lines.size && lines[i].trim().isNotEmpty() && lines[i].contains('|')) {
                        val row = cells(lines[i].trim())
                        rows += List(header.size) { row.getOrElse(it) { "" } }
                        i++
                    }
                    out += DocsBlock.Table(header, rows, List(header.size) { align.getOrElse(it) { DocsAlign.Start } })
                    continue
                }
                val marker = ITEM.matchEntire(line)
                if (marker != null && !(paragraph.isNotEmpty() && listIndents.isEmpty() && marker.groupValues[3].isEmpty())) {
                    flushParagraph()
                    flushItem()
                    val depth = depthFor(indent)
                    val token = marker.groupValues[2]
                    var text = marker.groupValues[3]
                    var checked: Boolean? = null
                    TASK.matchEntire(text)?.let {
                        checked = it.groupValues[1] != " "
                        text = it.groupValues[2]
                    }
                    item = DocsBlock.ListItem(depth, if (token[0].isDigit()) token.dropLast(1) + "." else "\u2022", "", checked)
                    itemLines += text
                    blankBefore = false
                    i++
                    continue
                }
                if (item != null) {
                    itemLines += trimmed
                    i++
                    continue
                }
                if (paragraph.isEmpty() && listIndents.isNotEmpty()) {
                    if (blankBefore && indent >= 2) {
                        item = DocsBlock.ListItem(contentDepth(indent), "", "")
                        itemLines += trimmed
                        blankBefore = false
                        i++
                        continue
                    }
                    listIndents.clear()
                }
                paragraph += line
                blankBefore = false
                i++
            }
            flushParagraph()
            flushItem()
            return out
        }

        fun contentDepth(indent: Int): Int {
            var depth = 0
            for ((index, start) in listIndents.withIndex()) if (indent > start) depth = index
            return depth
        }

        fun depthFor(indent: Int): Int {
            while (listIndents.isNotEmpty() && listIndents.last() > indent + 1) listIndents.removeAt(listIndents.lastIndex)
            if (listIndents.isEmpty() || indent >= listIndents.last() + 2) listIndents += indent
            return listIndents.size - 1
        }

        fun endBlocks() {
            flushParagraph()
            flushItem()
            listIndents.clear()
            blankBefore = false
        }

        fun anchorFor(text: String): String {
            val base = headingAnchor(text)
            val count = anchors[base] ?: 0
            anchors[base] = count + 1
            return if (count == 0) base else "$base-$count"
        }

        fun flushParagraph() {
            if (paragraph.isEmpty()) return
            val text = joinLines(paragraph)
            paragraph.clear()
            if ('<' in text && plain(text).isBlank()) return
            out += DocsBlock.Paragraph(text)
        }

        fun flushItem() {
            val current = item ?: return
            out += current.copy(text = joinLines(itemLines))
            item = null
            itemLines.clear()
        }
    }

    private fun joinLines(lines: List<String>): String {
        val out = StringBuilder()
        for ((index, line) in lines.withIndex()) {
            val hard = line.endsWith("  ") || (line.endsWith("\\") && !line.endsWith("\\\\"))
            var text = line.trim()
            if (hard && text.endsWith("\\")) text = text.dropLast(1)
            out.append(text)
            if (index < lines.lastIndex) out.append(if (hard) "\n" else " ")
        }
        return out.toString()
    }

    private fun cells(row: String): List<String> {
        val cells = ArrayList<String>()
        val current = StringBuilder()
        var code = false
        var i = 0
        while (i < row.length) {
            val c = row[i]
            when {
                c == '\\' && i + 1 < row.length && row[i + 1] == '|' -> {
                    current.append('|')
                    i++
                }
                c == '`' -> {
                    code = !code
                    current.append(c)
                }
                c == '|' && !code -> {
                    cells += current.toString().trim()
                    current.clear()
                }
                else -> current.append(c)
            }
            i++
        }
        cells += current.toString().trim()
        if (row.startsWith("|") && cells.isNotEmpty()) cells.removeAt(0)
        if (row.endsWith("|") && !row.endsWith("\\|") && cells.isNotEmpty()) cells.removeAt(cells.lastIndex)
        return cells
    }

    private data class Style(
        val bold: Boolean = false,
        val italic: Boolean = false,
        val code: Boolean = false,
        val strike: Boolean = false,
        val href: String? = null,
    )

    private class InlineParser(val references: Map<String, String>, val spans: MutableList<DocsSpan>) {
        fun emit(text: String, style: Style) {
            if (text.isEmpty()) return
            spans += DocsSpan(text, style.bold, style.italic, style.code, style.strike, style.href)
        }

        fun parse(text: String, style: Style) {
            val buffer = StringBuilder()
            fun flush() {
                emit(buffer.toString(), style)
                buffer.clear()
            }
            var i = 0
            while (i < text.length) {
                val c = text[i]
                when {
                    c == '\\' && i + 1 < text.length && text[i + 1] in PUNCTUATION -> {
                        buffer.append(text[i + 1])
                        i += 2
                    }
                    c == '`' -> {
                        val run = text.substring(i).takeWhile { it == '`' }.length
                        val close = closingTicks(text, i + run, run)
                        if (close < 0) {
                            buffer.append(text, i, i + run)
                            i += run
                        } else {
                            flush()
                            var code = text.substring(i + run, close)
                            if (code.length >= 2 && code.startsWith(" ") && code.endsWith(" ") && code.isNotBlank()) code = code.substring(1, code.length - 1)
                            emit(code, style.copy(code = true))
                            i = close + run
                        }
                    }
                    c == '!' && i + 1 < text.length && text[i + 1] == '[' -> {
                        val link = link(text, i + 1)
                        if (link == null) {
                            buffer.append(c)
                            i++
                        } else {
                            flush()
                            val label = plainOf(link.label).ifEmpty { "image" }
                            emit(label, style.copy(href = link.href ?: style.href))
                            i = link.end
                        }
                    }
                    c == '[' -> {
                        val link = link(text, i)
                        if (link?.href == null) {
                            buffer.append(c)
                            i++
                        } else {
                            flush()
                            parse(link.label, style.copy(href = link.href))
                            i = link.end
                        }
                    }
                    c == '<' -> {
                        val rest = text.substring(i)
                        val auto = AUTOLINK.find(rest)
                        val tag = TAG.find(rest)
                        when {
                            auto != null -> {
                                flush()
                                emit(auto.groupValues[1].removePrefix("mailto:"), style.copy(href = style.href ?: auto.groupValues[1]))
                                i += auto.value.length
                            }
                            tag != null && tag.groupValues[1].lowercase() in HTML_TAGS -> {
                                if (tag.groupValues[1].lowercase() == "br") buffer.append('\n')
                                i += tag.value.length
                            }
                            else -> {
                                buffer.append(c)
                                i++
                            }
                        }
                    }
                    c == '&' -> {
                        val entity = ENTITY.find(text.substring(i))
                        val decoded = entity?.let { decodeEntity(it.groupValues[1]) }
                        if (entity != null && decoded != null) {
                            buffer.append(decoded)
                            i += entity.value.length
                        } else {
                            buffer.append(c)
                            i++
                        }
                    }
                    (c == 'h' && style.href == null && (text.startsWith("https://", i) || text.startsWith("http://", i)) &&
                        (i == 0 || !text[i - 1].isLetterOrDigit())) -> {
                        var end = i
                        while (end < text.length && !text[end].isWhitespace() && text[end] != '<') end++
                        var url = text.substring(i, end)
                        while (url.isNotEmpty() && (url.last() in ".,;:!?'\"" || (url.last() == ')' && url.count { it == '(' } < url.count { it == ')' }))) {
                            url = url.dropLast(1)
                        }
                        if (url.length <= "https://".length) {
                            buffer.append(c)
                            i++
                        } else {
                            flush()
                            emit(url, style.copy(href = url))
                            i += url.length
                        }
                    }
                    c == '*' || c == '_' || c == '~' -> {
                        val run = text.substring(i).takeWhile { it == c }.length
                        val consumed = emphasis(text, i, c, run, style, ::flush)
                        if (consumed > 0) {
                            i = consumed
                        } else {
                            buffer.append(text, i, i + run)
                            i += run
                        }
                    }
                    else -> {
                        buffer.append(c)
                        i++
                    }
                }
            }
            flush()
        }

        fun emphasis(text: String, at: Int, c: Char, run: Int, style: Style, flush: () -> Unit): Int {
            if (c == '~' && run != 2) return 0
            val size = minOf(run, 3)
            if (run > 3) return 0
            val after = text.getOrNull(at + size) ?: return 0
            if (after.isWhitespace()) return 0
            if (c == '_' && at > 0 && text[at - 1].isLetterOrDigit()) return 0
            val delimiter = c.toString().repeat(size)
            var j = at + size
            while (j < text.length) {
                val ch = text[j]
                if (ch == '\\') {
                    j += 2
                    continue
                }
                if (ch == '`') {
                    val ticks = text.substring(j).takeWhile { it == '`' }.length
                    val close = closingTicks(text, j + ticks, ticks)
                    j = if (close < 0) j + ticks else close + ticks
                    continue
                }
                if (ch == c) {
                    val closing = text.substring(j).takeWhile { it == c }.length
                    val before = text[j - 1]
                    val next = text.getOrNull(j + closing)
                    if (closing == size && !before.isWhitespace() && j > at + size &&
                        (c != '_' || next == null || !next.isLetterOrDigit())
                    ) {
                        flush()
                        val inner = text.substring(at + size, j)
                        val styled = when {
                            c == '~' -> style.copy(strike = true)
                            size == 3 -> style.copy(bold = true, italic = true)
                            size == 2 -> style.copy(bold = true)
                            else -> style.copy(italic = true)
                        }
                        parse(inner, styled)
                        return j + size
                    }
                    j += closing
                    continue
                }
                j++
            }
            return 0
        }

        fun plainOf(text: String): String {
            val nested = ArrayList<DocsSpan>()
            InlineParser(references, nested).parse(text, Style())
            return nested.joinToString("") { it.text }
        }

        fun closingTicks(text: String, from: Int, run: Int): Int {
            var j = from
            while (j < text.length) {
                if (text[j] == '`') {
                    val count = text.substring(j).takeWhile { it == '`' }.length
                    if (count == run) return j
                    j += count
                } else {
                    j++
                }
            }
            return -1
        }

        fun link(text: String, open: Int): Link? {
            var depth = 0
            var j = open
            var close = -1
            while (j < text.length) {
                val ch = text[j]
                when {
                    ch == '\\' -> j++
                    ch == '`' -> {
                        val ticks = text.substring(j).takeWhile { it == '`' }.length
                        val end = closingTicks(text, j + ticks, ticks)
                        if (end >= 0) j = end + ticks - 1 else j += ticks - 1
                    }
                    ch == '[' -> depth++
                    ch == ']' -> {
                        depth--
                        if (depth == 0) {
                            close = j
                            break
                        }
                    }
                }
                j++
            }
            if (close < 0) return null
            val label = text.substring(open + 1, close)
            val next = text.getOrNull(close + 1)
            if (next == '(') {
                var parens = 0
                var k = close + 1
                var end = -1
                while (k < text.length) {
                    val ch = text[k]
                    if (ch == '\\') {
                        k += 2
                        continue
                    }
                    if (ch == '(') parens++
                    if (ch == ')') {
                        parens--
                        if (parens == 0) {
                            end = k
                            break
                        }
                    }
                    k++
                }
                if (end < 0) return null
                val inside = text.substring(close + 2, end).trim()
                val href = if (inside.startsWith("<")) inside.substring(1).substringBefore('>') else inside.split(' ', '\t').first()
                return Link(label, href, end + 1)
            }
            if (next == '[') {
                val refEnd = text.indexOf(']', close + 2)
                if (refEnd >= 0) {
                    val key = text.substring(close + 2, refEnd).ifEmpty { label }.trim().lowercase()
                    references[key]?.let { return Link(label, it, refEnd + 1) }
                }
            }
            references[label.trim().lowercase()]?.let { return Link(label, it, close + 1) }
            return Link(label, null, close + 1)
        }
    }

    private data class Link(val label: String, val href: String?, val end: Int)

    private fun decodeEntity(name: String): String? = when {
        name.startsWith("#x") || name.startsWith("#X") -> name.substring(2).toIntOrNull(16)?.let(::codePoint)
        name.startsWith("#") -> name.substring(1).toIntOrNull()?.let(::codePoint)
        else -> ENTITIES[name]
    }

    private fun codePoint(value: Int): String? =
        if (value in 1..0x10FFFF && value !in 0xD800..0xDFFF) String(Character.toChars(value)) else null

    private fun merge(spans: List<DocsSpan>): List<DocsSpan> {
        val out = ArrayList<DocsSpan>()
        for (span in spans) {
            val last = out.lastOrNull()
            if (last != null && last.copy(text = "") == span.copy(text = "")) {
                out[out.lastIndex] = last.copy(text = last.text + span.text)
            } else {
                out += span
            }
        }
        return out
    }
}
