package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.Member
import com.forkmesh.nativeapp.model.MemberGroup
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.format.TextStyle
import java.util.Locale
import kotlin.math.abs

const val GUEST_CHAT_CHANNEL = "general"

const val FOLLOW_NEW_MESSAGES_SLACK_DP = 160

fun guestComposeBlocked(isGuest: Boolean, conversation: String): String? {
    if (!isGuest) return null
    if (conversation == "#$GUEST_CHAT_CHANNEL") return null
    if (conversation == MESH_CONVERSATION) return null
    if (conversation.startsWith("@")) return "send direct messages"
    return "post in $conversation"
}

data class MessageBodySegment(val code: Boolean, val text: String)

fun messageBodySegments(source: String): List<MessageBodySegment> {
    val parts = source.split("```")
    val out = ArrayList<MessageBodySegment>()
    for (i in parts.indices) {
        val odd = i % 2 == 1
        val content = if (odd) parts[i].trimEnd() else parts[i]
        if (dartTrimText(content).isEmpty()) continue
        out.add(MessageBodySegment(code = odd, text = dartTrimText(content)))
    }
    return out
}

const val MESSAGE_GROUP_WINDOW_MS = 5 * 60_000L

fun continuesGroup(previous: ChatMessage?, message: ChatMessage, zone: ZoneId = ZoneId.systemDefault()): Boolean {
    val prior = previous ?: return false
    if (prior.senderName != message.senderName || prior.self != message.self) return false
    if (startsNewDay(prior, message, zone)) return false
    return abs(message.timestamp - prior.timestamp) < MESSAGE_GROUP_WINDOW_MS
}

fun startsNewDay(previous: ChatMessage?, message: ChatMessage, zone: ZoneId = ZoneId.systemDefault()): Boolean {
    val prior = previous ?: return true
    return localDay(prior.timestamp, zone) != localDay(message.timestamp, zone)
}

private fun localDay(timestampMs: Long, zone: ZoneId): LocalDate = Instant.ofEpochMilli(timestampMs).atZone(zone).toLocalDate()

fun dayDividerLabel(timestampMs: Long, nowMs: Long = System.currentTimeMillis(), zone: ZoneId = ZoneId.systemDefault()): String {
    val day = localDay(timestampMs, zone)
    val today = localDay(nowMs, zone)
    if (day == today) return "Today"
    if (day == today.minusDays(1)) return "Yesterday"
    val weekday = day.dayOfWeek.getDisplayName(TextStyle.FULL, Locale.ENGLISH)
    val month = day.month.getDisplayName(TextStyle.FULL, Locale.ENGLISH)
    val label = "$weekday, $month ${day.dayOfMonth}"
    return if (day.year == today.year) label else "$label, ${day.year}"
}

fun messageTime(timestampMs: Long, zone: ZoneId = ZoneId.systemDefault()): String {
    val time = Instant.ofEpochMilli(timestampMs).atZone(zone)
    val meridiem = if (time.hour < 12) "AM" else "PM"
    val hour = if (time.hour % 12 == 0) 12 else time.hour % 12
    return "$hour:${time.minute.toString().padStart(2, '0')} $meridiem"
}

fun meshBadge(networkActive: Boolean, bluetoothActive: Boolean, peerCount: Int = 0): String {
    val carried = ArrayList<String>()
    if (networkActive) carried.add("this network")
    if (bluetoothActive) carried.add("bluetooth")
    if (carried.isEmpty()) return "local only"
    val linked = when (peerCount) {
        0 -> ""
        1 -> " · 1 device"
        else -> " · $peerCount devices"
    }
    return "local · ${carried.joinToString(" + ")}$linked"
}

fun meshEmptyMessage(networkActive: Boolean, peerCount: Int, localNetworkBlocked: Boolean = false): String {
    if (localNetworkBlocked) {
        return "This phone is not allowed onto the local network, so no nearby device can be reached."
    }
    val searching = if (networkActive && peerCount == 0) " Looking for devices with ForkMesh open on this Wi-Fi." else ""
    return "Messages stay on this network and this Bluetooth link. Nothing here is sent to the internet.$searching"
}

fun canDeleteMessage(message: ChatMessage): Boolean = message.self && message.conversation != MESH_CONVERSATION

fun messageCopyText(message: ChatMessage): String = message.text.ifEmpty { message.fileName }

fun unreadBannerLabel(count: Int): String = if (count == 1) "1 unread message" else "$count unread messages"

fun conversationTitle(conversation: String, roster: List<Member>): String {
    if (conversation.startsWith("@")) {
        val member = roster.firstOrNull { it.id == conversation.substring(1) }
            ?: Member(id = conversation, name = "direct message")
        return "@${member.name}"
    }
    return conversation
}

fun meshMemberGroups(members: List<Member>): List<MemberGroup> = memberGroupsOf(members)

fun platformIconName(platform: String): String = when (platform.lowercase()) {
    "android" -> "android"
    "ios" -> "phone_iphone"
    "macos" -> "laptop_mac"
    "windows" -> "desktop_windows"
    "linux" -> "dvr_outlined"
    "web" -> "public"
    else -> "devices_other"
}

class TranscriptFollower {
    enum class Action { NONE, JUMP_TO_END, ANIMATE_TO_END }

    private var followedConversation: String? = null
    private var followedLastId = ""

    fun onMessages(conversation: String, messages: List<ChatMessage>, nearEnd: () -> Boolean): Action {
        val lastId = messages.lastOrNull()?.id ?: ""
        val switched = conversation != followedConversation
        if (!switched && lastId == followedLastId) return Action.NONE
        followedConversation = conversation
        followedLastId = lastId
        if (messages.isEmpty()) return Action.NONE
        if (switched) return Action.JUMP_TO_END
        return if (nearEnd() || messages.last().self) Action.ANIMATE_TO_END else Action.NONE
    }
}
