package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.model.dartIntTryParse

data class NewIssueDraft(
    val title: String,
    val body: String = "",
    val labels: List<String> = emptyList(),
    val milestone: String = "",
    val priority: Int = 0,
    val assignees: List<String> = emptyList(),
)

object NewIssueComposer {
    const val TITLE_REQUIRED = "Give the issue a title."

    fun title(parentNumber: Int): String = if (parentNumber > 0) "New sub-issue of #$parentNumber" else "New issue"

    fun initialBody(parentNumber: Int): String = if (parentNumber > 0) "Parent issue: #$parentNumber\n\n" else ""

    fun split(value: String): List<String> = value.split(',').map { it.trim() }.filter { it.isNotEmpty() }

    fun draft(
        title: String,
        body: String,
        labels: String,
        priority: String,
        milestone: String,
        assignees: String,
    ): NewIssueDraft? {
        val cleanTitle = title.trim()
        if (cleanTitle.isEmpty()) return null
        return NewIssueDraft(
            title = cleanTitle,
            body = body.trim(),
            labels = split(labels),
            milestone = milestone.trim(),
            priority = dartIntTryParse(priority.trim())?.toInt() ?: 0,
            assignees = split(assignees),
        )
    }
}
