package com.forkmesh.nativeapp.model

data class RepoComparison(
    val baseOid: String = "",
    val headOid: String = "",
    val mergeBaseOid: String = "",
    val commitCount: Int = 0,
    val patch: String = "",
    val commits: String = "",
) {
    companion object {
        fun fromJson(data: Any?): RepoComparison {
            if (data !is Map<*, *>) return RepoComparison()
            fun text(key: String): String = data[key]?.toString() ?: ""
            val count = data["commitCount"]
            return RepoComparison(
                baseOid = text("baseOid"),
                headOid = text("headOid"),
                mergeBaseOid = text("mergeBaseOid"),
                commitCount = when (count) {
                    is Number -> count.toInt()
                    else -> count?.toString()?.toIntOrNull() ?: 0
                },
                patch = text("patch"),
                commits = text("commits"),
            )
        }
    }
}
