package com.forkmesh.nativeapp.chat

object ChatProfileLink {
    fun senderProfile(row: ChatRow): String? {
        val message = row.message
        if (message.self || row.visitor) return null
        if (message.senderId == "forkbot") return null
        if (message.senderId.startsWith("app:")) return null
        return message.senderName.trim().takeIf { it.isNotEmpty() }
    }
}
