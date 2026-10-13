package com.forkmesh.nativeapp.planning

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import java.util.Base64

enum class TaskListFilter(val wire: String) {
    MINE("mine"), OPEN("open"), DONE("done");

    companion object {
        fun of(route: String?): TaskListFilter = entries.firstOrNull { it.wire == route?.lowercase() } ?: MINE
    }
}

data class TaskHistoryEntry(val id: String, val kind: String, val actor: String, val summary: String, val createdAtMs: Long) {
    companion object {
        fun fromJson(json: JsonMap) = TaskHistoryEntry(json.str("id"), json.str("kind"), json.str("actor"), json.str("summary"), json.long("createdAt"))
    }
}

class TaskAttachmentDraft(val name: String, val mime: String, val data: ByteArray) {
    val json: Map<String, Any?>
        get() = linkedMapOf("name" to name.take(120), "mime" to mime, "file" to Base64.getEncoder().encodeToString(data))

    companion object {
        const val MAX_COUNT = 4
        const val MAX_BYTES = 256 * 1024
        const val MAX_TOTAL_BYTES = 1024 * 1024
        val MIMES = listOf("image/png", "image/jpeg", "image/gif", "image/webp")

        fun validate(drafts: List<TaskAttachmentDraft>): String? {
            if (drafts.size > MAX_COUNT) return "Attach at most $MAX_COUNT images."
            if (drafts.any { it.mime !in MIMES }) return "Attach PNG, JPEG, GIF or WebP images."
            if (drafts.any { it.data.size > MAX_BYTES }) return "Each image must be under 256 KB."
            if (drafts.sumOf { it.data.size } > MAX_TOTAL_BYTES) return "Images must add up to under 1 MB."
            return null
        }
    }
}

data class TaskAgentInfo(val provider: String, val status: String, val sessionId: String) {
    val label: String
        get() {
            val state = when (status) {
                "queued" -> "Queued"
                "running" -> "Running"
                "waiting" -> "Waiting for you"
                "success" -> "Finished"
                "failed" -> "Failed"
                "stopped" -> "Stopped"
                else -> if (status.isEmpty()) "Linked" else status.lowercase().replaceFirstChar { it.uppercase() }
            }
            return if (provider.isEmpty()) "Agent: $state" else "${provider.lowercase().replaceFirstChar { it.uppercase() }}: $state"
        }

    companion object {
        fun of(task: JsonMap): TaskAgentInfo? {
            val agent = task.obj("agent")
            val session = task.str("agentSessionId").ifEmpty { agent.str("sessionId") }
            val status = agent.str("status")
            if (session.isEmpty() && status.isEmpty()) return null
            return TaskAgentInfo(agent.str("provider"), status, session)
        }
    }
}

object PlanningConversion {
    const val TASK_DETAILS_MAX = 4_000

    fun title(noteTitle: String, markdown: String): String {
        val trimmed = noteTitle.trim()
        if (trimmed.isNotEmpty()) return trimmed.take(160)
        val line = markdown.split('\n').map { it.trim() }.firstOrNull { it.isNotEmpty() } ?: ""
        val cleaned = line.dropWhile { it == '#' }.trim()
        return cleaned.ifEmpty { "Untitled note" }.take(160)
    }

    fun details(noteTitle: String, markdown: String, publicUrl: String?, limit: Int = TASK_DETAILS_MAX): String {
        var footer = "\n\n---\nConverted from the note \"${title(noteTitle, markdown)}\""
        if (!publicUrl.isNullOrEmpty()) footer += " (published at $publicUrl)"
        val room = maxOf(0, limit - footer.length)
        val body = if (markdown.length > room) markdown.take(maxOf(0, room - 1)) + "…" else markdown
        return body + footer
    }

    fun agentPrompt(task: JsonMap): String {
        var text = "[task:${task.str("id")}] ${task.str("title")}"
        val details = task.str("details").trim()
        if (details.isNotEmpty()) text += "\n\n$details"
        return text
    }
}

class TaskOperationException(message: String) : Exception(message)

fun interface IssueFiler {
    fun file(owner: String, name: String, title: String, body: String)
}

class TaskOperations(private val call: (String, String, Map<String, Any?>?) -> JsonMap) {
    var history: List<TaskHistoryEntry> = emptyList()
        private set

    private fun <T> run(fallback: String, work: () -> T): T = try {
        work()
    } catch (thrown: TaskOperationException) {
        throw thrown
    } catch (thrown: Exception) {
        throw TaskOperationException(PlanningErrors.text(thrown, fallback))
    }

    fun loadHistory(taskId: String) {
        val data = run("The history could not be loaded.") { call("GET", "/$taskId/history", null) }
        history = data.maps("history").take(200).map(TaskHistoryEntry::fromJson)
    }

    fun quickAdd(title: String, attachments: List<TaskAttachmentDraft> = emptyList()): JsonMap {
        val trimmed = title.trim()
        if (trimmed.isEmpty()) throw TaskOperationException("Give the task a title.")
        TaskAttachmentDraft.validate(attachments)?.let { throw TaskOperationException(it) }
        val body = linkedMapOf<String, Any?>(
            "title" to trimmed.take(160),
            "department" to "general",
            "destination" to "department",
            "assigneeKind" to "unassigned",
        )
        if (attachments.isNotEmpty()) body["attachments"] = attachments.map { it.json }
        return run("The task was not created.") { call("POST", "", body).obj("task") }
    }

    fun followUp(parentId: String, title: String): JsonMap {
        val trimmed = title.trim()
        if (trimmed.isEmpty()) throw TaskOperationException("Give the follow-up a title.")
        return run("The follow-up was not created.") {
            call("POST", "", linkedMapOf("title" to trimmed.take(160), "parentTaskId" to parentId)).obj("task")
        }
    }

    fun assignToAgent(taskId: String, repository: String): JsonMap {
        val repo = PlanningRepo.of(repository) ?: throw TaskOperationException("Choose a repository as owner/name.")
        return run("The task was not assigned.") {
            call("PATCH", "/$taskId", linkedMapOf("assigneeKind" to "agent", "repository" to repo.slug)).obj("task")
        }
    }

    fun setPriority(taskId: String, priority: Int): JsonMap {
        if (priority !in 1..99) throw TaskOperationException("Priority runs from 1 to 99.")
        return run("The priority was not changed.") { call("PATCH", "/$taskId", mapOf("priority" to priority.toLong())).obj("task") }
    }

    fun linkAgentSession(taskId: String, sessionId: String): JsonMap {
        val clean = sessionId.trim()
        if (clean.isEmpty() || clean.length > 128) throw TaskOperationException("That agent run cannot be linked.")
        return run("The agent run was not linked.") { call("PATCH", "/$taskId", mapOf("agentSessionId" to clean)).obj("task") }
    }

    fun moveToIssue(task: JsonMap, repository: String, filer: IssueFiler) {
        val repo = PlanningRepo.of(repository) ?: throw TaskOperationException("Choose a repository as owner/name.")
        val id = task.str("id")
        var body = task.str("details")
        val note = task.str("completionNote")
        if (note.isNotEmpty()) body += "\n\n$note"
        body += "\n\n---\nMoved from organization task $id."
        run("The issue was not filed.") {
            filer.file(repo.owner, repo.name, task.str("title"), body)
            call("DELETE", "/$id", emptyMap())
        }
    }

    fun deleteClosed(tasks: List<JsonMap>): Pair<Int, Int> {
        var deleted = 0
        var failed = 0
        for (task in tasks.take(200)) {
            if (task.str("status") != "done") continue
            try {
                call("DELETE", "/${task.str("id")}", emptyMap())
                deleted++
            } catch (_: Exception) {
                failed++
            }
        }
        return deleted to failed
    }
}

object NoteConversions {
    fun toTask(noteTitle: String, markdown: String, publicUrl: String?, call: (String, String, Map<String, Any?>?) -> JsonMap): JsonMap {
        val body = linkedMapOf<String, Any?>(
            "title" to PlanningConversion.title(noteTitle, markdown),
            "details" to PlanningConversion.details(noteTitle, markdown, publicUrl),
            "department" to "general",
            "destination" to "department",
            "assigneeKind" to "unassigned",
        )
        return try {
            call("POST", "", body).obj("task")
        } catch (thrown: Exception) {
            throw TaskOperationException(PlanningErrors.text(thrown, "The task was not created."))
        }
    }

    fun toIssue(noteTitle: String, markdown: String, publicUrl: String?, repository: String, filer: IssueFiler): String? {
        val repo = PlanningRepo.of(repository) ?: return "Choose a repository as owner/name."
        return try {
            filer.file(
                repo.owner, repo.name, PlanningConversion.title(noteTitle, markdown),
                PlanningConversion.details(noteTitle, markdown, publicUrl, limit = 60_000),
            )
            null
        } catch (_: Exception) {
            "The issue was not filed."
        }
    }
}
