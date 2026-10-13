package com.forkmesh.nativeapp.hosts

import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.double
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str
import java.util.Base64

enum class FileRootKind(val wire: String) {
    Repo("repo"),
    Worktree("worktree"),
    ;

    companion object {
        fun of(wire: String): FileRootKind = entries.firstOrNull { it.wire == wire } ?: Repo
    }
}

data class FileRoot(
    val id: String,
    val label: String,
    val kind: FileRootKind = FileRootKind.Repo,
    val branch: String = "",
    val repo: String = "",
) {
    companion object {
        fun fromJson(json: Map<String, Any?>) = FileRoot(
            id = json.str("id"),
            label = json.str("label"),
            kind = FileRootKind.of(json.str("kind")),
            branch = json.str("branch"),
            repo = json.str("repo"),
        )
    }
}

data class FileRootGroup(val repo: FileRoot, val worktrees: List<FileRoot>) {
    companion object {
        fun group(roots: List<FileRoot>): List<FileRootGroup> {
            val repos = ArrayList<FileRoot>()
            val worktrees = ArrayList<MutableList<FileRoot>>()
            val index = HashMap<String, Int>()
            for (root in roots) {
                if (root.kind != FileRootKind.Repo) continue
                index[root.id.lowercase()] = repos.size
                repos.add(root)
                worktrees.add(ArrayList())
            }
            for (root in roots) {
                if (root.kind != FileRootKind.Worktree) continue
                val key = root.repo.lowercase()
                val position = index[key]
                if (position != null) {
                    worktrees[position].add(root)
                } else {
                    index[key] = repos.size
                    repos.add(FileRoot(root.repo, root.repo))
                    worktrees.add(mutableListOf(root))
                }
            }
            return repos.indices.map { i ->
                FileRootGroup(repos[i], worktrees[i].sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.label }))
            }
        }
    }
}

enum class FileEntryKind(val wire: String) {
    Dir("dir"),
    File("file"),
    Symlink("symlink"),
    Dirlink("dirlink"),
    ;

    val isFolder: Boolean get() = this == Dir || this == Dirlink

    companion object {
        fun of(wire: String): FileEntryKind = entries.firstOrNull { it.wire == wire } ?: File
    }
}

data class FileEntry(
    val name: String,
    val kind: FileEntryKind,
    val size: Double,
    val modifiedMs: Long,
    val escapes: Boolean,
    val sensitive: Boolean,
) {
    val canOpen: Boolean get() = !escapes && !sensitive

    val sizeLabel: String get() = if (kind.isFolder) "" else RemoteFormat.bytes(size)

    companion object {
        fun fromJson(json: Map<String, Any?>) = FileEntry(
            name = json.str("name"),
            kind = FileEntryKind.of(json.str("kind")),
            size = json.double("size"),
            modifiedMs = json.long("modifiedMs"),
            escapes = json.bool("escapes"),
            sensitive = json.bool("sensitive"),
        )
    }
}

data class FileListing(val root: String, val path: String, val entries: List<FileEntry>, val total: Int, val truncated: Boolean) {
    companion object {
        fun fromJson(json: Map<String, Any?>) = FileListing(
            root = json.str("root"),
            path = json.str("path"),
            entries = json.maps("entries").map(FileEntry::fromJson),
            total = json.int("total"),
            truncated = json.bool("truncated"),
        )
    }
}

class FileSlice(
    val root: String,
    val path: String,
    val size: Double,
    val offset: Double,
    val length: Double,
    val eof: Boolean,
    val binary: Boolean,
    val text: String,
    val data: ByteArray?,
    val modifiedMs: Long,
) {
    val nextOffset: Double get() = offset + length

    companion object {
        fun fromJson(json: Map<String, Any?>): FileSlice {
            val binary = json.bool("binary")
            val data = if (binary) runCatching { Base64.getDecoder().decode(json.str("base64")) }.getOrNull() else null
            return FileSlice(
                root = json.str("root"),
                path = json.str("path"),
                size = json.double("size"),
                offset = json.double("offset"),
                length = json.double("length"),
                eof = json.bool("eof"),
                binary = binary,
                text = json.str("text"),
                data = data,
                modifiedMs = json.long("modifiedMs"),
            )
        }
    }
}

object RemotePath {
    fun join(base: String, name: String): String = if (base.isEmpty()) name else "$base/$name"

    fun parent(path: String): String? {
        if (path.isEmpty()) return null
        val parts = path.split('/').filter { it.isNotEmpty() }
        return parts.dropLast(1).joinToString("/")
    }

    fun crumbs(path: String): List<Pair<String, String>> {
        val out = ArrayList<Pair<String, String>>()
        var current = ""
        for (part in path.split('/').filter { it.isNotEmpty() }) {
            current = join(current, part)
            out.add(part to current)
        }
        return out
    }

    fun fileExtension(path: String): String {
        val name = path.split('/').lastOrNull { it.isNotEmpty() } ?: return ""
        val dot = name.lastIndexOf('.')
        if (dot < 0) return ""
        return name.substring(dot + 1).lowercase()
    }

    fun isImage(path: String): Boolean = fileExtension(path) in setOf("png", "jpg", "jpeg", "gif", "webp", "heic", "bmp", "ico")
}

enum class GitChangeKind(val wire: String, val letter: String) {
    Modified("modified", "M"),
    Added("added", "A"),
    Deleted("deleted", "D"),
    Renamed("renamed", "R"),
    Copied("copied", "C"),
    Untracked("untracked", "?"),
    Conflicted("conflicted", "!"),
    ;

    companion object {
        fun of(wire: String): GitChangeKind = entries.firstOrNull { it.wire == wire } ?: Modified
    }
}

data class GitFileChange(val path: String, val from: String, val index: String, val worktree: String, val kind: GitChangeKind) {
    val isStaged: Boolean get() = index != " " && index != "?" && index.isNotEmpty() && kind != GitChangeKind.Conflicted

    companion object {
        fun fromJson(json: Map<String, Any?>) = GitFileChange(
            path = json.str("path"),
            from = json.str("from"),
            index = json.str("index"),
            worktree = json.str("worktree"),
            kind = GitChangeKind.of(json.str("kind")),
        )
    }
}

data class GitWorkingStatus(
    val root: String = "",
    val label: String = "",
    val branch: String = "",
    val upstream: String = "",
    val ahead: Int = 0,
    val behind: Int = 0,
    val total: Int = 0,
    val staged: Int = 0,
    val unstaged: Int = 0,
    val untracked: Int = 0,
    val conflicted: Int = 0,
    val truncated: Boolean = false,
    val files: List<GitFileChange> = emptyList(),
) {
    val isClean: Boolean get() = total == 0

    val syncLine: String
        get() {
            if (upstream.isEmpty()) return "No upstream"
            return when {
                ahead == 0 && behind == 0 -> "Up to date with $upstream"
                behind == 0 -> "$ahead ahead of $upstream"
                ahead == 0 -> "$behind behind $upstream"
                else -> "$ahead ahead, $behind behind $upstream"
            }
        }

    val changesLine: String
        get() {
            if (isClean) return "No changes"
            val parts = ArrayList<String>()
            if (staged > 0) parts.add("$staged staged")
            if (unstaged > 0) parts.add("$unstaged modified")
            if (untracked > 0) parts.add("$untracked untracked")
            if (conflicted > 0) parts.add("$conflicted conflicted")
            return parts.joinToString(" · ")
        }

    val canFastForward: Boolean get() = behind > 0 && ahead == 0 && conflicted == 0

    companion object {
        fun fromJson(json: Map<String, Any?>) = GitWorkingStatus(
            root = json.str("root"),
            label = json.str("label"),
            branch = json.str("branch"),
            upstream = json.str("upstream"),
            ahead = json.int("ahead"),
            behind = json.int("behind"),
            total = json.int("total"),
            staged = json.int("staged"),
            unstaged = json.int("unstaged"),
            untracked = json.int("untracked"),
            conflicted = json.int("conflicted"),
            truncated = json.bool("truncated"),
            files = json.maps("files").map(GitFileChange::fromJson),
        )
    }
}

enum class GitRemoteAction(val wire: String, val title: String) {
    Fetch("fetch", "Fetch"),
    Pull("pull", "Pull (fast-forward)"),
}
