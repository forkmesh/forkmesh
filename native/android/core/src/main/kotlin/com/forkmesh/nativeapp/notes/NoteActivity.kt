package com.forkmesh.nativeapp.notes

import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.model.OrgApiException

enum class NoteLiveTab(val label: String) {
    History("History"),
    Activity("Activity"),
    Chat("Chat"),
}

object NoteHistory {
    const val EMPTY = "No saved versions yet."
    const val UNAVAILABLE = "History is unavailable."

    fun subtitle(createdAt: Long, author: String, now: Long = System.currentTimeMillis()): String =
        relativeTime(createdAt, now) + if (author.isNotBlank()) " · by @${author.trim()}" else ""

    fun restoreTitle(version: Long): String = "Restore version $version?"

    fun restorePrompt(version: Long): String =
        "The note goes back to the title and text of version $version, saved as a new version. The current text stays in the history."

    fun restored(version: Long): String = "Restored version $version as a new version"

    fun viewTitle(version: Long, current: Boolean): String = "Version $version" + if (current) " · Current" else ""
}

data class NoteActivityEntry(
    val kind: String,
    val actor: String = "",
    val at: Long = 0L,
    val count: Long = 1L,
    val version: Long = 0L,
    val fromVersion: Long = 0L,
    val readers: Long = 0L,
    val detail: JsonObject = emptyMap(),
) {
    val icon: String get() = NoteActivity.icon(kind)
    val text: String get() = NoteActivity.text(this)

    fun actorLabel(me: String): String = when {
        kind == "web" -> ""
        actor.isNotEmpty() && actor.equals(me.trim(), ignoreCase = true) -> "You"
        actor.isNotEmpty() -> actor
        else -> "Someone"
    }

    fun line(me: String): String = listOf(actorLabel(me), text).filter { it.isNotEmpty() }.joinToString(" ")
}

data class NoteActivityPayload(
    val entries: List<NoteActivityEntry> = emptyList(),
    val views: Long = 0L,
    val readers: Long = 0L,
    val versions: Long = 0L,
    val actions: Long = 0L,
) {
    val summary: String get() = listOf(
        NoteActivity.count(views, "view", "views"),
        NoteActivity.count(versions, "version", "versions"),
        NoteActivity.count(actions, "action", "actions"),
    ).joinToString(" · ")

    companion object {
        fun from(json: JsonObject): NoteActivityPayload {
            val counts = jsonObject(json["counts"])
            return NoteActivityPayload(
                entries = NoteActivity.entries(json),
                views = jsonInt(counts["views"]),
                readers = jsonInt(counts["readers"]),
                versions = jsonInt(counts["versions"]),
                actions = jsonInt(counts["actions"]),
            )
        }
    }
}

object NoteActivity {
    const val EDIT_GROUP_MS = 30 * 60 * 1000L
    const val RELOAD_MS = 300L
    const val EMPTY = "Nothing yet."
    const val UNAVAILABLE = "Activity is unavailable right now."

    private val icons = mapOf(
        "view" to "visibility", "web" to "public", "edit" to "edit", "create" to "note_add_outlined",
        "restore" to "restore", "share" to "person_add", "role" to "manage_accounts", "unshare" to "person_remove",
        "publish" to "public", "unpublish" to "visibility_off_outlined", "link" to "link", "unlink" to "link_off",
        "export" to "download_outlined", "copy-link" to "content_copy", "task" to "task_alt", "issue" to "adjust",
        "comment" to "chat_bubble_outline", "resolve" to "check_circle", "reopen" to "undo",
    )

    fun icon(kind: String): String = icons[kind] ?: "history"

    fun count(value: Long, one: String, many: String): String = "$value ${if (value == 1L) one else many}"

    fun entry(json: JsonObject): NoteActivityEntry? {
        val kind = jsonString(json["kind"]).trim()
        if (kind.isEmpty()) return null
        return NoteActivityEntry(
            kind = kind,
            actor = jsonString(json["actor"]).trim(),
            at = jsonInt(json["at"]),
            count = jsonInt(json["count"]).coerceAtLeast(1L),
            version = jsonInt(json["version"]),
            fromVersion = jsonInt(json["version"]),
            detail = jsonObject(json["detail"]),
        )
    }

    fun entries(json: JsonObject): List<NoteActivityEntry> {
        val counts = jsonObject(json["counts"])
        val events = jsonObjects(json["events"]).mapNotNull(::entry).toMutableList()
        val webViews = jsonInt(counts["webViews"])
        if (webViews > 0) {
            events.add(NoteActivityEntry("web", at = jsonInt(counts["webLastViewedAt"]), count = webViews, readers = jsonInt(counts["webReaders"])))
        }
        val sorted = events.sortedByDescending { it.at }
        val grouped = mutableListOf<NoteActivityEntry>()
        var oldestAt = 0L
        for (entry in sorted) {
            val last = grouped.lastOrNull()
            if (entry.kind == "edit" && last?.kind == "edit" && last.actor == entry.actor && oldestAt - entry.at <= EDIT_GROUP_MS) {
                grouped[grouped.size - 1] = last.copy(fromVersion = entry.version, count = last.count + 1)
            } else {
                grouped.add(if (entry.kind == "edit") entry.copy(fromVersion = entry.version, count = 1L) else entry)
            }
            oldestAt = entry.at
        }
        return grouped
    }

    private fun ref(detail: JsonObject): String {
        val owner = jsonString(detail["owner"])
        val repo = jsonString(detail["repo"])
        val path = if (owner.isNotEmpty()) "$owner/$repo" else repo
        return "$path#${jsonInt(detail["number"])}"
    }

    fun text(entry: NoteActivityEntry): String {
        val detail = entry.detail
        val name = jsonString(detail["name"]).ifEmpty { "someone" }
        val role = if (jsonString(detail["role"]) == "editor") "can edit" else "can view"
        val count = entry.count
        return when (entry.kind) {
            "view" -> if (count > 1) "viewed it $count times" else "viewed it"
            "web" -> "${count(count, "read", "reads")} of the published page" + if (entry.readers > 1) " by ${entry.readers} readers" else ""
            "edit" -> if (count > 1) "edited it · v${entry.fromVersion}–v${entry.version}, $count saves" else "edited it · v${entry.version}"
            "create" -> "created the note"
            "restore" -> "restored version ${jsonInt(detail["version"])}" + jsonInt(detail["as"]).let { if (it > 0) " as v$it" else "" }
            "share" -> "shared it with $name · $role"
            "role" -> "changed $name to $role"
            "unshare" -> "stopped sharing with $name"
            "publish" -> "published it to the web"
            "unpublish" -> "unpublished it"
            "link" -> "linked ${ref(detail)}"
            "unlink" -> "unlinked ${ref(detail)}"
            "export" -> "downloaded the Markdown"
            "copy-link" -> "copied the link"
            "task" -> "turned it into a task"
            "issue" -> if (jsonString(detail["repo"]).isNotEmpty()) "filed issue ${ref(detail)} from it" else "filed an issue from it"
            "comment" -> if (detail["reply"] == true) "replied to a comment" else "commented"
            "resolve" -> "resolved a comment"
            "reopen" -> "reopened a comment"
            else -> entry.kind
        }
    }
}

data class NoteChatMessage(val id: String, val author: String = "", val body: String = "", val createdAt: Long = 0L) {
    fun isBy(me: String): Boolean = me.isNotBlank() && author.equals(me.trim(), ignoreCase = true)

    fun authorLabel(me: String): String = when {
        isBy(me) -> "You"
        author.isNotEmpty() -> "@$author"
        else -> "Someone"
    }

    fun canDelete(me: String, canModerate: Boolean): Boolean = canModerate || isBy(me)

    companion object {
        fun from(json: JsonObject): NoteChatMessage? {
            val id = jsonString(json["id"]).trim()
            if (id.isEmpty()) return null
            return NoteChatMessage(id, jsonString(json["author"]).trim(), jsonString(json["body"]), jsonInt(json["createdAt"]))
        }
    }
}

data class NoteChatPayload(
    val messages: List<NoteChatMessage> = emptyList(),
    val canModerate: Boolean = false,
    val message: NoteChatMessage? = null,
) {
    val latestAt: Long get() = messages.maxOfOrNull { it.createdAt } ?: 0L

    companion object {
        fun from(json: JsonObject): NoteChatPayload = NoteChatPayload(
            messages = jsonObjects(json["messages"]).mapNotNull(NoteChatMessage::from).sortedBy { it.createdAt },
            canModerate = json["canModerate"] == true,
            message = (json["message"] as? Map<*, *>)?.let { NoteChatMessage.from(jsonObject(it)) },
        )
    }
}

object NoteChat {
    const val MAX_BODY = 2000
    const val RELOAD_MS = 300L
    const val EMPTY = "No messages yet. Say hello to everyone in this note."
    const val UNAVAILABLE = "Chat is unavailable right now."
    const val SEEN_PREFS = "forkmesh.notes"

    fun seenKey(noteId: String): String = "chatSeen.$noteId"

    fun canSend(body: String): Boolean = body.isNotBlank() && body.trim().length <= MAX_BODY

    fun unread(messages: List<NoteChatMessage>, me: String, seenAt: Long): Int =
        messages.count { !it.isBy(me) && it.createdAt > seenAt }

    fun badge(unread: Int): String = when {
        unread <= 0 -> ""
        unread > 99 -> "99+"
        else -> "$unread"
    }

    fun buttonLabel(unread: Int): String = when {
        unread <= 0 -> "Chat"
        unread == 1 -> "Chat, 1 unread message"
        else -> "Chat, $unread unread messages"
    }

    fun deleteTitle(): String = "Delete message?"

    fun deletePrompt(message: NoteChatMessage, me: String): String =
        if (message.isBy(me) || message.author.isEmpty()) "This message will be removed for everyone." else "${message.author}'s message will be removed for everyone."

    fun error(error: Throwable, fallback: String = "The message was not sent."): String {
        if (error is OrgApiException) {
            when (error.code) {
                "invalid_message" -> return "Write a message of up to $MAX_BODY characters."
                "message_author_required" -> return "Only the person who sent a message, or the note's owner, can delete it."
                "message_not_found" -> return "That message was already deleted."
            }
        }
        return apiErrorText(error, fallback)
    }
}

data class NoteVersion(val version: Long, val createdAt: Long, val author: String, val title: String, val markdown: String, val emoji: String) {
    companion object {
        fun from(json: JsonObject): NoteVersion = NoteVersion(
            version = jsonInt(json["version"]),
            createdAt = jsonInt(json["createdAt"]),
            author = jsonString(json["author"]).trim(),
            title = jsonString(json["title"]),
            markdown = jsonString(json["markdown"]),
            emoji = jsonString(json["emoji"]),
        )

        fun error(error: Throwable, fallback: String = "That version could not be opened."): String {
            if (error is OrgApiException) {
                when (error.code) {
                    "version_not_found" -> return "That version is no longer kept."
                    "invalid_version" -> return "That version could not be opened."
                }
            }
            return apiErrorText(error, fallback)
        }
    }
}
