package com.forkmesh.nativeapp.notes

import com.forkmesh.nativeapp.json.Json

data class NotePeer(val name: String, val seen: Long, val draftAt: Long = 0L)

sealed class NoteRoomEvent {
    data class Draft(val title: String, val markdown: String, val baseVersion: Long) : NoteRoomEvent()
    data class Updated(val version: Long) : NoteRoomEvent()
    data object Deleted : NoteRoomEvent()
    data object AccessChanged : NoteRoomEvent()
}

data class NoteRoomFrame(val event: NoteRoomEvent?, val newcomer: Boolean)

class NotePresence(val clientId: String) {
    private val peers = LinkedHashMap<String, NotePeer>()

    fun presenceFrame(): String = Json.stringify(linkedMapOf("type" to "presence", "clientId" to clientId))

    fun draftFrame(title: String, markdown: String, baseVersion: Long): String = Json.stringify(
        linkedMapOf("type" to "draft", "clientId" to clientId, "baseVersion" to baseVersion, "title" to title, "markdown" to markdown),
    )

    fun receive(text: String, now: Long): NoteRoomFrame {
        val frame = try {
            jsonObject(Json.parse(text))
        } catch (_: Exception) {
            return NoteRoomFrame(null, false)
        }
        val type = jsonString(frame["type"])
        val sender = jsonString(frame["clientId"])
        var newcomer = false
        if (type in PEER_FRAMES && sender.isNotEmpty() && sender != clientId) {
            val known = peers[sender]
            newcomer = known == null
            val name = jsonString(frame["name"]).trim().ifEmpty { "Collaborator" }.take(32)
            peers[sender] = NotePeer(name, now, if (type == "draft") now else known?.draftAt ?: 0L)
        }
        val event = when (type) {
            "draft" -> if (sender.isNotEmpty() && sender != clientId) {
                NoteRoomEvent.Draft(jsonString(frame["title"], "Untitled note"), jsonString(frame["markdown"]), jsonInt(frame["baseVersion"]))
            } else {
                null
            }
            "note-updated" -> NoteRoomEvent.Updated(jsonInt(frame["version"]))
            "note-deleted" -> NoteRoomEvent.Deleted
            "access-changed" -> NoteRoomEvent.AccessChanged
            else -> null
        }
        return NoteRoomFrame(event, newcomer)
    }

    fun peers(now: Long): List<NotePeer> {
        peers.entries.removeAll { now - it.value.seen > PRESENCE_MS * 5 / 2 }
        return peers.values.toList()
    }

    fun label(open: Boolean, now: Long): String {
        if (!open) return "Offline — edits still save"
        val present = peers(now)
        val editing = present.firstOrNull { it.draftAt > 0 && now - it.draftAt < EDITING_MS }
        return when {
            editing != null -> "${editing.name} is editing…"
            present.size == 1 -> "Live · ${present[0].name} is here"
            present.isNotEmpty() -> "Live · ${present.size} others here"
            else -> "Live"
        }
    }

    fun clear() {
        peers.clear()
    }

    companion object {
        const val PRESENCE_MS = 15_000L
        const val DRAFT_MS = 180L
        const val EDITING_MS = 5_000L
        private val PEER_FRAMES = setOf("presence", "cursor", "draft")

        fun roomUrl(origin: String, id: String): String {
            val base = origin.trimEnd('/')
            val socket = when {
                base.startsWith("https://") -> "wss://" + base.removePrefix("https://")
                base.startsWith("http://") -> "ws://" + base.removePrefix("http://")
                else -> base
            }
            return "$socket/api/notes/$id/ws"
        }
    }
}
