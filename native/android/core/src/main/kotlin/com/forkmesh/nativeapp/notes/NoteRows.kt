package com.forkmesh.nativeapp.notes

import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale

enum class NoteReach(val label: String) {
    Private("Private"),
    Shared("Shared"),
    Public("Public"),
}

data class NoteShareFace(val name: String, val isOrganization: Boolean)

data class NoteRowInfo(
    val unread: Boolean,
    val lastEditedBy: String,
    val myLastViewedAt: Long,
    val unreadComments: Int,
    val openComments: Int,
    val reach: NoteReach,
    val faces: List<NoteShareFace>,
    val extraFaces: Int,
) {
    val extraLabel: String get() = if (extraFaces > 0) "+$extraFaces" else ""
}

object NoteTimes {
    private val time = DateTimeFormatter.ofPattern("h:mm a", Locale.US)
    private val dayTime = DateTimeFormatter.ofPattern("MMM d, h:mm a", Locale.US)
    private val date = DateTimeFormatter.ofPattern("MMM d, yyyy", Locale.US)
    private val full = DateTimeFormatter.ofPattern("EEEE, MMMM d, yyyy 'at' h:mm a", Locale.US)

    fun full(epochMs: Long, zone: ZoneId): String = if (epochMs <= 0) "" else full.format(Instant.ofEpochMilli(epochMs).atZone(zone))

    fun lastEdit(epochMs: Long, nowMs: Long, zone: ZoneId): String {
        if (epochMs <= 0) return ""
        val at = Instant.ofEpochMilli(epochMs).atZone(zone)
        val today = Instant.ofEpochMilli(nowMs).atZone(zone).toLocalDate()
        val day = at.toLocalDate()
        return when {
            day == today -> "Today ${time.format(at)}"
            day == today.minusDays(1) -> "Yesterday ${time.format(at)}"
            day.year == today.year -> dayTime.format(at)
            else -> date.format(at)
        }
    }
}

object NoteRows {
    const val MAX_FACES = 3

    fun reach(note: JsonObject): NoteReach = when {
        note["visibility"] == "public" -> NoteReach.Public
        jsonObjects(note["shares"]).isNotEmpty() -> NoteReach.Shared
        else -> NoteReach.Private
    }

    fun info(note: JsonObject): NoteRowInfo {
        val faces = jsonObjects(note["shares"]).mapNotNull { share ->
            val name = jsonString(share["name"]).trim()
            if (name.isEmpty()) null else NoteShareFace(name, share["type"] == "organization")
        }
        return NoteRowInfo(
            unread = note["unread"] == true,
            lastEditedBy = jsonString(note["lastEditedBy"]).trim(),
            myLastViewedAt = jsonInt(note["myLastViewedAt"]),
            unreadComments = jsonInt(note["unreadComments"]).coerceIn(0L, Int.MAX_VALUE.toLong()).toInt(),
            openComments = jsonInt(note["openComments"]).coerceIn(0L, Int.MAX_VALUE.toLong()).toInt(),
            reach = reach(note),
            faces = faces.take(MAX_FACES),
            extraFaces = maxOf(0, faces.size - MAX_FACES),
        )
    }

    fun lastEdit(note: JsonObject, me: String, nowMs: Long, zone: ZoneId): String {
        val time = NoteTimes.lastEdit(jsonInt(note["updatedAt"]), nowMs, zone)
        val editor = jsonString(note["lastEditedBy"]).trim()
        if (time.isEmpty()) return ""
        return if (editor.isEmpty() || editor.equals(me.trim(), ignoreCase = true)) time else "$time · by $editor"
    }

    fun commentCountLabel(openComments: Int): String =
        if (openComments == 1) "1 open comment thread" else "$openComments open comment threads"

    fun markRead(note: JsonObject, viewedAt: Long): JsonObject =
        note + mapOf("unread" to false, "unreadComments" to 0L, "myLastViewedAt" to maxOf(viewedAt, jsonInt(note["myLastViewedAt"])))

    fun applySeen(notes: List<JsonObject>, seen: Map<String, Long>): List<JsonObject> {
        if (seen.isEmpty()) return notes
        return notes.map { note ->
            val at = seen[jsonString(note["id"])]
            if (at != null && note["unread"] == true && jsonInt(note["updatedAt"]) <= at) markRead(note, at) else note
        }
    }
}

data class NoteAccessSummary(
    val title: String,
    val role: String,
    val sharedWith: List<String>,
    val reach: NoteReach,
    val lastEdit: String,
    val isOwner: Boolean,
)

object NoteAccess {
    fun roleText(note: JsonObject): String = when (NotesLogic.role(note)) {
        "owner" -> "You own this note"
        "editor" -> "You can edit"
        else -> "View only"
    }

    fun shareLines(note: JsonObject): List<String> {
        val lines = jsonObjects(note["shares"]).mapNotNull { share ->
            val name = jsonString(share["name"]).trim()
            if (name.isEmpty()) return@mapNotNull null
            val who = if (share["type"] == "organization") "$name (organization)" else name
            "$who · ${NotesLogic.roleLabel(jsonString(share["role"], "viewer"))}"
        }
        return lines.ifEmpty { listOf("Not shared") }
    }

    fun lastEdit(note: JsonObject, me: String, zone: ZoneId): String {
        val time = NoteTimes.full(jsonInt(note["updatedAt"]), zone)
        if (time.isEmpty()) return ""
        val editor = jsonString(note["lastEditedBy"]).trim()
        val by = when {
            editor.isEmpty() -> ""
            editor.equals(me.trim(), ignoreCase = true) -> " by you"
            else -> " by $editor"
        }
        return "Last edited $time$by"
    }

    fun summary(note: JsonObject, me: String, zone: ZoneId): NoteAccessSummary = NoteAccessSummary(
        title = jsonString(note["title"], "Untitled note").ifBlank { "Untitled note" },
        role = roleText(note),
        sharedWith = shareLines(note),
        reach = NoteRows.reach(note),
        lastEdit = lastEdit(note, me, zone),
        isOwner = NotesLogic.role(note) == "owner",
    )
}
