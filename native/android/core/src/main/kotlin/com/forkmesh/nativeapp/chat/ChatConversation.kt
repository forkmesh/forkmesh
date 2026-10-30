package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.model.ChatReceipt
import java.text.BreakIterator
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.format.TextStyle
import java.time.temporal.ChronoUnit
import java.util.Locale

enum class ChatBubblePosition {
    SINGLE,
    FIRST,
    MIDDLE,
    LAST,
    ;

    val hasTail: Boolean get() = this == SINGLE || this == LAST

    val startsRun: Boolean get() = this == SINGLE || this == FIRST
}

data class ChatBubble(
    val row: ChatRow,
    val position: ChatBubblePosition,
    val showsSender: Boolean,
    val receipt: ChatReceipt?,
    val jumbo: Boolean,
) {
    val joinsAbove: Boolean get() = !position.startsRun

    val joinsBelow: Boolean get() = !position.hasTail
}

sealed class ChatConversationItem {
    abstract val id: String

    data class Time(val key: String, val at: Long) : ChatConversationItem() {
        override val id: String get() = "time-$key"
    }

    data class Notice(val row: ChatRow) : ChatConversationItem() {
        override val id: String get() = row.message.id
    }

    data class Message(val bubble: ChatBubble) : ChatConversationItem() {
        override val id: String get() = bubble.row.message.id
    }
}

object ChatConversationLayout {
    const val TIME_GAP_MS = 15 * 60 * 1000L
    const val MAX_JUMBO_EMOJI = 3

    fun isNotice(row: ChatRow): Boolean = row.message.ephemeral && row.message.senderId == DirectMessageFrames.SYSTEM_SENDER

    fun items(rows: List<ChatRow>, group: Boolean, zone: ZoneId = ZoneId.systemDefault()): List<ChatConversationItem> {
        val headed = ArrayList<Pair<ChatRow, Boolean>>(rows.size)
        var last: Long? = null
        for (row in rows) {
            val at = row.message.timestamp
            val header = last?.let { at - it >= TIME_GAP_MS || day(at, zone) != day(it, zone) } ?: true
            headed.add(row to header)
            last = at
        }
        val out = ArrayList<ChatConversationItem>()
        for ((index, entry) in headed.withIndex()) {
            val (row, header) = entry
            if (header) out.add(ChatConversationItem.Time(row.message.id, row.message.timestamp))
            if (isNotice(row)) {
                out.add(ChatConversationItem.Notice(row))
                continue
            }
            val joinsPrevious = index > 0 && !header && sameRun(headed[index - 1].first, row)
            val joinsNext = index + 1 < headed.size && !headed[index + 1].second && sameRun(headed[index + 1].first, row)
            val position = when {
                !joinsPrevious && !joinsNext -> ChatBubblePosition.SINGLE
                !joinsPrevious -> ChatBubblePosition.FIRST
                joinsNext -> ChatBubblePosition.MIDDLE
                else -> ChatBubblePosition.LAST
            }
            out.add(
                ChatConversationItem.Message(
                    ChatBubble(
                        row = row,
                        position = position,
                        showsSender = group && !row.message.self && position.startsRun,
                        receipt = receipt(row),
                        jumbo = row.message.fileName.isEmpty() && isJumboEmoji(row.text),
                    ),
                ),
            )
        }
        return out
    }

    private fun sameRun(other: ChatRow, row: ChatRow): Boolean {
        if (isNotice(other)) return false
        return other.message.self == row.message.self && other.message.senderId == row.message.senderId &&
            other.message.senderName == row.message.senderName
    }

    fun receipt(row: ChatRow): ChatReceipt? {
        if (!row.message.self) return null
        return row.receipt ?: ChatReceipt.SENT
    }

    fun isJumboEmoji(text: String): Boolean {
        val trimmed = text.trim()
        if (trimmed.isEmpty()) return false
        val compact = trimmed.filterNot { it.isWhitespace() }
        if (compact.isEmpty()) return false
        val clusters = graphemes(compact)
        if (clusters.isEmpty() || clusters.size > MAX_JUMBO_EMOJI) return false
        return clusters.all(::isEmojiCluster)
    }

    private fun graphemes(text: String): List<String> {
        val breaks = BreakIterator.getCharacterInstance(Locale.ROOT)
        breaks.setText(text)
        val out = ArrayList<String>()
        var start = breaks.first()
        var end = breaks.next()
        while (end != BreakIterator.DONE) {
            out.add(text.substring(start, end))
            start = end
            end = breaks.next()
        }
        return out
    }

    internal fun isEmojiCluster(cluster: String): Boolean {
        val scalars = cluster.codePoints().toArray()
        val first = scalars.firstOrNull() ?: return false
        if (first < 0x80) return false
        if (scalars.any(::isEmojiPresentation)) return true
        return isEmojiScalar(first) && scalars.contains(0xFE0F)
    }

    private val presentationSingles = intArrayOf(
        0x231A, 0x231B, 0x23F0, 0x23F3, 0x25FD, 0x25FE, 0x2614, 0x2615, 0x267F, 0x2693, 0x26A1, 0x26AA, 0x26AB,
        0x26BD, 0x26BE, 0x26C4, 0x26C5, 0x26CE, 0x26D4, 0x26EA, 0x26F2, 0x26F3, 0x26F5, 0x26FA, 0x26FD, 0x2705,
        0x270A, 0x270B, 0x2728, 0x274C, 0x274E, 0x2757, 0x27B0, 0x27BF, 0x2B1B, 0x2B1C, 0x2B50, 0x2B55, 0x1F004,
        0x1F0CF, 0x1F18E, 0x1F201, 0x1F21A, 0x1F22F, 0x1F250, 0x1F251,
    )

    private val presentationRanges = listOf(
        0x23E9..0x23EC, 0x2648..0x2653, 0x2753..0x2755, 0x2795..0x2797, 0x1F191..0x1F19A, 0x1F1E6..0x1F1FF,
        0x1F232..0x1F236, 0x1F238..0x1F23A, 0x1F300..0x1F320, 0x1F32D..0x1F335, 0x1F337..0x1F37C,
        0x1F37E..0x1F393, 0x1F3A0..0x1F3CA, 0x1F3CF..0x1F3D3, 0x1F3E0..0x1F3F0, 0x1F3F4..0x1F3F4,
        0x1F3F8..0x1F43E, 0x1F440..0x1F440, 0x1F442..0x1F4FC, 0x1F4FF..0x1F53D, 0x1F54B..0x1F54E,
        0x1F550..0x1F567, 0x1F57A..0x1F57A, 0x1F595..0x1F596, 0x1F5A4..0x1F5A4, 0x1F5FB..0x1F64F,
        0x1F680..0x1F6C5, 0x1F6CC..0x1F6CC, 0x1F6D0..0x1F6D2, 0x1F6D5..0x1F6D7, 0x1F6DC..0x1F6DF,
        0x1F6EB..0x1F6EC, 0x1F6F4..0x1F6FC, 0x1F7E0..0x1F7EB, 0x1F7F0..0x1F7F0, 0x1F90C..0x1F93A,
        0x1F93C..0x1F945, 0x1F947..0x1F9FF, 0x1FA70..0x1FAFF,
    )

    private fun isEmojiPresentation(scalar: Int): Boolean =
        scalar in presentationSingles || presentationRanges.any { scalar in it }

    private fun isEmojiScalar(scalar: Int): Boolean =
        scalar == 0xA9 || scalar == 0xAE || scalar == 0x203C || scalar == 0x2049 || scalar == 0x3030 || scalar == 0x303D ||
            scalar == 0x3297 || scalar == 0x3299 || scalar in 0x2122..0x2BFF || scalar in 0x1F000..0x1FAFF

    fun timeLabel(at: Long, nowMs: Long, zone: ZoneId = ZoneId.systemDefault()): String {
        val time = messageTime(at, zone)
        val date = Instant.ofEpochMilli(at).atZone(zone).toLocalDate()
        val today = Instant.ofEpochMilli(nowMs).atZone(zone).toLocalDate()
        val days = ChronoUnit.DAYS.between(date, today)
        if (days == 0L) return "Today $time"
        if (days == 1L) return "Yesterday $time"
        if (days in 2..6) return "${date.dayOfWeek.getDisplayName(TextStyle.FULL, Locale.ENGLISH)} $time"
        val month = date.month.getDisplayName(TextStyle.SHORT, Locale.ENGLISH)
        if (date.year == today.year) return "$month ${date.dayOfMonth} at $time"
        return "$month ${date.dayOfMonth}, ${date.year} at $time"
    }

    fun expiryRemaining(seconds: Double): String {
        val minutes = kotlin.math.max(1, kotlin.math.ceil(seconds / 60).toInt())
        if (minutes < 60) return if (minutes == 1) "1 minute" else "$minutes minutes"
        val hours = Math.round(minutes / 60.0).toInt()
        if (hours < 48) return if (hours == 1) "1 hour" else "$hours hours"
        return "${Math.round(hours / 24.0)} days"
    }

    private fun day(at: Long, zone: ZoneId): LocalDate = Instant.ofEpochMilli(at).atZone(zone).toLocalDate()
}
