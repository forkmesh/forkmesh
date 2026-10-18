package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.model.MemberGroup

enum class ChatShareKind { CHANNEL, OFFICE, DIRECT, PERSON, USER }

data class ChatShareTarget(
    val kind: ChatShareKind,
    val conversation: String,
    val title: String,
    val user: String = "",
    val private: Boolean = false,
) {
    val guestProbe: String get() = if (kind == ChatShareKind.DIRECT || kind == ChatShareKind.USER) "@$user" else conversation
}

object ChatShareTargets {
    const val MIN_SEARCH = 2

    fun build(
        channels: List<String>,
        offices: List<String>,
        directs: List<DirectConversation>,
        people: List<MemberGroup>,
        privateChannels: Set<String> = emptySet(),
    ): List<ChatShareTarget> {
        val out = ArrayList<ChatShareTarget>()
        for (channel in channels) {
            if (!channel.startsWith("#") || OfficeFrames.isOffice(channel)) continue
            out.add(ChatShareTarget(ChatShareKind.CHANNEL, channel, channel, private = channel in privateChannels))
        }
        for (office in offices) {
            if (!OfficeFrames.isOffice(office)) continue
            out.add(ChatShareTarget(ChatShareKind.OFFICE, office, "#" + office.removePrefix(OfficeFrames.PREFIX)))
        }
        val known = HashSet<String>()
        for (record in directs) {
            val name = record.otherUser.trim()
            if (name.isEmpty() || !known.add(name.lowercase())) continue
            out.add(ChatShareTarget(ChatShareKind.DIRECT, record.conversation, name, user = name))
        }
        for (group in people) {
            if (group.self || !group.online || group.members.isEmpty()) continue
            if (ChatVisitorPresence.isTransientVisitor(name = group.name) || group.name.trim().lowercase() in known) continue
            out.add(ChatShareTarget(ChatShareKind.PERSON, "@${group.id}", group.name))
        }
        return out.distinctBy { it.conversation }
    }

    fun users(found: List<String>, targets: List<ChatShareTarget>): List<ChatShareTarget> {
        val known = targets.filter { it.kind == ChatShareKind.DIRECT }.map { it.user.lowercase() }.toSet()
        return found.map { it.trim() }
            .filter { it.isNotEmpty() && it.lowercase() !in known }
            .distinctBy { it.lowercase() }
            .map { ChatShareTarget(ChatShareKind.USER, "", it, user = it) }
    }

    private fun key(text: String): String = text.trim().trimStart('#', '@').lowercase()

    fun filter(targets: List<ChatShareTarget>, query: String): List<ChatShareTarget> {
        val needle = key(query)
        if (needle.isEmpty()) return targets
        return targets.filter { key(it.title).contains(needle) }.sortedBy { if (key(it.title).startsWith(needle)) 0 else 1 }
    }

    fun searchable(query: String): Boolean = key(query).length >= MIN_SEARCH
}
