package com.forkmesh.nativeapp.notes

import com.forkmesh.nativeapp.json.Json

data class NotePeer(val name: String, val seen: Long, val draftAt: Long = 0L, val state: String = NotePresence.VIEWING, val clientId: String = "") {
    fun isEditing(now: Long): Boolean = state == NotePresence.EDITING || (draftAt > 0 && now - draftAt < NotePresence.EDITING_MS)

    fun stateLabel(now: Long): String = if (isEditing(now)) NotePresence.EDITING else NotePresence.VIEWING

    fun line(now: Long): String = "$name · ${stateLabel(now)}"
}

data class NoteFaces(val shown: List<NotePeer>, val extra: Int) {
    val extraLabel: String get() = if (extra > 0) "+$extra" else ""
}

sealed class NoteRoomEvent {
    data class Draft(val title: String, val markdown: String, val baseVersion: Long) : NoteRoomEvent()
    data class Updated(val version: Long) : NoteRoomEvent()
    data object Deleted : NoteRoomEvent()
    data object AccessChanged : NoteRoomEvent()
    data object Comments : NoteRoomEvent()
    data class Left(val clientId: String) : NoteRoomEvent()
}

data class NoteRoomFrame(val event: NoteRoomEvent?, val newcomer: Boolean)

class NotePresence(val clientId: String) {
    private val peers = LinkedHashMap<String, NotePeer>()
    private var sentState = ""

    fun presenceFrame(state: String = VIEWING): String {
        val clean = if (state == EDITING) EDITING else VIEWING
        sentState = clean
        return Json.stringify(linkedMapOf("type" to "presence", "clientId" to clientId, "state" to clean))
    }

    fun stateChangeFrame(state: String): String? = if ((if (state == EDITING) EDITING else VIEWING) != sentState) presenceFrame(state) else null

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
            val state = when (jsonString(frame["state"])) {
                EDITING -> EDITING
                VIEWING -> VIEWING
                else -> if (type == "presence") VIEWING else known?.state ?: VIEWING
            }
            peers[sender] = NotePeer(name, now, if (type == "draft") now else known?.draftAt ?: 0L, state, sender)
        }
        if (type == "leave" && sender.isNotEmpty() && sender != clientId) peers.remove(sender)
        val event = when (type) {
            "draft" -> if (sender.isNotEmpty() && sender != clientId) {
                NoteRoomEvent.Draft(jsonString(frame["title"], "Untitled note"), jsonString(frame["markdown"]), jsonInt(frame["baseVersion"]))
            } else {
                null
            }
            "note-updated" -> NoteRoomEvent.Updated(jsonInt(frame["version"]))
            "note-deleted" -> NoteRoomEvent.Deleted
            "access-changed" -> NoteRoomEvent.AccessChanged
            "note-comments" -> NoteRoomEvent.Comments
            "leave" -> if (sender.isNotEmpty() && sender != clientId) NoteRoomEvent.Left(sender) else null
            else -> null
        }
        return NoteRoomFrame(event, newcomer)
    }

    fun peers(now: Long): List<NotePeer> {
        peers.entries.removeAll { now - it.value.seen > PRESENCE_MS * 5 / 2 }
        return peers.values.sortedBy { if (it.isEditing(now)) 0 else 1 }
    }

    fun label(open: Boolean, now: Long): String = label(open, peers(now), now)

    fun clear() {
        peers.clear()
        sentState = ""
    }

    companion object {
        const val PRESENCE_MS = 15_000L
        const val DRAFT_MS = 180L
        const val EDITING_MS = 5_000L
        const val TYPING_MS = 10_000L
        const val MAX_FACES = 4
        const val VIEWING = "viewing"
        const val EDITING = "editing"
        private val PEER_FRAMES = setOf("presence", "cursor", "draft")

        fun localState(typedAt: Long, now: Long): String = if (typedAt > 0 && now - typedAt in 0 until TYPING_MS) EDITING else VIEWING

        fun msUntilViewing(typedAt: Long, now: Long): Long = if (localState(typedAt, now) == EDITING) TYPING_MS - (now - typedAt) else 0L

        fun faces(peers: List<NotePeer>): NoteFaces = NoteFaces(peers.take(MAX_FACES), maxOf(0, peers.size - MAX_FACES))

        fun label(open: Boolean, present: List<NotePeer>, now: Long): String {
            if (!open) return "Offline — edits still save"
            val editing = present.firstOrNull { it.isEditing(now) }
            return when {
                editing != null -> "${editing.name} is editing…"
                present.size == 1 -> "Live · ${present[0].name} is viewing"
                present.isNotEmpty() -> "Live · ${present.size} others here"
                else -> "Live"
            }
        }

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
