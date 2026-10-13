package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.code.diff.DiffFile
import com.forkmesh.nativeapp.code.diff.DiffFileStatus
import com.forkmesh.nativeapp.code.diff.DiffHunk
import com.forkmesh.nativeapp.code.diff.DiffLine
import com.forkmesh.nativeapp.code.diff.DiffLineKind
import com.forkmesh.nativeapp.code.diff.DiffSplitRow
import com.forkmesh.nativeapp.json.Json
import java.security.MessageDigest

class DiffReviewSection(val title: String = "", val files: List<DiffFile>)

data class DiffScrollPass(val lines: Int = 0, val changed: Int = 0, val flagged: Int = 0)

class DiffReviewFile(val index: Int, val section: Int, val key: String, val file: DiffFile, val fingerprint: String)

sealed class DiffReviewRow {
    abstract val id: Long
    abstract val file: Int?

    data class Section(val section: Int) : DiffReviewRow() {
        override val id: Long get() = makeId(section, 6, 0)
        override val file: Int? get() = null
    }

    data class Header(override val file: Int) : DiffReviewRow() {
        override val id: Long get() = headerId(file)
    }

    data class Notice(override val file: Int, val index: Int, val text: String) : DiffReviewRow() {
        override val id: Long get() = makeId(file, 1, index)
    }

    class Hunk(override val file: Int, val index: Int, val hunk: DiffHunk) : DiffReviewRow() {
        override val id: Long get() = makeId(file, 2, index)
    }

    class Line(override val file: Int, val ordinal: Int, val line: DiffLine) : DiffReviewRow() {
        override val id: Long get() = makeId(file, 3, ordinal)
    }

    class Pair(override val file: Int, val index: Int, val row: DiffSplitRow) : DiffReviewRow() {
        override val id: Long get() = makeId(file, 4, index)
    }

    data class End(override val file: Int) : DiffReviewRow() {
        override val id: Long get() = endId(file)
    }

    companion object {
        fun makeId(owner: Int, tag: Int, local: Int): Long = (owner.toLong() shl 32) or (tag.toLong() shl 28) or (local.toLong() and 0x0FFF_FFFFL)

        fun headerId(file: Int): Long = makeId(file, 0, 0)

        fun endId(file: Int): Long = makeId(file, 5, 0)
    }
}

data class DiffFindMatch(val row: Long, val right: Boolean, val start: Int, val length: Int)

object DiffReviewText {
    const val FIND_HINT = "Find in diff…"
    const val EMPTY_TITLE = "No changes"

    fun viewedCount(seen: Int, total: Int): String = "$seen of $total files viewed"

    fun kinds(files: List<DiffFile>): String {
        var added = 0
        var removed = 0
        var renamed = 0
        var binaries = 0
        for (file in files) {
            if (file.binary) binaries++
            when (file.status) {
                DiffFileStatus.Added -> added++
                DiffFileStatus.Deleted -> removed++
                DiffFileStatus.Renamed -> renamed++
                else -> Unit
            }
        }
        val modified = files.size - added - removed - renamed
        return buildList {
            if (modified > 0) add("$modified modified")
            if (added > 0) add("$added added")
            if (removed > 0) add("$removed removed")
            if (renamed > 0) add("$renamed renamed")
            if (binaries > 0) add("$binaries binary")
        }.joinToString(", ")
    }

    fun remaining(left: Int): String = if (left == 0) "every file reviewed" else "$left left to review"

    fun countsTooltip(file: DiffFile): String {
        val total = file.additions + file.deletions
        return "+${file.additions} −${file.deletions} · $total changed line${if (total == 1) "" else "s"}"
    }

    fun viewedHint(viewed: Boolean): String = if (viewed) "Reopen this file" else "Collapse this file and mark it viewed"

    fun foldHint(folded: Boolean): String = if (folded) "Expand this file (it stays marked viewed)" else "Collapse this file"

    fun findCount(index: Int, total: Int): String = if (total == 0) "No results" else "${index + 1}/$total"

    fun name(file: DiffFile): String = file.path.substringAfterLast('/')

    fun detail(file: DiffFile): String {
        val moved = (file.status == DiffFileStatus.Renamed || file.status == DiffFileStatus.Copied) && file.oldPath != file.newPath
        if (moved) return "${file.oldPath} →"
        val slash = file.path.lastIndexOf('/')
        return if (slash < 0) "" else file.path.substring(0, slash + 1)
    }
}

class DiffReviewModel(
    sections: List<DiffReviewSection>,
    val scope: String,
    split: Boolean = false,
    val git: DiffHealthGit = DiffHealthGit.Unknown,
    private val store: PrefStore? = null,
) {
    val sections: List<DiffReviewSection> = sections.filter { it.files.isNotEmpty() }
    val files: List<DiffReviewFile>
    val longestColumns: Int
    val gutterDigits: Int

    var onChange: (() -> Unit)? = null

    var viewed: Set<String> = emptySet()
        private set
    var expanded: Set<String> = emptySet()
        private set
    var rows: List<DiffReviewRow> = emptyList()
        private set
    var split: Boolean = split
        set(value) {
            if (field == value) return
            field = value
            rebuild()
            changed()
        }
    var query: String = ""
        set(value) {
            if (field == value) return
            field = value
            refind()
            changed()
        }
    var matches: List<DiffFindMatch> = emptyList()
        private set
    var matchIndex: Int = -1
        private set

    private var positions: HashMap<Long, Int> = HashMap()
    private var hunkPositions: IntArray = IntArray(0)
    private var matchRows: Map<Long, List<DiffFindMatch>> = emptyMap()
    private val flagCache = HashMap<DiffLine, List<DiffLineFlag>>()
    private val pendingEnds = HashSet<Int>()
    private var healthCache: List<DiffHealthMetric>? = null

    init {
        val list = ArrayList<DiffReviewFile>()
        this.sections.forEachIndexed { s, section ->
            for (file in section.files) {
                file.prepare()
                val key = if (this.sections.size > 1) "$s:${file.path}" else file.path
                list.add(DiffReviewFile(list.size, s, key, file, fingerprint(file.raw)))
            }
        }
        files = list
        longestColumns = (list.maxOfOrNull { it.file.longestLine } ?: 0).coerceIn(0, 400)
        gutterDigits = (list.maxOfOrNull { it.file.maxLineNumber } ?: 0).toString().length.coerceIn(2, 7)
        restore()
        rebuild()
    }

    val allFiles: List<DiffFile> get() = files.map { it.file }

    fun isViewed(index: Int): Boolean = index in files.indices && files[index].key in viewed

    fun isFolded(index: Int): Boolean {
        if (index !in files.indices) return false
        val key = files[index].key
        return key in viewed && key !in expanded
    }

    val seen: Int get() = files.count { it.key in viewed }
    val progress: Float get() = if (files.isEmpty()) 0f else seen.toFloat() / files.size
    val viewedLabel: String get() = DiffReviewText.viewedCount(seen, files.size)
    val remainingLabel: String get() = DiffReviewText.remaining(files.size - seen)
    val kindsLabel: String get() = DiffReviewText.kinds(allFiles)
    val additions: Int get() = files.sumOf { it.file.additions }
    val deletions: Int get() = files.sumOf { it.file.deletions }

    fun toggleViewed(index: Int): Int? {
        if (index !in files.indices) return null
        val key = files[index].key
        expanded = expanded - key
        if (key in viewed) {
            viewed = viewed - key
            persist()
            rebuild()
            changed()
            return null
        }
        viewed = viewed + key
        persist()
        rebuild()
        changed()
        return if (index + 1 < files.size) index + 1 else null
    }

    fun toggleFold(index: Int) {
        if (!isViewed(index)) return
        val key = files[index].key
        expanded = if (key in expanded) expanded - key else expanded + key
        rebuild()
        changed()
    }

    fun autoMark(index: Int): Boolean {
        if (index !in files.indices || isViewed(index)) return false
        val key = files[index].key
        viewed = viewed + key
        expanded = expanded + key
        persist()
        changed()
        return true
    }

    fun endAppeared(index: Int) {
        pendingEnds.add(index)
    }

    fun endDisappeared(index: Int) {
        pendingEnds.remove(index)
    }

    fun settleScroll() {
        for (index in pendingEnds.sorted()) autoMark(index)
    }

    fun autoMarkPassed(from: Int, to: Int): List<Int> {
        val start = maxOf(from, -1)
        val stop = minOf(to, rows.size - 1)
        if (stop <= start) return emptyList()
        return (start + 1..stop).mapNotNull { p -> (rows[p] as? DiffReviewRow.End)?.file?.takeIf { autoMark(it) } }
    }

    fun scrollPass(from: Int, to: Int): DiffScrollPass {
        val start = minOf(from, to).coerceAtLeast(-1)
        val stop = maxOf(from, to).coerceAtMost(rows.lastIndex)
        var lines = 0
        var changed = 0
        var flagged = 0
        for (p in start + 1..stop) {
            val (file, sides) = when (val row = rows[p]) {
                is DiffReviewRow.Line -> row.file to listOf(row.line)
                is DiffReviewRow.Pair -> row.file to listOfNotNull(row.row.left, row.row.right)
                is DiffReviewRow.Hunk -> row.file to emptyList()
                else -> continue
            }
            lines++
            if (sides.any { it.kind == DiffLineKind.Added || it.kind == DiffLineKind.Removed }) changed++
            if (sides.any { flags(file, it).isNotEmpty() }) flagged++
        }
        return DiffScrollPass(lines, changed, flagged)
    }

    fun nextUnviewed(after: Int): Int? {
        val count = files.size
        for (step in 1..count) {
            val candidate = Math.floorMod(after + step, count)
            if (candidate != after && !isViewed(candidate)) return candidate
        }
        return null
    }

    fun position(rowId: Long): Int? = positions[rowId]

    fun fileAtPosition(position: Int): Int? {
        var p = position.coerceAtMost(rows.lastIndex)
        while (p >= 0) {
            rows[p].file?.let { return it }
            p--
        }
        return null
    }

    fun nextChange(fromPosition: Int): Int? = hunkPositions.firstOrNull { it > fromPosition }

    fun previousChange(fromPosition: Int): Int? = hunkPositions.lastOrNull { it < fromPosition }

    private fun rebuild() {
        val out = ArrayList<DiffReviewRow>()
        var lastSection = -1
        for (entry in files) {
            if (sections.size > 1 && entry.section != lastSection) {
                lastSection = entry.section
                out.add(DiffReviewRow.Section(entry.section))
            }
            val i = entry.index
            out.add(DiffReviewRow.Header(i))
            if (isFolded(i)) continue
            DiffFileFilter.notices(entry.file).forEachIndexed { n, note -> out.add(DiffReviewRow.Notice(i, n, note)) }
            if (split) {
                var hunk = 0
                entry.file.splitRows.forEachIndexed { r, row ->
                    val header = row.hunk
                    if (header != null) out.add(DiffReviewRow.Hunk(i, hunk++, header)) else out.add(DiffReviewRow.Pair(i, r, row))
                }
            } else {
                var ordinal = 0
                entry.file.hunks.forEachIndexed { h, hunk ->
                    out.add(DiffReviewRow.Hunk(i, h, hunk))
                    for (line in hunk.lines) out.add(DiffReviewRow.Line(i, ordinal++, line))
                }
            }
            out.add(DiffReviewRow.End(i))
        }
        rows = out
        val map = HashMap<Long, Int>(out.size * 2)
        val hunks = ArrayList<Int>()
        out.forEachIndexed { p, row ->
            map[row.id] = p
            if (row is DiffReviewRow.Hunk) hunks.add(p)
        }
        positions = map
        hunkPositions = hunks.toIntArray()
        refind()
    }

    fun marks(file: Int, line: DiffLine): List<DiffLineMark> = DiffLineMark.of(flags(file, line))

    fun flags(file: Int, line: DiffLine): List<DiffLineFlag> {
        if (line.kind != DiffLineKind.Added || file !in files.indices) return emptyList()
        return flagCache.getOrPut(line) { DiffHealth.flags(files[file].file.path, line.text) }
    }

    val health: List<DiffHealthMetric>
        get() = healthCache ?: DiffHealth.report(allFiles, git).also { healthCache = it }

    fun matches(rowId: Long, right: Boolean): List<DiffFindMatch> = matchRows[rowId]?.filter { it.right == right } ?: emptyList()

    val currentMatch: DiffFindMatch? get() = matches.getOrNull(matchIndex)

    val findLabel: String get() = DiffReviewText.findCount(matchIndex, matches.size)

    fun stepMatch(forward: Boolean): Long? {
        if (matches.isEmpty()) return null
        matchIndex = if (forward) (matchIndex + 1) % matches.size else (matchIndex - 1 + matches.size) % matches.size
        changed()
        return matches[matchIndex].row
    }

    private fun refind() {
        val needle = query
        val found = ArrayList<DiffFindMatch>()
        val byRow = HashMap<Long, MutableList<DiffFindMatch>>()
        fun scan(text: String, row: Long, right: Boolean) {
            var from = 0
            while (from < text.length && found.size < MAX_MATCHES) {
                val hit = text.indexOf(needle, from, ignoreCase = true)
                if (hit < 0) break
                val match = DiffFindMatch(row, right, hit, needle.length)
                found.add(match)
                byRow.getOrPut(row) { ArrayList() }.add(match)
                from = hit + needle.length
            }
        }
        if (needle.isNotEmpty()) {
            for (row in rows) {
                if (found.size >= MAX_MATCHES) break
                when (row) {
                    is DiffReviewRow.Line -> scan(row.line.text, row.id, true)
                    is DiffReviewRow.Pair -> {
                        val left = row.row.left
                        if (left != null && left.kind != DiffLineKind.Context) scan(left.text, row.id, false)
                        row.row.right?.let { scan(it.text, row.id, true) }
                    }
                    else -> Unit
                }
            }
        }
        matches = found
        matchRows = byRow
        matchIndex = if (found.isEmpty()) -1 else 0
    }

    private fun changed() {
        onChange?.invoke()
    }

    private val storeKey: String get() = "diffViewed/$scope"

    private fun readList(key: String): List<String> {
        val raw = store?.getString(key) ?: return emptyList()
        return (runCatching { Json.parse(raw) }.getOrNull() as? List<*>)?.map { it.toString() } ?: emptyList()
    }

    private fun restore() {
        if (store == null || scope.isEmpty()) return
        val current = HashMap<String, String>()
        for (file in files) current.putIfAbsent(file.key, file.fingerprint)
        val kept = HashSet<String>()
        for (mark in readList(storeKey)) {
            val space = mark.indexOf(' ')
            if (space < 0) continue
            val key = mark.substring(space + 1)
            if (current[key] == mark.substring(0, space)) kept.add(key)
        }
        viewed = kept
    }

    private fun persist() {
        val store = store ?: return
        if (scope.isEmpty()) return
        val known = files.map { it.key }.toSet()
        val marks = files.filter { it.key in viewed }.map { "${it.fingerprint} ${it.key}" }
        val others = readList(storeKey).filter { mark ->
            val space = mark.indexOf(' ')
            space >= 0 && mark.substring(space + 1) !in known
        }
        val all = marks + others
        if (all.isEmpty()) store.remove(storeKey) else store.putString(storeKey, Json.stringify(all))
        val scopes = readList(SCOPES_KEY).filter { it != scope }.toMutableList()
        scopes.add(0, scope)
        while (scopes.size > MAX_SCOPES) store.remove("diffViewed/" + scopes.removeAt(scopes.lastIndex))
        store.putString(SCOPES_KEY, Json.stringify(scopes))
    }

    companion object {
        const val MAX_MATCHES = 5000
        const val MAX_SCOPES = 200
        const val FINGERPRINT_CHARS = 16
        private const val SCOPES_KEY = "diffViewed/scopes"

        fun fingerprint(raw: String): String {
            val digest = MessageDigest.getInstance("SHA-1").digest(raw.toByteArray(Charsets.UTF_8))
            return digest.joinToString("") { "%02x".format(it.toInt() and 0xff) }.take(FINGERPRINT_CHARS)
        }

        fun patchScope(patch: String): String = "patch/" + fingerprint(patch)

        fun of(files: List<DiffFile>, scope: String, split: Boolean = false, git: DiffHealthGit = DiffHealthGit.Unknown, store: PrefStore? = null) =
            DiffReviewModel(listOf(DiffReviewSection(files = files)), scope, split, git, store)
    }
}
