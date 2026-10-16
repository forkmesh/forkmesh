package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.Issue

class IssueTaskMoveException(val taskCreated: Boolean, val taskTrashed: Boolean, why: String) : RuntimeException(
    when {
        !taskCreated -> "The task could not be created, so the issue was kept. $why"
        taskTrashed -> "$why The new task was moved to trash, so the issue is unchanged."
        else -> "$why The new task could not be removed; delete it from Tasks."
    },
)

object IssueTaskMove {
    const val MAX_TITLE = 160
    const val MAX_DETAILS = 4000
    private val SEGMENT = Regex("^[A-Za-z0-9._-]{1,100}$")

    fun taskBody(owner: String, name: String, issue: Issue, mover: String): Map<String, Any?> {
        val repository = if (SEGMENT.matches(owner) && SEGMENT.matches(name)) "$owner/$name" else ""
        val who = mover.trim().ifEmpty { "a repository manager" }
        val source = "Moved from issue ${repository.ifEmpty { name }}#${issue.number} by $who."
        var title = issue.title.trim().ifEmpty { "Issue #${issue.number}" }
        if (title.length > MAX_TITLE) title = title.take(MAX_TITLE - 1) + "…"
        var text = issue.body.trim()
        val room = MAX_DETAILS - source.length - 2
        if (text.length > room) text = text.take(room - 1) + "…"
        return linkedMapOf(
            "title" to title,
            "details" to if (text.isEmpty()) source else "$text\n\n$source",
            "repository" to repository,
            "department" to "general",
            "destination" to "department",
            "assigneeKind" to "unassigned",
        )
    }

    fun move(
        issue: Issue,
        writer: CollabRecordWriter,
        mover: String,
        tasks: (method: String, suffix: String, body: Map<String, Any?>?) -> Map<String, Any?>,
    ): Map<String, Any?> {
        val task = try {
            @Suppress("UNCHECKED_CAST")
            (tasks("POST", "", taskBody(writer.owner, writer.name, issue, mover))["task"] as? Map<String, Any?>).orEmpty()
        } catch (error: Exception) {
            throw IssueTaskMoveException(taskCreated = false, taskTrashed = false, why = error.message.orEmpty())
        }
        val id = task.str("id")
        try {
            writer.sendIssue(issue.number, IssueEdit.deleteOwnIssue())
        } catch (error: Exception) {
            val trashed = id.isNotEmpty() && runCatching {
                tasks("DELETE", "/$id", linkedMapOf("reason" to "Issue #${issue.number} could not be deleted."))
            }.isSuccess
            throw IssueTaskMoveException(taskCreated = true, taskTrashed = trashed, why = error.message.orEmpty())
        }
        return task
    }
}
