package com.forkmesh.nativeapp.notes

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.model.OrgApiException
import java.net.URI
import java.net.URLEncoder
import java.nio.charset.StandardCharsets
import kotlin.random.Random

object NoteEmoji {
    const val MAX_CODE_POINTS = 16
    const val MAX_BYTES = 64
    private const val KEYCAP = 0x20E3
    private const val VARIATION = 0xFE0F

    val common: List<String> = listOf(
        "📝", "📌", "📎", "📚", "📖", "💡", "✅", "⭐",
        "🔥", "🚀", "🎯", "📅", "⏰", "🗂️", "📁", "🧠",
        "💬", "❓", "❗", "⚠️", "🐛", "🔧", "🔒", "🔑",
        "🌱", "🌍", "🏠", "💼", "💰", "📈", "🎉", "❤️",
        "👍", "👀", "🎨", "🎵", "☕", "🍕", "✈️", "🏁",
    )

    fun clean(raw: String): String = raw.trim()

    fun isValid(value: String): Boolean {
        if (value.isEmpty()) return false
        val points = value.codePoints().toArray()
        if (points.size !in 1..MAX_CODE_POINTS) return false
        if (value.toByteArray(StandardCharsets.UTF_8).size > MAX_BYTES) return false
        for ((index, point) in points.withIndex()) {
            if (Character.isWhitespace(point) || Character.isSpaceChar(point) || Character.isISOControl(point)) return false
            if (point in 'a'.code..'z'.code || point in 'A'.code..'Z'.code) return false
            if (point in '0'.code..'9'.code || point == '#'.code || point == '*'.code) {
                val next = points.getOrNull(index + 1)
                val keycap = next == KEYCAP || (next == VARIATION && points.getOrNull(index + 2) == KEYCAP)
                if (!keycap) return false
            }
        }
        return true
    }

    fun error(raw: String): String? {
        val value = clean(raw)
        if (value.isEmpty() || isValid(value)) return null
        return "Use a single emoji, such as 📝 or 🇫🇷."
    }

    fun titled(emoji: String, title: String): String = if (emoji.isBlank()) title else "${emoji.trim()} $title"

    fun of(json: JsonObject): String = jsonString(json["emoji"]).trim()
}

enum class PageAccent(val raw: String, val label: String) {
    Blue("blue", "Blue"),
    Purple("purple", "Purple"),
    Pink("pink", "Pink"),
    Red("red", "Red"),
    Orange("orange", "Orange"),
    Yellow("yellow", "Yellow"),
    Teal("teal", "Teal"),
    Gray("gray", "Gray");

    companion object {
        fun of(raw: String?): PageAccent = entries.firstOrNull { it.raw == raw } ?: Blue
    }
}

enum class PageLayout(val raw: String, val label: String) {
    Outline("outline", "Outline"),
    Continuous("continuous", "Continuous");

    companion object {
        fun of(raw: String?): PageLayout = entries.firstOrNull { it.raw == raw } ?: Outline
    }
}

data class PageEntry(
    val id: String,
    val kind: String,
    val depth: Int = 0,
    val label: String = "",
    val noteId: String = "",
    val url: String = "",
    val collapsed: Boolean = false,
) {
    val isNote: Boolean get() = kind == NOTE
    val isSection: Boolean get() = kind == SECTION
    val isLink: Boolean get() = kind == LINK

    fun body(): JsonObject = linkedMapOf(
        "id" to id, "kind" to kind, "depth" to depth, "label" to label, "noteId" to noteId, "url" to url, "collapsed" to collapsed,
    )

    companion object {
        const val NOTE = "note"
        const val SECTION = "section"
        const val LINK = "link"

        fun from(json: JsonObject): PageEntry? {
            val id = jsonString(json["id"]).trim()
            val kind = jsonString(json["kind"])
            if (id.isEmpty() || kind !in setOf(NOTE, SECTION, LINK)) return null
            return PageEntry(
                id = id,
                kind = kind,
                depth = jsonInt(json["depth"]).coerceIn(0L, PageOutline.MAX_DEPTH.toLong()).toInt(),
                label = jsonString(json["label"]),
                noteId = jsonString(json["noteId"]).trim(),
                url = jsonString(json["url"]).trim(),
                collapsed = json["collapsed"] == true,
            )
        }
    }
}

data class PageNote(
    val id: String,
    val title: String = "",
    val emoji: String = "",
    val updatedAt: Long = 0L,
    val lastEditedBy: String = "",
    val unread: Boolean = false,
    val role: String = "viewer",
    val markdown: String? = null,
) {
    val displayTitle: String get() = title.ifBlank { "Untitled note" }

    companion object {
        fun from(json: JsonObject, fallbackId: String = ""): PageNote? {
            val id = jsonString(json["id"]).trim().ifEmpty { fallbackId }
            if (id.isEmpty()) return null
            return PageNote(
                id = id,
                title = jsonString(json["title"]),
                emoji = NoteEmoji.of(json),
                updatedAt = jsonInt(json["updatedAt"]),
                lastEditedBy = jsonString(json["lastEditedBy"]).trim(),
                unread = json["unread"] == true,
                role = jsonString(json["role"], "viewer"),
                markdown = json["markdown"] as? String,
            )
        }
    }
}

data class NotePage(
    val id: String,
    val title: String = "",
    val emoji: String = "",
    val description: String = "",
    val accent: PageAccent = PageAccent.Blue,
    val layout: PageLayout = PageLayout.Outline,
    val showEmoji: Boolean = true,
    val outline: List<PageEntry> = emptyList(),
    val role: String = "owner",
    val version: Long = 0L,
    val createdAt: Long = 0L,
    val updatedAt: Long = 0L,
    val shares: List<JsonObject> = emptyList(),
    val entryCount: Int = 0,
) {
    val displayTitle: String get() = title.ifBlank { "Untitled page" }
    val isOwner: Boolean get() = role == "owner"
    val canEdit: Boolean get() = role == "owner" || role == "editor"
    val entriesLabel: String get() = if (entryCount == 1) "1 entry" else "$entryCount entries"

    fun fields(): JsonObject = linkedMapOf(
        "title" to title.trim().take(PageOutline.MAX_TITLE),
        "emoji" to emoji.trim(),
        "description" to description.take(PageOutline.MAX_DESCRIPTION),
        "accent" to accent.raw,
        "layout" to layout.raw,
        "showEmoji" to showEmoji,
        "outline" to outline.map { it.body() },
    )

    companion object {
        fun from(json: JsonObject): NotePage? {
            val id = jsonString(json["id"]).trim()
            if (id.isEmpty()) return null
            val outline = jsonObjects(json["outline"]).mapNotNull(PageEntry::from)
            val entries = if (json.containsKey("entries")) jsonInt(json["entries"]).toInt() else outline.size
            return NotePage(
                id = id,
                title = jsonString(json["title"]),
                emoji = NoteEmoji.of(json),
                description = jsonString(json["description"]),
                accent = PageAccent.of(json["accent"] as? String),
                layout = PageLayout.of(json["layout"] as? String),
                showEmoji = json["showEmoji"] != false,
                outline = outline,
                role = jsonString(json["role"], "owner"),
                version = jsonInt(json["version"]),
                createdAt = jsonInt(json["createdAt"]),
                updatedAt = jsonInt(json["updatedAt"]),
                shares = jsonObjects(json["shares"]),
                entryCount = maxOf(0, entries),
            )
        }

        fun list(value: Any?): List<NotePage> = jsonObjects(value).mapNotNull(::from)
    }
}

data class NotePageDetail(val page: NotePage, val notes: Map<String, PageNote>) {
    fun withNote(note: PageNote): NotePageDetail = copy(notes = notes + (note.id to note))

    companion object {
        fun from(json: JsonObject): NotePageDetail? {
            val page = NotePage.from(jsonObject(json["page"])) ?: return null
            val notes = jsonObject(json["notes"]).mapNotNull { (key, value) -> PageNote.from(jsonObject(value), key)?.let { key to it } }.toMap()
            return NotePageDetail(page, notes)
        }
    }
}

data class OutlineRow(
    val entry: PageEntry,
    val index: Int,
    val title: String,
    val emoji: String,
    val note: PageNote?,
    val collapsed: Boolean,
    val hasChildren: Boolean,
) {
    val locked: Boolean get() = entry.isNote && note == null
    val unread: Boolean get() = note?.unread == true
}

object PageOutline {
    const val MAX_DEPTH = 3
    const val MAX_ENTRIES = 300
    const val MAX_TITLE = 200
    const val MAX_DESCRIPTION = 2000
    const val LOCKED = "You don't have access to this note"
    private const val ID_CHARS = "abcdefghijklmnopqrstuvwxyz0123456789"

    fun newId(random: Random = Random.Default): String = (1..12).map { ID_CHARS[random.nextInt(ID_CHARS.length)] }.joinToString("")

    fun title(entry: PageEntry, note: PageNote?): String = when {
        entry.isSection -> entry.label.ifBlank { "Untitled section" }
        entry.isLink -> entry.label.ifBlank { entry.url }
        else -> entry.label.ifBlank { note?.displayTitle ?: "Locked note" }
    }

    private fun groups(outline: List<PageEntry>, index: Int): Boolean {
        val section = outline.getOrNull(index)?.takeIf { it.isSection } ?: return false
        val next = outline.getOrNull(index + 1) ?: return false
        return !(next.isSection && next.depth <= section.depth)
    }

    fun rows(outline: List<PageEntry>, notes: Map<String, PageNote>, showEmoji: Boolean, toggled: Set<String> = emptySet()): List<OutlineRow> {
        val out = mutableListOf<OutlineRow>()
        var hideBelow = -1
        for ((index, entry) in outline.withIndex()) {
            if (hideBelow >= 0) {
                if (entry.isSection && entry.depth <= hideBelow) hideBelow = -1 else continue
            }
            val note = if (entry.isNote) notes[entry.noteId] else null
            val collapsed = entry.isSection && (entry.collapsed != (entry.id in toggled))
            val children = groups(outline, index)
            out += OutlineRow(entry, index, title(entry, note), if (showEmoji) note?.emoji.orEmpty() else "", note, collapsed, children)
            if (collapsed && children) hideBelow = entry.depth
        }
        return out
    }

    fun readable(outline: List<PageEntry>, notes: Map<String, PageNote>): List<Pair<PageEntry, PageNote>> {
        val seen = HashSet<String>()
        return outline.mapNotNull { entry ->
            val note = if (entry.isNote) notes[entry.noteId] else null
            if (note == null || !seen.add(note.id)) null else entry to note
        }
    }

    fun firstNote(outline: List<PageEntry>, notes: Map<String, PageNote>): String? = readable(outline, notes).firstOrNull()?.second?.id

    fun neighbours(outline: List<PageEntry>, notes: Map<String, PageNote>, noteId: String): Pair<PageNote?, PageNote?> {
        val list = readable(outline, notes).map { it.second }
        val at = list.indexOfFirst { it.id == noteId }
        if (at < 0) return null to null
        return list.getOrNull(at - 1) to list.getOrNull(at + 1)
    }

    fun canAdd(outline: List<PageEntry>): Boolean = outline.size < MAX_ENTRIES

    private fun childDepth(outline: List<PageEntry>): Int {
        val last = outline.lastOrNull() ?: return 0
        return if (last.isSection) minOf(MAX_DEPTH, last.depth + 1) else last.depth
    }

    fun addNote(outline: List<PageEntry>, noteId: String, random: Random = Random.Default): List<PageEntry> {
        if (!canAdd(outline) || noteId.isBlank()) return outline
        return outline + PageEntry(newId(random), PageEntry.NOTE, childDepth(outline), noteId = noteId)
    }

    fun addSection(outline: List<PageEntry>, label: String, random: Random = Random.Default): List<PageEntry> {
        if (!canAdd(outline) || label.isBlank()) return outline
        return outline + PageEntry(newId(random), PageEntry.SECTION, 0, label = label.trim().take(MAX_TITLE))
    }

    fun addLink(outline: List<PageEntry>, label: String, url: String, random: Random = Random.Default): List<PageEntry> {
        if (!canAdd(outline) || !isLink(url)) return outline
        return outline + PageEntry(newId(random), PageEntry.LINK, childDepth(outline), label = label.trim().take(MAX_TITLE), url = url.trim())
    }

    fun isLink(raw: String): Boolean {
        val url = raw.trim()
        if (!url.startsWith("https://") || url.length > 2000) return false
        return try {
            !URI(url).host.isNullOrBlank()
        } catch (_: Exception) {
            false
        }
    }

    fun indent(outline: List<PageEntry>, index: Int): List<PageEntry> {
        val entry = outline.getOrNull(index) ?: return outline
        val limit = if (index == 0) 0 else minOf(MAX_DEPTH, outline[index - 1].depth + 1)
        val depth = maxOf(entry.depth, minOf(entry.depth + 1, limit))
        return outline.toMutableList().also { it[index] = entry.copy(depth = depth) }
    }

    fun outdent(outline: List<PageEntry>, index: Int): List<PageEntry> {
        val entry = outline.getOrNull(index) ?: return outline
        return outline.toMutableList().also { it[index] = entry.copy(depth = maxOf(0, entry.depth - 1)) }
    }

    fun move(outline: List<PageEntry>, from: Int, to: Int): List<PageEntry> {
        if (from !in outline.indices) return outline
        val target = to.coerceIn(0, outline.lastIndex)
        if (from == target) return outline
        return outline.toMutableList().also { it.add(target, it.removeAt(from)) }
    }

    fun remove(outline: List<PageEntry>, index: Int): List<PageEntry> =
        if (index in outline.indices) outline.toMutableList().also { it.removeAt(index) } else outline

    fun rename(outline: List<PageEntry>, index: Int, label: String): List<PageEntry> {
        val entry = outline.getOrNull(index) ?: return outline
        return outline.toMutableList().also { it[index] = entry.copy(label = label.trim().take(MAX_TITLE)) }
    }

    fun toggle(outline: List<PageEntry>, id: String): List<PageEntry> = outline.map { if (it.id == id && it.isSection) it.copy(collapsed = !it.collapsed) else it }

    fun accessUrl(origin: String, pageId: String, noteId: String? = null): String {
        val base = "${origin.trimEnd('/')}/dashboard/notes?page=${URLEncoder.encode(pageId, StandardCharsets.UTF_8)}"
        return if (noteId.isNullOrEmpty()) base else "$base&note=${URLEncoder.encode(noteId, StandardCharsets.UTF_8)}"
    }

    fun error(error: Throwable, fallback: String): String {
        if (error is PageConflictException) return "Someone else changed this page. Reload to see their changes, then make yours again."
        if (error is OrgApiException) {
            when (error.code) {
                "page_not_found" -> return "This page was deleted, or you no longer have access to it."
                "invalid_page" -> return "Check the page's fields: a title up to 200 characters, a description up to 2000, and https:// links."
                "too_many_entries" -> return "A page holds at most $MAX_ENTRIES entries."
                "invalid_emoji" -> return "Use a single emoji, such as 📝 or 🇫🇷."
                "note_not_found" -> return "You can only add notes you can open."
            }
        }
        return apiErrorText(error, fallback)
    }
}

class PageConflictException(val latest: NotePage?) : RuntimeException("version_conflict")

class NotePagesApi(private val api: ApiService) {
    private fun call(method: String, suffix: String = "", body: JsonObject? = null): JsonObject =
        api.accountApiCall(method, "/api/notes/pages$suffix", body)

    fun list(): List<NotePage> = NotePage.list(call("GET")["pages"])

    fun create(fields: JsonObject = emptyMap()): NotePage =
        NotePage.from(jsonObject(call("POST", "", fields)["page"])) ?: throw OrgApiException("page_not_found", "The page was not created.")

    fun get(id: String, content: Boolean = true): NotePageDetail =
        NotePageDetail.from(call("GET", "/$id" + if (content) "?content=1" else "")) ?: throw OrgApiException("page_not_found", "Page not found")

    fun save(id: String, baseVersion: Long, fields: JsonObject): NotePage {
        val body = linkedMapOf<String, Any?>("baseVersion" to baseVersion).apply { putAll(fields) }
        return try {
            NotePage.from(jsonObject(call("PATCH", "/$id", body)["page"])) ?: throw OrgApiException("page_not_found", "Page not found")
        } catch (error: OrgApiException) {
            if (error.code != "version_conflict") throw error
            throw PageConflictException(runCatching { get(id, content = false).page }.getOrNull())
        }
    }

    fun delete(id: String) {
        call("DELETE", "/$id")
    }

    fun addShare(id: String, type: String, name: String, role: String, includeNotes: Boolean): List<JsonObject> = jsonObjects(
        call("POST", "/$id/shares", linkedMapOf("type" to type, "name" to NotesLogic.shareName(name), "role" to role, "includeNotes" to includeNotes))["shares"],
    )

    fun removeShare(id: String, type: String, name: String): List<JsonObject> =
        jsonObjects(call("DELETE", "/$id/shares", linkedMapOf("type" to type, "name" to name))["shares"])

    fun shareSuggestions(id: String, raw: String): List<NoteShareSuggestion> {
        val query = NotesLogic.suggestionQuery(raw)
        if (query.isEmpty()) return emptyList()
        return NotesLogic.suggestions(call("GET", "/$id/shares?query=${URLEncoder.encode(query, StandardCharsets.UTF_8)}"))
    }
}
