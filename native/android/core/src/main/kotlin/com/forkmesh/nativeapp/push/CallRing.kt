package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.chat.DirectCallEngine
import com.forkmesh.nativeapp.chat.DirectMessageFrames
import com.forkmesh.nativeapp.chat.OfficeFrames
import com.forkmesh.nativeapp.json.jsonText

object CallRings {
    const val WAKE_WAIT_MS = 6_000L
    const val RING_MS = DirectCallEngine.RING_TIMEOUT_MS

    fun conversation(frame: Map<String, Any?>): String? {
        if (jsonText(frame["topic"]) != DirectCallEngine.RING_TOPIC) return null
        val raw = jsonText(frame["conversation"])
        if (!raw.startsWith(DirectMessageFrames.PREFIX)) return null
        return if (OfficeFrames.validChannelId(raw.removePrefix(DirectMessageFrames.PREFIX))) raw else null
    }

    fun partner(conversations: Any?, conversation: String): String {
        val id = conversation.removePrefix(DirectMessageFrames.PREFIX)
        val rows = conversations as? List<*> ?: return ""
        for (row in rows) {
            val map = row as? Map<*, *> ?: continue
            if (jsonText(map["id"]) == id) return jsonText(map["otherUser"]).trim().lowercase()
        }
        return ""
    }

    fun target(projectId: String, conversation: String): PushTarget =
        PushTarget(projectId, DIRECT_MESSAGE_PING_ID, "$DIRECT_MESSAGE_HREF/${conversation.removePrefix(DirectMessageFrames.PREFIX)}")

    fun title(partner: String): String = if (partner.isEmpty()) "Incoming call" else "@$partner"

    fun text(partner: String): String = if (partner.isEmpty()) "Someone is calling you in ForkMesh" else "@$partner is calling you"
}
