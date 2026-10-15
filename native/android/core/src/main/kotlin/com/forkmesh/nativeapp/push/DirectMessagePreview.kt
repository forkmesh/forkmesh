package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.chat.ChatAccountCalling
import com.forkmesh.nativeapp.chat.ChatLimits
import com.forkmesh.nativeapp.chat.DirectMessageFrames
import com.forkmesh.nativeapp.chat.OfficeFrames
import com.forkmesh.nativeapp.crypto.RoomCrypto
import com.forkmesh.nativeapp.crypto.RoomSeal
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.security.SecretStore

enum class NotificationContent(val wire: String, val label: String, val detail: String) {
    NAME_AND_MESSAGE("name-and-message", "Name and message", "Shows who wrote and what they said."),
    NAME_ONLY("name-only", "Name only", "Shows who wrote, not what they said."),
    NOTHING("none", "No name or message", "Shows only that a new message arrived.");

    companion object {
        const val PREFS_KEY = "notifications/content"
        const val TITLE = "Notification content"
        const val NOTE = "What a direct message notification shows. The phone decrypts the message itself; the server never sees the text."

        fun of(wire: String?): NotificationContent = entries.firstOrNull { it.wire == wire } ?: NAME_AND_MESSAGE
    }
}

data class DirectMessageEvent(val conversationId: String, val ref: String, val keyVersion: Int = 1) {
    val conversation: String get() = DirectMessageFrames.PREFIX + conversationId

    val pendingPath: String get() = "${DirectMessageFrames.ENDPOINT}/$conversationId/pending?ref=$ref"

    val deliveredPath: String get() = "${DirectMessageFrames.ENDPOINT}/$conversationId/delivered?ref=$ref"
}

data class DirectMessagePreview(
    val messageId: String,
    val conversationId: String,
    val sender: String,
    val text: String,
    val fileName: String,
    val timestamp: Long,
    val reply: Boolean = false,
)

data class PreviewNotice(val title: String, val body: String, val sender: String, val messageText: String)

data class PreviewLine(val messageId: String, val sender: String, val text: String, val timestamp: Long)

object DirectMessagePreviews {
    const val GENERIC_TITLE = "ForkMesh"
    const val GENERIC_BODY = "New message"
    const val MAX_BODY_CHARS = 1000
    const val MAX_LINES = 8
    private val REF = Regex("^[0-9a-f]{40}$")

    fun event(frame: Map<String, Any?>): DirectMessageEvent? {
        if (jsonText(frame["topic"]) != DIRECT_MESSAGES_TOPIC) return null
        val raw = jsonText(frame["conversation"])
        val id = if (raw.startsWith(DirectMessageFrames.PREFIX)) raw.removePrefix(DirectMessageFrames.PREFIX) else jsonText(frame["conversationId"])
        if (!OfficeFrames.validChannelId(id)) return null
        val ref = jsonText(frame["ref"])
        if (!REF.matches(ref)) return null
        return DirectMessageEvent(id, ref, maxOf(1L, jsonLong(frame["keyVersion"], 1)).toInt())
    }

    fun fetch(api: ChatAccountCalling, event: DirectMessageEvent, secrets: SecretStore): DirectMessagePreview? = try {
        open(event, api.accountApiCall("GET", event.pendingPath, null), secrets)
    } catch (_: Exception) {
        null
    }

    fun confirmDelivery(api: ChatAccountCalling, event: DirectMessageEvent): Boolean = try {
        api.accountApiCall("POST", event.deliveredPath, null)
        true
    } catch (_: Exception) {
        false
    }

    fun open(event: DirectMessageEvent, payload: Map<String, Any?>, secrets: SecretStore): DirectMessagePreview? {
        val conversation = jsonText(payload["conversation"])
        if (conversation.isNotEmpty() && conversation != event.conversation) return null
        val room = jsonText(payload["room"])
        val passphrase = jsonText(payload["passphrase"])
        val stored = jsonText(payload["frame"])
        if (passphrase.isEmpty() || stored.isEmpty() || !room.startsWith("chat-direct:${event.conversationId}:")) return null
        val envelope = try {
            Json.parseObject(stored)
        } catch (_: Exception) {
            return null
        }
        val framed = RoomCrypto.withPassphrase(room, passphrase).decrypt(envelope) ?: return null
        if (!RoomSeal.isSealedFrame(framed)) return null
        val seal = RoomSeal.existing(room, secureStore = secrets) ?: return null
        val plain = seal.unseal(framed) ?: return null
        return preview(plain, event)
    }

    fun preview(plain: Map<String, Any?>, event: DirectMessageEvent): DirectMessagePreview? {
        val type = jsonText(plain["type"])
        if (type != "chat" && type != "thread-reply") return null
        if (jsonText(plain["channel"]) != event.conversation) return null
        val id = jsonText(plain["id"])
        if (id.isEmpty() || id.length > 64) return null
        val text = jsonText(plain["text"]).take(ChatLimits.MAX_TEXT_CHARS).trim()
        val fileName = jsonText(plain["fileName"]).take(ChatLimits.MAX_FILE_NAME_CHARS).trim()
        if (text.isEmpty() && fileName.isEmpty()) return null
        return DirectMessagePreview(
            messageId = id,
            conversationId = event.conversationId,
            sender = jsonText(plain["sender"]).trim().take(ChatLimits.MAX_DISPLAY_NAME_CHARS),
            text = text,
            fileName = fileName,
            timestamp = jsonLong(plain["ts"]),
            reply = type == "thread-reply",
        )
    }

    fun body(preview: DirectMessagePreview): String {
        val text = preview.text
        val body = when {
            preview.fileName.isEmpty() -> text
            text.isEmpty() -> "📎 ${preview.fileName}"
            else -> "📎 ${preview.fileName}\n$text"
        }
        return if (body.length > MAX_BODY_CHARS) body.take(MAX_BODY_CHARS - 1) + "…" else body
    }

    fun notice(preview: DirectMessagePreview, content: NotificationContent, fallbackSender: String = ""): PreviewNotice {
        val name = preview.sender.ifEmpty { fallbackSender.trim() }
        return when (content) {
            NotificationContent.NAME_AND_MESSAGE -> {
                val text = body(preview)
                PreviewNotice(name.ifEmpty { GENERIC_BODY }, text, name, text)
            }
            NotificationContent.NAME_ONLY -> PreviewNotice(name.ifEmpty { GENERIC_TITLE }, GENERIC_BODY, name, GENERIC_BODY)
            NotificationContent.NOTHING -> PreviewNotice(GENERIC_TITLE, GENERIC_BODY, "", GENERIC_BODY)
        }
    }

    fun merge(existing: List<PreviewLine>, line: PreviewLine): List<PreviewLine>? {
        if (existing.any { it.messageId == line.messageId }) return null
        return (existing + line).sortedBy { it.timestamp }.takeLast(MAX_LINES)
    }
}
