package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.model.ChatMessage
import java.time.Instant
import java.time.ZoneId
import java.time.format.TextStyle
import java.time.temporal.ChronoUnit
import java.util.Locale

data class ChatListPreview(val line: String, val timestamp: Long, val messageId: String)

object ChatListPreviews {
    const val MAX_LINE_CHARS = 160
    const val SELF_PREFIX = "You: "
    const val FILE_PREFIX = "📎 "

    private val FENCE = Regex("```[a-zA-Z0-9_+-]*")
    private val EMPHASIS = Regex("(\\*\\*|__|~~|\\*|`)")
    private val LINE_MARK = Regex("^\\s{0,3}(#{1,6}\\s+|>\\s?|[-*+]\\s+(\\[[ xX]]\\s+)?)")
    private val SPACE = Regex("\\s+")

    fun plain(text: String): String {
        if (text.isEmpty()) return ""
        val unfenced = FENCE.replace(text, " ")
        val lines = unfenced.split('\n').joinToString(" ") { LINE_MARK.replace(it, "") }
        val joined = parseChatText(lines).joinToString("") { it.text }
        val stripped = EMPHASIS.replace(joined, "")
        val collapsed = SPACE.replace(stripped, " ").trim()
        return if (collapsed.length > MAX_LINE_CHARS) collapsed.take(MAX_LINE_CHARS - 1).trimEnd() + "…" else collapsed
    }

    fun body(text: String, fileName: String): String {
        val plainText = plain(text)
        if (plainText.isNotEmpty()) return plainText
        val name = SPACE.replace(fileName, " ").trim()
        return if (name.isEmpty()) "" else FILE_PREFIX + name
    }

    fun line(message: ChatMessage, body: String, direct: Boolean): String = when {
        body.isEmpty() -> ""
        message.self -> SELF_PREFIX + body
        direct -> body
        else -> "${message.senderName.trim().ifEmpty { "Someone" }}: $body"
    }

    fun newest(
        messages: List<ChatMessage>,
        direct: Boolean,
        skip: (ChatMessage) -> Boolean = { false },
        text: (ChatMessage) -> String = { it.text },
        fileName: (ChatMessage) -> String = { it.fileName },
    ): ChatListPreview? {
        var best: ChatListPreview? = null
        for (message in messages.asReversed()) {
            if (best != null && message.timestamp <= best.timestamp) continue
            if (message.ephemeral || skip(message)) continue
            val line = line(message, body(text(message), fileName(message)), direct)
            if (line.isEmpty()) continue
            best = ChatListPreview(line, message.timestamp, message.id)
        }
        return best
    }

    fun recencyMs(updatedAt: Long): Long = if (updatedAt in 1 until 100_000_000_000L) updatedAt * 1000 else updatedAt

    fun timeLabel(timestamp: Long, nowMs: Long, zone: ZoneId = ZoneId.systemDefault(), locale: Locale = Locale.getDefault()): String {
        if (timestamp <= 0) return ""
        val at = Instant.ofEpochMilli(timestamp).atZone(zone)
        val today = Instant.ofEpochMilli(nowMs).atZone(zone).toLocalDate()
        val day = at.toLocalDate()
        val days = ChronoUnit.DAYS.between(day, today)
        return when {
            days <= 0 -> "%02d:%02d".format(at.hour, at.minute)
            days < 7 -> at.dayOfWeek.getDisplayName(TextStyle.SHORT, locale)
            else -> "${at.dayOfMonth} ${at.month.getDisplayName(TextStyle.SHORT, locale)}"
        }
    }
}
