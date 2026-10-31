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
    val avatar: String = "",
    val links: List<ProfileLink> = emptyList(),
    val mastodon: String = "",
    val mastodonUrl: String = "",
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

    val tipAddress: String get() = if (SolanaAddress.isValid(solana)) solana else ""

    val walletQrText: String get() = SolanaAddress.qrText(solana)

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
            avatar = map.text("avatarPng"),
            links = ProfileLink.parseAll(map.maps("profileLinks")),
            mastodon = map.text("mastodon").take(120),
            mastodonUrl = map.text("mastodonUrl").takeIf { ProfileLink.isWebUrl(it) }.orEmpty(),
        )
    }
}

data class ProfileLink(val label: String, val url: String, val verified: Boolean) {
    companion object {
        fun isWebUrl(url: String): Boolean = url.length <= 500 && (url.startsWith("https://") || url.startsWith("http://"))

        fun parseAll(items: List<Map<String, Any?>>): List<ProfileLink> = items.mapNotNull { item ->
            val url = item.text("url").trim()
            if (!isWebUrl(url)) return@mapNotNull null
            ProfileLink(item.text("label").trim().take(60).ifEmpty { item.text("domain").ifEmpty { url } }, url, item.flag("verified"))
        }.take(10)
    }
}

object SolanaAddress {
    private const val ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"

    fun isValid(address: String): Boolean = address.length in 32..44 && address.all { it in ALPHABET }

    fun qrText(address: String): String = if (isValid(address)) "solana:$address" else ""

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
