package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.account.projects.ProjectDefaults
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str

data class FeedPost(
    val id: String,
    val author: String,
    val authorName: String,
    val text: String,
    val spoiler: String = "",
    val createdAt: Long = 0,
    val replyTo: String = "",
    val replyToAuthor: String = "",
    val replies: Int = 0,
    val boosts: Int = 0,
    val favourites: Int = 0,
    val boosted: Boolean = false,
    val favourited: Boolean = false,
    val mine: Boolean = false,
    val canDelete: Boolean = false,
) {
    val displayName: String get() = authorName.ifEmpty { author }

    companion object {
        fun fromJson(json: JsonMap): FeedPost? {
            val id = json.str("id")
            if (!PackagesText.isHexId(id)) return null
            return FeedPost(
                id, json.str("author"), json.str("authorName"), json.str("text"), json.str("spoiler"),
                json.long("createdAt"), json.str("replyTo"), json.str("replyToAuthor"),
                json.int("replies").coerceAtLeast(0), json.int("boosts").coerceAtLeast(0),
                json.int("favourites").coerceAtLeast(0), json.bool("boosted"), json.bool("favourited"),
                json.bool("mine"), json.bool("canDelete"),
            )
        }
    }
}

data class FeedEntry(val id: String, val post: FeedPost, val booster: String = "", val boosterName: String = "", val boostedAt: Long = 0) {
    val isBoost: Boolean get() = booster.isNotEmpty()

    companion object {
        fun fromJson(json: JsonMap): FeedEntry? {
            if (json.str("type") == "boost") {
                val post = FeedPost.fromJson(json.obj("post")) ?: return null
                val booster = json.str("booster")
                if (booster.isEmpty()) return null
                return FeedEntry(json.str("id"), post, booster, json.str("boosterName").ifEmpty { booster }, json.long("createdAt"))
            }
            val post = FeedPost.fromJson(json) ?: return null
            return FeedEntry(post.id, post)
        }
    }
}

data class FeedProfile(val name: String, val displayName: String, val posts: Int = 0, val since: Long = 0) {
    companion object {
        fun fromJson(json: JsonMap): FeedProfile? {
            val name = json.str("name")
            if (name.isEmpty()) return null
            return FeedProfile(name, json.str("displayName").ifEmpty { name }, json.int("posts").coerceAtLeast(0), json.long("since"))
        }
    }
}

data class FeedTimeline(
    val installed: Boolean,
    val actor: String = "",
    val admin: Boolean = false,
    val maxLength: Int = InstanceFeed.MAX_LENGTH,
    val entries: List<FeedEntry> = emptyList(),
    val next: String = "",
    val profile: FeedProfile? = null,
) {
    companion object {
        fun fromJson(json: JsonMap) = FeedTimeline(
            json["installed"] != false,
            json.str("actor"),
            json.bool("admin"),
            json.int("maxLength", InstanceFeed.MAX_LENGTH).takeIf { it > 0 } ?: InstanceFeed.MAX_LENGTH,
            json.maps("posts").mapNotNull(FeedEntry::fromJson),
            json.str("next"),
            FeedProfile.fromJson(json.obj("profile")),
        )
    }
}

data class FeedThread(val post: FeedPost?, val ancestors: List<FeedPost> = emptyList(), val replies: List<FeedPost> = emptyList()) {
    val posts: List<FeedPost> get() = ancestors + listOfNotNull(post) + replies

    companion object {
        fun fromJson(json: JsonMap) = FeedThread(
            FeedPost.fromJson(json.obj("post")),
            json.maps("ancestors").mapNotNull(FeedPost::fromJson),
            json.maps("replies").mapNotNull(FeedPost::fromJson),
        )
    }
}

sealed class FeedSpan {
    data class Text(val text: String) : FeedSpan()
    data class Link(val url: String) : FeedSpan()
    data class Mention(val name: String, val text: String) : FeedSpan()
}

object InstanceFeed {
    const val PACKAGE_ID = "instance-feed"
    const val API = "/api/instance-feed"
    const val MAX_LENGTH = 500
    const val MAX_SPOILER = 100
    const val PUBLIC_SITE = "https://forkmesh.com"
    private val OFFICIAL_HOSTS = ProjectDefaults.PROD_HOST_ALIASES + setOf("www.forkmesh.com", "world.forkmesh.com")

    private val AUTHOR = Regex("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
    private val CURSOR = Regex("^\\d{1,15}-[0-9a-f]{32}$")
    private val SPANS = Regex("(https://[^\\s<>\"']+[^\\s<>\"'.,;:!?)\\]])|(^|[^A-Za-z0-9_@/])@([A-Za-z](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)\\b")

    fun isAuthor(value: String): Boolean = AUTHOR.matches(value)

    fun siteOrigin(origin: String): String {
        val trimmed = origin.trim().trimEnd('/')
        val host = runCatching { java.net.URI(trimmed).host }.getOrNull()?.lowercase().orEmpty()
        if (host.isEmpty() || host in OFFICIAL_HOSTS || ProjectDefaults.isNextHost(host)) return PUBLIC_SITE
        return trimmed
    }

    fun publicUrl(postId: String, origin: String): String? =
        if (PackagesText.isHexId(postId)) "${siteOrigin(origin)}/feed?post=$postId" else null

    fun timelineQuery(author: String?, before: String?): List<Pair<String, String>> = buildList {
        val name = author?.trim()?.removePrefix("@")?.lowercase().orEmpty()
        if (name.isNotEmpty()) {
            if (!isAuthor(name)) throw PackagesException("account_not_found", 404)
            add("author" to name)
        }
        val cursor = before?.trim().orEmpty()
        if (cursor.isNotEmpty() && CURSOR.matches(cursor)) add("before" to cursor)
    }

    fun timeline(client: PackagesClient, author: String? = null, before: String? = null): FeedTimeline =
        FeedTimeline.fromJson(guard { client.call("GET", API, timelineQuery(author, before)) })

    fun thread(client: PackagesClient, id: String): FeedThread =
        FeedThread.fromJson(guard { client.call("GET", "$API/posts/${client.item(id)}") })

    fun post(client: PackagesClient, text: String, spoiler: String = "", replyTo: String? = null): FeedPost {
        val body = text.trim()
        if (body.isEmpty()) throw PackagesException("text_required", 400)
        if (body.length > MAX_LENGTH) throw PackagesException("text_too_long", 400)
        val parent = replyTo?.takeIf { it.isNotEmpty() }?.let(client::item).orEmpty()
        val reply = guard {
            client.call("POST", "$API/posts", body = linkedMapOf("text" to body, "spoiler" to spoiler.trim().take(MAX_SPOILER), "replyTo" to parent))
        }
        return FeedPost.fromJson(reply.obj("post")) ?: throw PackagesException("post_not_found", 404)
    }

    fun favourite(client: PackagesClient, id: String, on: Boolean): FeedPost = toggle(client, id, "favourite", on)

    fun boost(client: PackagesClient, id: String, on: Boolean): FeedPost = toggle(client, id, "boost", on)

    fun delete(client: PackagesClient, id: String) {
        guard { client.call("DELETE", "$API/posts/${client.item(id)}") }
    }

    fun spans(text: String): List<FeedSpan> {
        val out = mutableListOf<FeedSpan>()
        var last = 0
        for (match in SPANS.findAll(text)) {
            val link = match.groups[1]?.value
            val lead = match.groups[2]?.value.orEmpty()
            val start = match.range.first
            val plain = text.substring(last, start) + if (link == null) lead else ""
            if (plain.isNotEmpty()) out += FeedSpan.Text(plain)
            if (link != null) out += FeedSpan.Link(link)
            else {
                val name = match.groups[3]?.value.orEmpty()
                out += FeedSpan.Mention(name.lowercase(), "@$name")
            }
            last = match.range.last + 1
        }
        if (last < text.length) out += FeedSpan.Text(text.substring(last))
        return out
    }

    private fun toggle(client: PackagesClient, id: String, kind: String, on: Boolean): FeedPost {
        val reply = guard { client.call(if (on) "POST" else "DELETE", "$API/posts/${client.item(id)}/$kind", body = if (on) emptyMap() else null) }
        return FeedPost.fromJson(reply.obj("post")) ?: throw PackagesException("post_not_found", 404)
    }

    private fun <T> guard(block: () -> T): T = try {
        block()
    } catch (error: PackagesException) {
        if (error.code == "plugin_not_installed") throw PackagesException("install_first", error.status)
        throw error
    }
}
