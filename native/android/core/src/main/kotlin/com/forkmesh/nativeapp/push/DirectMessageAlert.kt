package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.chat.DirectMessageFrames
import com.forkmesh.nativeapp.chat.OfficeFrames
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.PingSettings

const val DIRECT_MESSAGES_TOPIC = "direct-messages"
const val DIRECT_MESSAGE_HREF = "/chat/direct-messages"
const val DIRECT_MESSAGE_PING_ID = "direct-messages"

data class DirectMessageAlert(val title: String, val body: String, val target: PushTarget, val sender: String = "")

data class UnreadDirectMessage(val sender: String = "", val conversationId: String = "")

fun directMessageAlert(
    topic: String,
    settings: PingSettings?,
    nowMs: Long,
    projectId: String,
    projectLabel: String,
    multipleProjects: Boolean,
    sender: String = "",
    conversationId: String = "",
): DirectMessageAlert? {
    if (topic != DIRECT_MESSAGES_TOPIC) return null
    if (pingAlertsHeld(settings, nowMs)) return null
    val from = sender.trim().lowercase()
    val headline = withPingIcon(if (from.isEmpty()) "New direct message" else "Direct message from $from", PING_DIRECT_MESSAGE_ICON)
    val (title, body) = pingNotificationTitle(headline, "Open Chat in ForkMesh to reply.", projectLabel, multipleProjects)
    val href = if (OfficeFrames.validChannelId(conversationId)) "$DIRECT_MESSAGE_HREF/$conversationId" else DIRECT_MESSAGE_HREF
    return DirectMessageAlert(title, body, PushTarget(projectId, DIRECT_MESSAGE_PING_ID, href), from)
}

fun unreadDirectMessage(conversations: Any?): UnreadDirectMessage {
    val rows = conversations as? List<*> ?: return UnreadDirectMessage()
    for (row in rows) {
        val map = row as? Map<*, *> ?: continue
        val unread = (map["unreadCount"] as? Number)?.toLong() ?: 0L
        val other = jsonText(map["otherUser"]).trim().lowercase()
        if (unread > 0 && other.isNotEmpty()) {
            val id = jsonText(map["id"])
            return UnreadDirectMessage(other, if (OfficeFrames.validChannelId(id)) id else "")
        }
    }
    return UnreadDirectMessage()
}

fun directMessageSender(conversations: Any?): String = unreadDirectMessage(conversations).sender

fun directMessageTargetConversation(target: PushTarget): String {
    val prefix = "$DIRECT_MESSAGE_HREF/"
    if (!target.href.startsWith(prefix)) return ""
    val id = target.href.removePrefix(prefix)
    return if (OfficeFrames.validChannelId(id)) DirectMessageFrames.PREFIX + id else ""
}

fun isDirectMessageTarget(target: PushTarget): Boolean =
    target.href == DIRECT_MESSAGE_HREF || directMessageTargetConversation(target).isNotEmpty()

fun directMessageConversation(unread: List<String>, meshConversation: String): String? =
    unread.firstOrNull { it.startsWith("@") && it != meshConversation }
