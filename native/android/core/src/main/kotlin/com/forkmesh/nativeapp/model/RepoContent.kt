package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.str
import java.time.Instant

data class RepoTreeEntry(
    val name: String,
    val path: String,
    val type: String,
    val size: Long = 0,
    val sha: String = "",
    val updatedMs: Long = 0,
    val lastMessage: String = "",
) {
    val isDirectory: Boolean get() = type == "dir" || type == "tree" || type == "folder"

    val updatedAt: Instant? get() = if (updatedMs > 0) Instant.ofEpochMilli(updatedMs) else null

    fun copyWith(path: String? = null): RepoTreeEntry = copy(path = path ?: this.path)

    companion object {
        private fun asEpochMs(v: Any?): Long {
            if (v == null) return 0
            if (v is Number) {
                val n = modelInt(v)
                return if (n > 100_000_000_000L) n else n * 1000
            }
            val text = jsonText(v).trim()
            if (text.isEmpty()) return 0
            val n = dartIntTryParse(text)
            if (n != null) return if (n > 100_000_000_000L) n else n * 1000
            return dartDateTimeTryParseMs(text) ?: 0
        }

        fun fromJson(j: Map<String, Any?>): RepoTreeEntry {
            val rawMessage = jsonText(j["subject"] ?: j["commitMessage"] ?: j["message"])
            val rawPath = jsonText(j["path"] ?: j["name"])
            val name = jsonText(j["name"] ?: rawPath.split('/').lastOrNull { it.isNotEmpty() } ?: rawPath)
            val rawType = jsonText(j["type"] ?: j["kind"] ?: j["mode"]).lowercase()
            val type = if (rawType.contains("tree") || rawType == "directory" || rawType == "folder" || rawType == "dir") {
                "dir"
            } else {
                "file"
            }
            return RepoTreeEntry(
                name = name,
                path = rawPath,
                type = type,
                size = modelInt(j["size"] ?: j["bytes"]),
                sha = jsonText(j["sha"] ?: j["hash"] ?: j["oid"]),
                updatedMs = asEpochMs(j["date"] ?: j["updatedAt"] ?: j["mtime"]),
                lastMessage = rawMessage.split('\n').first().trim(),
            )
        }
    }
}

data class RepoTree(
    val path: String,
    val entries: List<RepoTreeEntry>,
    val source: String = "",
    val cachedAt: Long = 0,
    val commit: String = "",
    val pullRefs: Map<String, String> = emptyMap(),
) {
    companion object {
        private fun pullRefsFrom(raw: Any?): Map<String, String> {
            if (raw !is Map<*, *>) return emptyMap()
            val refs = LinkedHashMap<String, String>()
            for ((key, value) in raw) {
                val number = key?.toString() ?: continue
                if (number.isEmpty() || !number.all { it in '0'..'9' }) continue
                if (value !is String || value.isBlank()) continue
                refs[number] = value.trim()
            }
            return refs
        }

        fun fromJson(data: Any?, path: String = ""): RepoTree {
            var treePath = path
            var rawEntries: List<Any?> = emptyList()
            var source = ""
            var cachedAt = 0L
            var commit = ""
            var pullRefs: Map<String, String> = emptyMap()
            if (data is List<*>) {
                rawEntries = data
            } else if (data is Map<*, *>) {
                val map = data.asJsonMap()
                for (key in listOf("entries", "tree", "items", "files", "data")) {
                    val value = map[key]
                    if (value is List<*>) {
                        rawEntries = value
                        break
                    }
                }
                source = jsonText(map["source"] ?: map["servedBy"])
                cachedAt = modelInt(map["cachedAt"] ?: map["ts"])
                treePath = jsonText(map["path"] ?: treePath)
                commit = jsonText(map["commit"] ?: (map["analysis"] as? Map<*, *>)?.get("commit"))
                pullRefs = pullRefsFrom(map["pullRefs"])
            }
            val entries = jsonMaps(rawEntries)
                .map { RepoTreeEntry.fromJson(it) }
                .map { e -> if (treePath.isEmpty() || e.path.contains('/')) e else e.copyWith(path = "$treePath/${e.path}") }
                .filter { it.name.isNotEmpty() }
                .sortedWith { a, b ->
                    if (a.isDirectory != b.isDirectory) {
                        if (a.isDirectory) -1 else 1
                    } else {
                        a.name.lowercase().compareTo(b.name.lowercase())
                    }
                }
            return RepoTree(
                path = treePath,
                entries = entries,
                source = source,
                cachedAt = cachedAt,
                commit = commit,
                pullRefs = pullRefs,
            )
        }
    }
}

data class RepoBlob(
    val path: String,
    val content: String,
    val encoding: String = "",
    val size: Long = 0,
    val source: String = "",
) {
    companion object {
        fun fromJson(data: Any?, path: String = ""): RepoBlob {
            if (data is String) return RepoBlob(path = path, content = data, size = data.length.toLong())
            if (data is Map<*, *>) {
                val map = data.asJsonMap()
                return RepoBlob(
                    path = jsonText(map["path"] ?: path),
                    content = jsonText(map["content"] ?: map["text"] ?: map["data"]),
                    encoding = map.str("encoding"),
                    size = modelInt(map["size"] ?: map["bytes"]),
                    source = jsonText(map["source"] ?: map["servedBy"]),
                )
            }
            return RepoBlob(path = path, content = "")
        }
    }
}

data class RepoCommitFile(
    val path: String,
    val adds: String = "",
    val dels: String = "",
) {
    companion object {
        fun fromJson(j: Map<String, Any?>): RepoCommitFile = RepoCommitFile(
            path = jsonText(j["path"] ?: j["file"]),
            adds = jsonText(j["adds"] ?: j["additions"]),
            dels = jsonText(j["dels"] ?: j["deletions"]),
        )
    }
}

data class RepoCommitDetail(
    val commit: Map<String, Any?>,
    val files: List<RepoCommitFile> = emptyList(),
    val diff: String = "",
    val truncated: Boolean = false,
    val source: String = "",
) {
    companion object {
        fun fromJson(data: Any?, fallbackCommit: Map<String, Any?> = emptyMap()): RepoCommitDetail {
            if (data !is Map<*, *>) return RepoCommitDetail(commit = fallbackCommit)
            val map = data.asJsonMap()
            val commit = if (map["commit"] is Map<*, *>) LinkedHashMap(map["commit"].asJsonMap()) else LinkedHashMap(fallbackCommit)
            return RepoCommitDetail(
                commit = commit,
                files = jsonMaps(map["files"]).map { RepoCommitFile.fromJson(LinkedHashMap(it)) }.filter { it.path.isNotEmpty() },
                diff = map.str("diff"),
                truncated = map["truncated"] == true,
                source = jsonText(map["source"] ?: map["servedBy"]),
            )
        }
    }
}

data class RepoBranch(
    val name: String,
    val sha: String = "",
    val isDefault: Boolean = false,
    val worktreePath: String = "",
) {
    val hasWorktree: Boolean get() = worktreePath.isNotEmpty()

    companion object {
        fun fromJson(j: Map<String, Any?>): RepoBranch = RepoBranch(
            name = jsonText(j["name"] ?: j["branch"]),
            sha = jsonText(j["sha"] ?: j["hash"] ?: j["commit"]),
            isDefault = j["default"] == true || j["isDefault"] == true,
            worktreePath = jsonText(j["worktree"] ?: j["worktreePath"]),
        )
    }
}

data class RepoCodeSearchMatch(
    val path: String,
    val line: Int = 0,
    val text: String = "",
) {
    companion object {
        fun fromJson(j: Map<String, Any?>): RepoCodeSearchMatch = RepoCodeSearchMatch(
            path = jsonText(j["path"] ?: j["file"]),
            line = modelInt(j["line"] ?: j["lineNumber"]).toInt(),
            text = jsonText(j["text"] ?: j["snippet"] ?: j["match"]),
        )
    }
}

data class RepoSearchResults(
    val code: List<RepoCodeSearchMatch> = emptyList(),
) {
    companion object {
        fun fromJson(data: Any?): RepoSearchResults {
            if (data !is Map<*, *>) return RepoSearchResults()
            return RepoSearchResults(
                code = jsonMaps(data["code"]).map { RepoCodeSearchMatch.fromJson(it) }.filter { it.path.isNotEmpty() },
            )
        }
    }
}

data class RepoMirror(
    val owner: String,
    val name: String,
    val online: Boolean = false,
    val lastSeenMs: Long = 0,
    val firstHostedMs: Long = 0,
    val node: String = "",
    val mirrorUrl: String = "",
    val endpoint: String = "",
    val integrity: String = "",
    val lastSyncMs: Long = 0,
    val cloneAvailable: Boolean = false,
    val endpointHealthy: Boolean = false,
    val endpointFresh: Boolean = false,
    val abuseBlocked: Boolean = false,
    val behind: Boolean = false,
    val latencyMs: Long = 0,
) {
    val label: String get() = if (owner.isEmpty()) name else "$owner/$name"

    val isHealthy: Boolean
        get() = online && integrity == "ok" && lastSyncMs > 0 && cloneAvailable && endpointHealthy &&
            endpointFresh && !abuseBlocked

    companion object {
        fun fromJson(j: Map<String, Any?>): RepoMirror = RepoMirror(
            owner = jsonText(j["owner"] ?: j["node"] ?: j["name"]),
            name = jsonText(j["repo"] ?: j["repository"] ?: j["name"]),
            online = j["online"] == true || j["live"] == true || j["status"] == "online",
            lastSeenMs = modelInt(j["lastSeen"] ?: j["lastSeenAt"] ?: j["ts"]),
            firstHostedMs = modelInt(j["firstHosted"] ?: j["firstHostedAt"]),
            node = jsonText(j["node"] ?: j["owner"]),
            mirrorUrl = jsonText(j["mirrorUrl"]).trim(),
            endpoint = jsonText(j["endpoint"]).trim(),
            integrity = jsonText(j["integrity"]).trim().lowercase(),
            lastSyncMs = modelInt(j["lastSync"]),
            cloneAvailable = j["cloneAvailable"] == true,
            endpointHealthy = j["endpointHealthy"] == true,
            endpointFresh = j["endpointFresh"] == true,
            abuseBlocked = j["abuseBlocked"] == true,
            behind = j["behind"] == true,
            latencyMs = modelInt(j["directLatencyMs"] ?: j["latencyMs"]).let { if (it == 0L) modelInt(j["latencyMs"]) else it },
        )
    }
}

data class RepoMirrorSet(
    val mirrors: List<RepoMirror>,
    val syncStatus: Map<String, Any?>? = null,
)
