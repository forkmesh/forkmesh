package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.crypto.TrustState
import com.forkmesh.nativeapp.model.MemberGroup

enum class ChatPeerPresence(val label: String) {
    ONLINE("Online"),
    AWAY("Away"),
    OFFLINE("Offline"),
}

fun ChatHub.peerGroup(conversation: String): MemberGroup? {
    val groups = relay.rosterGroups().filter { !it.self }
    if (conversation.startsWith("@")) {
        val id = conversation.drop(1)
        return groups.firstOrNull { group -> group.members.any { it.id == id } }
    }
    val service = direct ?: return null
    if (!DirectMessageFrames.isDirect(conversation)) return null
    val other = service.record(conversation)?.otherUser.orEmpty().lowercase()
    val peers = service.devices(conversation).filter { !it.ownAccount && it.state != TrustState.BLOCKED }
    val nodes = peers.map { it.node }.filter { it.isNotEmpty() }.toSet()
    val accounts = (peers.map { it.acct.lowercase() }.filter { it.isNotEmpty() } + listOfNotNull(other.ifEmpty { null })).toSet()
    val matches = groups.filter { group ->
        group.members.any { it.id in nodes || it.owner.lowercase() in accounts }
    }
    matches.firstOrNull { it.online }?.let { return it }
    matches.firstOrNull()?.let { return it }
    if (other.isEmpty()) return null
    return groups.firstOrNull { it.name.lowercase() == other }
}

fun ChatHub.peerActive(conversation: String): Boolean {
    if (extras.typingNames(conversation).isNotEmpty()) return true
    val engine = live
    if (engine != null && (engine.partnerMotion(conversation) != null || engine.taps(conversation).any { !it.isSelf })) return true
    return DirectMessageFrames.isDirect(conversation) && direct?.routeLabel(conversation) == DirectMessageFrames.ROUTE_DIRECT_ONLY
}

fun ChatHub.peerPresence(conversation: String, nowMs: Long = System.currentTimeMillis()): ChatPeerPresence {
    val group = peerGroup(conversation)
    val online = if (conversation.startsWith("@")) {
        val id = conversation.drop(1)
        relay.roster().firstOrNull { it.id == id }?.online == true || group?.online == true
    } else {
        group?.online == true
    }
    if (online) {
        val away = group?.members?.firstNotNullOfOrNull { it.userStatus }?.visible(nowMs)?.away == true
        return if (away) ChatPeerPresence.AWAY else ChatPeerPresence.ONLINE
    }
    return if (peerActive(conversation)) ChatPeerPresence.ONLINE else ChatPeerPresence.OFFLINE
}

fun ChatHub.peerOnline(conversation: String): Boolean =
    !isGroupConversation(conversation) && peerPresence(conversation) == ChatPeerPresence.ONLINE
