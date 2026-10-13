package com.forkmesh.nativeapp.code.diff

import java.io.ByteArrayOutputStream
import java.time.ZoneOffset
import java.time.ZonedDateTime
import java.util.Base64

enum class DiffFileStatus { Added, Modified, Deleted, Renamed, Copied }

enum class DiffLineKind { Context, Added, Removed }

class DiffLine(
    val kind: DiffLineKind,
    val text: String,
    val oldNumber: Int = 0,
    val newNumber: Int = 0,
) {
    var noNewlineAtEnd: Boolean = false

    var emphasis: List<DiffRange> = emptyList()
}

data class DiffRange(val start: Int, val end: Int) {
    override fun toString(): String = "[$start,$end)"
}

class DiffHunk(
    val oldStart: Int,
    val oldCount: Int,
    val newStart: Int,
    val newCount: Int,
    val section: String = "",
) {
    val lines: MutableList<DiffLine> = ArrayList()

    val header: String
        get() = "@@ -$oldStart,$oldCount +$newStart,$newCount @@" + if (section.isEmpty()) "" else " $section"
}

class DiffSplitRow(val left: DiffLine? = null, val right: DiffLine? = null, val hunk: DiffHunk? = null)

class DiffFile(
    val oldPath: String,
    val newPath: String,
    val status: DiffFileStatus,
    val binary: Boolean = false,
    val oldMode: String = "",
    val newMode: String = "",
    val similarity: Int = -1,
    val hunks: List<DiffHunk> = emptyList(),
    val raw: String = "",
) {
    val path: String get() = if (status == DiffFileStatus.Deleted) oldPath else newPath

    val modeChanged: Boolean get() = oldMode.isNotEmpty() && newMode.isNotEmpty() && oldMode != newMode

    val additions: Int get() = count(DiffLineKind.Added)
    val deletions: Int get() = count(DiffLineKind.Removed)

    private fun count(kind: DiffLineKind): Int = hunks.sumOf { hunk -> hunk.lines.count { it.kind == kind } }

    val maxLineNumber: Int
        get() {
            var max = 0
            for (hunk in hunks) {
                val oldEnd = hunk.oldStart + hunk.oldCount
                val newEnd = hunk.newStart + hunk.newCount
                if (oldEnd > max) max = oldEnd
                if (newEnd > max) max = newEnd
            }
            return max
        }

    val longestLine: Int get() = hunks.maxOfOrNull { hunk -> hunk.lines.maxOfOrNull { it.text.length } ?: 0 } ?: 0

    private var prepared = false
    private var splitRowList: List<DiffSplitRow> = emptyList()

    val splitRows: List<DiffSplitRow>
        get() {
            prepare()
            return splitRowList
        }

    @Synchronized
    fun prepare() {
        if (prepared) return
        prepared = true
        val rows = ArrayList<DiffSplitRow>()
        for (hunk in hunks) {
            rows.add(DiffSplitRow(hunk = hunk))
            val lines = hunk.lines
            var i = 0
            while (i < lines.size) {
                val line = lines[i]
                if (line.kind == DiffLineKind.Context) {
                    rows.add(DiffSplitRow(left = line, right = line))
                    i++
                    continue
                }
                val removed = ArrayList<DiffLine>()
                val added = ArrayList<DiffLine>()
                while (i < lines.size && lines[i].kind == DiffLineKind.Removed) removed.add(lines[i++])
                while (i < lines.size && lines[i].kind == DiffLineKind.Added) added.add(lines[i++])
                val pairs = maxOf(removed.size, added.size)
                for (p in 0 until pairs) {
                    val left = removed.getOrNull(p)
                    val right = added.getOrNull(p)
                    if (left != null && right != null) {
                        intraLineChanges(left.text, right.text)?.let { (a, b) ->
                            left.emphasis = a
                            right.emphasis = b
                        }
                    }
                    rows.add(DiffSplitRow(left = left, right = right))
                }
            }
        }
        splitRowList = rows
    }
}

private fun expandTabs(line: String): String {
    if (!line.contains('\t')) return line
    val out = StringBuilder()
    var column = 0
    var index = 0
    while (index < line.length) {
        val rune = line.codePointAt(index)
        index += Character.charCount(rune)
        if (rune == 0x09) {
            val pad = 4 - (column % 4)
            repeat(pad) { out.append(' ') }
            column += pad
        } else {
            out.appendCodePoint(rune)
            column++
        }
    }
    return out.toString()
}

private val HUNK_HEADER = Regex("""^@@+ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@+ ?(.*)$""")

private fun stripPrefix(raw: String): String {
    var path = raw.trim()
    val tab = path.indexOf('\t')
    if (tab >= 0) path = path.substring(0, tab)
    if (path.length >= 2 && path.startsWith("\"") && path.endsWith("\"")) {
        path = unquote(path.substring(1, path.length - 1))
    }
    if (path.startsWith("a/") || path.startsWith("b/")) path = path.substring(2)
    return path
}

private val OCTAL = Regex("^[0-7]{3}")

private fun unquote(quoted: String): String {
    val bytes = ByteArrayOutputStream()
    fun addText(text: String) = bytes.write(text.toByteArray(Charsets.UTF_8))
    var i = 0
    while (i < quoted.length) {
        val c = quoted[i]
        if (c != '\\' || i + 1 >= quoted.length) {
            addText(c.toString())
            i++
            continue
        }
        i++
        when (val next = quoted[i]) {
            'n' -> bytes.write(0x0a)
            't' -> bytes.write(0x09)
            '"' -> bytes.write(0x22)
            '\\' -> bytes.write(0x5c)
            else -> {
                val octal = OCTAL.find(quoted.substring(i))
                if (octal != null) {
                    bytes.write(octal.value.toInt(8))
                    i += 2
                } else {
                    addText(next.toString())
                }
            }
        }
        i++
    }
    return String(bytes.toByteArray(), Charsets.UTF_8)
}

private fun gitHeaderPaths(line: String): Pair<String, String> {
    val rest = line.substring("diff --git ".length)
    if (rest.startsWith("\"")) {
        val close = rest.indexOf("\" ", 1)
        if (close > 0) return stripPrefix(rest.substring(0, close + 1)) to stripPrefix(rest.substring(close + 2))
    }
    if (rest.startsWith("a/")) {
        val half = (rest.length - 1) / 2
        if (rest.length % 2 == 1 && rest.substring(half, half + 3) == " b/" &&
            rest.substring(2, half) == rest.substring(half + 3)
        ) {
            val same = rest.substring(2, half)
            return same to same
        }
        val split = rest.indexOf(" b/")
        if (split > 0) return rest.substring(2, split) to stripPrefix(rest.substring(split + 1))
    }
    return rest to rest
}

internal fun splitLines(text: String): List<String> {
    val out = ArrayList<String>()
    var start = 0
    var i = 0
    while (i < text.length) {
        val c = text[i]
        if (c == '\n' || c == '\r') {
            out.add(text.substring(start, i))
            if (c == '\r' && i + 1 < text.length && text[i + 1] == '\n') i++
            start = i + 1
        }
        i++
    }
    if (start < text.length) out.add(text.substring(start))
    return out
}

fun parseUnifiedDiff(patch: String): List<DiffFile> {
    if (patch.isBlank()) return emptyList()
    val lines = splitLines(patch)
    val files = ArrayList<DiffFile>()

    var oldPath: String? = null
    var newPath: String? = null
    var status = DiffFileStatus.Modified
    var binary = false
    var oldMode = ""
    var newMode = ""
    var similarity = -1
    var hunks = ArrayList<DiffHunk>()
    var started = false
    var rawStart = 0
    var hunk: DiffHunk? = null
    var oldLeft = 0
    var newLeft = 0
    var oldNo = 0
    var newNo = 0
    var skippingBinary = false

    fun flush(endLine: Int) {
        if (!started) return
        val o = oldPath ?: newPath ?: ""
        val n = newPath ?: oldPath ?: ""
        if (o.isEmpty() && n.isEmpty()) return
        files.add(
            DiffFile(
                oldPath = o,
                newPath = n,
                status = status,
                binary = binary,
                oldMode = oldMode,
                newMode = newMode,
                similarity = similarity,
                hunks = hunks,
                raw = lines.subList(rawStart, endLine).joinToString("\n"),
            ),
        )
    }

    fun begin(index: Int) {
        flush(index)
        started = true
        rawStart = index
        oldPath = null
        newPath = null
        status = DiffFileStatus.Modified
        binary = false
        oldMode = ""
        newMode = ""
        similarity = -1
        hunks = ArrayList()
        hunk = null
        oldLeft = 0
        newLeft = 0
        skippingBinary = false
    }

    for (index in lines.indices) {
        val line = lines[index]
        val current = hunk

        if (current != null && (oldLeft > 0 || newLeft > 0)) {
            if (line.startsWith("\\")) {
                current.lines.lastOrNull()?.noNewlineAtEnd = true
                continue
            }
            val marker = if (line.isEmpty()) ' ' else line[0]
            val text = expandTabs(if (line.isEmpty()) "" else line.substring(1))
            when (marker) {
                '+' -> {
                    current.lines.add(DiffLine(DiffLineKind.Added, text, newNumber = newNo++))
                    newLeft--
                    continue
                }
                '-' -> {
                    current.lines.add(DiffLine(DiffLineKind.Removed, text, oldNumber = oldNo++))
                    oldLeft--
                    continue
                }
                ' ' -> {
                    current.lines.add(DiffLine(DiffLineKind.Context, text, oldNumber = oldNo++, newNumber = newNo++))
                    oldLeft--
                    newLeft--
                    continue
                }
            }
            oldLeft = 0
            newLeft = 0
        }

        if (line.startsWith("\\") && current != null) {
            current.lines.lastOrNull()?.noNewlineAtEnd = true
            continue
        }
        if (line.startsWith("diff --git ")) {
            begin(index)
            val (a, b) = gitHeaderPaths(line)
            oldPath = a
            newPath = b
            continue
        }
        if (!started && (line.startsWith("--- ") || line.startsWith("diff "))) {
            begin(index)
        }
        if (!started) continue
        if (line.startsWith("--- ") && hunks.isNotEmpty() && index + 1 < lines.size && lines[index + 1].startsWith("+++ ")) {
            begin(index)
        }
        if (skippingBinary) continue

        val match = HUNK_HEADER.find(line)
        if (match != null) {
            val oldStart = match.groupValues[1].toInt()
            val oldCount = match.groupValues[2].toIntOrNull() ?: 1
            val newStart = match.groupValues[3].toInt()
            val newCount = match.groupValues[4].toIntOrNull() ?: 1
            val next = DiffHunk(oldStart, oldCount, newStart, newCount, section = match.groupValues[5].trim())
            hunks.add(next)
            hunk = next
            oldLeft = oldCount
            newLeft = newCount
            oldNo = if (oldStart == 0 && oldCount == 0) 0 else oldStart
            newNo = if (newStart == 0 && newCount == 0) 0 else newStart
            continue
        }
        when {
            line.startsWith("--- ") -> {
                val path = line.substring(4)
                if (path.trim() == "/dev/null") status = DiffFileStatus.Added else oldPath = stripPrefix(path)
            }
            line.startsWith("+++ ") -> {
                val path = line.substring(4)
                if (path.trim() == "/dev/null") status = DiffFileStatus.Deleted else newPath = stripPrefix(path)
            }
            line.startsWith("new file mode ") -> {
                status = DiffFileStatus.Added
                newMode = line.substring("new file mode ".length).trim()
            }
            line.startsWith("deleted file mode ") -> {
                status = DiffFileStatus.Deleted
                oldMode = line.substring("deleted file mode ".length).trim()
            }
            line.startsWith("old mode ") -> oldMode = line.substring("old mode ".length).trim()
            line.startsWith("new mode ") -> newMode = line.substring("new mode ".length).trim()
            line.startsWith("rename from ") -> {
                status = DiffFileStatus.Renamed
                oldPath = stripPrefix(line.substring("rename from ".length))
            }
            line.startsWith("rename to ") -> {
                status = DiffFileStatus.Renamed
                newPath = stripPrefix(line.substring("rename to ".length))
            }
            line.startsWith("copy from ") -> {
                status = DiffFileStatus.Copied
                oldPath = stripPrefix(line.substring("copy from ".length))
            }
            line.startsWith("copy to ") -> {
                status = DiffFileStatus.Copied
                newPath = stripPrefix(line.substring("copy to ".length))
            }
            line.startsWith("similarity index ") ->
                similarity = line.substring("similarity index ".length).replace("%", "").toIntOrNull() ?: -1
            line.startsWith("index ") -> {
                val parts = line.split(' ')
                if (parts.size >= 3 && oldMode.isEmpty() && newMode.isEmpty()) {
                    oldMode = parts[2]
                    newMode = parts[2]
                }
            }
            line.startsWith("Binary files ") -> binary = true
            line == "GIT binary patch" -> {
                binary = true
                skippingBinary = true
            }
        }
    }
    flush(lines.size)
    return files
}

data class PatchCommit(
    val hash: String,
    val subject: String,
    val author: String = "",
    val email: String = "",
    val date: Long? = null,
    val body: String = "",
    val patch: String = "",
) {
    val shortHash: String get() = if (hash.length > 7) hash.substring(0, 7) else hash
}

private val MBOX_FROM = Regex("^From ([0-9a-f]{40}|[0-9a-f]{64}) Mon Sep 17 00:00:00 2001$")
private val PATCH_TAG = Regex("""^\[PATCH[^\]]*\]\s*""")

fun parseFormatPatchSeries(series: String): List<PatchCommit> {
    if (series.isBlank()) return emptyList()
    val lines = splitLines(series)
    val starts = lines.indices.filter { MBOX_FROM.matches(lines[it]) }
    return starts.mapIndexed { s, start ->
        val end = if (s + 1 < starts.size) starts[s + 1] else lines.size
        parseMboxMessage(lines.subList(start, end))
    }
}

private val ANGLE = Regex("""^(.*?)\s*<([^>]*)>\s*$""")

private fun parseMboxMessage(lines: List<String>): PatchCommit {
    val hash = MBOX_FROM.find(lines.first())!!.groupValues[1]
    val headers = LinkedHashMap<String, String>()
    var i = 1
    var lastKey: String? = null
    while (i < lines.size) {
        val line = lines[i]
        if (line.isEmpty()) {
            i++
            break
        }
        if ((line.startsWith(" ") || line.startsWith("\t")) && lastKey != null) {
            headers[lastKey] = "${headers[lastKey]} ${line.trim()}"
            i++
            continue
        }
        val colon = line.indexOf(':')
        if (colon > 0) {
            lastKey = line.substring(0, colon).lowercase()
            headers[lastKey] = line.substring(colon + 1).trim()
        }
        i++
    }

    val body = ArrayList<String>()
    var diffStart = -1
    var inStat = false
    while (i < lines.size) {
        val line = lines[i]
        if (line.startsWith("diff --git ")) {
            diffStart = i
            break
        }
        if (line == "---") {
            inStat = true
        } else if (!inStat) {
            body.add(line)
        }
        i++
    }
    var patchLines: List<String> = if (diffStart < 0) emptyList() else lines.subList(diffStart, lines.size)
    val signature = patchLines.lastIndexOf("-- ")
    if (signature >= 0 && signature >= patchLines.size - 3) patchLines = patchLines.subList(0, signature)

    val from = decodeMimeHeader(headers["from"] ?: "")
    var author = from
    var email = ""
    ANGLE.find(from)?.let {
        author = it.groupValues[1].replace("\"", "").trim()
        email = it.groupValues[2].trim()
    }
    val subject = decodeMimeHeader(headers["subject"] ?: "").replaceFirst(PATCH_TAG, "").trim()
    return PatchCommit(
        hash = hash,
        subject = subject.ifEmpty { "(no subject)" },
        author = author.ifEmpty { email },
        email = email,
        date = parseRfc2822(headers["date"] ?: ""),
        body = body.joinToString("\n").trim(),
        patch = patchLines.joinToString("\n"),
    )
}

private val ENCODED_WORD = Regex("""=\?([^?]+)\?([bBqQ])\?([^?]*)\?=""")
private val ADJACENT_WORDS = Regex("""\?=\s+=\?""")

fun decodeMimeHeader(value: String): String {
    if (!value.contains("=?")) return value
    val joined = value.replace(ADJACENT_WORDS, "?==?")
    return ENCODED_WORD.replace(joined) { match ->
        val encoding = match.groupValues[2].lowercase()
        val payload = match.groupValues[3]
        try {
            val bytes = if (encoding == "b") {
                Base64.getDecoder().decode(payload)
            } else {
                val out = ByteArrayOutputStream()
                var i = 0
                while (i < payload.length) {
                    val c = payload[i]
                    if (c == '_') {
                        out.write(0x20)
                    } else if (c == '=' && i + 2 < payload.length) {
                        out.write(payload.substring(i + 1, i + 3).toInt(16))
                        i += 2
                    } else {
                        out.write(c.toString().toByteArray(Charsets.UTF_8))
                    }
                    i++
                }
                out.toByteArray()
            }
            String(bytes, Charsets.UTF_8)
        } catch (_: Exception) {
            match.value
        }
    }
}

private val MONTHS = mapOf(
    "jan" to 1, "feb" to 2, "mar" to 3, "apr" to 4, "may" to 5, "jun" to 6,
    "jul" to 7, "aug" to 8, "sep" to 9, "oct" to 10, "nov" to 11, "dec" to 12,
)
private val RFC2822 = Regex("""(\d{1,2})\s+([A-Za-z]{3})\s+(\d{4})\s+(\d{2}):(\d{2})(?::(\d{2}))?\s*([+-]\d{4})?""")

internal fun parseRfc2822(value: String): Long? {
    val match = RFC2822.find(value) ?: return null
    val g = match.groupValues
    val month = MONTHS[g[2].lowercase()] ?: return null
    val utc = try {
        ZonedDateTime.of(g[3].toInt(), month, g[1].toInt(), g[4].toInt(), g[5].toInt(), g[6].toIntOrNull() ?: 0, 0, ZoneOffset.UTC)
    } catch (_: Exception) {
        return null
    }
    val millis = utc.toInstant().toEpochMilli()
    val zone = g[7]
    if (zone.isEmpty()) return millis
    val sign = if (zone.startsWith("-")) -1 else 1
    val offset = (zone.substring(1, 3).toLong() * 60 + zone.substring(3, 5).toLong()) * 60_000
    return if (sign > 0) millis - offset else millis + offset
}

private val TOKEN = Regex("""\w+|\s+|[^\w\s]""")

fun intraLineChanges(a: String, b: String): Pair<List<DiffRange>, List<DiffRange>>? {
    if (a == b) return null
    val ta = TOKEN.findAll(a).toList()
    val tb = TOKEN.findAll(b).toList()
    if (ta.isEmpty() || tb.isEmpty() || ta.size.toLong() * tb.size > 40000) return null

    var prefix = 0
    while (prefix < ta.size && prefix < tb.size && ta[prefix].value == tb[prefix].value) prefix++
    var suffix = 0
    while (suffix < ta.size - prefix && suffix < tb.size - prefix &&
        ta[ta.size - 1 - suffix].value == tb[tb.size - 1 - suffix].value
    ) {
        suffix++
    }
    val na = ta.size - prefix - suffix
    val nb = tb.size - prefix - suffix
    val keepA = BooleanArray(ta.size) { true }
    val keepB = BooleanArray(tb.size) { true }
    if (na > 0 || nb > 0) {
        val table = Array(na + 1) { IntArray(nb + 1) }
        for (i in na - 1 downTo 0) {
            for (j in nb - 1 downTo 0) {
                table[i][j] = if (ta[prefix + i].value == tb[prefix + j].value) {
                    table[i + 1][j + 1] + 1
                } else {
                    maxOf(table[i + 1][j], table[i][j + 1])
                }
            }
        }
        for (k in 0 until na) keepA[prefix + k] = false
        for (k in 0 until nb) keepB[prefix + k] = false
        var i = 0
        var j = 0
        while (i < na && j < nb) {
            if (ta[prefix + i].value == tb[prefix + j].value) {
                keepA[prefix + i] = true
                keepB[prefix + j] = true
                i++
                j++
            } else if (table[i + 1][j] >= table[i][j + 1]) {
                i++
            } else {
                j++
            }
        }
    }

    fun ranges(tokens: List<MatchResult>, keep: BooleanArray): List<DiffRange> {
        val out = ArrayList<DiffRange>()
        for (k in tokens.indices) {
            if (keep[k]) continue
            val start = tokens[k].range.first
            val end = tokens[k].range.last + 1
            if (out.isNotEmpty() && out.last().end == start) {
                out[out.size - 1] = DiffRange(out.last().start, end)
            } else {
                out.add(DiffRange(start, end))
            }
        }
        return out
    }

    val ra = ranges(ta, keepA)
    val rb = ranges(tb, keepB)
    fun covered(r: List<DiffRange>): Int = r.sumOf { it.end - it.start }
    val changedA = covered(ra).toDouble() / (if (a.isEmpty()) 1 else a.length)
    val changedB = covered(rb).toDouble() / (if (b.isEmpty()) 1 else b.length)
    if (changedA > .7 && changedB > .7) return null
    return ra to rb
}
