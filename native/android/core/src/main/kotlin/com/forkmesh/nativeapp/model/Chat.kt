package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.apps.AppBlock

data class Member(
    val id: String,
    val name: String,
    val self: Boolean = false,
    val online: Boolean = false,
    val platform: String = "",
    val version: String = "",
    val solanaAddress: String = "",
    val mirrors: List<String> = emptyList(),
    val owner: String = "",
    val nodeName: String = "",
    val userStatus: UserStatus? = null,
    val diagnostics: List<Any?>? = null,
)

data class MemberGroup(
    val name: String,
    val members: List<Member>,
    val self: Boolean = false,
    val disambiguator: String = "",
) {
    val online: Boolean get() = members.any { it.online }

    val primary: Member get() = members.first()

    val id: String get() = primary.id

    val nodeCount: Int get() = members.size
}

data class ChatMessage(
    val id: String,
    val conversation: String,
    val senderId: String,
    val senderName: String,
    val text: String,
    val timestamp: Long,
    val self: Boolean = false,
    val fileName: String = "",
    val recipients: Int? = null,
    val blocks: List<AppBlock> = emptyList(),
    val appMessageId: String = "",
    val ephemeral: Boolean = false,
)

enum class ChatReceipt(val label: String) {
    SENDING("Sending"),
    SENT("Sent"),
    DELIVERED("Delivered"),
    READ("Read"),
}
