package com.forkmesh.nativeapp.control

data class TranscriptTool(val name: String, val detail: String)

enum class TranscriptAlign { Start, Center, End }

enum class TranscriptTaskTone { Done, Failed, Running }

sealed class TranscriptBlock {
    data class Status(val lines: List<String>) : TranscriptBlock()

    data class Error(val text: String) : TranscriptBlock()

    data class Prompt(val text: String) : TranscriptBlock()

    data class Steer(val from: String, val text: String) : TranscriptBlock() {
        val fromYou: Boolean get() = from.startsWith("ForkMesh")
    }

    data class Tools(val calls: List<TranscriptTool>) : TranscriptBlock()

    data class Heading(val level: Int, val text: String) : TranscriptBlock()

    data class Bullet(val marker: String, val depth: Int, val text: String) : TranscriptBlock()

    data class Code(val language: String, val text: String) : TranscriptBlock()

    data class Json(val text: String) : TranscriptBlock()

    data class Table(val header: List<String>, val rows: List<List<String>>, val align: List<TranscriptAlign>) : TranscriptBlock()

    data class Task(val status: String, val summary: String, val body: String) : TranscriptBlock() {
        val tone: TranscriptTaskTone
            get() = when (status) {
                "completed", "done", "success", "succeeded" -> TranscriptTaskTone.Done
                "failed", "killed", "error", "cancelled", "canceled", "stopped", "timeout" -> TranscriptTaskTone.Failed
                else -> TranscriptTaskTone.Running
            }
    }

    data object Rule : TranscriptBlock()

    data class Text(val text: String) : TranscriptBlock()
}

enum class TranscriptInlineStyle { Plain, Bold, Italic, Code, Link }

data class TranscriptInline(val text: String, val style: TranscriptInlineStyle = TranscriptInlineStyle.Plain, val url: String = "")

class TranscriptBlockCache {
    private var parser = AgentTranscriptFormat.Parser()
    private var consumed = ""

    fun update(text: String): List<TranscriptBlock> {
        if (!text.startsWith(consumed)) {
            parser = AgentTranscriptFormat.Parser()
            consumed = ""
        }
        val end = text.lastIndexOf('\n') + 1
        if (end > consumed.length) {
            for (raw in text.substring(consumed.length, end - 1).split('\n')) parser.feed(raw.removeSuffix("\r"))
            consumed = text.substring(0, end)
        }
        val tail = parser.copy()
        if (end < text.length) tail.feed(text.substring(end).removeSuffix("\r"))
        tail.finish()
        return tail.out.toList()
    }
}

object TranscriptJson {
    fun pretty(text: String): String? {
        val source = text.trim()
        if (source.length < 2 || source.length > 200_000 || (source[0] != '{' && source[0] != '[')) return null
        val reader = Reader(source)
        val out = StringBuilder()
        return try {
            reader.value(out, 0, 0)
            reader.space()
            if (reader.index == source.length) out.toString() else null
        } catch (bad: IllegalArgumentException) {
            null
        }
    }

    private class Reader(val s: String) {
        var index = 0

        fun space() {
            while (index < s.length && s[index] in " \t\r\n") index++
        }

        private fun fail(): Nothing = throw IllegalArgumentException("json")

        private fun indent(out: StringBuilder, level: Int) {
            repeat(level) { out.append("  ") }
        }

        fun value(out: StringBuilder, level: Int, depth: Int) {
            if (depth > 64) fail()
            space()
            if (index >= s.length) fail()
            when (s[index]) {
                '{' -> container(out, level, depth, '}', true)
                '[' -> container(out, level, depth, ']', false)
                '"' -> string(out)
                't' -> literal(out, "true")
                'f' -> literal(out, "false")
                'n' -> literal(out, "null")
                else -> number(out)
            }
        }

        private fun container(out: StringBuilder, level: Int, depth: Int, close: Char, keyed: Boolean) {
            index++
            space()
            if (index < s.length && s[index] == close) {
                index++
                out.append(if (keyed) "{}" else "[]")
                return
            }
            out.append(if (keyed) "{\n" else "[\n")
            while (true) {
                indent(out, level + 1)
                if (keyed) {
                    space()
                    if (index >= s.length || s[index] != '"') fail()
                    string(out)
                    space()
                    if (index >= s.length || s[index] != ':') fail()
                    index++
                    out.append(": ")
                }
                value(out, level + 1, depth + 1)
                space()
                if (index >= s.length) fail()
                when (s[index]) {
                    ',' -> {
                        index++
                        out.append(",\n")
                    }
                    close -> {
                        index++
                        out.append('\n')
                        indent(out, level)
                        out.append(close)
                        return
                    }
                    else -> fail()
                }
            }
        }

        private fun string(out: StringBuilder) {
            val start = index
            index++
            while (index < s.length) {
                val c = s[index]
                if (c == '\\') {
                    index += 2
                    continue
                }
                if (c == '\n') fail()
                index++
                if (c == '"') {
                    out.append(s, start, index)
                    return
                }
            }
            fail()
        }

        private fun literal(out: StringBuilder, word: String) {
            if (!s.startsWith(word, index)) fail()
            index += word.length
            out.append(word)
        }

        private fun number(out: StringBuilder) {
            val start = index
            while (index < s.length && (s[index].isDigit() || s[index] in "+-.eE")) index++
            val text = s.substring(start, index)
            if (!NUMBER.matches(text)) fail()
            out.append(text)
        }
    }

    private val NUMBER = Regex("-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?")
}

object AgentTranscriptFormat {
    const val STEER_MARK = "⟵ "

    fun blocks(transcript: String): List<TranscriptBlock> {
        val parser = Parser()
        for (raw in transcript.split('\n')) parser.feed(raw.removeSuffix("\r"))
        parser.finish()
        return parser.out
    }

    fun inline(text: String): List<TranscriptInline> {
        val out = mutableListOf<TranscriptInline>()
        val plain = StringBuilder()
        fun flush() {
            if (plain.isNotEmpty()) out.add(TranscriptInline(plain.toString()))
            plain.clear()
        }
        fun wordChar(index: Int) = index in text.indices && text[index].isLetterOrDigit()
        var i = 0
        while (i < text.length) {
            val c = text[i]
            if (c == '`') {
                val end = text.indexOf('`', i + 1)
                if (end > i + 1) {
                    flush()
                    out.add(TranscriptInline(text.substring(i + 1, end), TranscriptInlineStyle.Code))
                    i = end + 1
                    continue
                }
            }
            if ((c == '*' || c == '_') && i + 1 < text.length && text[i + 1] == c && (c == '*' || !wordChar(i - 1))) {
                val end = text.indexOf("$c$c", i + 2)
                if (end > i + 2 && (c == '*' || !wordChar(end + 2))) {
                    flush()
                    out.add(TranscriptInline(text.substring(i + 2, end), TranscriptInlineStyle.Bold))
                    i = end + 2
                    continue
                }
            }
            if ((c == '*' || c == '_') && i + 1 < text.length && text[i + 1] != ' ' && text[i + 1] != c && !wordChar(i - 1)) {
                val end = text.indexOf(c, i + 1)
                if (end > i + 1 && text[end - 1] != ' ' && !wordChar(end + 1) && (end + 1 >= text.length || text[end + 1] != c)) {
                    flush()
                    out.add(TranscriptInline(text.substring(i + 1, end), TranscriptInlineStyle.Italic))
                    i = end + 1
                    continue
                }
            }
            if (c == '[') {
                val close = text.indexOf("](", i + 1)
                val end = if (close > i + 1) text.indexOf(')', close + 2) else -1
                if (end > 0) {
                    val url = text.substring(close + 2, end)
                    if (url.startsWith("https://") || url.startsWith("http://")) {
                        flush()
                        out.add(TranscriptInline(text.substring(i + 1, close), TranscriptInlineStyle.Link, url))
                        i = end + 1
                        continue
                    }
                }
            }
            if (c == 'h' && (i == 0 || text[i - 1].isWhitespace() || text[i - 1] == '(') &&
                (text.startsWith("https://", i) || text.startsWith("http://", i))
            ) {
                var end = i
                while (end < text.length && !text[end].isWhitespace() && text[end] !in "<>\"'`") end++
                while (end > i && text[end - 1] in ".,;:!?)") end--
                val url = text.substring(i, end)
                if (url.length > "https://".length) {
                    flush()
                    out.add(TranscriptInline(url, TranscriptInlineStyle.Link, url))
                    i = end
                    continue
                }
            }
            plain.append(c)
            i++
        }
        flush()
        return out
    }

    internal fun tool(line: String): Pair<String, String>? {
        if (line.length <= 3 || !line.startsWith("[") || !line.endsWith("]")) return null
        val inner = line.substring(1, line.length - 1)
        val space = inner.indexOf(' ')
        val name = if (space < 0) inner else inner.substring(0, space)
        val detail = if (space < 0) "" else inner.substring(space + 1).trim()
        if (name.length < 2 || name.length > 80 || !name[0].isLetter()) return null
        val allowed = name.all { it.isLetterOrDigit() || it == '_' || it == '.' || it == ':' || it == '-' }
        return if (allowed) name to detail else null
    }

    internal const val TOOL_SPAN = 300

    internal fun toolOpening(line: String): Boolean {
        if (!line.startsWith("[") || line.endsWith("]") || line.contains("](")) return false
        val space = line.indexOf(' ')
        return space > 0 && tool(line.substring(0, space) + "]") != null
    }

    internal fun heading(line: String): Pair<Int, String>? {
        val hashes = line.takeWhile { it == '#' }.length
        if (hashes < 1 || hashes > 6) return null
        val rest = line.substring(hashes)
        if (!rest.startsWith(" ")) return null
        val text = rest.trim()
        return if (text.isEmpty()) null else hashes to text
    }

    internal fun bullet(line: String): Triple<String, Int, String>? {
        var indent = 0
        var index = 0
        while (index < line.length && (line[index] == ' ' || line[index] == '\t')) {
            indent += if (line[index] == '\t') 4 else 1
            index++
        }
        val body = line.substring(index)
        if (body.length >= 2 && body[0] in "-*+" && body[1] == ' ') {
            val text = body.substring(2).trim()
            return if (text.isEmpty()) null else Triple("•", indent / 2, text)
        }
        val digits = body.takeWhile { it in '0'..'9' }
        if (digits.isEmpty() || digits.length > 3) return null
        val after = body.substring(digits.length)
        if (after.length < 2 || (after[0] != '.' && after[0] != ')') || after[1] != ' ') return null
        val text = after.substring(2).trim()
        return if (text.isEmpty()) null else Triple("$digits.", indent / 2, text)
    }

    internal fun isRule(line: String): Boolean {
        val bare = line.filter { it != ' ' }
        if (bare.length < 3 || bare[0] !in "-*_") return false
        return bare.all { it == bare[0] }
    }

    internal fun task(text: String): Pair<String, String>? {
        if (!text.startsWith("Task ")) return null
        val colon = text.indexOf(": ", 5)
        if (colon < 0) return null
        val status = text.substring(5, colon)
        if (status.isEmpty() || status.length > 20 || status == "cost" || !status.all { it in 'a'..'z' || it == '_' }) return null
        val summary = text.substring(colon + 2).trim()
        return if (summary.isEmpty()) null else status to summary
    }

    internal fun cells(line: String): List<String> {
        var body = line.trim()
        if (body.startsWith("|")) body = body.substring(1)
        if (body.endsWith("|") && !body.endsWith("\\|")) body = body.substring(0, body.length - 1)
        val out = mutableListOf<String>()
        val cell = StringBuilder()
        var code = false
        var i = 0
        while (i < body.length) {
            val c = body[i]
            if (c == '\\' && i + 1 < body.length && body[i + 1] == '|') {
                cell.append('|')
                i += 2
                continue
            }
            if (c == '`') code = !code
            if (c == '|' && !code) {
                out.add(cell.toString().trim())
                cell.clear()
            } else {
                cell.append(c)
            }
            i++
        }
        out.add(cell.toString().trim())
        return out
    }

    internal fun table(lines: List<String>): TranscriptBlock.Table? {
        if (lines.size < 2) return null
        val separator = cells(lines[1])
        if (separator.isEmpty() || !separator.all { SEPARATOR.matches(it) }) return null
        val header = cells(lines[0])
        val width = header.size
        if (width == 0 || separator.size != width) return null
        val align = separator.map {
            when {
                it.startsWith(":") && it.endsWith(":") -> TranscriptAlign.Center
                it.endsWith(":") -> TranscriptAlign.End
                else -> TranscriptAlign.Start
            }
        }
        val rows = lines.drop(2).map { line ->
            val row = cells(line)
            List(width) { row.getOrElse(it) { "" } }
        }
        return TranscriptBlock.Table(header, rows, align)
    }

    private val SEPARATOR = Regex(":?-{1,}:?")

    internal class TaskDraft(val status: String, val summary: String) {
        val lines = mutableListOf<String>()
        var blanks = 0

        fun copy() = TaskDraft(status, summary).also {
            it.lines.addAll(lines)
            it.blanks = blanks
        }
    }

    class Parser {
        val out = mutableListOf<TranscriptBlock>()
        private val status = mutableListOf<String>()
        private val paragraph = mutableListOf<String>()
        private var prompt: MutableList<String>? = null
        private var fence: MutableList<String>? = null
        private var fenceLanguage = ""
        private val table = mutableListOf<String>()
        private val tools = mutableListOf<TranscriptTool>()
        private var task: TaskDraft? = null
        private var openTool: MutableList<String>? = null
        private var openToolClose: Int? = null

        fun copy(): Parser {
            val other = Parser()
            other.out.addAll(out)
            other.status.addAll(status)
            other.paragraph.addAll(paragraph)
            other.prompt = prompt?.toMutableList()
            other.fence = fence?.toMutableList()
            other.fenceLanguage = fenceLanguage
            other.table.addAll(table)
            other.tools.addAll(tools)
            other.task = task?.copy()
            other.openTool = openTool?.toMutableList()
            other.openToolClose = openToolClose
            return other
        }

        private fun flushParagraph() {
            if (paragraph.isNotEmpty()) {
                val text = paragraph.joinToString("\n")
                val json = if (text.trimStart().let { it.startsWith("{") || it.startsWith("[") }) TranscriptJson.pretty(text) else null
                out.add(if (json != null) TranscriptBlock.Json(json) else TranscriptBlock.Text(text))
            }
            paragraph.clear()
        }

        private fun flushTable() {
            if (table.isNotEmpty()) out.add(table(table) ?: TranscriptBlock.Code("", table.joinToString("\n")))
            table.clear()
        }

        private fun flushText() {
            flushParagraph()
            flushTable()
            prompt?.let { lines ->
                val text = lines.joinToString("\n").trim()
                if (text.startsWith(STEER_MARK) && text.indexOf(": ") > STEER_MARK.length) {
                    val colon = text.indexOf(": ")
                    out.add(TranscriptBlock.Steer(text.substring(STEER_MARK.length, colon).trim(), text.substring(colon + 2).trim()))
                } else if (text.isNotEmpty()) {
                    out.add(TranscriptBlock.Prompt(text))
                }
            }
            prompt = null
        }

        private fun flushStatus() {
            if (status.isNotEmpty()) out.add(TranscriptBlock.Status(status.toList()))
            status.clear()
        }

        private fun flushTools() {
            if (tools.isNotEmpty()) out.add(TranscriptBlock.Tools(tools.toList()))
            tools.clear()
        }

        private fun flushTask() {
            task?.let { out.add(TranscriptBlock.Task(it.status, it.summary, it.lines.joinToString("\n").trimEnd())) }
            task = null
        }

        private fun flushAll() {
            flushText()
            flushStatus()
            flushTools()
            flushTask()
        }

        private fun emit(block: TranscriptBlock) {
            flushAll()
            out.add(block)
        }

        private fun code(language: String, lines: List<String>): TranscriptBlock {
            val text = lines.joinToString("\n")
            val lower = language.lowercase()
            if (lower == "json" || lower == "jsonc" || lower.isEmpty()) {
                TranscriptJson.pretty(text)?.let { return TranscriptBlock.Json(it) }
            }
            return TranscriptBlock.Code(language, text)
        }

        private fun settleTool() {
            val lines = openTool ?: return
            val close = openToolClose
            openTool = null
            openToolClose = null
            val found = close?.let { tool(lines.subList(0, it + 1).joinToString("\n").trim(' ', '\t')) }
            if (close != null && found != null) {
                tools.add(TranscriptTool(found.first, found.second))
                lines.drop(close + 1).forEach { feed(it) }
                return
            }
            feed(lines[0], opening = false)
            lines.drop(1).forEach { feed(it) }
        }

        private fun continueTool(line: String, trimmed: String) {
            val lines = openTool ?: return
            if (trimmed.isEmpty() && openToolClose != null) {
                settleTool()
                feed(line)
                return
            }
            lines.add(line)
            if (trimmed.endsWith("]")) {
                openToolClose = lines.size - 1
                if (trimmed.endsWith("…]")) {
                    settleTool()
                    return
                }
            }
            if (lines.sumOf { it.length + 1 } > TOOL_SPAN) settleTool()
        }

        fun feed(line: String, opening: Boolean = true) {
            val trimmed = line.trim(' ', '\t')
            if (openTool != null) {
                continueTool(line, trimmed)
                return
            }
            fence?.let { lines ->
                if (trimmed.startsWith("```")) {
                    fence = null
                    out.add(code(fenceLanguage, lines))
                } else {
                    lines.add(line)
                }
                return
            }
            task?.let { draft ->
                if (line.startsWith("    ")) {
                    repeat(draft.blanks) { draft.lines.add("") }
                    draft.blanks = 0
                    draft.lines.add(line.substring(4))
                    return
                }
                if (trimmed.isEmpty()) {
                    draft.blanks++
                    return
                }
                flushTask()
            }
            if (trimmed.isEmpty()) {
                flushText()
                return
            }
            prompt?.let { lines ->
                if (!line.startsWith("==>") && !line.startsWith("!!") && tool(trimmed) == null) {
                    lines.add(line)
                    return
                }
                flushText()
            }
            if (trimmed.startsWith("```")) {
                flushAll()
                fence = mutableListOf()
                fenceLanguage = trimmed.substring(3).trim()
                return
            }
            if (line.startsWith("==>")) {
                val text = line.substring(3).trim()
                flushText()
                flushTools()
                val found = task(text)
                if (found != null) {
                    flushAll()
                    task = TaskDraft(found.first, found.second)
                } else if (text == "User steering prompt") {
                    flushStatus()
                    prompt = mutableListOf()
                } else if (text.isNotEmpty()) {
                    status.add(text)
                }
                return
            }
            if (line.startsWith("!!")) {
                emit(TranscriptBlock.Error(line.substring(2).trim()))
                return
            }
            if (line.startsWith(">") && paragraph.isEmpty() && table.isEmpty()) {
                flushAll()
                prompt = mutableListOf(line.substring(1).trim())
                return
            }
            tool(trimmed)?.let { (name, detail) ->
                flushText()
                flushStatus()
                tools.add(TranscriptTool(name, detail))
                return
            }
            if (opening && paragraph.isEmpty() && table.isEmpty() && toolOpening(trimmed)) {
                flushText()
                flushStatus()
                openTool = mutableListOf(line)
                return
            }
            if (trimmed.startsWith("|")) {
                flushParagraph()
                flushStatus()
                flushTools()
                table.add(trimmed)
                return
            }
            if (table.isNotEmpty()) flushText()
            heading(trimmed)?.let { (level, text) ->
                emit(TranscriptBlock.Heading(level, text))
                return
            }
            if (isRule(trimmed)) {
                emit(TranscriptBlock.Rule)
                return
            }
            bullet(line)?.let { (marker, depth, text) ->
                emit(TranscriptBlock.Bullet(marker, depth, text))
                return
            }
            flushStatus()
            flushTools()
            paragraph.add(line)
        }

        fun finish() {
            while (openTool != null) settleTool()
            fence?.let { lines ->
                out.add(code(fenceLanguage, lines))
                fence = null
            }
            flushAll()
        }
    }
}
