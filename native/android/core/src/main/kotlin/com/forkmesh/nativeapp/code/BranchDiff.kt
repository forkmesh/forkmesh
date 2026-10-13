package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.code.diff.DiffFile
import com.forkmesh.nativeapp.code.diff.PatchCommit
import com.forkmesh.nativeapp.code.diff.parseFormatPatchSeries
import com.forkmesh.nativeapp.code.diff.parseUnifiedDiff
import com.forkmesh.nativeapp.model.RepoComparison

class BranchComparison(val raw: RepoComparison) {
    val files: List<DiffFile> = parseUnifiedDiff(raw.patch)
    val commits: List<PatchCommit> = parseFormatPatchSeries(raw.commits)
    private val commitFiles = HashMap<String, List<DiffFile>>()

    @Synchronized
    fun filesOf(commit: PatchCommit): List<DiffFile> = commitFiles.getOrPut(commit.hash) { parseUnifiedDiff(commit.patch) }

    val commitTabCount: Int get() = if (commits.isNotEmpty()) commits.size else raw.commitCount
}

object BranchDiff {
    fun base(base: String, defaultBranch: String): String {
        val trimmed = base.trim()
        if (trimmed.isNotEmpty()) return trimmed
        val fallback = defaultBranch.trim()
        return fallback.ifEmpty { "main" }
    }

    fun describeError(error: Any?, head: String, base: String): String {
        val text = error?.toString() ?: "null"
        return when {
            text.contains("ref was not found") || text.contains("not_found") ->
                "$head is not on a serving mirror yet. Branches appear here once the node that ran the agent has pushed them."
            text.contains("merge base") ->
                "$head and $base share no history, so there is no diff between them to show."
            text.contains("too large") || text.contains("output limit") ->
                "This branch changes more than a phone-sized comparison can carry. Open it on a desktop node instead."
            else -> text
        }
    }

    fun emptyMessage(commitCount: Int, head: String, base: String): String = if (commitCount == 0) {
        "$head has nothing that $base does not already have — it may have been merged."
    } else {
        "The commits on $head do not change any files."
    }

    fun summary(commits: Int, files: Int): String =
        "$commits ${if (commits == 1) "commit" else "commits"} · $files ${if (files == 1) "file" else "files"}"

    fun short(oid: String): String = if (oid.length > 7) oid.substring(0, 7) else oid

    fun headLine(raw: RepoComparison): String =
        "head ${short(raw.headOid)}" + if (raw.mergeBaseOid.isNotEmpty()) " · from ${short(raw.mergeBaseOid)}" else ""

    fun noCommitsTitle(count: Int): String = if (count == 0) "No commits" else "$count commits"

    fun noCommitsMessage(count: Int): String = if (count == 0) {
        "This branch has no commits of its own."
    } else {
        "The range includes a merge commit, so the mirror did not send the commits one by one. The Files tab shows their combined change."
    }

    fun age(dateMillis: Long, nowMillis: Long = System.currentTimeMillis()): String {
        val diff = nowMillis - dateMillis
        val minutes = diff / 60_000
        val hours = diff / 3_600_000
        val days = diff / 86_400_000
        return when {
            minutes < 1 -> "now"
            hours < 1 -> "${minutes}m ago"
            days < 1 -> "${hours}h ago"
            days < 30 -> "${days}d ago"
            days < 365 -> "${days / 30}mo ago"
            else -> "${days / 365}y ago"
        }
    }

    fun commitMeta(commit: PatchCommit, nowMillis: Long = System.currentTimeMillis()): String = buildList {
        add(commit.shortHash)
        if (commit.author.isNotEmpty()) add(commit.author)
        commit.date?.let { add(age(it, nowMillis)) }
    }.joinToString(" · ")

    fun totals(files: List<DiffFile>): Pair<Int, Int> = files.sumOf { it.additions } to files.sumOf { it.deletions }

    fun filesCount(count: Int): String = "$count ${if (count == 1) "file" else "files"}"
}

object DiffFileFilter {
    fun apply(files: List<DiffFile>, filter: String): List<DiffFile> {
        val query = filter.trim().lowercase()
        if (query.isEmpty()) return files
        return files.filter { it.path.lowercase().contains(query) || it.oldPath.lowercase().contains(query) }
    }

    fun toggle(viewed: Set<String>, path: String): Set<String> = if (path in viewed) viewed - path else viewed + path

    fun viewedCount(files: List<DiffFile>, viewed: Set<String>): Int = files.count { it.path in viewed }

    fun statBar(additions: Int, deletions: Int): Pair<Int, Int> {
        val total = additions + deletions
        val green = if (total == 0) 0 else Math.round(additions * 5.0 / total).toInt()
        val red = if (total == 0) 0 else (5 - green).coerceIn(0, 5)
        return green to red
    }

    fun hiddenBefore(file: DiffFile, index: Int): Int {
        val hunk = file.hunks[index]
        return if (index > 0) {
            val previous = file.hunks[index - 1]
            hunk.newStart - (previous.newStart + previous.newCount)
        } else {
            hunk.newStart - 1
        }
    }

    fun hunkLabel(hidden: Int): String = if (hidden > 0) "⋯ $hidden unchanged ${if (hidden == 1) "line" else "lines"}" else ""

    fun notices(file: DiffFile): List<String> = buildList {
        if (file.binary) add("Binary file — contents are not shown.")
        val moved = (file.status == com.forkmesh.nativeapp.code.diff.DiffFileStatus.Renamed ||
            file.status == com.forkmesh.nativeapp.code.diff.DiffFileStatus.Copied) && file.oldPath != file.newPath
        if (moved) {
            val verb = if (file.status == com.forkmesh.nativeapp.code.diff.DiffFileStatus.Copied) "Copied" else "Renamed"
            add("$verb from ${file.oldPath}" + if (file.similarity >= 0) " (${file.similarity}% similar)" else "")
        }
        if (file.modeChanged) add("Mode changed ${file.oldMode} → ${file.newMode}")
        if (file.status == com.forkmesh.nativeapp.code.diff.DiffFileStatus.Added && file.hunks.isEmpty()) add("New empty file.")
        if (file.status == com.forkmesh.nativeapp.code.diff.DiffFileStatus.Deleted && file.hunks.isEmpty()) add("Deleted empty file.")
    }
}
