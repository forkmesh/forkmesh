package com.forkmesh.nativeapp.notes

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.model.OrgApiException

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

    fun get(id: String): JsonObject = jsonObject(call("GET", "/$id")["note"])

    fun create(title: String, markdown: String): JsonObject =
        jsonObject(call("POST", "", linkedMapOf("title" to title.trim(), "markdown" to markdown, "visibility" to "private"))["note"])

    fun save(id: String, baseVersion: Long, title: String, markdown: String): JsonObject =
        jsonObject(call("PATCH", "/$id", linkedMapOf("baseVersion" to baseVersion, "title" to title.trim(), "markdown" to markdown))["note"])

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
}

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

    fun filter(notes: List<JsonObject>, query: String): List<JsonObject> {
        val q = query.trim().lowercase()
        if (q.isEmpty()) return notes
        return notes.filter { jsonString(it["title"], "null").lowercase().contains(q) }
    }

    fun shareName(raw: String): String = raw.trim().replace(Regex("^@+"), "").lowercase()

    fun saveError(error: Throwable): String {
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
