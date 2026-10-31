package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.str

data class PullRequest(
    val number: Int,
    val title: String,
    val status: String = "open",
    val body: String = "",
    val author: String = "",
    val baseBranch: String = "",
    val headBranch: String = "",
    val createdMs: Long = 0,
) {
    val createdDate: String get() = if (createdMs <= 0) "" else modelLocalDate(createdMs)

    companion object {
        fun fromJson(j: Map<String, Any?>): PullRequest = PullRequest(
            number = modelInt(j["number"]).toInt(),
            title = j.str("title"),
            status = j.str("status", "open"),
            body = j.str("body"),
            author = j.str("author"),
            baseBranch = jsonText(j["base"] ?: j["baseBranch"]),
            headBranch = jsonText(j["head"] ?: j["headBranch"]),
            createdMs = modelInt(j["createdAt"] ?: j["ts"] ?: j["submittedAt"] ?: 0L),
        )
    }
}

data class PullEvent(
    val type: String,
    val body: String = "",
    val author: String = "",
    val authorName: String = "",
    val state: String = "",
    val ts: Long = 0,
) {
    val displayAuthor: String get() = if (authorName.isNotEmpty()) authorName else author

    val displayTitle: String
        get() {
            if (type == "review") {
                return when (state) {
                    "approve", "approved" -> "Approved"
                    "request-changes", "changes-requested", "changes_requested", "requested_changes" -> "Requested changes"
                    "comment" -> "Review comment"
                    else -> "Review"
                }
            }
            return when (type) {
                "merged" -> "Merged"
                "status" -> if (state == "open") "Reopened" else "Closed"
                "line-comment" -> "Line comment"
                "thread-comment" -> "Thread comment"
                "thread-reply" -> "Thread reply"
                "thread-state" -> "Thread state"
                "suggestion-state" -> "Suggestion state"
                else -> "Comment"
            }
        }
}

data class PublishedPull(
    val number: Int,
    val title: String,
    val status: String = "open",
    val body: String = "",
    val base: String = "",
    val head: String = "",
    val patch: String = "",
    val signed: Boolean = false,
    val events: List<PullEvent> = emptyList(),
    val branchBacked: Boolean = false,
    val author: String = "",
    val authorName: String = "",
    val createdMs: Long = 0,
    val mergedMs: Long = 0,
    val mergedBy: String = "",
) {
    val isOpen: Boolean get() = status != "closed" && status != "merged"
}
