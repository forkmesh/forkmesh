package com.forkmesh.nativeapp.notes

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.model.OrgApiException
import java.net.URI
import java.net.URLEncoder
import java.nio.charset.StandardCharsets

typealias JsonObject = Map<String, Any?>

@Suppress("UNCHECKED_CAST")
fun jsonObjects(value: Any?): List<JsonObject> =
    (value as? List<*>)?.mapNotNull { it as? Map<String, Any?> } ?: emptyList()

@Suppress("UNCHECKED_CAST")
fun jsonObject(value: Any?): JsonObject = value as? Map<String, Any?> ?: emptyMap()

fun jsonInt(value: Any?): Long = when (value) {
    is Number -> value.toLong()
    is String -> value.toLongOrNull() ?: 0L
    else -> 0L
}

fun jsonString(value: Any?, fallback: String = ""): String = when (value) {
    null -> fallback
    is String -> value
    is Double -> if (value == Math.floor(value) && !value.isInfinite()) value.toLong().toString() else value.toString()
    else -> value.toString()
}

fun apiErrorText(error: Throwable, fallback: String): String = (error as? OrgApiException)?.message ?: fallback

class NotesApi(private val api: ApiService) {
    fun call(method: String, suffix: String = "", body: JsonObject? = null): JsonObject =
        api.accountApiCall(method, "/api/notes$suffix", body)

    fun list(): List<JsonObject> = jsonObjects(call("GET")["notes"])

    fun search(raw: String): List<JsonObject> {
        val query = NotesLogic.searchQuery(raw)
        if (query.isEmpty()) return emptyList()
        return jsonObjects(call("GET", "?query=${URLEncoder.encode(query, StandardCharsets.UTF_8)}")["notes"])
    }

    fun get(id: String): JsonObject = jsonObject(call("GET", "/$id")["note"])

    fun create(title: String, markdown: String, emoji: String = ""): JsonObject {
        val body = linkedMapOf<String, Any?>("title" to title.trim(), "markdown" to markdown, "visibility" to "private")
        if (emoji.isNotBlank()) body["emoji"] = emoji.trim()
        return jsonObject(call("POST", "", body)["note"])
    }

    fun setEmoji(id: String, baseVersion: Long, emoji: String): JsonObject =
        jsonObject(call("PATCH", "/$id", linkedMapOf("baseVersion" to baseVersion, "emoji" to emoji.trim()))["note"])

    fun save(id: String, baseVersion: Long, title: String, markdown: String): JsonObject = try {
        jsonObject(call("PATCH", "/$id", linkedMapOf("baseVersion" to baseVersion, "title" to title.trim(), "markdown" to markdown))["note"])
    } catch (error: OrgApiException) {
        if (error.code != "version_conflict") throw error
        throw NoteConflictException(get(id))
    }

    fun setVisibility(id: String, baseVersion: Long, visibility: String): JsonObject =
        jsonObject(call("PATCH", "/$id", linkedMapOf("baseVersion" to baseVersion, "visibility" to visibility))["note"])

    fun delete(id: String) {
        call("DELETE", "/$id")
    }

    fun versions(id: String): List<JsonObject> = jsonObjects(call("GET", "/$id/versions")["versions"])

    fun restore(id: String, version: Long, baseVersion: Long): JsonObject =
        jsonObject(call("POST", "/$id/versions", linkedMapOf("version" to version, "baseVersion" to baseVersion))["note"])

    fun addShare(id: String, type: String, name: String, role: String): List<JsonObject> =
        jsonObjects(call("POST", "/$id/shares", linkedMapOf("type" to type, "name" to NotesLogic.shareName(name), "role" to role))["shares"])

    fun removeShare(id: String, type: String, name: String): List<JsonObject> =
        jsonObjects(call("DELETE", "/$id/shares", linkedMapOf("type" to type, "name" to name))["shares"])

    fun shareSuggestions(id: String, raw: String): List<NoteShareSuggestion> {
        val query = NotesLogic.suggestionQuery(raw)
        if (query.isEmpty()) return emptyList()
        val encoded = URLEncoder.encode(query, StandardCharsets.UTF_8)
        return NotesLogic.suggestions(call("GET", "/$id/shares?query=$encoded"))
    }

    fun links(id: String): List<NoteLink> = NoteLink.list(call("GET", "/$id/links")["links"])

    fun addLink(id: String, link: NoteLink): List<NoteLink> = NoteLink.list(call("POST", "/$id/links", link.body())["links"])

    fun removeLink(id: String, link: NoteLink): List<NoteLink> = NoteLink.list(call("DELETE", "/$id/links", link.body())["links"])

    fun recordView(id: String) {
        call("POST", "/$id/activity", linkedMapOf("kind" to "view"))
    }

    fun comments(id: String): NoteCommentsPayload = NoteCommentsPayload.from(call("GET", "/$id/comments"))

    fun addComment(id: String, body: String, parentId: String? = null, quote: NoteQuote? = null): NoteCommentsPayload {
        val payload = linkedMapOf<String, Any?>("body" to body.trim())
        if (!parentId.isNullOrEmpty()) payload["parentId"] = parentId else quote?.let { payload.putAll(it.body()) }
        return NoteCommentsPayload.from(call("POST", "/$id/comments", payload))
    }

    fun editComment(id: String, commentId: String, body: String): NoteCommentsPayload =
        NoteCommentsPayload.from(call("PATCH", "/$id/comments/$commentId", linkedMapOf("body" to body.trim())))

    fun resolveComment(id: String, commentId: String, resolved: Boolean): NoteCommentsPayload =
        NoteCommentsPayload.from(call("PATCH", "/$id/comments/$commentId", linkedMapOf("resolved" to resolved)))

    fun deleteComment(id: String, commentId: String): NoteCommentsPayload =
        NoteCommentsPayload.from(call("DELETE", "/$id/comments/$commentId"))
}

class NoteConflictException(val latest: JsonObject) : RuntimeException("version_conflict")

data class NoteShareSuggestion(val type: String, val name: String) {
    val typeLabel: String get() = if (type == "organization") "Organization" else "User"
}

data class NoteLink(val owner: String, val repo: String, val kind: String, val number: Long) {
    val label: String get() = "$repo#$number"
    val kindLabel: String get() = when (kind) {
        "pull" -> "Pull request"
        "discussion" -> "Discussion"
        else -> "Issue"
    }
    val path: String get() = "/$owner/$repo/${if (kind == "pull") "pulls" else "${kind}s"}/$number"

    fun body(): JsonObject = linkedMapOf("owner" to owner, "repo" to repo, "kind" to kind, "number" to number)

    companion object {
        val kinds: List<String> = listOf("issue", "pull", "discussion")
        private val shorthand = Regex("^([\\w.-]+)/([\\w.-]+)#(\\d+)$")
        private val pathLink = Regex("^/([\\w.-]+)/([\\w.-]+)/(issues|issue|pull|pulls|discussions|discussion)/(\\d+)(?:/.*)?$")
        private val sections = mapOf(
            "issues" to "issue", "issue" to "issue", "pull" to "pull", "pulls" to "pull",
            "discussions" to "discussion", "discussion" to "discussion",
        )

        fun list(value: Any?): List<NoteLink> = jsonObjects(value).mapNotNull(::from)

        fun from(json: JsonObject): NoteLink? {
            val number = jsonInt(json["number"])
            val owner = jsonString(json["owner"])
            val repo = jsonString(json["repo"])
            if (owner.isEmpty() || repo.isEmpty() || number < 1) return null
            return NoteLink(owner, repo, jsonString(json["kind"], "issue"), number)
        }

        fun parse(raw: String, fallbackKind: String = "issue"): NoteLink? {
            val text = raw.trim()
            if (text.isEmpty()) return null
            shorthand.matchEntire(text)?.let { match ->
                val number = match.groupValues[3].toLongOrNull()?.takeIf { it > 0 } ?: return null
                return NoteLink(match.groupValues[1], match.groupValues[2], fallbackKind.takeIf { it in kinds } ?: "issue", number)
            }
            val uri = try {
                URI(text)
            } catch (_: Exception) {
                return null
            }
            val path = uri.rawPath ?: return null
            pathLink.matchEntire(path)?.let { match ->
                val number = match.groupValues[4].toLongOrNull()?.takeIf { it > 0 } ?: return null
                return NoteLink(match.groupValues[1], match.groupValues[2], sections.getValue(match.groupValues[3]), number)
            }
            val parts = path.split('/').filter { it.isNotEmpty() }
            val tab = (uri.rawQuery ?: "").split('&').firstOrNull { it.startsWith("tab=") }?.substringAfter('=')
            val kind = sections[tab ?: ""]
            val number = uri.rawFragment?.toLongOrNull()?.takeIf { it > 0 }
            if (parts.size == 2 && kind != null && number != null) return NoteLink(parts[0], parts[1], kind, number)
            return null
        }
    }
}

enum class NoteFilter(val label: String) {
    All("All"),
    Mine("Mine"),
    Shared("Shared"),
    Public("Public"),
}

enum class NoteSaveState { Saved, Dirty, Saving, Conflict, Failed, ReadOnly }

data class NoteEdit(val text: String, val start: Int, val end: Int)

data class NoteSearch(val query: String, val notes: List<JsonObject>)

object NotesLogic {
    fun reach(note: JsonObject): String {
        if (note["visibility"] == "public") return "Public"
        val shares = jsonObjects(note["shares"]).size
        return if (shares > 0) "Shared with $shares" else "Private"
    }

    fun role(note: JsonObject?): String = jsonString(note?.get("role"), "owner")

    fun canEdit(note: JsonObject?): Boolean = role(note) == "owner" || role(note) == "editor"

    fun rowSubtitle(note: JsonObject, now: Long = System.currentTimeMillis()): String {
        val parts = mutableListOf("Edited ${relativeTime(jsonInt(note["updatedAt"]), now)}", reach(note))
        val role = role(note)
        if (role != "owner") parts.add(if (role == "editor") "Can edit" else "Read only")
        return parts.joinToString(" · ")
    }

    fun editorMeta(note: JsonObject?, now: Long = System.currentTimeMillis()): String {
        note ?: return "Only you can see a new note until you share or publish it."
        val parts = mutableListOf(
            "Version ${jsonInt(note["version"])}",
            "edited ${relativeTime(jsonInt(note["updatedAt"]), now)}",
            reach(note),
        )
        if (!canEdit(note)) parts.add("read only")
        return parts.joinToString(" · ")
    }

    const val AUTOSAVE_MS = 1500L
    const val SUGGEST_MS = 150L
    const val SEARCH_MS = 250L
    const val MAX_SEARCH_QUERY = 200
    const val MAX_SUGGEST_QUERY = 63

    fun matches(note: JsonObject, filter: NoteFilter): Boolean = when (filter) {
        NoteFilter.All -> true
        NoteFilter.Mine -> role(note) == "owner"
        NoteFilter.Shared -> role(note) != "owner"
        NoteFilter.Public -> note["visibility"] == "public"
    }

    fun counts(notes: List<JsonObject>): Map<NoteFilter, Int> =
        NoteFilter.entries.associateWith { filter -> notes.count { matches(it, filter) } }

    fun filter(notes: List<JsonObject>, query: String, filter: NoteFilter = NoteFilter.All): List<JsonObject> {
        val q = query.trim().lowercase()
        return notes.filter { note -> matches(note, filter) && (q.isEmpty() || jsonString(note["title"], "null").lowercase().contains(q)) }
    }

    fun searchQuery(raw: String): String = raw.trim().lowercase().takeIf { it.length <= MAX_SEARCH_QUERY } ?: ""

    fun searchResults(notes: List<JsonObject>, query: String, answer: NoteSearch?): List<JsonObject> {
        if (query.isBlank()) return notes
        if (answer != null && answer.query == searchQuery(query)) return answer.notes
        return filter(notes, query)
    }

    fun searchPending(query: String, answer: NoteSearch?): Boolean {
        val q = searchQuery(query)
        return q.isNotEmpty() && answer?.query != q
    }

    fun emptyListText(query: String, filter: NoteFilter, pending: Boolean = false): String = when {
        pending -> "Searching notes\u2026"
        query.isNotBlank() -> "No notes match \u201c${query.trim()}\u201d."
        filter == NoteFilter.Shared -> "Nobody has shared a note with you yet."
        filter == NoteFilter.Public -> "You haven't published any notes."
        filter == NoteFilter.Mine -> "You haven't written any notes yet."
        else -> "No notes yet. Create one to get started."
    }

    fun shareName(raw: String): String = raw.trim().replace(Regex("^@+"), "").lowercase()

    fun suggestionQuery(raw: String): String = shareName(raw).takeIf { it.length <= MAX_SUGGEST_QUERY } ?: ""

    fun suggestions(payload: JsonObject): List<NoteShareSuggestion> = jsonObjects(payload["suggestions"]).mapNotNull { item ->
        val name = jsonString(item["name"]).trim()
        if (name.isEmpty()) null else NoteShareSuggestion(if (item["type"] == "organization") "organization" else "user", name)
    }

    fun accessUrl(origin: String, id: String): String =
        "${origin.trimEnd('/')}/dashboard/notes?note=${URLEncoder.encode(id, StandardCharsets.UTF_8)}"

    fun publicUrl(origin: String, id: String): String = "${origin.trimEnd('/')}/notes/$id"

    fun linkUrl(origin: String, link: NoteLink): String = "${origin.trimEnd('/')}${link.path}"

    fun sharedNotice(name: String): String = "Shared with ${shareName(name)}. Copy the link below to send it."

    fun roleNotice(name: String, role: String): String = "$name can now ${if (role == "editor") "edit" else "view"}"

    fun roleLabel(role: String): String = if (role == "editor") "Can edit" else "Can view"

    fun exportFileName(title: String): String {
        val slug = title.replace(Regex("[^\\w.-]+"), "-").trim('-').lowercase()
        return "${slug.ifEmpty { "note" }}.md"
    }

    fun exportMarkdown(title: String, markdown: String): String = "# ${title.ifBlank { "Untitled note" }}\n\n$markdown"

    fun format(text: String, start: Int, end: Int, kind: String): NoteEdit {
        val from = start.coerceIn(0, text.length)
        val to = end.coerceIn(from, text.length)
        val wraps = mapOf(
            "bold" to Triple("**", "**", "bold text"),
            "italic" to Triple("_", "_", "italic text"),
            "code" to Triple("`", "`", "code"),
            "link" to Triple("[", "](https://)", "link text"),
        )
        wraps[kind]?.let { (before, after, placeholder) ->
            val body = text.substring(from, to).ifEmpty { placeholder }
            val next = text.substring(0, from) + before + body + after + text.substring(to)
            val cursor = from + before.length
            return NoteEdit(next, cursor, cursor + body.length)
        }
        val prefix = when (kind) {
            "heading" -> "## "
            "list" -> "- "
            "task" -> "- [ ] "
            else -> return NoteEdit(text, from, to)
        }
        val lineStart = if (from == 0) 0 else text.lastIndexOf('\n', from - 1) + 1
        return NoteEdit(text.substring(0, lineStart) + prefix + text.substring(lineStart), from + prefix.length, to + prefix.length)
    }

    fun saveStateLabel(state: NoteSaveState, detail: String = ""): String = when (state) {
        NoteSaveState.Saved -> "Saved"
        NoteSaveState.Dirty -> "Unsaved changes"
        NoteSaveState.Saving -> "Saving\u2026"
        NoteSaveState.Conflict -> "Not saved \u2014 resolve the conflict"
        NoteSaveState.Failed -> detail.ifEmpty { "Not saved" }
        NoteSaveState.ReadOnly -> "View only"
    }

    fun conflictText(latest: JsonObject): String {
        val version = jsonInt(latest["version"])
        return "Someone else saved version $version while you were editing. Keep your text on top of it, or take theirs."
    }

    fun linkError(error: Throwable): String {
        if (error is OrgApiException) {
            when (error.code) {
                "invalid_link" -> return "Paste an issue or pull request URL, or type owner/repo#123."
                "repository_not_found" -> return "That repository doesn't exist, or you don't have access to it."
            }
        }
        return apiErrorText(error, "The link was not changed.")
    }

    fun saveError(error: Throwable): String {
        if (error is NoteConflictException) return "Someone else saved this note first."
        if (error is OrgApiException) {
            if (error.code == "version_conflict") return "Someone else saved this note first. Copy your changes, then refresh to see theirs."
            return error.message
        }
        return "The note could not be saved."
    }

    fun shareError(error: Throwable, type: String): String {
        if (error is OrgApiException && error.code == "principal_not_found") {
            return "No ${if (type == "user") "user" else "organization"} has that name."
        }
        return apiErrorText(error, "Sharing was not changed.")
    }
}
