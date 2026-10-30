package com.forkmesh.nativeapp.composer

import com.forkmesh.nativeapp.agents.AgentModelChoice
import com.forkmesh.nativeapp.agents.PromptAttachment

enum class QuickPromptAction(val raw: String, val label: String, val note: String) {
    BugReport("bug-report", "Bug report", "Send this to the ForkMesh team instead of starting an agent"),
    Task("task", "Task · assign", "Create an organization task for the member you pick"),
    Issue("manual", "Manual · create issue", "File an issue from this prompt instead of starting an agent"),
    ActivityPub("activitypub", "ActivityPub · post", "Publish this prompt to the repository's ActivityPub followers"),
    Note("note", "Take a note", "Save this prompt as a note instead of starting an agent"),
    ;

    val choice: AgentModelChoice get() = AgentModelChoice(PROVIDER, raw, "", label, note)

    val takesPhotos: Boolean get() = this == BugReport

    val needsRepository: Boolean get() = this == Issue || this == ActivityPub

    val needsAccount: Boolean get() = this != BugReport

    companion object {
        const val PROVIDER = "prompt-action"
        const val STORE_KEY = "agent/quick/action"
        const val ASSIGNEE_KEY = "tasks/composerAssignee"

        fun of(raw: String?): QuickPromptAction? = entries.firstOrNull { it.raw == raw }

        fun of(choice: AgentModelChoice): QuickPromptAction? = if (choice.provider == PROVIDER) of(choice.alias) else null

        fun titleAndBody(prompt: String, limit: Int = 160): Pair<String, String> {
            val text = prompt.trim()
            val cut = text.indexOf('\n')
            val first = if (cut < 0) text else text.substring(0, cut)
            var title = first.split(Regex("\\s+")).filter { it.isNotEmpty() }.joinToString(" ")
            var body = if (cut < 0) "" else text.substring(cut + 1).trim()
            if (title.isEmpty()) {
                title = "Update"
                body = text
            }
            if (title.length > limit) title = title.take(limit - 1).trim() + "…"
            return title to body
        }
    }
}

class QuickActionError(override val message: String) : RuntimeException(message) {
    override fun toString(): String = message
}

data class QuickTaskPeople(val actor: String, val members: List<String>) {
    val names: List<String>
        get() {
            val out = ArrayList<String>()
            for (name in listOf(actor) + members) if (name.isNotEmpty() && name !in out) out += name
            return out
        }
}

interface QuickPromptActions {
    fun reportBug(text: String, photos: List<PromptAttachment>)

    fun taskPeople(): QuickTaskPeople

    fun createTask(title: String, details: String, assignee: String, repository: String)

    fun createIssue(owner: String, name: String, title: String, body: String)

    fun postToActivityPub(owner: String, name: String, title: String, body: String)

    fun takeNote(title: String, markdown: String)
}
