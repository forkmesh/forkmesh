package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.json.firstStr
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str
import kotlin.math.roundToLong

data class CommitGraphEdge(val from: Int, val to: Int)

data class CommitGraphRow(
    val hash: String,
    val column: Int,
    val lanesIn: List<Int>,
    val lanesOut: List<Int>,
    val edges: List<CommitGraphEdge>,
    val isMerge: Boolean,
    val overflow: Boolean,
)

object CommitGraph {
    const val MAX_LANES = 6

    fun parents(commit: Map<String, Any?>): List<String> {
        val list = commit["parents"]
        if (list is List<*>) {
            return list.map { item ->
                @Suppress("UNCHECKED_CAST")
                val map = item as? Map<String, Any?>
                (map?.firstStr("sha", "hash") ?: item?.toString().orEmpty()).trim().lowercase()
            }.filter { it.isNotEmpty() }
        }
        return commit.str("parents").split(" ").map { it.lowercase() }.filter { it.isNotEmpty() }
    }

    fun hash(commit: Map<String, Any?>): String = commit.firstStr("hash", "sha").trim().lowercase()

    fun layout(commits: List<Pair<String, List<String>>>): List<CommitGraphRow> {
        val lanes = ArrayList<String?>()
        val rows = ArrayList<CommitGraphRow>()
        for ((rawHash, rawParents) in commits) {
            val hash = rawHash.lowercase()
            val before = lanes.indices.filter { lanes[it] != null }
            var column = lanes.indexOf(hash)
            if (column < 0) {
                column = lanes.indexOfFirst { it == null }.let { if (it < 0) lanes.size else it }
                if (column == lanes.size) lanes.add(null)
            }
            val edges = ArrayList<CommitGraphEdge>()
            for (index in lanes.indices) {
                if (lanes[index] == hash && index != column) {
                    lanes[index] = null
                    edges.add(CommitGraphEdge(index, column))
                }
            }
            val parents = rawParents.map { it.lowercase() }
            lanes[column] = parents.firstOrNull()
            if (parents.isNotEmpty()) edges.add(CommitGraphEdge(column, column))
            for (parent in parents.drop(1)) {
                val existing = lanes.indexOf(parent)
                if (existing >= 0) {
                    edges.add(CommitGraphEdge(column, existing))
                    continue
                }
                val slot = lanes.indexOfFirst { it == null }.let { if (it < 0) lanes.size else it }
                if (slot == lanes.size) lanes.add(parent) else lanes[slot] = parent
                edges.add(CommitGraphEdge(column, slot))
            }
            while (lanes.isNotEmpty() && lanes.last() == null) lanes.removeAt(lanes.lastIndex)
            val after = lanes.indices.filter { lanes[it] != null }
            val overflow = column >= MAX_LANES || after.any { it >= MAX_LANES }
            rows.add(
                CommitGraphRow(
                    hash = hash,
                    column = minOf(column, MAX_LANES - 1),
                    lanesIn = before.filter { it < MAX_LANES },
                    lanesOut = after.filter { it < MAX_LANES },
                    edges = edges.filter { it.from < MAX_LANES && it.to < MAX_LANES },
                    isMerge = parents.size > 1,
                    overflow = overflow,
                ),
            )
        }
        return rows
    }

    fun layoutJson(commits: List<Map<String, Any?>>): List<CommitGraphRow> = layout(commits.map { hash(it) to parents(it) })

    fun width(rows: List<CommitGraphRow>): Int = rows.fold(1) { acc, row ->
        maxOf(acc, ((row.lanesIn + row.lanesOut + row.column).maxOrNull() ?: 0) + 1)
    }
}

data class CodeMapNode(
    val name: String,
    val path: String,
    val size: Long,
    val isFile: Boolean,
    val children: List<CodeMapNode> = emptyList(),
) {
    fun find(target: String): CodeMapNode? {
        if (path == target) return this
        for (child in children) {
            if (target == child.path || target.startsWith(child.path + "/") || child.path.isEmpty()) {
                child.find(target)?.let { return it }
            }
        }
        return null
    }

    val fileCount: Int get() = if (isFile) 1 else children.sumOf { it.fileCount }

    companion object {
        fun fromJson(j: Map<String, Any?>, parent: String = "", depth: Int = 0): CodeMapNode {
            val name = j.str("name")
            val isFile = j.str("type") == "file"
            val explicit = j.str("path")
            val path = when {
                explicit.isNotEmpty() -> explicit
                parent.isEmpty() -> name
                name.isEmpty() -> parent
                else -> "$parent/$name"
            }
            val kids = if (depth >= CodeMapLayout.MAX_DEPTH) {
                emptyList()
            } else {
                j.maps("children").take(CodeMapLayout.MAX_CHILDREN).map { fromJson(it, path, depth + 1) }
            }
            val own = maxOf(0L, (j["size"] as? Number)?.toLong() ?: j.int("size").toLong())
            return CodeMapNode(name, path, maxOf(own, kids.sumOf { it.size }), isFile, kids)
        }
    }
}

data class CodeMapRect(val x: Double, val y: Double, val width: Double, val height: Double) {
    val area: Double get() = width * height
    val shortSide: Double get() = minOf(width, height)
}

data class CodeMapCell(val node: CodeMapNode, val rect: CodeMapRect, val depth: Int, val colorSlot: Int)

data class CodeMapCrumb(val name: String, val path: String)

object CodeMapLayout {
    const val MAX_DEPTH = 6
    const val MAX_CHILDREN = 400
    const val MAX_CELLS = 1500
    const val MIN_CELL_SIDE = 3.0

    fun squarify(sizes: List<Double>, rect: CodeMapRect): List<CodeMapRect> {
        val total = sizes.sum()
        val empty = CodeMapRect(rect.x, rect.y, 0.0, 0.0)
        if (total <= 0 || rect.area <= 0) return sizes.map { empty }
        val scale = rect.area / total
        val areas = sizes.map { maxOf(0.0, it) * scale }
        val out = MutableList(sizes.size) { empty }
        var free = rect
        var start = 0
        while (start < areas.size) {
            val side = free.shortSide
            if (side <= 0) break
            var end = start + 1
            var best = worst(areas.subList(start, end), side)
            while (end < areas.size) {
                val next = worst(areas.subList(start, end + 1), side)
                if (next > best) break
                best = next
                end++
            }
            val rowArea = areas.subList(start, end).sum()
            val horizontal = free.width >= free.height
            val thickness = rowArea / side
            var offset = 0.0
            for (i in start until end) {
                val length = if (thickness > 0) areas[i] / thickness else 0.0
                out[i] = if (horizontal) {
                    CodeMapRect(free.x, free.y + offset, thickness, length)
                } else {
                    CodeMapRect(free.x + offset, free.y, length, thickness)
                }
                offset += length
            }
            free = if (horizontal) {
                CodeMapRect(free.x + thickness, free.y, maxOf(0.0, free.width - thickness), free.height)
            } else {
                CodeMapRect(free.x, free.y + thickness, free.width, maxOf(0.0, free.height - thickness))
            }
            start = end
        }
        return out
    }

    private fun worst(row: List<Double>, side: Double): Double {
        val sum = row.sum()
        val maxArea = row.maxOrNull() ?: return Double.POSITIVE_INFINITY
        val minArea = row.minOrNull() ?: return Double.POSITIVE_INFINITY
        if (sum <= 0 || side <= 0 || minArea <= 0) return Double.POSITIVE_INFINITY
        val s2 = side * side
        val sum2 = sum * sum
        return maxOf(s2 * maxArea / sum2, sum2 / (s2 * minArea))
    }

    fun cells(root: CodeMapNode, rect: CodeMapRect, maxLevel: Int = 3, inset: Double = 2.0): List<CodeMapCell> {
        val out = ArrayList<CodeMapCell>()
        fun walk(node: CodeMapNode, area: CodeMapRect, level: Int, slot: Int) {
            val kids = node.children.filter { it.size > 0 }.sortedByDescending { it.size }
            if (kids.isEmpty()) return
            val rects = squarify(kids.map { it.size.toDouble() }, area)
            kids.forEachIndexed { index, child ->
                if (out.size >= MAX_CELLS) return
                val r = rects[index]
                if (r.shortSide < MIN_CELL_SIDE) return@forEachIndexed
                val colorSlot = if (level == 1) minOf(index, 8) else slot
                out.add(CodeMapCell(child, r, level, colorSlot))
                if (level < maxLevel && !child.isFile) {
                    val pad = if (r.shortSide > inset * 6) inset else 0.0
                    val header = if (r.height > 28) 14.0 else 0.0
                    val inner = CodeMapRect(
                        r.x + pad,
                        r.y + pad + header,
                        maxOf(0.0, r.width - 2 * pad),
                        maxOf(0.0, r.height - 2 * pad - header),
                    )
                    walk(child, inner, level + 1, colorSlot)
                }
            }
        }
        walk(root, rect, 1, 8)
        return out
    }

    fun hit(cells: List<CodeMapCell>, x: Double, y: Double): CodeMapCell? =
        cells.lastOrNull { x >= it.rect.x && x <= it.rect.x + it.rect.width && y >= it.rect.y && y <= it.rect.y + it.rect.height }

    fun breadcrumb(path: String): List<CodeMapCrumb> {
        val out = mutableListOf(CodeMapCrumb("/", ""))
        var current = ""
        for (part in path.split("/").filter { it.isNotEmpty() }) {
            current = if (current.isEmpty()) part else "$current/$part"
            out.add(CodeMapCrumb(part, current))
        }
        return out
    }

    fun sizeLabel(bytes: Long): String {
        if (bytes < 1024) return "$bytes B"
        val units = listOf("KB", "MB", "GB")
        var value = bytes / 1024.0
        var unit = 0
        while (value >= 1024 && unit < units.size - 1) {
            value /= 1024
            unit++
        }
        return if (value >= 10) "${value.roundToLong()} ${units[unit]}" else "${"%.1f".format(java.util.Locale.ROOT, value)} ${units[unit]}"
    }
}

enum class ConflictChoice(val title: String) {
    Ours("Keep ours"), Theirs("Keep theirs"), Both("Keep both"), BothTheirsFirst("Both, theirs first")
}

data class ConflictHunk(
    val id: Int,
    val ours: List<String>,
    val base: List<String>?,
    val theirs: List<String>,
    val oursLabel: String,
    val theirsLabel: String,
    val startLine: Int,
)

sealed class ConflictSegment {
    data class Text(val lines: List<String>) : ConflictSegment()
    data class Conflict(val hunk: ConflictHunk) : ConflictSegment()
}

data class ConflictFile(val path: String, val segments: List<ConflictSegment>, val trailingNewline: Boolean) {
    val hunks: List<ConflictHunk> get() = segments.mapNotNull { (it as? ConflictSegment.Conflict)?.hunk }
    val hasConflicts: Boolean get() = hunks.isNotEmpty()
}

object ConflictResolution {
    const val MALFORMED = "The conflict markers in this file are incomplete, so it cannot be resolved here."

    fun parse(path: String, text: String): Result<ConflictFile> {
        val lines = text.split("\n").toMutableList()
        val trailing = text.endsWith("\n")
        if (trailing) lines.removeAt(lines.lastIndex)
        val segments = ArrayList<ConflictSegment>()
        var plain = ArrayList<String>()
        var index = 0
        var hunkId = 0
        while (index < lines.size) {
            val line = lines[index]
            if (!line.startsWith("<<<<<<<")) {
                plain.add(line)
                index++
                continue
            }
            if (plain.isNotEmpty()) {
                segments.add(ConflictSegment.Text(plain))
                plain = ArrayList()
            }
            val start = index + 1
            val oursLabel = line.substring(7).trim()
            val ours = ArrayList<String>()
            var base: MutableList<String>? = null
            val theirs = ArrayList<String>()
            var theirsLabel = ""
            var stage = 0
            index++
            var closed = false
            while (index < lines.size) {
                val current = lines[index]
                if (current.startsWith("<<<<<<<")) return Result.failure(IllegalStateException(MALFORMED))
                if (stage == 0 && current.startsWith("|||||||")) {
                    stage = 1
                    base = ArrayList()
                } else if (stage < 2 && current.startsWith("=======") && current.trim() == "=======") {
                    stage = 2
                } else if (stage == 2 && current.startsWith(">>>>>>>")) {
                    theirsLabel = current.substring(7).trim()
                    closed = true
                    index++
                    break
                } else if (stage == 0) {
                    ours.add(current)
                } else if (stage == 1) {
                    base?.add(current)
                } else {
                    theirs.add(current)
                }
                index++
            }
            if (!closed) return Result.failure(IllegalStateException(MALFORMED))
            segments.add(ConflictSegment.Conflict(ConflictHunk(hunkId, ours, base, theirs, oursLabel, theirsLabel, start)))
            hunkId++
        }
        if (plain.isNotEmpty()) segments.add(ConflictSegment.Text(plain))
        return Result.success(ConflictFile(path, segments, trailing))
    }

    fun lines(hunk: ConflictHunk, choice: ConflictChoice): List<String> = when (choice) {
        ConflictChoice.Ours -> hunk.ours
        ConflictChoice.Theirs -> hunk.theirs
        ConflictChoice.Both -> hunk.ours + hunk.theirs
        ConflictChoice.BothTheirsFirst -> hunk.theirs + hunk.ours
    }

    fun unresolved(file: ConflictFile, choices: Map<Int, ConflictChoice>): List<Int> = file.hunks.map { it.id }.filter { it !in choices }

    fun resolve(file: ConflictFile, choices: Map<Int, ConflictChoice>): String? {
        if (unresolved(file, choices).isNotEmpty()) return null
        val out = ArrayList<String>()
        for (segment in file.segments) {
            when (segment) {
                is ConflictSegment.Text -> out.addAll(segment.lines)
                is ConflictSegment.Conflict -> out.addAll(lines(segment.hunk, choices.getValue(segment.hunk.id)))
            }
        }
        val body = out.joinToString("\n")
        return if (file.trailingNewline) body + "\n" else body
    }

    fun agentPrompt(branch: String, base: String, files: List<String>): String {
        val list = files.take(40).joinToString("\n") { "- $it" }
        return "Merge $base into $branch and resolve every conflict, keeping the intent of both sides. Run the tests, then push the branch.\n\nConflicting files:\n$list"
    }

    const val FIX_SENT = "Asked the agent to merge the base in and resolve the conflicts."

    fun sessionPrompt(base: String, files: List<String> = emptyList()): String {
        val into = if (base.isEmpty()) "the base branch" else "`$base`"
        val head = "Merge $into into your branch and resolve all merge conflicts. Make sure the build and tests still pass, then commit."
        if (files.isEmpty()) return head
        return head + "\n\nConflicting files:\n" + files.take(40).joinToString("\n") { "- $it" }
    }
}

data class TextMatch(val line: Int, val column: Int, val offset: Int, val length: Int)

data class FindOptions(val caseSensitive: Boolean = false, val wholeWord: Boolean = false)

class FindInText {
    var query: String = ""
        private set
    var matches: List<TextMatch> = emptyList()
        private set
    var current: Int? = null
        private set
    var options: FindOptions = FindOptions()

    fun search(query: String, text: String) {
        this.query = query
        matches = emptyList()
        current = null
        if (query.isEmpty()) return
        val hay = if (options.caseSensitive) text else text.lowercase()
        val needle = if (options.caseSensitive) query else query.lowercase()
        if (needle.length > hay.length) return
        val found = ArrayList<TextMatch>()
        var line = 1
        var lineStart = 0
        var i = 0
        while (i + needle.length <= hay.length) {
            if (hay[i] == '\n') {
                line++
                lineStart = i + 1
                i++
                continue
            }
            if (hay.regionMatches(i, needle, 0, needle.length)) {
                val before = i == 0 || !isWord(hay[i - 1])
                val afterIndex = i + needle.length
                val after = afterIndex >= hay.length || !isWord(hay[afterIndex])
                if (!options.wholeWord || (before && after)) {
                    found.add(TextMatch(line, i - lineStart + 1, i, needle.length))
                    if (found.size >= MAX_MATCHES) break
                    i += maxOf(1, needle.length)
                    continue
                }
            }
            i++
        }
        matches = found
        current = if (found.isEmpty()) null else 0
    }

    fun next() {
        if (matches.isEmpty()) return
        current = ((current ?: -1) + 1) % matches.size
    }

    fun previous() {
        if (matches.isEmpty()) return
        current = ((current ?: 0) - 1 + matches.size) % matches.size
    }

    fun jumpToLine(line: Int) {
        val index = matches.indexOfFirst { it.line >= line }
        if (index >= 0) current = index
    }

    val currentMatch: TextMatch? get() = current?.let { matches[it] }

    val status: String
        get() {
            if (query.isEmpty()) return ""
            if (matches.isEmpty()) return "No results"
            val more = if (matches.size >= MAX_MATCHES) "+" else ""
            return "${(current ?: 0) + 1} of ${matches.size}$more"
        }

    companion object {
        const val MAX_MATCHES = 5000

        private fun isWord(c: Char): Boolean = c.isLetterOrDigit() || c == '_'

        fun replacing(text: String, query: String, replacement: String, options: FindOptions = FindOptions()): Pair<String, Int> {
            val finder = FindInText()
            finder.options = options
            finder.search(query, text)
            if (finder.matches.isEmpty()) return text to 0
            val out = StringBuilder(text)
            for (match in finder.matches.asReversed()) out.replace(match.offset, match.offset + match.length, replacement)
            return out.toString() to finder.matches.size
        }
    }
}

object GoToLine {
    fun parse(text: String, lineCount: Int): Int? {
        val clean = text.trim().split(":").firstOrNull().orEmpty()
        val line = clean.toIntOrNull() ?: return null
        if (line <= 0 || lineCount <= 0) return null
        return minOf(line, lineCount)
    }
}

data class CodeSymbol(val name: String, val kind: String, val line: Int, val depth: Int)

data class CodeReference(val line: Int, val text: String, val isCall: Boolean)

object CodeOutline {
    const val MAX_SYMBOLS = 2000

    private val PATTERNS: List<Pair<String, Regex>> = listOf(
        "func" to """^\s*(?:(?:public|private|internal|fileprivate|open|static|class|override|final|mutating|nonisolated|@\w+(?:\([^)]*\))?)\s+)*func\s+([A-Za-z_][A-Za-z0-9_]*)""",
        "type" to """^\s*(?:(?:public|private|internal|fileprivate|open|final|export|default|abstract|pub(?:\([^)]*\))?)\s+)*(?:class|struct|enum|protocol|actor|extension|interface|trait|union)\s+([A-Za-z_][A-Za-z0-9_.]*)""",
        "func" to """^\s*(?:async\s+)?def\s+([A-Za-z_][A-Za-z0-9_]*)""",
        "type" to """^\s*class\s+([A-Za-z_][A-Za-z0-9_]*)""",
        "func" to """^\s*(?:export\s+)?(?:default\s+)?(?:async\s+)?function\s*\*?\s*([A-Za-z_${'$'}][A-Za-z0-9_${'$'}]*)""",
        "func" to """^\s*(?:pub(?:\([^)]*\))?\s+)?(?:async\s+)?(?:unsafe\s+)?(?:const\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)""",
        "func" to """^\s*func\s+(?:\([^)]*\)\s*)?([A-Za-z_][A-Za-z0-9_]*)""",
        "type" to """^\s*type\s+([A-Za-z_][A-Za-z0-9_]*)\s+(?:struct|interface)""",
        "func" to """^[A-Za-z_][\w:<>,\s\*&~]*?\b([A-Za-z_~][A-Za-z0-9_]*(?:::[A-Za-z_~][A-Za-z0-9_]*)*)\s*\([^;]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?\{?\s*${'$'}""",
        "const" to """^\s*(?:export\s+)?(?:const|let)\s+([A-Za-z_${'$'}][A-Za-z0-9_${'$'}]*)\s*=\s*(?:async\s*)?(?:\([^)]*\)|[A-Za-z_${'$'}][A-Za-z0-9_${'$'}]*)\s*=>""",
    ).map { it.first to Regex(it.second) }

    private val NOT_NAMES = setOf("if", "for", "while", "switch", "return", "catch", "else", "sizeof", "do")

    fun symbols(text: String): List<CodeSymbol> {
        val out = ArrayList<CodeSymbol>()
        for ((index, line) in text.split("\n").withIndex()) {
            if (line.length > 400) continue
            val trimmed = line.trim()
            if (trimmed.startsWith("//") || (trimmed.startsWith("#") && !trimmed.startsWith("#define")) || trimmed.startsWith("*")) continue
            for ((kind, re) in PATTERNS) {
                val match = re.find(line) ?: continue
                val symbol = match.groups[1]?.value ?: continue
                if (symbol in NOT_NAMES) break
                val indent = line.takeWhile { it == ' ' || it == '\t' }.sumOf { if (it == '\t') 4L else 1L }.toInt()
                out.add(CodeSymbol(symbol, kind, index + 1, minOf(indent / 4, 6)))
                break
            }
            if (out.size >= MAX_SYMBOLS) break
        }
        return out
    }

    fun references(name: String, text: String, excludingLine: Int? = null): List<CodeReference> {
        if (name.isEmpty()) return emptyList()
        val short = name.split(":").lastOrNull().orEmpty().ifEmpty { name }
        val out = ArrayList<CodeReference>()
        val finder = FindInText()
        finder.options = FindOptions(caseSensitive = true, wholeWord = true)
        for ((index, line) in text.split("\n").withIndex()) {
            if (index + 1 == excludingLine) continue
            finder.search(short, line)
            val match = finder.matches.firstOrNull() ?: continue
            var after = match.offset + match.length
            while (after < line.length && line[after] == ' ') after++
            out.add(CodeReference(index + 1, line.trim().take(160), after < line.length && line[after] == '('))
            if (out.size >= 500) break
        }
        return out
    }
}
