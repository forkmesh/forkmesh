package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.OrgApiException
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.SecureRandom
import java.util.Base64
import javax.crypto.AEADBadTagException
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

enum class ViewOnceKind(
    val wire: String,
    val text: String,
    val label: String,
    val maxBytes: Int,
    val maxDurationMs: Long,
    val acceptedDurationMs: Long,
    val mimes: Set<String>,
) {
    VIDEO(
        "video", "🎥 Video message", "Video message", 12 * 1024 * 1024, 60_000L, 61_000L,
        setOf("video/mp4", "video/webm", "video/quicktime", "video/3gpp"),
    ),
    VOICE(
        "voice", "🎤 Voice message", "Voice message", 4 * 1024 * 1024, 120_000L, 121_000L,
        setOf("audio/mp4", "audio/m4a", "audio/x-m4a", "audio/aac", "audio/webm", "audio/ogg", "audio/mpeg", "audio/3gpp"),
    ),
    ;

    companion object {
        fun of(wire: String): ViewOnceKind? = entries.firstOrNull { it.wire == wire }
    }
}

class ViewOnceMedia(
    val kind: ViewOnceKind,
    val media: String,
    val key: ByteArray,
    val iv: ByteArray,
    val mime: String,
    val durationMs: Long,
    val size: Long,
    val parts: Int,
)

class ViewOnceException(message: String) : RuntimeException(message)

class ViewOnceGone(val state: String) : RuntimeException("This message is no longer available.")

object ViewOnceCodec {
    const val VERSION = 1
    const val FIELD = "viewOnce"
    const val PART_BYTES = 524_288
    const val TAG_BYTES = 16
    const val KEY_BYTES = 32
    const val IV_BYTES = 12
    const val SEAL_OVERHEAD_BYTES = 1024
    const val STATUS_IDS_MAX = 50
    private val MEDIA_ID = Regex("^[0-9a-f]{32}$")

    fun validMediaId(id: String): Boolean = MEDIA_ID.matches(id)

    fun partsFor(size: Long): Int = ((size + PART_BYTES - 1) / PART_BYTES).toInt()

    fun maxCipherBytes(kind: ViewOnceKind): Long = kind.maxBytes.toLong() + SEAL_OVERHEAD_BYTES

    fun split(bytes: ByteArray): List<ByteArray> {
        val out = ArrayList<ByteArray>()
        var offset = 0
        while (offset < bytes.size) {
            val end = minOf(bytes.size, offset + PART_BYTES)
            out.add(bytes.copyOfRange(offset, end))
            offset = end
        }
        return out
    }

    fun encodeKey(bytes: ByteArray): String = Base64.getUrlEncoder().withoutPadding().encodeToString(bytes)

    fun decodeKey(text: String): ByteArray? {
        val cleaned = text.trim().replace('+', '-').replace('/', '_').trimEnd('=')
        if (cleaned.isEmpty()) return null
        return runCatching { Base64.getUrlDecoder().decode(cleaned) }.getOrNull()
    }

    fun normalizeMime(mime: String): String = mime.substringBefore(';').trim().lowercase()

    fun parse(value: Any?): ViewOnceMedia? {
        val map = value as? Map<*, *> ?: return null
        if (jsonLong(map["v"]) != VERSION.toLong()) return null
        val kind = ViewOnceKind.of(jsonText(map["kind"])) ?: return null
        val media = jsonText(map["media"])
        if (!validMediaId(media)) return null
        val key = decodeKey(jsonText(map["key"]))?.takeIf { it.size == KEY_BYTES } ?: return null
        val iv = decodeKey(jsonText(map["iv"]))?.takeIf { it.size == IV_BYTES } ?: return null
        val mime = normalizeMime(jsonText(map["mime"]))
        if (mime !in kind.mimes) return null
        val duration = jsonLong(map["durationMs"], -1)
        if (duration < 0 || duration > kind.acceptedDurationMs) return null
        val size = jsonLong(map["size"], -1)
        if (size <= TAG_BYTES || size > maxCipherBytes(kind)) return null
        val parts = jsonLong(map["parts"], -1)
        if (parts != partsFor(size).toLong()) return null
        return ViewOnceMedia(kind, media, key, iv, mime, duration, size, parts.toInt())
    }

    fun field(media: ViewOnceMedia): LinkedHashMap<String, Any?> = linkedMapOf(
        "v" to VERSION,
        "kind" to media.kind.wire,
        "media" to media.media,
        "key" to encodeKey(media.key),
        "iv" to encodeKey(media.iv),
        "mime" to media.mime,
        "durationMs" to media.durationMs,
        "size" to media.size,
        "parts" to media.parts,
    )

    fun durationLabel(durationMs: Long): String {
        val seconds = ((durationMs + 500) / 1000).coerceAtLeast(0)
        return "%d:%02d".format(seconds / 60, seconds % 60)
    }
}

object ViewOnceCrypto {
    private const val TRANSFORM = "AES/GCM/NoPadding"
    private val random = SecureRandom()

    fun newKey(): ByteArray = ByteArray(ViewOnceCodec.KEY_BYTES).also(random::nextBytes)

    fun newIv(): ByteArray = ByteArray(ViewOnceCodec.IV_BYTES).also(random::nextBytes)

    fun encrypt(plain: ByteArray, key: ByteArray, iv: ByteArray): ByteArray {
        require(key.size == ViewOnceCodec.KEY_BYTES && iv.size == ViewOnceCodec.IV_BYTES)
        val cipher = Cipher.getInstance(TRANSFORM)
        cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(ViewOnceCodec.TAG_BYTES * 8, iv))
        return cipher.doFinal(plain)
    }

    fun decrypt(sealed: ByteArray, key: ByteArray, iv: ByteArray): ByteArray? {
        if (key.size != ViewOnceCodec.KEY_BYTES || iv.size != ViewOnceCodec.IV_BYTES || sealed.size < ViewOnceCodec.TAG_BYTES) return null
        return try {
            val cipher = Cipher.getInstance(TRANSFORM)
            cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(ViewOnceCodec.TAG_BYTES * 8, iv))
            cipher.doFinal(sealed)
        } catch (_: AEADBadTagException) {
            null
        } catch (_: Exception) {
            null
        }
    }
}

enum class ViewOnceState(val wire: String) {
    UPLOADING("uploading"),
    SENT("sent"),
    READY("ready"),
    OPENED("opened"),
    EXPIRED("expired"),
    WITHDRAWN("withdrawn"),
    ;

    val terminal: Boolean get() = this == OPENED || this == EXPIRED || this == WITHDRAWN

    companion object {
        fun fromServer(state: String, self: Boolean): ViewOnceState? = when (state) {
            "uploading", "ready" -> if (self) SENT else READY
            "opened" -> OPENED
            "expired" -> EXPIRED
            "withdrawn" -> WITHDRAWN
            else -> null
        }
    }
}

class ViewOnceEntry(
    val messageId: String,
    val conversation: String,
    val mediaId: String,
    val kind: ViewOnceKind,
    val mime: String,
    val durationMs: Long,
    val size: Long,
    val parts: Int,
    val self: Boolean,
    val sentAt: Long,
    val state: ViewOnceState,
    internal val key: ByteArray?,
    internal val iv: ByteArray?,
) {
    val hasKey: Boolean get() = key != null

    val playable: Boolean get() = !self && state == ViewOnceState.READY && key != null && iv != null

    val durationLabel: String get() = ViewOnceCodec.durationLabel(durationMs)

    val statusLabel: String
        get() = when (state) {
            ViewOnceState.UPLOADING -> "Sending"
            ViewOnceState.SENT -> "Sent"
            ViewOnceState.READY -> if (key == null) "Not available" else "Tap to play once"
            ViewOnceState.OPENED -> "Opened"
            ViewOnceState.EXPIRED -> "Expired"
            ViewOnceState.WITHDRAWN -> if (self) "Unsent" else "Unsent by the sender"
        }

    val description: String get() = "${kind.label}, $durationLabel, $statusLabel"

    internal fun with(state: ViewOnceState): ViewOnceEntry {
        val drop = state.terminal
        return ViewOnceEntry(
            messageId, conversation, mediaId, kind, mime, durationMs, size, parts, self, sentAt, state,
            if (drop) null else key, if (drop) null else iv,
        )
    }
}

class ViewOnceStore(private val maxEntries: Int = 1000) {
    private val lock = Any()
    private val byMessage = LinkedHashMap<String, ViewOnceEntry>()
    private val byMedia = HashMap<String, String>()
    private val early = LinkedHashMap<String, String>()
    private val consumeDue = LinkedHashSet<String>()

    fun accept(messageId: String, conversation: String, media: ViewOnceMedia, self: Boolean, sentAt: Long): ViewOnceEntry =
        synchronized(lock) {
            byMessage[messageId]?.let { return@synchronized it }
            byMedia[media.media]?.let { existing -> byMessage[existing]?.let { return@synchronized it } }
            var entry = ViewOnceEntry(
                messageId, conversation, media.media, media.kind, media.mime, media.durationMs, media.size, media.parts,
                self, sentAt, if (self) ViewOnceState.SENT else ViewOnceState.READY,
                if (self) null else media.key.copyOf(), if (self) null else media.iv.copyOf(),
            )
            early.remove(media.media)?.let { wire -> ViewOnceState.fromServer(wire, self)?.let { entry = entry.with(it) } }
            byMessage[messageId] = entry
            byMedia[media.media] = messageId
            while (byMessage.size > maxEntries) {
                val oldest = byMessage.keys.first()
                byMessage.remove(oldest)?.let { byMedia.remove(it.mediaId) }
            }
            entry
        }

    fun entry(messageId: String): ViewOnceEntry? = synchronized(lock) { byMessage[messageId] }

    fun byMedia(mediaId: String): ViewOnceEntry? = synchronized(lock) { byMedia[mediaId]?.let { byMessage[it] } }

    fun entries(conversation: String): List<ViewOnceEntry> =
        synchronized(lock) { byMessage.values.filter { it.conversation == conversation } }

    fun pending(conversation: String): List<String> = synchronized(lock) {
        byMessage.values.filter { it.conversation == conversation && (!it.state.terminal || it.mediaId in consumeDue) }.map { it.mediaId }
    }

    fun update(mediaId: String, wire: String): ViewOnceEntry? = synchronized(lock) {
        val messageId = byMedia[mediaId]
        val current = messageId?.let { byMessage[it] }
        if (current == null) {
            if (ViewOnceCodec.validMediaId(mediaId) && ViewOnceState.fromServer(wire, false)?.terminal == true) {
                early[mediaId] = wire
                while (early.size > maxEntries) early.remove(early.keys.first())
            }
            return@synchronized null
        }
        val next = ViewOnceState.fromServer(wire, current.self) ?: return@synchronized null
        set(current, next)
    }

    fun mark(messageId: String, state: ViewOnceState): ViewOnceEntry? = synchronized(lock) {
        val current = byMessage[messageId] ?: return@synchronized null
        set(current, state)
    }

    private fun set(current: ViewOnceEntry, next: ViewOnceState): ViewOnceEntry? {
        if (current.state == next) return null
        if (current.state.terminal && !next.terminal) return null
        val updated = current.with(next)
        byMessage[current.messageId] = updated
        return updated
    }

    fun keyFor(messageId: String): Pair<ByteArray, ByteArray>? = synchronized(lock) {
        val entry = byMessage[messageId] ?: return@synchronized null
        val key = entry.key ?: return@synchronized null
        val iv = entry.iv ?: return@synchronized null
        key.copyOf() to iv.copyOf()
    }

    fun needsConsume(mediaId: String, due: Boolean) = synchronized(lock) {
        if (due) consumeDue.add(mediaId) else consumeDue.remove(mediaId)
    }

    fun consumeDue(mediaId: String): Boolean = synchronized(lock) { mediaId in consumeDue }

    fun forgetConversation(conversation: String) = synchronized(lock) {
        val gone = byMessage.values.filter { it.conversation == conversation }
        for (entry in gone) {
            byMessage.remove(entry.messageId)
            byMedia.remove(entry.mediaId)
            consumeDue.remove(entry.mediaId)
        }
    }

    fun clear() = synchronized(lock) {
        byMessage.clear()
        byMedia.clear()
        early.clear()
        consumeDue.clear()
    }
}

object ViewOnceFrames {
    const val KIND = "forkmesh-media"
    private val STATES = setOf("uploading", "ready", "opened", "expired", "withdrawn")

    fun relayState(envelope: Map<String, Any?>): Pair<String, String>? {
        if (envelope["kind"] != KIND) return null
        if (jsonLong(envelope["v"]) != 1L) return null
        val id = jsonText(envelope["id"])
        val state = jsonText(envelope["state"])
        if (!ViewOnceCodec.validMediaId(id) || state !in STATES) return null
        return id to state
    }

    fun errorMessage(code: String): String = when (code) {
        "media_quota_count" -> "Too many unopened messages are waiting. Try again after some are watched."
        "media_quota_bytes" -> "Too much unopened media is waiting. Try again after some is watched."
        "media_too_large" -> "That recording is too large to send."
        "media_too_long" -> "That recording is too long to send."
        "media_gone", "http_410" -> "This message is no longer available."
        "recipient_only" -> "Only the recipient can play this message."
        "sender_only" -> "Only the sender can do that."
        "invalid_session" -> "Sign in again to use direct messages."
        "" -> "The message could not be sent."
        else -> code.replace('_', ' ')
    }
}

interface MessageCaptureStage {
    val active: Boolean get() = false

    fun process(input: File, mime: String): File = input
}

object PassThroughCaptureStage : MessageCaptureStage

interface MessageCaptureFilters {
    val face: MessageCaptureStage get() = PassThroughCaptureStage
    val voice: MessageCaptureStage get() = PassThroughCaptureStage

    fun apply(kind: ViewOnceKind, input: File, mime: String): File =
        (if (kind == ViewOnceKind.VIDEO) face else voice).process(input, mime)
}

object PassThroughCaptureFilters : MessageCaptureFilters

class ViewOnceClient(private val api: ChatAccountCalling) {
    fun path(conversationId: String): String = "${DirectMessageFrames.ENDPOINT}/$conversationId/media"

    fun upload(
        conversationId: String,
        kind: ViewOnceKind,
        sealed: ByteArray,
        durationMs: Long,
        onProgress: (Int, Int) -> Unit = { _, _ -> },
    ): String {
        val size = sealed.size.toLong()
        if (size <= ViewOnceCodec.TAG_BYTES || size > ViewOnceCodec.maxCipherBytes(kind)) throw ViewOnceException(ViewOnceFrames.errorMessage("media_too_large"))
        val pieces = ViewOnceCodec.split(sealed)
        val created = call(
            "POST", path(conversationId),
            linkedMapOf("kind" to kind.wire, "size" to size, "parts" to pieces.size, "durationMs" to durationMs),
        )
        val id = jsonText((created["media"] as? Map<*, *>)?.get("id"))
        if (!ViewOnceCodec.validMediaId(id)) throw ViewOnceException("The server did not accept the message.")
        try {
            onProgress(0, pieces.size)
            pieces.forEachIndexed { index, piece ->
                call("PUT", "${path(conversationId)}/$id/parts/$index", linkedMapOf("data" to Base64.getEncoder().encodeToString(piece)))
                onProgress(index + 1, pieces.size)
            }
            call("POST", "${path(conversationId)}/$id/finish", emptyMap())
        } catch (error: Exception) {
            runCatching { withdraw(conversationId, id) }
            throw error
        }
        return id
    }

    fun download(conversationId: String, mediaId: String, parts: Int, size: Long): ByteArray {
        if (!ViewOnceCodec.validMediaId(mediaId) || parts < 1) throw ViewOnceException("This message is not valid.")
        val out = ByteArrayOutputStream(size.coerceIn(0, Int.MAX_VALUE.toLong()).toInt())
        for (index in 0 until parts) {
            val reply = gone(conversationId, mediaId) { call("GET", "${path(conversationId)}/$mediaId/parts/$index", null) }
            val data = runCatching { Base64.getDecoder().decode(jsonText(reply["data"])) }.getOrNull()
                ?: throw ViewOnceException("The message could not be downloaded.")
            out.write(data)
            if (out.size() > size) throw ViewOnceException("The message could not be downloaded.")
        }
        if (out.size().toLong() != size) throw ViewOnceException("The message could not be downloaded.")
        return out.toByteArray()
    }

    fun consume(conversationId: String, mediaId: String): String {
        val reply = gone(conversationId, mediaId) { call("POST", "${path(conversationId)}/$mediaId/consume", emptyMap()) }
        return jsonText((reply["media"] as? Map<*, *>)?.get("state")).ifEmpty { "opened" }
    }

    fun withdraw(conversationId: String, mediaId: String): String {
        val reply = call("DELETE", "${path(conversationId)}/$mediaId", null)
        return jsonText((reply["media"] as? Map<*, *>)?.get("state")).ifEmpty { "withdrawn" }
    }

    fun state(conversationId: String, mediaId: String): String {
        val reply = call("GET", "${path(conversationId)}/$mediaId", null)
        return jsonText((reply["media"] as? Map<*, *>)?.get("state"))
    }

    fun status(conversationId: String, ids: List<String>): Map<String, String> {
        val wanted = ids.filter(ViewOnceCodec::validMediaId).distinct()
        val out = LinkedHashMap<String, String>()
        for (chunk in wanted.chunked(ViewOnceCodec.STATUS_IDS_MAX)) {
            val reply = call("GET", "${path(conversationId)}?ids=${chunk.joinToString(",")}", null)
            for (row in jsonMaps(reply["media"])) {
                val id = jsonText(row["id"])
                val state = jsonText(row["state"])
                if (id in chunk && state.isNotEmpty()) out[id] = state
            }
        }
        return out
    }

    private fun <T> gone(conversationId: String, mediaId: String, block: () -> T): T = try {
        block()
    } catch (error: OrgApiException) {
        if (error.code == "media_gone" || error.code == "http_410") {
            val state = runCatching { state(conversationId, mediaId) }.getOrDefault("").takeIf { it.isNotEmpty() && it != "ready" } ?: "expired"
            throw ViewOnceGone(state)
        }
        throw error
    }

    private fun call(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> = api.accountApiCall(method, path, body)
}
