package com.forkmesh.nativeapp.model

data class RepoDiscussion(
    val number: Int,
    val title: String,
    val body: String = "",
    val author: String = "",
    val category: String = "general",
    val updatedMs: Long = 0,
    val events: List<DiscussionEvent> = emptyList(),
    val authorKey: String = "",
    val answer: DiscussionEvent? = null,
)

data class DiscussionEvent(
    val type: String,
    val body: String = "",
    val author: String = "",
    val authorName: String = "",
    val ts: Long = 0,
    val id: String = "",
    val target: String = "",
) {
    val displayAuthor: String get() = if (authorName.isNotEmpty()) authorName else author
}
