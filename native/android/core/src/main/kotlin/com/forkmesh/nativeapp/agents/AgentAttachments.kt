package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.modelInt

data class AgentAttachmentMeta(
    val id: String,
    val name: String,
    val mediaType: String,
    val size: Int = -1,
) {
    val wire: Map<String, Any?> get() = linkedMapOf("id" to id, "name" to name, "mediaType" to mediaType, "size" to size)

    companion object {
        const val HEX_ID_PATTERN = "^[0-9a-f]{32}$"

        fun list(value: Any?, idPattern: String = HEX_ID_PATTERN, limit: Int = 20): List<AgentAttachmentMeta> {
            val pattern = Regex(idPattern)
            val result = ArrayList<AgentAttachmentMeta>()
            val seen = HashSet<String>()
            for (item in value as? List<*> ?: return result) {
                val obj = item as? Map<*, *> ?: continue
                val id = jsonText(obj["id"]).lowercase()
                if (!pattern.containsMatchIn(id) || !seen.add(id)) continue
                val name = AgentAttachmentMedia.displayName(jsonText(obj["name"]), "attachment")
                var mediaType = jsonText(obj["mediaType"]).lowercase().trim()
                if (mediaType.isEmpty()) mediaType = AgentAttachmentMedia.typeForName(name)
                val size = if (obj["size"] == null) -1 else modelInt(obj["size"], -1).toInt()
                result.add(AgentAttachmentMeta(id, name, mediaType, maxOf(-1, size)))
                if (result.size >= limit) break
            }
            return result
        }
    }
}

sealed class AttachmentOrigin {
    data class Relay(val owner: String, val repo: String, val sessionId: String, val orgScope: Boolean) : AttachmentOrigin()

    data class Desktop(val sessionId: Int) : AttachmentOrigin()

    data object Elsewhere : AttachmentOrigin()

    data object Local : AttachmentOrigin()
}

data class AgentAttachmentRef(val meta: AgentAttachmentMeta, val origin: AttachmentOrigin) {
    val name: String get() = meta.name
    val mediaType: String get() = meta.mediaType
    val size: Int get() = meta.size

    val cacheKey: String
        get() = when (origin) {
            is AttachmentOrigin.Relay ->
                "relay:${if (origin.orgScope) "org" else "owner"}:${origin.owner.lowercase()}/${origin.repo.lowercase()}:${origin.sessionId}:${meta.id}"
            is AttachmentOrigin.Desktop -> "desktop:${origin.sessionId}:${meta.id}"
            AttachmentOrigin.Elsewhere -> "path:${meta.id}"
            AttachmentOrigin.Local -> "local:${meta.id}"
        }

    val isImage: Boolean get() = AgentAttachmentMedia.isImage(mediaType)
    val isVideo: Boolean get() = mediaType.startsWith("video/")
    val canFetch: Boolean get() = origin != AttachmentOrigin.Elsewhere
    val showsThumbnail: Boolean get() = isImage && canFetch
    val sizeLabel: String get() = if (size < 0) "" else AgentAttachmentMedia.sizeLabel(size.toLong())

    val rowDetail: String
        get() {
            val parts = ArrayList<String>()
            if (sizeLabel.isNotEmpty()) parts.add(sizeLabel)
            if (origin == AttachmentOrigin.Elsewhere) parts.add("on the machine that ran the agent")
            return parts.joinToString(" · ")
        }

    companion object {
        fun local(photo: PromptAttachment): AgentAttachmentRef = AgentAttachmentRef(
            AgentAttachmentMeta(photo.id, AgentAttachmentMedia.displayName(photo.name, "photo.jpg"), photo.mediaType, photo.size),
            AttachmentOrigin.Local,
        )
    }
}

class AgentAttachmentError(override val message: String) : RuntimeException(message) {
    override fun toString(): String = message
}

object AgentAttachmentMedia {
    val imageTypes = setOf("image/jpeg", "image/png", "image/gif", "image/webp", "image/heic")

    const val MAX_IMAGE_BYTES = 5 * 1024 * 1024

    fun isImage(mediaType: String): Boolean = mediaType.lowercase() in imageTypes

    fun typeForName(name: String): String {
        val dot = name.lastIndexOf('.')
        val ext = if (dot < 0) "" else name.substring(dot + 1).lowercase()
        return when (ext) {
            "jpg", "jpeg" -> "image/jpeg"
            "png" -> "image/png"
            "gif" -> "image/gif"
            "webp" -> "image/webp"
            "heic", "heif" -> "image/heic"
            "mp4", "m4v" -> "video/mp4"
            "mov" -> "video/quicktime"
            "webm" -> "video/webm"
            "pdf" -> "application/pdf"
            "txt", "log", "md" -> "text/plain"
            "json" -> "application/json"
            "zip" -> "application/zip"
            else -> "application/octet-stream"
        }
    }

    fun displayName(raw: String, fallback: String): String {
        val last = raw.replace('\\', '/').split('/').lastOrNull { it.isNotEmpty() } ?: ""
        val printable = last.filter { !Character.isISOControl(it) }
        val trimmed = printable.trim().take(120)
        return trimmed.ifEmpty { fallback }
    }

    fun sizeLabel(bytes: Long): String = when {
        bytes < 1024 -> "$bytes B"
        bytes < 1024 * 1024 -> String.format(java.util.Locale.US, "%.1f KB", bytes / 1024.0)
        else -> String.format(java.util.Locale.US, "%.1f MB", bytes / (1024.0 * 1024.0))
    }

    fun sniff(data: ByteArray): String? {
        fun at(i: Int): Int = if (i < data.size) data[i].toInt() and 0xff else -1
        fun ascii(from: Int, text: String): Boolean =
            data.size >= from + text.length && text.indices.all { at(from + it) == text[it].code }
        if (at(0) == 0xFF && at(1) == 0xD8 && at(2) == 0xFF) return "image/jpeg"
        val png = intArrayOf(0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A)
        if (data.size >= 8 && png.indices.all { at(it) == png[it] }) return "image/png"
        if (ascii(0, "GIF87a") || ascii(0, "GIF89a")) return "image/gif"
        if (ascii(0, "RIFF") && ascii(8, "WEBP")) return "image/webp"
        if (ascii(4, "ftyp") && data.size >= 12) {
            val brand = String(data, 8, 4, Charsets.US_ASCII)
            if (brand in setOf("heic", "heix", "hevc", "heim", "heis", "mif1", "msf1")) return "image/heic"
        }
        return null
    }

    fun checkImage(data: ByteArray, maxBytes: Int = MAX_IMAGE_BYTES): String {
        if (data.isEmpty()) throw AgentAttachmentError("The attachment is empty.")
        if (data.size > maxBytes) throw AgentAttachmentError("The attachment is larger than ${sizeLabel(maxBytes.toLong())}.")
        return sniff(data) ?: throw AgentAttachmentError("The attachment is not an image.")
    }
}

object AgentTranscriptAttachments {
    const val BLOCK_HEADER = "Attached images (open them with your file-reading tool):"
    const val LIMIT = 50

    fun paths(text: String): List<String> {
        if (!text.contains("Attached ")) return emptyList()
        val paths = ArrayList<String>()
        fun add(raw: String) {
            val path = raw.trim()
            if (path.isEmpty() || path.length > 1024 || path in paths || paths.size >= LIMIT) return
            paths.add(path)
        }
        var inBlock = false
        for (rawLine in text.split('\n')) {
            val line = rawLine.removeSuffix("\r")
            if (inBlock) {
                if (line.startsWith("- ")) {
                    add(line.substring(2))
                    continue
                }
                inBlock = false
            }
            if (line.trim() == BLOCK_HEADER) {
                inBlock = true
                continue
            }
            for (marker in listOf("Attached image:", "Attached video:")) {
                var rest = line
                while (true) {
                    val at = rest.indexOf(marker)
                    if (at < 0) break
                    val tail = rest.substring(at + marker.length)
                    var end = tail.indexOfFirst { it == '"' || it == '\\' }
                    if (end < 0) end = tail.length
                    add(tail.substring(0, end))
                    rest = tail.substring(end)
                }
            }
        }
        return paths
    }

    fun references(text: String): List<AgentAttachmentRef> = paths(text).map { path ->
        val name = AgentAttachmentMedia.displayName(path, "attachment")
        AgentAttachmentRef(AgentAttachmentMeta(path, name, AgentAttachmentMedia.typeForName(name)), AttachmentOrigin.Elsewhere)
    }
}

object AgentAttachmentReconcile {
    fun pair(sent: List<PromptAttachment>, listed: List<AgentAttachmentMeta>): List<Pair<AgentAttachmentMeta, PromptAttachment>>? {
        if (sent.isEmpty() || sent.size != listed.size) return null
        val pairs = ArrayList<Pair<AgentAttachmentMeta, PromptAttachment>>()
        for ((meta, photo) in listed.zip(sent)) {
            if (meta.size >= 0 && meta.size != photo.size) return null
            if (meta.mediaType.isNotEmpty() && meta.mediaType != photo.mediaType.lowercase()) return null
            pairs.add(meta to photo)
        }
        return pairs
    }

    fun turn(sent: List<PromptAttachment>, history: List<AgentLaunchMessage>): Int? {
        for (index in history.indices.reversed()) {
            if (history[index].isUser && pair(sent, history[index].attachments) != null) return index
        }
        return null
    }
}

class AgentSentPhotos(sessionLimit: Int = 12) {
    private val limit = maxOf(1, sessionLimit)

    private data class Turn(var sessionId: String, val text: String, val photos: List<PromptAttachment>)

    private val turns = ArrayList<Turn>()

    @Synchronized
    fun record(sessionId: String, text: String, photos: List<PromptAttachment>) {
        if (sessionId.isEmpty() || photos.isEmpty()) return
        turns.removeAll { it.sessionId == sessionId && it.text == text }
        turns.add(Turn(sessionId, text, photos))
        val sessions = ArrayList<String>()
        for (turn in turns.asReversed()) if (turn.sessionId !in sessions) sessions.add(turn.sessionId)
        if (sessions.size > limit) {
            val keep = sessions.take(limit).toSet()
            turns.removeAll { it.sessionId !in keep }
        }
    }

    @Synchronized
    fun photos(sessionId: String, text: String): List<PromptAttachment>? =
        turns.lastOrNull { it.sessionId == sessionId && it.text == text }?.photos

    @Synchronized
    fun rename(from: String, to: String) {
        for (turn in turns) if (turn.sessionId == from) turn.sessionId = to
    }

    @Synchronized
    fun clear() {
        turns.clear()
    }

    companion object {
        val shared = AgentSentPhotos()
    }
}
