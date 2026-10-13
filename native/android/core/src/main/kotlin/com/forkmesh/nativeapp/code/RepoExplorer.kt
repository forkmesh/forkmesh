package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.api.PublishedMarkdown
import com.forkmesh.nativeapp.model.RepoBranch
import com.forkmesh.nativeapp.model.RepoCodeSearchMatch
import com.forkmesh.nativeapp.model.RepoTreeEntry
import com.forkmesh.nativeapp.model.dartToStringAsFixed
import java.util.Base64

data class ExplorerCrumb(val label: String, val target: String, val current: Boolean)

enum class PreviewKind { Text, Image, Video }

object RepoExplorer {
    const val ROOT_LABEL = "Root"

    fun breadcrumbs(path: String): List<ExplorerCrumb> {
        val segments = path.split('/').filter { it.isNotEmpty() }
        val crumbs = ArrayList<ExplorerCrumb>()
        crumbs += ExplorerCrumb(ROOT_LABEL, "", current = segments.isEmpty())
        var prefix = ""
        segments.forEachIndexed { i, segment ->
            prefix = if (prefix.isEmpty()) segment else "$prefix/$segment"
            crumbs += ExplorerCrumb(segment, prefix, current = i == segments.size - 1)
        }
        return crumbs
    }

    fun parentPath(path: String): String {
        if (path.isEmpty()) return ""
        val parts = path.split('/').toMutableList()
        parts.removeAt(parts.lastIndex)
        return parts.joinToString("/")
    }

    fun fileName(path: String): String = path.split('/').last()

    fun humanSize(bytes: Long): String {
        if (bytes < 1024) return "$bytes B"
        val kb = bytes / 1024.0
        if (kb < 1024) return "${dartToStringAsFixed(kb, if (kb < 10) 1 else 0)} KB"
        val mb = kb / 1024
        if (mb < 1024) return "${dartToStringAsFixed(mb, 1)} MB"
        return "${dartToStringAsFixed(mb / 1024, 1)} GB"
    }

    fun fileRowMeta(entry: RepoTreeEntry, nowMillis: Long = System.currentTimeMillis()): String {
        val meta = ArrayList<String>()
        meta += when {
            entry.isDirectory -> "Directory"
            entry.size > 0 -> humanSize(entry.size)
            else -> "File"
        }
        if (entry.updatedMs > 0) meta += agoLabel(entry.updatedMs, nowMillis)
        return meta.joinToString(" · ")
    }

    private val CODE_EXTENSIONS = setOf(
        "dart", "ts", "tsx", "js", "jsx", "mjs", "cjs", "py", "go", "rs", "c", "cc", "cpp", "h", "hpp", "java", "kt",
        "kts", "swift", "rb", "php", "cs", "sh", "bash", "zsh", "sql", "lua", "scala", "m", "mm", "html", "css", "scss",
        "vue", "svelte",
    )
    private val DATA_EXTENSIONS = setOf("json", "yaml", "yml", "toml", "xml", "lock", "csv", "ini")
    private val IMAGE_EXTENSIONS = setOf("png", "jpg", "jpeg", "gif", "webp", "svg", "bmp", "ico")
    private val DOC_EXTENSIONS = setOf("md", "mdx", "txt", "rst", "adoc")

    fun fileGlyph(name: String): String {
        val lower = name.lowercase()
        val dot = lower.lastIndexOf('.')
        val ext = if (dot < 0) "" else lower.substring(dot + 1)
        return when {
            ext in CODE_EXTENSIONS -> "code_rounded"
            ext in DATA_EXTENSIONS -> "data_object_rounded"
            ext in IMAGE_EXTENSIONS -> "image_outlined"
            ext in DOC_EXTENSIONS -> "article_outlined"
            lower == "dockerfile" || lower == "makefile" || lower == "license" -> "terminal_rounded"
            else -> "description_outlined"
        }
    }

    fun previewKind(path: String): PreviewKind {
        val lower = path.lowercase()
        if (listOf(".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp").any { lower.endsWith(it) }) return PreviewKind.Image
        if (listOf(".mp4", ".mov", ".webm", ".mkv").any { lower.endsWith(it) }) return PreviewKind.Video
        return PreviewKind.Text
    }

    fun decodedText(content: String, encoding: String): String? {
        if (encoding.lowercase() == "base64") {
            return try {
                val compact = content.replace(Regex("\\s+"), "").replace('-', '+').replace('_', '/')
                val bytes = Base64.getDecoder().decode(compact)
                for (i in 0 until minOf(512, bytes.size)) if (bytes[i] == 0.toByte()) return null
                String(bytes, Charsets.UTF_8)
            } catch (_: IllegalArgumentException) {
                null
            }
        }
        var index = 0
        var seen = 0
        while (index < content.length && seen < 512) {
            val codePoint = content.codePointAt(index)
            if (codePoint == 0) return null
            index += Character.charCount(codePoint)
            seen++
        }
        return content
    }

    fun sourceLabel(source: String): String? {
        val value = source.trim()
        if (value.isEmpty()) return null
        val lower = value.lowercase()
        if (lower == "live" || lower == "live_source" || lower == "live-source") return "live source"
        if (lower == "live mirror" || lower == "live_mirror" || lower == "live-mirror") return "live mirror"
        if (lower.startsWith("served by ") || lower.startsWith("live ")) return value
        return "served by $value"
    }

    fun branchPillLabel(selectedRef: String): String =
        "Branch: ${if (selectedRef.isEmpty()) "Default branch" else selectedRef}"

    fun branchMenuNames(selectedRef: String, branches: List<RepoBranch>): List<String> {
        val names = LinkedHashSet<String>()
        if (selectedRef.isNotEmpty()) names += selectedRef
        for (branch in branches) if (branch.name.isNotEmpty()) names += branch.name
        return names.toList()
    }

    fun isCurrentBranch(branch: RepoBranch, defaultBranch: String): Boolean =
        branch.isDefault || (defaultBranch.isNotEmpty() && branch.name == defaultBranch)

    fun sortBranches(branches: List<RepoBranch>, defaultBranch: String): List<RepoBranch> =
        branches.sortedWith { a, b ->
            val ca = if (isCurrentBranch(a, defaultBranch)) 0 else 1
            val cb = if (isCurrentBranch(b, defaultBranch)) 0 else 1
            if (ca != cb) ca - cb else a.name.lowercase().compareTo(b.name.lowercase())
        }

    fun branchRowSubtitle(branch: RepoBranch): String {
        val short = if (branch.sha.length > 7) branch.sha.substring(0, 7) else branch.sha
        val meta = ArrayList<String>()
        if (short.isNotEmpty()) meta += short
        if (branch.hasWorktree) meta += "worktree"
        return if (meta.isEmpty()) "Branch" else meta.joinToString(" · ")
    }

    fun branchTipCommit(branch: RepoBranch): Map<String, Any?> =
        linkedMapOf("hash" to branch.sha, "message" to "Tip of ${branch.name}")

    fun worktreeBranches(branches: List<RepoBranch>): List<RepoBranch> = branches.filter { it.hasWorktree }

    fun worktreeShortSha(sha: String): String = sha.take(7)

    fun searchQuery(raw: String): String? = raw.trim().takeIf { it.length >= 2 }

    fun searchRowSubtitle(match: RepoCodeSearchMatch): String? = when {
        match.text.isEmpty() -> null
        match.line > 0 -> "${match.line}: ${match.text}"
        else -> match.text
    }

    fun codeLines(content: String): List<String> = PublishedMarkdown.lines(content)

    fun gutterWidth(lineCount: Int): Float = (lineCount.toString().length * 8.5f + 24f).coerceIn(44f, 84f)

    fun estimatedWidth(longestLine: Int, gutterWidth: Float): Float = longestLine * 8.2f + gutterWidth + 40f

    fun statusBarText(activeLine: Int, lineCount: Int, byteCount: Long): String =
        "Ln $activeLine · $lineCount ${if (lineCount == 1) "line" else "lines"} · ${humanSize(byteCount)}"
}
