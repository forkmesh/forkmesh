package com.forkmesh.nativeapp.coves

import com.forkmesh.nativeapp.json.jsonLong

private fun Map<*, *>.text(key: String, fallback: String = ""): String = (this[key] as? String) ?: fallback

data class CoveDocument(
    val id: String,
    val name: String,
    val mime: String = "text/markdown",
    val body: String = "",
    val updatedAtMs: Long = 0,
) {
    fun toJson(): Map<String, Any?> =
        linkedMapOf("body" to body, "id" to id, "mime" to mime, "name" to name, "updatedAtMs" to updatedAtMs)

    companion object {
        fun fromJson(json: Map<*, *>): CoveDocument = CoveDocument(
            id = json.text("id"),
            name = json.text("name"),
            mime = json.text("mime", "text/markdown"),
            body = json.text("body"),
            updatedAtMs = jsonLong(json["updatedAtMs"]),
        )
    }
}

data class CoveAccessEntry(
    val who: String = "",
    val name: String = "",
    val node: String = "",
    val ts: Long = 0,
    val action: String = "open",
) {
    fun toJson(): Map<String, Any?> = linkedMapOf("action" to action, "name" to name, "node" to node, "ts" to ts, "who" to who)

    companion object {
        fun fromJson(json: Map<*, *>): CoveAccessEntry = CoveAccessEntry(
            who = json.text("who"),
            name = json.text("name"),
            node = json.text("node"),
            ts = jsonLong(json["ts"]),
            action = json.text("action", "open"),
        )
    }
}

object CoveAccessMode {
    const val PASSWORD = "password"
    const val ACCOUNT = "account"
}

data class Cove(
    val id: String = "",
    val name: String = "",
    val slug: String = "",
    val relPath: String = "",
    val version: Int = 1,
    val creator: String = "",
    val accessMode: String = CoveAccessMode.PASSWORD,
    val creatorAccount: String = "",
    val invitedAccounts: List<String> = emptyList(),
    val createdAtMs: Long = 0,
    val notifyOnOpen: Boolean = false,
    val salt: ByteArray = ByteArray(0),
    val rounds: Int = 0,
    val cipher: CoveCipher? = null,
    val grants: List<CoveCipher> = emptyList(),
    val unlocked: Boolean = false,
    val contentKey: ByteArray = ByteArray(0),
    val documents: List<CoveDocument> = emptyList(),
    val accessLog: List<CoveAccessEntry> = emptyList(),
) {
    val accountScoped: Boolean get() = accessMode == CoveAccessMode.ACCOUNT

    val members: List<String>
        get() {
            val people = CoveStore.normalizedAccounts(invitedAccounts).toMutableList()
            val owner = CoveStore.normalizedAccount(creatorAccount)
            if (owner.isNotEmpty() && owner !in people) people.add(owner)
            return people.sortedBy { it.lowercase() }
        }

    fun createdBy(account: String): Boolean {
        val me = CoveStore.normalizedAccount(account)
        return me.isNotEmpty() && CoveStore.normalizedAccount(creatorAccount) == me
    }

    fun document(named: String): CoveDocument? {
        val wanted = CoveStore.cleanPath(named)
        return documents.firstOrNull { CoveStore.cleanPath(it.name) == wanted }
    }

    val displayName: String
        get() {
            val trimmed = name.trim()
            if (trimmed.isNotEmpty()) return trimmed
            return if (unlocked) "Secure cove" else "Locked cove"
        }

    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is Cove) return false
        return id == other.id && name == other.name && slug == other.slug && relPath == other.relPath &&
            version == other.version && creator == other.creator && accessMode == other.accessMode &&
            creatorAccount == other.creatorAccount && invitedAccounts == other.invitedAccounts &&
            createdAtMs == other.createdAtMs && notifyOnOpen == other.notifyOnOpen && salt.contentEquals(other.salt) &&
            rounds == other.rounds && cipher == other.cipher && grants == other.grants && unlocked == other.unlocked &&
            contentKey.contentEquals(other.contentKey) && documents == other.documents && accessLog == other.accessLog
    }

    override fun hashCode(): Int = id.hashCode() * 31 + name.hashCode()
}
