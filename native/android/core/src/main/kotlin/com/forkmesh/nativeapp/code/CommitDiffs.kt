package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.api.PublishedMarkdown
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.RepoCommitFile
import com.forkmesh.nativeapp.model.dartDateTimeTryParseMs
import com.forkmesh.nativeapp.model.dartIntTryParse
import kotlin.math.roundToLong

enum class ChangeKind { Added, Modified, Deleted, Renamed }

data class PatchFile(val path: String, val kind: ChangeKind, val adds: Int = 0, val dels: Int = 0)

enum class DiffLineKind { Added, Removed, Hunk, Context }

object CommitFields {
    fun hash(commit: Map<String, Any?>): String = jsonText(commit["hash"] ?: commit["sha"] ?: commit["id"] ?: "")

    fun short(commit: Map<String, Any?>): String = shortHash(hash(commit))

    fun shortHash(hash: String): String = if (hash.length < 7) hash else hash.substring(0, 7)

    fun subject(commit: Map<String, Any?>): String {
        val raw = jsonText(commit["subject"] ?: commit["message"] ?: "Commit")
        val firstLine = raw.split('\n').first().trim()
        return firstLine.ifEmpty { "Commit" }
    }

    fun author(commit: Map<String, Any?>): String {
        val author = commit["author"]
        if (author is Map<*, *>) return jsonText(author["name"] ?: author["email"] ?: "")
        return jsonText(author ?: commit["authorName"] ?: commit["committer"] ?: "")
    }

    fun body(commit: Map<String, Any?>): String = jsonText(commit["body"] ?: commit["description"] ?: "")

    private val DATE_KEYS = listOf("date", "time", "timestamp", "createdAt", "committedDate", "authorDate", "ts")

    fun dateMs(commit: Map<String, Any?>): Long? {
        for (key in DATE_KEYS) {
            val value = commit[key]
            if (value is Long || value is Int) {
                val n = (value as Number).toLong()
                return if (n > 100_000_000_000L) n else n * 1000
            }
            if (value is Double) {
                val rounded = value.roundToLong()
                return if (rounded > 100_000_000_000L) rounded else rounded * 1000
            }
            if (value is String && value.trim().isNotEmpty()) {
                val parsedNumber = dartIntTryParse(value)
                if (parsedNumber != null) return if (parsedNumber > 100_000_000_000L) parsedNumber else parsedNumber * 1000
                val parsed = dartDateTimeTryParseMs(value)
                if (parsed != null) return parsed
            }
        }
        return null
    }

    fun relativeTime(commit: Map<String, Any?>, nowMillis: Long = System.currentTimeMillis()): String {
        val date = dateMs(commit) ?: return ""
        return compactAge(date, nowMillis)
    }
}

fun compactAge(dateMs: Long, nowMillis: Long = System.currentTimeMillis()): String {
    val diff = nowMillis - dateMs
    val minutes = diff / 60_000
    val hours = diff / 3_600_000
    val days = diff / 86_400_000
    if (minutes < 1) return "now"
    if (hours < 1) return "${minutes}m"
    if (days < 1) return "${hours}h"
    if (days < 30) return "${days}d"
    if (days < 365) return "${days / 30}mo"
    return "${days / 365}y"
}

fun agoLabel(dateMs: Long, nowMillis: Long = System.currentTimeMillis()): String {
    val rel = compactAge(dateMs, nowMillis)
    return if (rel == "now") "just now" else "$rel ago"
}

object Patches {
    private val HEADER = Regex("^diff --git a/(.*?) b/(.*)$")

    fun parsePatchFiles(diff: String): List<PatchFile> {
        if (diff.trim().isEmpty()) return emptyList()
        val files = ArrayList<PatchFile>()
        var path: String? = null
        var kind = ChangeKind.Modified
        var adds = 0
        var dels = 0
        fun flush() {
            val current = path
            if (current.isNullOrEmpty()) return
            files += PatchFile(current, kind, adds, dels)
        }
        for (line in PublishedMarkdown.lines(diff)) {
            when {
                line.startsWith("diff --git ") -> {
                    flush()
                    val match = HEADER.find(line)
                    path = (match?.groupValues?.get(2) ?: line.substring("diff --git ".length)).trim()
                    kind = ChangeKind.Modified
                    adds = 0
                    dels = 0
                }
                line.startsWith("new file mode") || line.startsWith("--- /dev/null") -> kind = ChangeKind.Added
                line.startsWith("deleted file mode") || line.startsWith("+++ /dev/null") -> kind = ChangeKind.Deleted
                line.startsWith("rename from ") || line.startsWith("similarity index") -> kind = ChangeKind.Renamed
                line.startsWith("rename to ") -> path = line.substring("rename to ".length).trim()
                line.startsWith("+++ b/") -> if (path == null) path = line.substring("+++ b/".length).trim()
                line.startsWith("+++ ") || line.startsWith("--- ") -> Unit
                line.startsWith("+") -> adds++
                line.startsWith("-") -> dels++
            }
        }
        flush()
        return files
    }

    fun changedFilesTitle(count: Int): String = "$count changed ${if (count == 1) "file" else "files"}"

    fun commitFileTotals(files: List<RepoCommitFile>): Pair<Int, Int> {
        var adds = 0
        var dels = 0
        for (file in files) {
            adds += dartIntTryParse(file.adds)?.toInt() ?: 0
            dels += dartIntTryParse(file.dels)?.toInt() ?: 0
        }
        return adds to dels
    }

    fun visibleCount(count: String): String = if (count == "0") "" else count

    fun splitPath(path: String): Pair<String, String> {
        val slash = path.lastIndexOf('/')
        return if (slash < 0) "" to path else path.substring(0, slash + 1) to path.substring(slash + 1)
    }

    fun diffLineKind(line: String): DiffLineKind = when {
        line.startsWith("+") && !line.startsWith("+++") -> DiffLineKind.Added
        line.startsWith("-") && !line.startsWith("---") -> DiffLineKind.Removed
        line.startsWith("@@") -> DiffLineKind.Hunk
        else -> DiffLineKind.Context
    }
}
