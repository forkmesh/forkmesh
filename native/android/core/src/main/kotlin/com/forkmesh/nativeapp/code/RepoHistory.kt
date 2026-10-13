package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.model.RepoBranch

data class RepoHistoryRead(val commits: List<Map<String, Any?>>, val head: String? = null) {
    val tip: String?
        get() {
            head?.let { return it.lowercase() }
            val first = commits.firstOrNull()?.let(CommitFields::hash).orEmpty()
            return first.ifEmpty { null }?.lowercase()
        }

    companion object {
        @Suppress("UNCHECKED_CAST")
        fun fromJson(json: Any?): RepoHistoryRead {
            val obj = json as? Map<String, Any?>
            val list = (json as? List<*>) ?: (obj?.get("commits") as? List<*>) ?: emptyList<Any?>()
            val head = (obj?.get("commit") as? String).orEmpty()
            return RepoHistoryRead(list.mapNotNull { it as? Map<String, Any?> }, head.ifEmpty { null })
        }
    }
}

enum class RepoHistoryRefSupport {
    Honored,
    Ignored,
    Unknown,
    ;

    companion object {
        fun detect(ref: String, read: RepoHistoryRead, branches: List<RepoBranch>, defaultBranch: String): RepoHistoryRefSupport {
            val tip = read.tip ?: return Unknown
            val requested = branches.firstOrNull { it.name == ref }?.sha ?: if (isCommitId(ref)) ref else ""
            val fallback = branches.firstOrNull { it.isDefault || (defaultBranch.isNotEmpty() && it.name == defaultBranch) }?.sha.orEmpty()
            if (sameCommit(requested, tip)) return Honored
            if (sameCommit(fallback, tip)) {
                return if (requested.isEmpty() || sameCommit(requested, fallback)) Unknown else Ignored
            }
            return if (fallback.isEmpty() && requested.isEmpty()) Unknown else Honored
        }

        fun sameCommit(left: String, right: String): Boolean {
            val a = left.lowercase()
            val b = right.lowercase()
            if (a.length < 7 || b.length < 7) return false
            return a.startsWith(b) || b.startsWith(a)
        }

        fun isCommitId(value: String): Boolean =
            value.length in 7..64 && value.all { it in '0'..'9' || it in 'a'..'f' || it in 'A'..'F' }
    }
}

object RepoHistoryText {
    fun title(ref: String): String = "History of $ref"

    fun notice(ref: String, support: RepoHistoryRefSupport?, defaultBranch: String): String? {
        if (support != RepoHistoryRefSupport.Ignored) return null
        val label = defaultBranch.ifEmpty { "the default branch" }
        return "This server lists only the default branch's history, so these are the commits of $label, not $ref."
    }
}
