package com.forkmesh.nativeapp.notes

import com.forkmesh.nativeapp.docs.DocsAlign
import com.forkmesh.nativeapp.docs.DocsBlock
import com.forkmesh.nativeapp.docs.DocsMarkdown
import com.forkmesh.nativeapp.docs.DocsSpan
import java.time.LocalDate
import java.time.format.DateTimeFormatter
import java.util.Base64
import java.util.Locale

enum class NotePaper(val key: String, val label: String, val widthMm: Double, val heightMm: Double) {
    A4("a4", "A4", 210.0, 297.0),
    Letter("letter", "Letter", 215.9, 279.4),
    Legal("legal", "Legal", 215.9, 355.6),
}

enum class NoteOrientation(val key: String, val label: String) {
    Portrait("portrait", "Portrait"),
    Landscape("landscape", "Landscape"),
}

enum class NoteMargins(val key: String, val label: String, val mm: Double) {
    Narrow("narrow", "Narrow", 12.7),
    Normal("normal", "Normal", 25.4),
    Wide("wide", "Wide", 38.1),
}

data class NoteLayout(
    val paper: NotePaper = NotePaper.A4,
    val orientation: NoteOrientation = NoteOrientation.Portrait,
    val margins: NoteMargins = NoteMargins.Normal,
    val cover: Boolean = false,
    val contents: Boolean = false,
    val numbers: Boolean = true,
) {
    val pageWidthMm: Double get() = if (orientation == NoteOrientation.Landscape) paper.heightMm else paper.widthMm
    val pageHeightMm: Double get() = if (orientation == NoteOrientation.Landscape) paper.widthMm else paper.heightMm

    fun line(): String {
        fun flag(on: Boolean) = if (on) "yes" else "no"
        return "<!-- page: size=${paper.key}; orientation=${orientation.key}; margins=${margins.key}; " +
            "cover=${flag(cover)}; contents=${flag(contents)}; numbers=${flag(numbers)} -->"
    }
}

data class NoteFigure(val svg: String, val caption: String)

data class NoteHeading(val level: Int, val text: String, val id: String)

data class NoteFigureBox(val width: Double?, val aspect: Double)

sealed class NoteSegment {
    data class Markdown(val text: String) : NoteSegment()
    data class Figure(val figure: NoteFigure) : NoteSegment()
    data object PageBreak : NoteSegment()
}

object NoteDocument {
    const val MAX_SVG_BYTES = 262144
    const val MENU_LABEL = "Export PDF"
    private val PAGE_BREAK = Regex("^\\s*<!--\\s*pagebreak\\s*-->\\s*$", RegexOption.IGNORE_CASE)
    private val LAYOUT = Regex("^\\s*<!--\\s*page:(.*?)-->\\s*$", RegexOption.IGNORE_CASE)
    private val PROLOG = Regex("^(?:\\s+|<\\?xml[\\s\\S]*?\\?>|<!--[\\s\\S]*?-->)*")
    private val SVG_START = Regex("^<svg[\\s>/]", RegexOption.IGNORE_CASE)
    private val SVG_TAG = Regex("^<svg\\b[^>]*>", RegexOption.IGNORE_CASE)
    private val SVG_ATTRIBUTE = Regex("\\s(width|height|viewBox)\\s*=\\s*(\"[^\"]*\"|'[^']*')", RegexOption.IGNORE_CASE)
    private val LENGTH = Regex("^\\s*([0-9]*\\.?[0-9]+(?:[eE][-+]?[0-9]+)?)\\s*(px|pt|pc|in|cm|mm|em|ex)?\\s*$", RegexOption.IGNORE_CASE)
    private val IMAGE = Regex("!\\[([^\\]]*)]\\(\\s*<?([^\\s)>]+)>?(?:\\s+\"[^\"]*\")?\\s*\\)")
    private val SAFE_IMAGE = Regex("^(?:https?://|data:image/(?:png|jpe?g|gif|webp|svg\\+xml)[;,])", RegexOption.IGNORE_CASE)
    private val SAFE_LINK = Regex("^(?:https?://|mailto:|#)", RegexOption.IGNORE_CASE)
    private val DATE = DateTimeFormatter.ofPattern("MMMM d, yyyy", Locale.US)
    private val UNITS = mapOf("px" to 1.0, "pt" to 4.0 / 3.0, "pc" to 16.0, "in" to 96.0, "cm" to 96.0 / 2.54, "mm" to 96.0 / 25.4, "em" to 16.0, "ex" to 8.0)
    private const val SANS = "Roboto, \"Noto Sans\", -apple-system, \"Segoe UI\", system-ui, \"Helvetica Neue\", Arial, sans-serif"
    private const val MONO = "\"Roboto Mono\", \"Droid Sans Mono\", ui-monospace, Menlo, Consolas, monospace"

    private fun lines(markdown: String): List<String> = markdown.replace("\r\n", "\n").replace('\r', '\n').split('\n')

    private class Fence(val char: Char, val length: Int, val info: String)

    private fun openingFence(line: String): Fence? {
        val trimmed = line.trim()
        val c = trimmed.firstOrNull() ?: return null
        if (c != '`' && c != '~') return null
        val run = trimmed.takeWhile { it == c }.length
        if (run < 3) return null
        val info = trimmed.substring(run)
        if (c == '`' && info.contains('`')) return null
        return Fence(c, run, info.trim())
    }

    private fun closesFence(line: String, fence: Fence): Boolean {
        val trimmed = line.trim()
        val run = trimmed.takeWhile { it == fence.char }.length
        return run >= fence.length && trimmed.substring(run).isBlank()
    }

    private fun outsideFences(lines: List<String>): BooleanArray {
        val outside = BooleanArray(lines.size)
        var fence: Fence? = null
        for ((index, line) in lines.withIndex()) {
            val open = fence
            if (open != null) {
                if (closesFence(line, open)) fence = null
                continue
            }
            val next = openingFence(line)
            if (next != null) fence = next else outside[index] = true
        }
        return outside
    }

    private fun layoutIndex(lines: List<String>): Int {
        val outside = outsideFences(lines)
        return lines.indices.firstOrNull { outside[it] && LAYOUT.matches(lines[it]) } ?: -1
    }

    fun isPageBreak(line: String): Boolean = PAGE_BREAK.matches(line)

    fun isLayoutLine(line: String): Boolean = LAYOUT.matches(line)

    fun layout(markdown: String): NoteLayout {
        val all = lines(markdown)
        val at = layoutIndex(all)
        if (at < 0) return NoteLayout()
        var layout = NoteLayout()
        for (pair in LAYOUT.matchEntire(all[at])!!.groupValues[1].split(';')) {
            val parts = pair.split('=')
            val key = parts[0].trim().lowercase()
            val value = parts.getOrNull(1)?.trim()?.lowercase() ?: continue
            val flag = when (value) {
                "yes" -> true
                "no" -> false
                else -> null
            }
            layout = when (key) {
                "size" -> NotePaper.entries.firstOrNull { it.key == value }?.let { layout.copy(paper = it) }
                "orientation" -> NoteOrientation.entries.firstOrNull { it.key == value }?.let { layout.copy(orientation = it) }
                "margins" -> NoteMargins.entries.firstOrNull { it.key == value }?.let { layout.copy(margins = it) }
                "cover" -> flag?.let { layout.copy(cover = it) }
                "contents" -> flag?.let { layout.copy(contents = it) }
                "numbers" -> flag?.let { layout.copy(numbers = it) }
                else -> null
            } ?: layout
        }
        return layout
    }

    fun withLayout(markdown: String, layout: NoteLayout): String {
        val all = lines(markdown).toMutableList()
        val at = layoutIndex(all)
        if (at >= 0) all.removeAt(at)
        while (all.isNotEmpty() && all.first().isBlank()) all.removeAt(0)
        return layout.line() + "\n\n" + all.joinToString("\n")
    }

    fun figure(info: String, body: String): NoteFigure? {
        val trimmed = info.trim()
        val word = trimmed.takeWhile { !it.isWhitespace() }
        if (!word.equals("svg", ignoreCase = true)) return null
        val start = body.replaceFirst(PROLOG, "")
        if (!SVG_START.containsMatchIn(start)) return null
        if (body.toByteArray(Charsets.UTF_8).size > MAX_SVG_BYTES) return null
        return NoteFigure(body, trimmed.substring(word.length).trim())
    }

    fun segments(markdown: String): List<NoteSegment> {
        val out = ArrayList<NoteSegment>()
        val buffer = ArrayList<String>()
        fun flush() {
            if (buffer.isEmpty()) return
            if (buffer.any { it.isNotBlank() }) out += NoteSegment.Markdown(buffer.joinToString("\n"))
            buffer.clear()
        }
        val all = lines(markdown)
        var i = 0
        while (i < all.size) {
            val line = all[i]
            val fence = openingFence(line)
            if (fence != null) {
                var end = i + 1
                while (end < all.size && !closesFence(all[end], fence)) end++
                val body = all.subList(i + 1, minOf(end, all.size)).joinToString("\n")
                val figure = figure(fence.info, body)
                if (figure != null) {
                    flush()
                    out += NoteSegment.Figure(figure)
                } else {
                    buffer.addAll(all.subList(i, minOf(end + 1, all.size)))
                }
                i = end + 1
                continue
            }
            when {
                isPageBreak(line) -> {
                    flush()
                    out += NoteSegment.PageBreak
                }
                isLayoutLine(line) -> Unit
                else -> buffer += line
            }
            i++
        }
        flush()
        return out
    }

    fun headings(markdown: String): List<NoteHeading> {
        val out = ArrayList<NoteHeading>()
        var index = 0
        for (segment in segments(markdown)) {
            if (segment !is NoteSegment.Markdown) continue
            for (block in DocsMarkdown.blocks(segment.text)) {
                if (block !is DocsBlock.Heading) continue
                index++
                if (block.level <= 2) out += NoteHeading(block.level, DocsMarkdown.plain(block.text), "note-h-$index")
            }
        }
        return out
    }

    fun pdfFileName(title: String): String = NotesLogic.exportFileName(title).removeSuffix(".md") + ".pdf"

    fun dataUri(svg: String): String = "data:image/svg+xml;base64," + Base64.getEncoder().encodeToString(svg.toByteArray(Charsets.UTF_8))

    fun figureBox(svg: String): NoteFigureBox {
        val tag = SVG_TAG.find(svg.replaceFirst(PROLOG, ""))?.value.orEmpty()
        val attributes = HashMap<String, String>()
        for (match in SVG_ATTRIBUTE.findAll(tag)) attributes.putIfAbsent(match.groupValues[1].lowercase(), match.groupValues[2].drop(1).dropLast(1))
        fun length(key: String): Double? {
            val match = LENGTH.matchEntire(attributes[key] ?: return null) ?: return null
            val value = match.groupValues[1].toDoubleOrNull() ?: return null
            val scale = UNITS[match.groupValues[2].lowercase().ifEmpty { "px" }] ?: return null
            return (value * scale).takeIf { it > 0 && it.isFinite() }
        }
        val box = attributes["viewbox"]?.trim()?.split(Regex("[\\s,]+"))?.mapNotNull { it.toDoubleOrNull() }
        val boxAspect = if (box != null && box.size == 4 && box[2] > 0 && box[3] > 0) box[3] / box[2] else null
        val width = length("width")
        val height = length("height")
        return when {
            width != null && height != null -> NoteFigureBox(width, height / width)
            width != null -> NoteFigureBox(width, boxAspect ?: 0.5)
            height != null && boxAspect != null -> NoteFigureBox(height / boxAspect, boxAspect)
            boxAspect != null -> NoteFigureBox(null, boxAspect)
            else -> NoteFigureBox(300.0, 0.5)
        }
    }

    fun figurePreviewHtml(figure: NoteFigure): String =
        "<!doctype html><html><head><meta charset=\"utf-8\">" +
            "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src data:; style-src 'unsafe-inline'\">" +
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">" +
            "<style>html,body{margin:0;padding:0;background:#fff;overflow:hidden}img{display:block;width:100%;height:100%}</style>" +
            "</head><body><img src=\"${dataUri(figure.svg)}\" alt=\"${escape(figure.caption.ifEmpty { "Diagram" })}\"></body></html>"

    fun escape(text: String): String {
        val out = StringBuilder(text.length)
        for (c in text) {
            when (c) {
                '&' -> out.append("&amp;")
                '<' -> out.append("&lt;")
                '>' -> out.append("&gt;")
                '"' -> out.append("&quot;")
                '\'' -> out.append("&#39;")
                else -> out.append(c)
            }
        }
        return out.toString()
    }

    fun printTitle(title: String): String = title.trim().ifEmpty { "Untitled note" }

    fun printHtml(title: String, emoji: String, markdown: String, layout: NoteLayout, date: LocalDate): String {
        val heading = printTitle(title)
        val references = DocsMarkdown.parse(markdown).references
        val headings = headings(markdown)
        val body = StringBuilder()
        if (!layout.cover) body.append("<h1>").append(escape(heading)).append("</h1>")
        var headingIndex = 0
        for (segment in segments(markdown)) {
            when (segment) {
                is NoteSegment.PageBreak -> body.append("<div class=\"note-page-break\"></div>")
                is NoteSegment.Figure -> body.append(figureHtml(segment.figure))
                is NoteSegment.Markdown -> {
                    val blocks = DocsMarkdown.blocks(segment.text)
                    headingIndex = blocksHtml(blocks, references, body, headingIndex)
                }
            }
        }
        val html = StringBuilder()
        html.append("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">")
        html.append("<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src data: https: http:; style-src 'unsafe-inline'; font-src data:\">")
        html.append("<meta name=\"color-scheme\" content=\"light\">")
        html.append("<title>").append(escape(pdfFileName(title).removeSuffix(".pdf"))).append("</title>")
        html.append("<style>").append(printStyles(layout)).append("</style></head><body>")
        if (layout.cover) {
            html.append("<section class=\"note-cover\">")
            if (emoji.isNotBlank()) html.append("<div class=\"note-cover-emoji\">").append(escape(emoji.trim())).append("</div>")
            html.append("<h1>").append(escape(heading)).append("</h1>")
            html.append("<div class=\"note-cover-date\">").append(escape(DATE.format(date))).append("</div></section>")
        }
        if (layout.contents) {
            html.append("<section class=\"note-contents\"><h1>Contents</h1><ol>")
            for (entry in headings) {
                html.append("<li class=\"note-contents-${entry.level}\"><a href=\"#${entry.id}\">").append(escape(entry.text)).append("</a></li>")
            }
            html.append("</ol></section>")
        }
        html.append("<article>").append(body).append("</article></body></html>")
        return html.toString()
    }

    private fun figureHtml(figure: NoteFigure): String {
        val caption = if (figure.caption.isEmpty()) "" else "<figcaption>${escape(figure.caption)}</figcaption>"
        return "<figure class=\"note-figure\"><img src=\"${dataUri(figure.svg)}\" alt=\"${escape(figure.caption.ifEmpty { "Diagram" })}\">$caption</figure>"
    }

    private fun blocksHtml(blocks: List<DocsBlock>, references: Map<String, String>, out: StringBuilder, startIndex: Int, top: Boolean = true): Int {
        var index = startIndex
        for (block in blocks) {
            when (block) {
                is DocsBlock.Heading -> {
                    val level = block.level.coerceIn(1, 6)
                    if (top) {
                        index++
                        out.append("<h$level id=\"note-h-$index\">")
                    } else {
                        out.append("<h$level>")
                    }
                    out.append(inlineHtml(block.text, references)).append("</h$level>")
                }
                is DocsBlock.Paragraph -> out.append("<p>").append(inlineHtml(block.text, references)).append("</p>")
                is DocsBlock.ListItem -> {
                    val marker = when {
                        block.checked == true -> "\u2611"
                        block.checked == false -> "\u2610"
                        else -> block.marker
                    }
                    out.append("<div class=\"note-item\" style=\"padding-left:${block.depth * 18}pt\">")
                    out.append("<span class=\"note-marker\">").append(escape(marker)).append("</span>")
                    out.append("<div class=\"note-item-text\">").append(inlineHtml(block.text, references)).append("</div></div>")
                }
                is DocsBlock.Quote -> {
                    out.append("<blockquote>")
                    blocksHtml(block.blocks, references, out, 0, top = false)
                    out.append("</blockquote>")
                }
                is DocsBlock.Code -> out.append("<pre><code>").append(escape(block.text)).append("</code></pre>")
                is DocsBlock.Table -> {
                    fun align(column: Int) = when (block.align.getOrNull(column)) {
                        DocsAlign.Center -> " style=\"text-align:center\""
                        DocsAlign.End -> " style=\"text-align:right\""
                        else -> ""
                    }
                    out.append("<table><thead><tr>")
                    block.header.forEachIndexed { column, cell -> out.append("<th${align(column)}>").append(inlineHtml(cell, references)).append("</th>") }
                    out.append("</tr></thead><tbody>")
                    for (row in block.rows) {
                        out.append("<tr>")
                        row.forEachIndexed { column, cell -> out.append("<td${align(column)}>").append(inlineHtml(cell, references)).append("</td>") }
                        out.append("</tr>")
                    }
                    out.append("</tbody></table>")
                }
                is DocsBlock.Anchor -> out.append("<a id=\"").append(escape(block.id)).append("\"></a>")
                DocsBlock.Rule -> out.append("<hr>")
            }
        }
        return index
    }

    private fun inlineHtml(text: String, references: Map<String, String>): String {
        val out = StringBuilder()
        var from = 0
        var i = 0
        while (i < text.length) {
            val c = text[i]
            if (c == '`') {
                val run = text.substring(i).takeWhile { it == '`' }.length
                val close = text.indexOf("`".repeat(run), i + run)
                i = if (close < 0) i + run else close + run
                continue
            }
            if (c == '!' && (i == 0 || text[i - 1] != '\\')) {
                val match = IMAGE.matchAt(text, i)
                if (match != null) {
                    spansHtml(DocsMarkdown.inline(text.substring(from, i), references), out)
                    val alt = match.groupValues[1]
                    val src = match.groupValues[2]
                    if (SAFE_IMAGE.containsMatchIn(src)) {
                        out.append("<img src=\"").append(escape(src)).append("\" alt=\"").append(escape(alt)).append("\">")
                    } else {
                        out.append(escape(alt))
                    }
                    i = match.range.last + 1
                    from = i
                    continue
                }
            }
            i++
        }
        spansHtml(DocsMarkdown.inline(text.substring(from), references), out)
        return out.toString()
    }

    private fun spansHtml(spans: List<DocsSpan>, out: StringBuilder) {
        for (span in spans) {
            var html = escape(span.text).replace("\n", "<br>")
            if (span.code) html = "<code>$html</code>"
            if (span.strike) html = "<del>$html</del>"
            if (span.italic) html = "<em>$html</em>"
            if (span.bold) html = "<strong>$html</strong>"
            val href = span.href
            if (href != null && SAFE_LINK.containsMatchIn(href)) html = "<a href=\"${escape(href)}\">$html</a>"
            out.append(html)
        }
    }

    private fun mm(value: Double): String = String.format(Locale.US, "%.1fmm", value)

    fun printStyles(layout: NoteLayout): String {
        val numbers = if (layout.numbers) {
            "@bottom-center { content: counter(page) \" / \" counter(pages); font: 9pt $SANS; color: #57606a; }"
        } else {
            ""
        }
        val cover = if (layout.cover && layout.numbers) "@page :first { @bottom-center { content: none; } }" else ""
        val coverTop = (layout.pageHeightMm - 2 * layout.margins.mm) * 0.22
        return """
@page { size: ${mm(layout.pageWidthMm)} ${mm(layout.pageHeightMm)}; margin: ${mm(layout.margins.mm)}; $numbers }
$cover
html { print-color-adjust: exact; -webkit-print-color-adjust: exact; color-scheme: light; }
body { margin: 0; background: #fff; color: #1f2328; font: 11pt/1.45 $SANS; overflow-wrap: anywhere; }
h1, h2, h3, h4, h5, h6 { margin: 16pt 0 6pt; line-height: 1.25; font-weight: 600; break-after: avoid; page-break-after: avoid; }
h1 { font-size: 20pt; } h2 { font-size: 15pt; } h3 { font-size: 12.5pt; } h4 { font-size: 11pt; } h5, h6 { font-size: 10pt; }
article > :first-child { margin-top: 0; }
p { margin: 0 0 8pt; }
a { color: #0969da; text-decoration: none; }
.note-item { display: flex; align-items: flex-start; gap: 6pt; margin: 2pt 0; break-inside: avoid; }
.note-marker { min-width: 12pt; color: #57606a; }
.note-item-text { flex: 1; }
pre { margin: 0 0 10pt; padding: 8pt; border: .5pt solid #d0d7de; border-radius: 4pt; background: #f6f8fa; font: 9pt/1.4 $MONO; white-space: pre-wrap; break-inside: avoid; }
code { border-radius: 2pt; background: #f6f8fa; padding: 0 2pt; font: 9pt $MONO; }
pre code { background: none; padding: 0; }
blockquote { margin: 0 0 8pt; border-left: 2pt solid #d0d7de; padding-left: 10pt; color: #57606a; }
table { width: 100%; margin: 0 0 10pt; border-collapse: collapse; font-size: 9.5pt; }
th, td { border: .5pt solid #d0d7de; padding: 3pt 5pt; text-align: left; vertical-align: top; }
th { background: #f6f8fa; }
tr, img, figure { break-inside: avoid; }
img { max-width: 100%; }
hr { margin: 12pt 0; border: 0; border-top: .5pt solid #d0d7de; }
.note-figure { margin: 10pt 0 12pt; text-align: center; break-inside: avoid; }
.note-figure img { display: block; max-width: 100%; height: auto; margin: 0 auto; }
.note-figure figcaption { margin-top: 4pt; font-size: 9pt; color: #57606a; }
.note-page-break { break-before: page; page-break-before: always; height: 0; margin: 0; border: 0; }
.note-cover { break-after: page; page-break-after: always; padding-top: ${mm(coverTop)}; text-align: center; }
.note-cover-emoji { font-size: 48pt; line-height: 1.2; }
.note-cover h1 { margin: 10pt 0 8pt; font-size: 28pt; }
.note-cover-date { font-size: 12pt; color: #57606a; }
.note-contents { break-after: page; page-break-after: always; }
.note-contents ol { list-style: none; margin: 0; padding: 0; }
.note-contents li { margin: 0 0 5pt; }
.note-contents .note-contents-2 { padding-left: 16pt; font-size: 10pt; }
""".trim()
    }
}
