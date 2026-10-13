package com.forkmesh.nativeapp.settings

data class AccountProfile(
    val name: String,
    val exists: Boolean,
    val isPrivate: Boolean,
    val kind: String,
    val status: String,
    val online: Boolean,
    val owner: String,
    val nodes: List<String>,
    val solana: String,
    val pubkey: String,
    val createdAt: Long,
    val isAdmin: Boolean,
    val bio: String,
    val location: String,
    val followers: Int,
    val following: Int,
    val mirrorCount: Int,
) {
    val isNode: Boolean get() = kind == "node"

    val kindLabel: String
        get() = if (isNode) (if (online) "Node · online" else "Node · offline") else if (isAdmin) "User · admin" else "User"

    val accountStatus: String
        get() = when (status) {
            "active" -> "Active"
            "pending" -> "Pending email verification"
            "" -> "Unknown"
            else -> status.take(1).uppercase() + status.drop(1)
        }

    val linkedNodesLine: String
        get() {
            if (isNode) return if (owner.isEmpty()) "Not linked to a user" else "Linked to @$owner"
            if (nodes.isEmpty()) return "No nodes linked"
            return "${nodes.size} node${if (nodes.size == 1) "" else "s"} linked"
        }

    fun canLinkNode(viewer: String): Boolean = !isNode && viewer.isNotEmpty() && viewer.lowercase() == name.lowercase()

    val identityQrText: String get() = if (pubkey.isEmpty()) "" else "forkmesh:identity/${name.lowercase()}/$pubkey"

    val walletQrText: String get() = if (solana.isEmpty()) "" else "solana:$solana"

    companion object {
        fun parse(map: Map<String, Any?>, requested: String): AccountProfile = AccountProfile(
            name = map.text("name").ifEmpty { requested },
            exists = map["exists"] != false,
            isPrivate = map.flag("profilePrivate") && map["kind"] == null,
            kind = map.text("kind"),
            status = map.text("status"),
            online = map.flag("online"),
            owner = map.text("owner"),
            nodes = map.strings("nodes").filter { NodeLink.isNodeName(it.lowercase()) }.take(200),
            solana = map.text("solana"),
            pubkey = map.text("pubkey"),
            createdAt = map.long("createdAt"),
            isAdmin = map.flag("isAdmin"),
            bio = map.text("profileBio").take(500),
            location = map.text("profileLocation").take(120),
            followers = maxOf(0, map.int("followers")),
            following = maxOf(0, map.int("following")),
            mirrorCount = maxOf(0, map.int("mirrorCount")),
        )
    }
}

object SolanaAddress {
    private const val ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"

    fun isValid(address: String): Boolean = address.length in 32..44 && address.all { it in ALPHABET }

    fun short(address: String): String = if (address.length <= 12) address else "${address.take(6)}…${address.takeLast(4)}"
}

class AccountProfileModel(name: String, private val client: AccountEndpointCalling) {
    val name: String = name.trim().trim('@')
    @Volatile var profile: AccountProfile? = null
        private set
    @Volatile var loading = false
        private set
    @Volatile var error = ""
        private set

    val validName: Boolean get() = NodeLink.isNodeName(name.lowercase())

    fun load() {
        if (!validName) {
            error = "That is not a ForkMesh name."
            return
        }
        loading = true
        try {
            profile = AccountProfile.parse(client.accountCall("GET", "/api/accounts/${name.lowercase()}", null), name)
            error = ""
        } catch (failure: Exception) {
            error = AccountErrorText.text(failure, "Could not load @$name.")
        } finally {
            loading = false
        }
    }
}
