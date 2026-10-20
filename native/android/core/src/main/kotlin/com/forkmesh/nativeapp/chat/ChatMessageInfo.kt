package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.ChatReceipt
import com.forkmesh.nativeapp.model.ChatReceiptTimes
import java.time.Instant
import java.time.ZoneId
import java.time.temporal.ChronoUnit

data class ChatInfoLine(
    val title: String,
    val value: String,
    val icon: String,
    val detail: String = "",
    val accent: Boolean = false,
) {
    val pending: Boolean get() = value == ChatMessageInfo.NOT_YET
}

data class ChatMessageInfoPage(
    val heading: String,
    val timeline: List<ChatInfoLine>,
    val more: List<ChatInfoLine>,
    val note: String?,
)

object ChatMessageInfo {
    const val NOT_YET = "Not yet"
    const val OPEN_FRACTION = 0.3f
    const val CLOSE_FRACTION = 0.35f
    const val FLING_FRACTION = 0.5f
    const val SIDEWAYS_RATIO = 1.5f

    private val MONTHS = listOf("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
    private val WEEKDAYS = listOf("Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday")

    fun clock(ms: Long, zone: ZoneId = ZoneId.systemDefault()): String = messageTime(ms, zone)

    fun seconds(ms: Long, zone: ZoneId = ZoneId.systemDefault()): String {
        val time = Instant.ofEpochMilli(ms).atZone(zone)
        val hour = if (time.hour % 12 == 0) 12 else time.hour % 12
        val meridiem = if (time.hour < 12) "AM" else "PM"
        return "$hour:${time.minute.toString().padStart(2, '0')}:${time.second.toString().padStart(2, '0')} $meridiem"
    }

    private fun daysBetween(ms: Long, nowMs: Long, zone: ZoneId): Long {
        val day = Instant.ofEpochMilli(ms).atZone(zone).toLocalDate()
        val today = Instant.ofEpochMilli(nowMs).atZone(zone).toLocalDate()
        return ChronoUnit.DAYS.between(day, today)
    }

    fun timeLabel(ms: Long, nowMs: Long, zone: ZoneId = ZoneId.systemDefault()): String {
        val time = clock(ms, zone)
        val days = daysBetween(ms, nowMs, zone)
        if (days == 0L) return "Today $time"
        if (days == 1L) return "Yesterday $time"
        val date = Instant.ofEpochMilli(ms).atZone(zone)
        if (days in 2..6) return "${WEEKDAYS[date.dayOfWeek.value - 1]} $time"
        val month = MONTHS[date.monthValue - 1]
        val year = Instant.ofEpochMilli(nowMs).atZone(zone).year
        return if (date.year == year) "$month ${date.dayOfMonth} at $time" else "$month ${date.dayOfMonth}, ${date.year} at $time"
    }

    fun stamp(ms: Long, nowMs: Long, zone: ZoneId = ZoneId.systemDefault()): String {
        val time = clock(ms, zone)
        val days = daysBetween(ms, nowMs, zone)
        if (days == 0L) return "Today at $time"
        if (days == 1L) return "Yesterday at $time"
        val date = Instant.ofEpochMilli(ms).atZone(zone)
        return "${MONTHS[date.monthValue - 1]} ${date.dayOfMonth}, ${date.year} at $time"
    }

    fun receiptStamp(ms: Long, nowMs: Long, zone: ZoneId = ZoneId.systemDefault()): String =
        if (daysBetween(ms, nowMs, zone) == 0L) clock(ms, zone) else timeLabel(ms, nowMs, zone)

    fun receiptStatus(
        message: ChatMessage,
        receipt: ChatReceipt?,
        times: ChatReceiptTimes,
        nowMs: Long,
        zone: ZoneId = ZoneId.systemDefault(),
    ): String? {
        if (!DirectMessageFrames.isDirect(message.conversation)) return null
        return when (receipt) {
            ChatReceipt.READ -> times.read?.let { "Read ${receiptStamp(it, nowMs, zone)}" } ?: "Read"
            ChatReceipt.DELIVERED -> times.delivered?.let { "Delivered ${receiptStamp(it, nowMs, zone)}" } ?: "Delivered"
            else -> null
        }
    }

    fun lastOwnId(rows: List<ChatRow>): String? =
        rows.lastOrNull { it.message.self && !it.message.ephemeral && it.message.senderId != DirectMessageFrames.SYSTEM_SENDER }?.message?.id

    fun previewText(row: ChatRow): String {
        if (row.text.isNotEmpty()) return row.text
        when (row.attachment?.kind) {
            ChatAttachmentKind.IMAGE -> return "Photo"
            ChatAttachmentKind.AUDIO -> return "Audio message"
            ChatAttachmentKind.VIDEO -> return "Video"
            else -> {}
        }
        return row.message.fileName.ifEmpty { "Message" }
    }

    fun page(
        row: ChatRow,
        times: ChatReceiptTimes,
        expiresAt: Long?,
        onServer: Boolean,
        nowMs: Long,
        zone: ZoneId = ZoneId.systemDefault(),
    ): ChatMessageInfoPage {
        val message = row.message
        val mine = message.self
        val direct = DirectMessageFrames.isDirect(message.conversation)
        fun timed(title: String, ms: Long, icon: String, accent: Boolean = false) =
            ChatInfoLine(title, stamp(ms, nowMs, zone), icon, detail = seconds(ms, zone), accent = accent)

        val timeline = ArrayList<ChatInfoLine>()
        if (!mine && message.senderName.isNotEmpty()) timeline.add(ChatInfoLine("From", message.senderName, "account_circle"))
        if (row.receipt == ChatReceipt.SENDING) {
            timeline.add(ChatInfoLine("Sent", "Sending…", "send_outlined"))
        } else {
            timeline.add(timed("Sent", message.timestamp, "send_outlined"))
        }
        if (mine && direct) {
            val rank = row.receipt?.ordinal ?: 0
            val delivered = times.delivered
            timeline.add(
                if (delivered != null) {
                    timed("Delivered", delivered, "check_circle_outlined")
                } else {
                    ChatInfoLine("Delivered", if (rank >= ChatReceipt.DELIVERED.ordinal) "Delivered" else NOT_YET, "check_circle_outlined")
                },
            )
            val read = times.read
            timeline.add(
                if (read != null) {
                    timed("Read", read, "visibility_outlined", accent = true)
                } else {
                    ChatInfoLine("Read", if (rank >= ChatReceipt.READ.ordinal) "Read" else NOT_YET, "visibility_outlined")
                },
            )
        }

        val more = ArrayList<ChatInfoLine>()
        OfficeFrames.recipientsDescription(message.recipients)?.let { more.add(ChatInfoLine("Recipients", it, "group_outlined")) }
        if (row.edited) more.add(ChatInfoLine("Edited", "Yes", "edit_outlined"))
        if (expiresAt != null) more.add(timed("Disappears", expiresAt, "timer_outlined"))
        if (onServer) more.add(ChatInfoLine("Server copy", "Waiting for pickup", "cloud_outlined"))

        val note = if (mine && direct && row.receipt != ChatReceipt.SENDING && times.read == null && row.receipt == ChatReceipt.READ) {
            "Read before this device kept read times."
        } else {
            null
        }
        return ChatMessageInfoPage(if (mine) "Your message" else "Message", timeline, more, note)
    }

    fun opens(dragX: Float, flingX: Float, width: Float): Boolean =
        -dragX > width * OPEN_FRACTION || -flingX > width * FLING_FRACTION

    fun closes(dragX: Float, flingX: Float, width: Float): Boolean =
        dragX > width * CLOSE_FRACTION || flingX > width * FLING_FRACTION
}
