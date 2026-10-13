package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.api.PublishedMarkdown
import com.forkmesh.nativeapp.code.diff.DiffLine
import com.forkmesh.nativeapp.code.diff.DiffLineKind as UnifiedLineKind
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.str
import java.util.Base64
import kotlin.math.abs

const val REVIEW_ANCHOR_MAX_LINES = 12

enum class DiffSide(val raw: String) {
    Old("old"), New("new");

    companion object {
        fun wire(value: String): DiffSide = if (value == "old") Old else New
    }
}

enum class ReviewThreadPlacement { Current, Moved, Outdated, Legacy }

class DiffSideIndex(val lines: MutableList<Int> = ArrayList(), val texts: MutableList<String> = ArrayList()) {
    fun indexOfLine(line: Int): Int {
        var lo = 0
        var hi = lines.size - 1
        while (lo <= hi) {
            val mid = (lo + hi) / 2
            when {
                lines[mid] == line -> return mid
                lines[mid] < line -> lo = mid + 1
                else -> hi = mid - 1
            }
        }
        return -1
    }

    fun contains(line: Int): Boolean = indexOfLine(line) >= 0
}

class DiffSideIndexes(patch: String) {
    private val sides = HashMap<String, DiffSideIndex>()

    init {
        var path = ""
        var inHunk = false
        var binary = false
        var oldNo = 0
        var newNo = 0
        var oldSide = DiffSideIndex()
        var newSide = DiffSideIndex()
        fun flush() {
            if (path.isEmpty()) return
            sides.getOrPut(key(path, DiffSide.Old)) { DiffSideIndex() }.also {
                it.lines.addAll(oldSide.lines)
                it.texts.addAll(oldSide.texts)
            }
            sides.getOrPut(key(path, DiffSide.New)) { DiffSideIndex() }.also {
                it.lines.addAll(newSide.lines)
                it.texts.addAll(newSide.texts)
            }
        }
        for (raw in patch.split("\n")) {
            val line = raw.removeSuffix("\r")
            if (line.startsWith("diff --git ")) {
                flush()
                oldSide = DiffSideIndex()
                newSide = DiffSideIndex()
                val marker = line.indexOf(" b/")
                path = if (marker >= 0) line.substring(marker + 3) else ""
                inHunk = false
                binary = false
                continue
            }
            if (path.isEmpty() || binary) continue
            if (line.startsWith("@@")) {
                val starts = hunkStarts(line)
                if (starts != null) {
                    oldNo = starts.first
                    newNo = starts.second
                    inHunk = true
                }
            } else if (line.startsWith("GIT binary patch") || line.startsWith("Binary files ")) {
                binary = true
                inHunk = false
            } else if (inHunk && line.isNotEmpty()) {
                val marker = line[0]
                if (marker == '+' && !line.startsWith("+++ ")) {
                    newSide.lines.add(newNo)
                    newSide.texts.add(line.substring(1))
                    newNo++
                } else if (marker == '-' && !line.startsWith("--- ")) {
                    oldSide.lines.add(oldNo)
                    oldSide.texts.add(line.substring(1))
                    oldNo++
                } else if (marker == ' ') {
                    val context = line.substring(1)
                    oldSide.lines.add(oldNo)
                    oldSide.texts.add(context)
                    newSide.lines.add(newNo)
                    newSide.texts.add(context)
                    oldNo++
                    newNo++
                } else if (marker != '\\') {
                    inHunk = false
                }
            } else if (inHunk && line.isEmpty()) {
                oldSide.lines.add(oldNo)
                oldSide.texts.add("")
                newSide.lines.add(newNo)
                newSide.texts.add("")
                oldNo++
                newNo++
            }
        }
        flush()
    }

    fun side(path: String, side: DiffSide): DiffSideIndex? = sides[key(path, side)]

    companion object {
        fun key(path: String, side: DiffSide): String = path + "\u001F" + side.raw

        fun hunkStarts(line: String): Pair<Int, Int>? {
            var rest = line.substring(2)
            if (!rest.startsWith(" -")) return null
            rest = rest.substring(2)
            fun number(): Int? {
                val digits = rest.takeWhile { it in '0'..'9' }
                if (digits.isEmpty()) return null
                rest = rest.substring(digits.length)
                return digits.toIntOrNull()
            }
            fun skipCount(): Boolean {
                if (!rest.startsWith(",")) return true
                rest = rest.substring(1)
                return number() != null
            }
            val oldStart = number() ?: return null
            if (!skipCount() || !rest.startsWith(" +")) return null
            rest = rest.substring(2)
            val newStart = number() ?: return null
            if (!skipCount() || !rest.startsWith(" @@")) return null
            return oldStart to newStart
        }
    }
}

object ReviewAnchoring {
    fun anchorLines(side: DiffSideIndex, lineStart: Int, lineEnd: Int): String {
        if (lineStart <= 0) return ""
        val end = maxOf(lineEnd, lineStart)
        val first = side.indexOfLine(lineStart)
        if (first < 0) return ""
        val out = ArrayList<String>()
        val wanted = minOf(end - lineStart + 1, REVIEW_ANCHOR_MAX_LINES)
        for (i in 0 until wanted) {
            val at = first + i
            if (at >= side.lines.size || side.lines[at] != lineStart + i) break
            out.add(side.texts[at])
        }
        return out.joinToString("\n")
    }

    fun anchorLines(patch: String, path: String, side: DiffSide, lineStart: Int, lineEnd: Int): String {
        if (path.isEmpty() || lineStart <= 0) return ""
        val index = DiffSideIndexes(patch).side(path, side) ?: return ""
        return anchorLines(index, lineStart, lineEnd)
    }

    fun nearestMatch(side: DiffSideIndex, needle: String, preferred: Int): Int {
        val want = needle.split("\n")
        if (want.isEmpty()) return -1
        var best = -1
        var i = 0
        while (i + want.size <= side.texts.size) {
            if (side.texts[i] == want[0]) {
                var ok = true
                for (k in 1 until want.size) {
                    if (side.texts[i + k] != want[k] || side.lines[i + k] != side.lines[i] + k) {
                        ok = false
                        break
                    }
                }
                if (ok) {
                    val line = side.lines[i]
                    if (best < 0 || abs(line - preferred) < abs(best - preferred)) best = line
                    if (line >= preferred) break
                }
            }
            i++
        }
        return best
    }

    data class Anchor(
        val path: String,
        val side: DiffSide,
        val lineStart: Int,
        val lineEnd: Int,
        val patchset: String = "",
        val anchorLines: String = "",
    )

    data class Placement(val placement: ReviewThreadPlacement, val currentLineStart: Int, val currentLineEnd: Int) {
        val isOutdated: Boolean get() = placement == ReviewThreadPlacement.Outdated
    }

    fun place(anchor: Anchor, side: DiffSideIndex?, currentPatchset: String): Placement {
        if (anchor.path.isEmpty() || anchor.lineStart <= 0) return Placement(ReviewThreadPlacement.Legacy, 0, 0)
        val span = maxOf(0, anchor.lineEnd - anchor.lineStart)
        fun at(placement: ReviewThreadPlacement, start: Int) = Placement(placement, start, start + span)
        val outdated = Placement(ReviewThreadPlacement.Outdated, 0, 0)
        val lineExists = side?.contains(anchor.lineStart) ?: false
        if (anchor.patchset.isEmpty() && anchor.anchorLines.isEmpty()) {
            return if (lineExists) at(ReviewThreadPlacement.Legacy, anchor.lineStart) else outdated
        }
        if (anchor.patchset.isNotEmpty() && currentPatchset.isNotEmpty() && anchor.patchset.lowercase() == currentPatchset.lowercase()) {
            return at(ReviewThreadPlacement.Current, anchor.lineStart)
        }
        if (anchor.anchorLines.isNotEmpty()) {
            val found = side?.let { nearestMatch(it, anchor.anchorLines, anchor.lineStart) } ?: -1
            if (found < 0) return outdated
            return at(if (found == anchor.lineStart) ReviewThreadPlacement.Current else ReviewThreadPlacement.Moved, found)
        }
        return if (currentPatchset.isEmpty() && lineExists) at(ReviewThreadPlacement.Current, anchor.lineStart) else outdated
    }

    fun label(placement: ReviewThreadPlacement, lineStart: Int, lineEnd: Int): String = when (placement) {
        ReviewThreadPlacement.Outdated -> "Outdated"
        ReviewThreadPlacement.Moved -> if (lineEnd > lineStart) "Moved from lines $lineStart–$lineEnd" else "Moved from line $lineStart"
        else -> ""
    }
}

data class ReviewEvent(
    val type: String,
    val id: String = "",
    val author: String = "",
    val authorName: String = "",
    val ts: Long = 0,
    val state: String = "",
    val path: String = "",
    val side: String = "",
    val line: Int = 0,
    val lineStart: Int = 0,
    val lineEnd: Int = 0,
    val threadId: String = "",
    val parentId: String = "",
    val suggestionPatch: String = "",
    val targetPath: String = "",
    val appliedCommit: String = "",
    val patchset: String = "",
    val anchorLines: String = "",
    val body: String = "",
    val sig: String = "",
) {
    val displayAuthor: String get() = authorName.ifEmpty { author }

    val reviewStanding: String
        get() {
            if (type != "review") return ""
            return when (state) {
                "approve", "approved" -> "approved"
                "request-changes", "changes-requested", "changes_requested", "requested_changes" -> "changes_requested"
                else -> ""
            }
        }

    companion object {
        fun fromMarkdown(md: String): ReviewEvent {
            val meta = PublishedMarkdown.frontMatter(md)
            fun text(key: String) = meta[key] ?: ""
            fun number(key: String) = text(key).trim().toIntOrNull() ?: 0
            return ReviewEvent(
                type = meta["type"] ?: "comment",
                id = text("id"),
                author = text("author"),
                authorName = text("authorName"),
                ts = text("ts").trim().toLongOrNull() ?: 0,
                state = text("state"),
                path = text("path"),
                side = text("side"),
                line = number("line"),
                lineStart = number("lineStart"),
                lineEnd = number("lineEnd"),
                threadId = text("threadId"),
                parentId = text("parentId"),
                suggestionPatch = decodeBlob(text("suggestionPatchB64")),
                targetPath = text("targetPath"),
                appliedCommit = text("appliedCommit"),
                patchset = text("patchset"),
                anchorLines = decodeBlob(text("anchorLinesB64")),
                body = PublishedMarkdown.bodyWithoutFrontMatter(md),
                sig = text("sig"),
            )
        }

        fun fromJson(j: Map<String, Any?>): ReviewEvent = ReviewEvent(
            type = j.str("type").ifEmpty { "comment" },
            id = j.str("id"),
            author = j.str("author"),
            authorName = j.str("authorName"),
            ts = (j["ts"] as? Number)?.toLong() ?: j.str("ts").toLongOrNull() ?: 0,
            state = j.str("state"),
            path = j.str("path"),
            side = j.str("side"),
            line = j.int("line"),
            lineStart = j.int("lineStart"),
            lineEnd = j.int("lineEnd"),
            threadId = j.str("threadId"),
            parentId = j.str("parentId"),
            suggestionPatch = j.str("suggestionPatch"),
            targetPath = j.str("targetPath"),
            appliedCommit = j.str("appliedCommit"),
            patchset = j.str("patchset"),
            anchorLines = j.str("anchorLines"),
            body = j.str("body"),
            sig = j.str("sig"),
        )

        fun decodeBlob(text: String): String {
            if (text.isEmpty()) return ""
            return try {
                val normal = text.replace('-', '+').replace('_', '/')
                val padded = normal + "=".repeat((4 - normal.length % 4) % 4)
                String(Base64.getDecoder().decode(padded), Charsets.UTF_8)
            } catch (_: IllegalArgumentException) {
                ""
            }
        }

        fun encodeBlob(text: String): String =
            if (text.isEmpty()) "" else Base64.getUrlEncoder().withoutPadding().encodeToString(text.toByteArray(Charsets.UTF_8))
    }
}

object ReviewRecordRules {
    fun commitOid(text: String): String {
        val value = text.trim().lowercase()
        if (value.length != 40 && value.length != 64) return ""
        return if (value.all { it in '0'..'9' || it in 'a'..'f' }) value else ""
    }
}

data class PullReviewRecord(
    val number: Int,
    val title: String = "",
    val status: String = "open",
    val author: String = "",
    val authorName: String = "",
    val base: String = "",
    val head: String = "",
    val branchBacked: Boolean = false,
    val creationBaseOid: String = "",
    val creationHeadOid: String = "",
    val description: String = "",
    val patch: String = "",
    val events: List<ReviewEvent> = emptyList(),
) {
    val currentPatchset: String get() = creationHeadOid
    val isOpen: Boolean get() = status.trim().lowercase() == "open"

    companion object {
        fun fromMarkdown(number: Int, md: String, events: List<ReviewEvent> = emptyList(), patch: String = ""): PullReviewRecord {
            val meta = PublishedMarkdown.frontMatter(md)
            val body = PublishedMarkdown.bodyWithoutFrontMatter(md)
            val inlinePatch = meta["patch"] ?: meta["diff"] ?: PublishedMarkdown.extractPatch(body)
            return PullReviewRecord(
                number = meta["number"]?.trim()?.toIntOrNull() ?: number,
                title = meta["title"] ?: PublishedMarkdown.firstHeading(md) ?: "Pull request #$number",
                status = meta["status"] ?: "open",
                author = meta["author"] ?: "",
                authorName = meta["authorName"] ?: "",
                base = meta["base"] ?: "",
                head = meta["head"] ?: "",
                branchBacked = meta["derive"] == "branch",
                creationBaseOid = ReviewRecordRules.commitOid(meta["creationBaseOid"] ?: ""),
                creationHeadOid = ReviewRecordRules.commitOid(meta["creationHeadOid"] ?: ""),
                description = body,
                patch = patch.ifEmpty { inlinePatch },
                events = events,
            )
        }
    }
}

data class ReviewThread(
    val id: String,
    val path: String = "",
    val side: DiffSide = DiffSide.New,
    val lineStart: Int = 0,
    val lineEnd: Int = 0,
    val resolved: Boolean = false,
    val hasSuggestion: Boolean = false,
    val suggestionState: String = "",
    val appliedCommit: String = "",
    val patchset: String = "",
    val anchorLines: String = "",
    val placement: ReviewThreadPlacement = ReviewThreadPlacement.Legacy,
    val currentLineStart: Int = 0,
    val currentLineEnd: Int = 0,
    val events: List<ReviewEvent> = emptyList(),
) {
    val isOutdated: Boolean get() = placement == ReviewThreadPlacement.Outdated
    val isMoved: Boolean get() = placement == ReviewThreadPlacement.Moved
    val placementLabel: String get() = ReviewAnchoring.label(placement, lineStart, lineEnd)
    val opening: ReviewEvent? get() = events.firstOrNull()
    val lastEventId: String get() = events.lastOrNull { it.id.isNotEmpty() }?.id ?: id

    val paintedLine: Int?
        get() = if (isOutdated || path.isEmpty() || currentLineEnd <= 0) null else currentLineEnd
}

data class ReviewFileSummary(
    val path: String,
    val totalThreads: Int = 0,
    val unresolvedThreads: Int = 0,
    val resolvedThreads: Int = 0,
    val suggestions: Int = 0,
    val outdatedThreads: Int = 0,
)

data class ReviewSnapshot(
    val reviewSummary: String = "",
    val topLevelItems: Int = 0,
    val threads: List<ReviewThread> = emptyList(),
    val files: Map<String, ReviewFileSummary> = emptyMap(),
) {
    val totalThreads: Int get() = threads.size
    val unresolvedThreads: Int get() = threads.count { !it.resolved }
    val resolvedThreads: Int get() = threads.count { it.resolved }
    val suggestions: Int get() = threads.count { it.hasSuggestion }
    val outdatedThreads: Int get() = threads.count { it.isOutdated }

    fun thread(id: String): ReviewThread? = threads.firstOrNull { it.id == id }

    fun threadsByLine(path: String): Map<DiffSide, Map<Int, List<ReviewThread>>> {
        val out = HashMap<DiffSide, HashMap<Int, ArrayList<ReviewThread>>>()
        for (thread in threads) {
            if (thread.path != path) continue
            val line = thread.paintedLine ?: continue
            out.getOrPut(thread.side) { HashMap() }.getOrPut(line) { ArrayList() }.add(thread)
        }
        return out
    }

    fun outdatedThreads(path: String): List<ReviewThread> = threads.filter { it.path == path && it.paintedLine == null && it.isOutdated }

    fun fileThreads(path: String): List<ReviewThread> = threads.filter { it.path == path && it.lineStart <= 0 }
}

object ReviewSnapshotBuilder {
    fun build(events: List<ReviewEvent>, patch: String, currentPatchset: String): ReviewSnapshot {
        val threads = ArrayList<ReviewThread>()
        val indexById = HashMap<String, Int>()
        var topLevel = 0

        fun ensure(id: String): Int {
            val key = id.ifEmpty { "thread:${threads.size + 1}" }
            indexById[key]?.let { return it }
            threads.add(ReviewThread(key))
            indexById[key] = threads.size - 1
            return threads.size - 1
        }

        fun copyAnchor(index: Int, ev: ReviewEvent) {
            var thread = threads[index]
            if (ev.path.isNotEmpty()) thread = thread.copy(path = ev.path)
            if (ev.side.isNotEmpty()) thread = thread.copy(side = DiffSide.wire(ev.side))
            if (ev.lineStart > 0) thread = thread.copy(lineStart = ev.lineStart)
            if (ev.lineEnd > 0) thread = thread.copy(lineEnd = ev.lineEnd)
            if (thread.lineStart == 0 && ev.line > 0) thread = thread.copy(lineStart = ev.line)
            if (thread.lineEnd == 0 && ev.line > 0) thread = thread.copy(lineEnd = ev.line)
            if (ev.patchset.isNotEmpty()) thread = thread.copy(patchset = ev.patchset)
            if (ev.anchorLines.isNotEmpty()) thread = thread.copy(anchorLines = ev.anchorLines)
            threads[index] = thread
        }

        fun append(index: Int, ev: ReviewEvent) {
            threads[index] = threads[index].copy(events = threads[index].events + ev)
        }

        events.forEachIndexed { i, ev ->
            when (ev.type) {
                "comment", "review" -> topLevel++
                "line-comment" -> {
                    val index = ensure(ev.id.ifEmpty { "legacy:${ev.path}:${ev.side}:${ev.line}:$i" })
                    copyAnchor(index, ev)
                    append(index, ev)
                }
                "thread-comment" -> {
                    val index = ensure(ev.threadId)
                    copyAnchor(index, ev)
                    if (ev.suggestionPatch.isNotEmpty()) threads[index] = threads[index].copy(hasSuggestion = true)
                    append(index, ev)
                }
                "thread-reply" -> append(ensure(ev.threadId), ev)
                "thread-state" -> {
                    val index = ensure(ev.threadId)
                    threads[index] = threads[index].copy(resolved = ev.state == "resolved")
                    append(index, ev)
                }
                "suggestion-state" -> {
                    val index = ensure(ev.threadId)
                    threads[index] = threads[index].copy(suggestionState = ev.state, appliedCommit = ev.appliedCommit)
                    append(index, ev)
                }
            }
        }

        var indexes: DiffSideIndexes? = null
        for (i in threads.indices) {
            val thread = threads[i]
            if (thread.path.isEmpty() || thread.lineStart <= 0) {
                threads[i] = thread.copy(placement = ReviewThreadPlacement.Legacy, currentLineStart = 0, currentLineEnd = 0)
                continue
            }
            val sides = indexes ?: DiffSideIndexes(patch).also { indexes = it }
            val placed = ReviewAnchoring.place(
                ReviewAnchoring.Anchor(thread.path, thread.side, thread.lineStart, thread.lineEnd, thread.patchset, thread.anchorLines),
                sides.side(thread.path, thread.side),
                currentPatchset,
            )
            threads[i] = thread.copy(
                placement = placed.placement,
                currentLineStart = placed.currentLineStart,
                currentLineEnd = placed.currentLineEnd,
            )
        }

        val files = LinkedHashMap<String, ReviewFileSummary>()
        for (thread in threads) {
            if (thread.path.isEmpty()) continue
            val summary = files[thread.path] ?: ReviewFileSummary(thread.path)
            files[thread.path] = summary.copy(
                totalThreads = summary.totalThreads + 1,
                resolvedThreads = summary.resolvedThreads + if (thread.resolved) 1 else 0,
                unresolvedThreads = summary.unresolvedThreads + if (thread.resolved) 0 else 1,
                suggestions = summary.suggestions + if (thread.hasSuggestion) 1 else 0,
                outdatedThreads = summary.outdatedThreads + if (thread.isOutdated) 1 else 0,
            )
        }
        return ReviewSnapshot(ReviewGate.summary(events), topLevel, threads, files)
    }

    fun build(record: PullReviewRecord): ReviewSnapshot = build(record.events, record.patch, record.currentPatchset)
}

object ReviewGate {
    fun standings(events: List<ReviewEvent>, excluding: String? = null): List<Pair<String, String>> {
        val order = ArrayList<String>()
        val latest = HashMap<String, String>()
        for (ev in events) {
            if (ev.type != "review") continue
            if (excluding != null && (ev.author.isEmpty() || ev.author == excluding)) continue
            val standing = ev.reviewStanding
            if (standing.isEmpty()) {
                latest.remove(ev.author)
            } else {
                if (latest[ev.author] == null && ev.author !in order) order.add(ev.author)
                latest[ev.author] = standing
            }
        }
        return order.mapNotNull { who -> latest[who]?.let { who to it } }
    }

    fun summary(events: List<ReviewEvent>): String {
        val all = standings(events)
        if (all.any { it.second == "changes_requested" }) return "changes_requested"
        return if (all.any { it.second == "approved" }) "approved" else ""
    }

    fun changesRequestedBy(events: List<ReviewEvent>, author: String): List<String> {
        val names = HashMap<String, String>()
        for (ev in events) names[ev.author] = ev.displayAuthor
        return standings(events, author).filter { it.second == "changes_requested" }.map { names[it.first] ?: it.first }
    }

    fun independentApprovals(events: List<ReviewEvent>, author: String): Int =
        standings(events, author).count { it.second == "approved" }

    fun satisfied(events: List<ReviewEvent>, author: String): Boolean =
        changesRequestedBy(events, author).isEmpty() && independentApprovals(events, author) > 0
}

data class DiffLineSelection(val path: String, val side: DiffSide, val startLine: Int, val endLine: Int = startLine) {
    val lineStart: Int get() = minOf(startLine, endLine)
    val lineEnd: Int get() = maxOf(startLine, endLine)
    val isRange: Boolean get() = lineStart != lineEnd

    fun extended(line: DiffLine): DiffLineSelection {
        val other = of(path, line) ?: return this
        if (other.side != side) return this
        var copy = copy(endLine = other.startLine)
        if (copy.lineEnd - copy.lineStart + 1 > ReviewComposer.MAX_SELECTION_LINES) {
            val direction = if (other.startLine >= copy.startLine) 1 else -1
            copy = copy.copy(endLine = copy.startLine + direction * (ReviewComposer.MAX_SELECTION_LINES - 1))
        }
        return copy
    }

    fun contains(line: DiffLine): Boolean {
        val other = of(path, line) ?: return false
        return other.side == side && other.startLine in lineStart..lineEnd
    }

    val label: String
        get() {
            val range = if (isRange) "lines $lineStart–$lineEnd" else "line $lineStart"
            return "$path · ${side.raw} $range"
        }

    companion object {
        fun next(current: DiffLineSelection?, path: String, line: DiffLine): DiffLineSelection? {
            val picked = of(path, line) ?: return current
            return when {
                current != null && current.path == path && current.side == picked.side && !current.isRange &&
                    current.startLine != picked.startLine -> current.extended(line)
                current == picked -> null
                else -> picked
            }
        }

        fun of(path: String, line: DiffLine): DiffLineSelection? = when (line.kind) {
            UnifiedLineKind.Removed -> if (line.oldNumber > 0) DiffLineSelection(path, DiffSide.Old, line.oldNumber) else null
            UnifiedLineKind.Added, UnifiedLineKind.Context ->
                if (line.newNumber > 0) DiffLineSelection(path, DiffSide.New, line.newNumber) else null
        }
    }
}

interface ReviewThreadActions {
    val owner: String
    val name: String
    val canWrite: Boolean
    fun isPending(event: ReviewEvent): Boolean
    fun reply(thread: ReviewThread, body: String): CollabNotice
    fun setResolved(thread: ReviewThread, resolved: Boolean): CollabNotice
}

data class ReviewEventFields(val type: String, val fields: Map<String, Any?>) {
    val body: String get() = fields.str("body")
}

data class ReviewDraft(val id: String, val selection: DiffLineSelection, val body: String, val suggestion: String = "")

enum class ReviewVerdict(val raw: String, val title: String) {
    Comment("comment", "Comment"),
    Approve("approve", "Approve"),
    RequestChanges("request-changes", "Request changes"),
}

object ReviewComposer {
    const val MAX_SELECTION_LINES = 200
    const val MAX_DRAFTS = 50
    const val MAX_BODY_BYTES = 60_000
    const val EMPTY_BODY = "Write a comment first."
    const val TOO_LONG = "That comment is longer than the relay accepts."
    const val REQUEST_CHANGES_NEEDS_NOTE = "A note is required to request changes."
    const val TOO_MANY_DRAFTS = "A review holds at most 50 comments. Submit this one first."

    fun cleanBody(text: String): String = text.trimEnd('\n', '\r')

    fun validate(body: String): String? {
        val clean = cleanBody(body)
        if (clean.isBlank()) return EMPTY_BODY
        if (clean.toByteArray(Charsets.UTF_8).size > MAX_BODY_BYTES) return TOO_LONG
        return null
    }

    fun eventId(type: String, ts: Long, random: String): String = "$type-android-$ts-$random"

    fun inlineComment(
        selection: DiffLineSelection,
        body: String,
        patch: String,
        patchset: String,
        suggestion: String = "",
        newId: () -> String,
    ): ReviewEventFields {
        val start = selection.lineStart
        val end = selection.lineEnd
        val clean = cleanBody(body)
        val id = newId()
        val fields = LinkedHashMap<String, Any?>()
        val type: String
        if (start == end && suggestion.isEmpty()) {
            type = "line-comment"
            fields["id"] = id
            fields["path"] = selection.path
            fields["side"] = selection.side.raw
            fields["line"] = start.toLong()
            fields["body"] = clean
        } else {
            type = "thread-comment"
            fields["id"] = id
            fields["threadId"] = id
            fields["path"] = selection.path
            fields["side"] = selection.side.raw
            fields["lineStart"] = start.toLong()
            fields["lineEnd"] = end.toLong()
            fields["body"] = clean
            if (suggestion.isNotEmpty()) fields["suggestionPatch"] = suggestion
        }
        val anchorLines = ReviewAnchoring.anchorLines(patch, selection.path, selection.side, start, end)
        if (patchset.isNotEmpty()) fields["patchset"] = patchset
        if (anchorLines.isNotEmpty()) fields["anchorLines"] = anchorLines
        return ReviewEventFields(type, fields)
    }

    fun branchComment(
        selection: DiffLineSelection,
        body: String,
        patch: String,
        patchset: String,
        newId: () -> String,
    ): ReviewEventFields {
        val start = selection.lineStart
        val end = selection.lineEnd
        val id = newId()
        val fields = linkedMapOf<String, Any?>(
            "id" to id,
            "threadId" to id,
            "path" to selection.path,
            "side" to selection.side.raw,
            "lineStart" to start.toLong(),
            "lineEnd" to end.toLong(),
            "body" to cleanBody(body),
        )
        val anchorLines = ReviewAnchoring.anchorLines(patch, selection.path, selection.side, start, end)
        if (patchset.isNotEmpty()) fields["patchset"] = patchset
        if (anchorLines.isNotEmpty()) fields["anchorLines"] = anchorLines
        return ReviewEventFields("thread-comment", fields)
    }

    fun reply(thread: ReviewThread, body: String) = ReviewEventFields(
        "thread-reply",
        linkedMapOf("threadId" to thread.id, "parentId" to thread.lastEventId, "body" to cleanBody(body)),
    )

    fun setResolved(thread: ReviewThread, resolved: Boolean, note: String = "", reopened: String = "open") = ReviewEventFields(
        "thread-state",
        linkedMapOf("threadId" to thread.id, "state" to if (resolved) "resolved" else reopened, "body" to cleanBody(note)),
    )

    fun review(verdict: ReviewVerdict, body: String) =
        ReviewEventFields("review", linkedMapOf("state" to verdict.raw, "body" to cleanBody(body)))

    fun comment(body: String) = ReviewEventFields("comment", linkedMapOf("body" to cleanBody(body)))

    fun validateReview(verdict: ReviewVerdict, body: String, drafts: List<ReviewDraft>): String? {
        val clean = cleanBody(body)
        if (verdict == ReviewVerdict.RequestChanges && clean.isBlank() && drafts.isEmpty()) return REQUEST_CHANGES_NEEDS_NOTE
        if (verdict == ReviewVerdict.Comment && clean.isBlank() && drafts.isEmpty()) return EMPTY_BODY
        if (clean.toByteArray(Charsets.UTF_8).size > MAX_BODY_BYTES) return TOO_LONG
        for (draft in drafts) validate(draft.body)?.let { return it }
        return null
    }

    fun submission(
        verdict: ReviewVerdict,
        body: String,
        drafts: List<ReviewDraft>,
        patch: String,
        patchset: String,
        newId: () -> String,
    ): List<ReviewEventFields> {
        val out = drafts.map { inlineComment(it.selection, it.body, patch, patchset, it.suggestion, newId) }.toMutableList()
        val clean = cleanBody(body)
        if (verdict != ReviewVerdict.Comment || clean.isNotBlank()) out.add(review(verdict, clean))
        return out
    }
}

class ReviewDraftBook(drafts: List<ReviewDraft> = emptyList()) {
    var drafts: List<ReviewDraft> = drafts
        private set

    fun add(selection: DiffLineSelection, body: String, id: String, suggestion: String = ""): String? {
        ReviewComposer.validate(body)?.let { return it }
        if (drafts.size >= ReviewComposer.MAX_DRAFTS) return ReviewComposer.TOO_MANY_DRAFTS
        drafts = drafts + ReviewDraft(id, selection, ReviewComposer.cleanBody(body), suggestion)
        return null
    }

    fun update(id: String, body: String) {
        drafts = drafts.map { if (it.id == id) it.copy(body = ReviewComposer.cleanBody(body)) else it }
    }

    fun remove(id: String) {
        drafts = drafts.filterNot { it.id == id }
    }

    fun clear() {
        drafts = emptyList()
    }

    fun inFile(path: String): List<ReviewDraft> = drafts.filter { it.selection.path == path }

    val isEmpty: Boolean get() = drafts.isEmpty()
    val count: Int get() = drafts.size
    val summary: String
        get() = if (drafts.isEmpty()) "No pending comments" else "${drafts.size} pending comment${if (drafts.size == 1) "" else "s"}"
}
