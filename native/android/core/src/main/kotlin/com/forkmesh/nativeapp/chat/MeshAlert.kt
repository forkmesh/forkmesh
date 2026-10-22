package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.push.DirectMessagePreviews
import com.forkmesh.nativeapp.push.NotificationContent

const val MESH_ALERT_FRESH_MS = 10 * 60_000L
const val MESH_ALERT_PREVIEW_CHARS = 180
const val MESH_ALERT_HREF = "/chat/mesh"

data class MeshAlert(val title: String, val body: String, val sender: String)

fun meshAlert(message: ChatMessage, unread: Int, watching: Boolean, nowMs: Long): MeshAlert? {
    if (message.self || watching || message.conversation != MESH_CONVERSATION) return null
    if (nowMs - message.timestamp > MESH_ALERT_FRESH_MS) return null
    val sender = message.senderName.trim().ifEmpty { "Someone nearby" }
    val text = message.text.split(Regex("\\s+")).filter { it.isNotEmpty() }.joinToString(" ")
    val preview = text.ifEmpty { if (message.fileName.isEmpty()) "New message" else "File: ${message.fileName}" }
    val body = if (preview.length > MESH_ALERT_PREVIEW_CHARS) preview.take(MESH_ALERT_PREVIEW_CHARS - 3) + "..." else preview
    val title = if (unread > 1) "$sender in $MESH_CONVERSATION · $unread new" else "$sender in $MESH_CONVERSATION"
    return MeshAlert(title, body, sender)
}

fun MeshAlert.shown(content: NotificationContent): MeshAlert = when (content) {
    NotificationContent.NAME_AND_MESSAGE -> this
    NotificationContent.NAME_ONLY -> copy(body = DirectMessagePreviews.GENERIC_BODY)
    NotificationContent.NOTHING -> MeshAlert(DirectMessagePreviews.GENERIC_TITLE, DirectMessagePreviews.GENERIC_BODY, "")
}
