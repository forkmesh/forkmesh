package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.json.jsonText
import java.security.SecureRandom
import java.util.Base64

object GroupCallFrames {
    const val PRESENCE_TYPE = "group-call-presence"
    const val KEY_TYPE = "group-call-key"
    val TYPES = setOf(PRESENCE_TYPE, KEY_TYPE)
    const val HEARTBEAT_MS = 20_000L
    const val PRESENCE_TTL_MS = 60_000L
    const val ROTATE_DELAY_MS = 1_000L
    const val PING_EVERY_MS = 20_000L
    const val KEY_BYTES = 16

    fun validPeer(peer: String): Boolean = peer.length == 32 && peer.all { it in '0'..'9' || it in 'a'..'f' }

    fun presence(peer: String, state: String, media: String = ""): Map<String, Any?> {
        val fields = linkedMapOf<String, Any?>("peer" to peer, "call" to state)
        if (media.isNotEmpty()) fields["media"] = media
        return fields
    }

    fun key(peer: String, keyId: Int, raw: ByteArray): Map<String, Any?> =
        linkedMapOf("peer" to peer, "keyId" to keyId, "key" to Base64.getEncoder().encodeToString(raw))
}

data class GroupCallTicket(val ticket: String, val url: String, val peer: String, val room: String) {
    companion object {
        fun from(json: Map<String, Any?>): GroupCallTicket? {
            val ticket = jsonText(json["ticket"])
            val url = jsonText(json["url"])
            val peer = jsonText(json["peer"])
            if (ticket.isEmpty() || !url.startsWith("wss://") || !GroupCallFrames.validPeer(peer)) return null
            return GroupCallTicket(ticket, url, peer, jsonText(json["room"]))
        }
    }
}

data class GroupCallMember(val peer: String, val name: String)

class GroupCallKey(val keyId: Int, val raw: ByteArray, val name: String) {
    override fun equals(other: Any?): Boolean =
        other is GroupCallKey && other.keyId == keyId && other.raw.contentEquals(raw) && other.name == name

    override fun hashCode(): Int = (keyId * 31 + raw.contentHashCode()) * 31 + name.hashCode()
}

sealed class GroupCallFrameEvent {
    data class Joined(val peer: String, val isNew: Boolean) : GroupCallFrameEvent()
    data class Left(val peer: String) : GroupCallFrameEvent()
    data class Key(val peer: String, val key: GroupCallKey) : GroupCallFrameEvent()
}

class GroupCallRoster(private val now: () -> Long = System::currentTimeMillis) {
    private class Seen(val name: String, val at: Long)

    private val lock = Any()
    private val presence = HashMap<String, HashMap<String, Seen>>()
    private val owners = HashMap<String, HashMap<String, Seen>>()
    private val keyring = HashMap<String, HashMap<String, GroupCallKey>>()

    fun handle(plain: Map<String, Any?>, conversation: String, selfPeer: String): GroupCallFrameEvent? {
        val type = jsonText(plain["type"])
        if (type !in GroupCallFrames.TYPES) return null
        val peer = jsonText(plain["peer"])
        if (!GroupCallFrames.validPeer(peer) || peer == selfPeer) return null
        val name = jsonText(plain["sender"]).trim().lowercase()
        if (name.isEmpty()) return null
        synchronized(lock) {
            if (type == GroupCallFrames.PRESENCE_TYPE) {
                if (!claim(conversation, peer, name)) return null
                val out = jsonText(plain["call"]) == "out"
                val entries = presence.getOrPut(conversation) { HashMap() }
                val known = entries.containsKey(peer)
                if (out) {
                    entries.remove(peer)
                    owners[conversation]?.remove(peer)
                    keyring[conversation]?.remove(peer)
                } else {
                    entries[peer] = Seen(name, now())
                }
                return if (out) GroupCallFrameEvent.Left(peer) else GroupCallFrameEvent.Joined(peer, !known)
            }
            val keyId = (plain["keyId"] as? Number)?.toDouble() ?: return null
            if (keyId != Math.floor(keyId) || keyId < 0 || keyId > 255) return null
            val raw = runCatching { Base64.getDecoder().decode(jsonText(plain["key"])) }.getOrNull() ?: return null
            if (raw.size != GroupCallFrames.KEY_BYTES) return null
            if (!claim(conversation, peer, name)) return null
            val key = GroupCallKey(keyId.toInt(), raw, name)
            keyring.getOrPut(conversation) { HashMap() }[peer] = key
            return GroupCallFrameEvent.Key(peer, key)
        }
    }

    private fun claim(conversation: String, peer: String, name: String): Boolean {
        val at = now()
        val owner = owners[conversation]?.get(peer)
        if (owner != null && at - owner.at <= GroupCallFrames.PRESENCE_TTL_MS && owner.name != name) return false
        owners.getOrPut(conversation) { HashMap() }[peer] = Seen(name, at)
        return true
    }

    fun roster(conversation: String): List<GroupCallMember> = synchronized(lock) {
        val cutoff = now() - GroupCallFrames.PRESENCE_TTL_MS
        val entries = presence[conversation] ?: return emptyList()
        entries.entries.removeIf { it.value.at < cutoff }
        entries.map { GroupCallMember(it.key, it.value.name) }.sortedBy { it.name }
    }

    fun name(conversation: String, peer: String): String = synchronized(lock) { presence[conversation]?.get(peer)?.name.orEmpty() }

    fun keys(conversation: String): Map<String, GroupCallKey> = synchronized(lock) { HashMap(keyring[conversation] ?: emptyMap()) }
}

class GroupCallSendKey(private val random: () -> ByteArray = ::randomKey) {
    var keyId: Int = 0
        private set
    var raw: ByteArray = random()
        private set

    fun rotate() {
        keyId = (keyId + 1) and 0xff
        raw = random()
    }

    companion object {
        private val secure = SecureRandom()

        fun randomKey(): ByteArray = ByteArray(GroupCallFrames.KEY_BYTES).also { secure.nextBytes(it) }
    }
}
