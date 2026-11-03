package com.forkmesh.nativeapp.mastodon

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.jsonOf
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import java.time.Instant
import java.time.OffsetDateTime

private fun Map<String, Any?>.firstText(vararg keys: String): String =
    keys.map { str(it) }.firstOrNull { it.isNotEmpty() }.orEmpty()

internal fun mastodonTime(value: String): Long {
    if (value.isEmpty()) return 0L
    return runCatching { Instant.parse(value).toEpochMilli() }.getOrNull()
        ?: runCatching { OffsetDateTime.parse(value).toInstant().toEpochMilli() }.getOrDefault(0L)
}

data class MastodonAccount(
    val id: String,
    val username: String,
    val acct: String,
    val displayName: String,
    val avatar: String = "",
    val header: String = "",
    val url: String = "",
    val note: String = "",
    val locked: Boolean = false,
    val bot: Boolean = false,
    val followersCount: Int = 0,
    val followingCount: Int = 0,
    val statusesCount: Int = 0,
) {
    companion object {
        fun from(json: Map<String, Any?>): MastodonAccount? {
            val id = json.str("id").ifEmpty { return null }
            val username = json.str("username")
            return MastodonAccount(
                id = id,
                username = username,
                acct = json.str("acct").ifEmpty { username },
                displayName = json.str("display_name").ifBlank { username },
                avatar = MastodonServer.webLink(json.firstText("avatar_static", "avatar")),
                header = MastodonServer.webLink(json.firstText("header_static", "header")),
                url = MastodonServer.webLink(json.str("url")),
                note = json.str("note"),
                locked = json.bool("locked"),
                bot = json.bool("bot"),
                followersCount = json.int("followers_count"),
                followingCount = json.int("following_count"),
                statusesCount = json.int("statuses_count"),
            )
        }
    }
}

data class MastodonAttachment(val id: String, val type: String, val url: String, val previewUrl: String, val description: String) {
    companion object {
        fun from(json: Map<String, Any?>): MastodonAttachment? {
            val id = json.str("id").ifEmpty { return null }
            return MastodonAttachment(
                id,
                json.str("type"),
                MastodonServer.webLink(json.str("url")),
                MastodonServer.webLink(json.str("preview_url")),
                json.str("description"),
            )
        }
    }
}

data class MastodonMention(val id: String, val acct: String, val url: String) {
    companion object {
        fun from(json: Map<String, Any?>): MastodonMention? {
            val id = json.str("id").ifEmpty { return null }
            return MastodonMention(id, json.str("acct").ifEmpty { json.str("username") }, json.str("url"))
        }
    }
}

data class MastodonTag(val name: String, val url: String) {
    companion object {
        fun from(json: Map<String, Any?>): MastodonTag? {
            val name = json.str("name").ifEmpty { return null }
            return MastodonTag(name, json.str("url"))
        }
    }
}

data class MastodonPollOption(val title: String, val votesCount: Int)

data class MastodonPoll(val options: List<MastodonPollOption>, val votesCount: Int, val expired: Boolean) {
    fun percent(option: MastodonPollOption): Int =
        if (votesCount <= 0) 0 else Math.round(option.votesCount * 100.0 / votesCount).toInt()

    companion object {
        fun from(json: Map<String, Any?>): MastodonPoll? {
            if (json.isEmpty()) return null
            val options = json.maps("options").map { MastodonPollOption(it.str("title"), it.int("votes_count")) }
            if (options.isEmpty()) return null
            return MastodonPoll(options, json.int("votes_count"), json.bool("expired"))
        }
    }
}

data class MastodonStatus(
    val id: String,
    val url: String,
    val account: MastodonAccount,
    val contentHtml: String,
    val spoilerText: String = "",
    val visibility: String = "public",
    val sensitive: Boolean = false,
    val createdAt: Long = 0L,
    val repliesCount: Int = 0,
    val reblogsCount: Int = 0,
    val favouritesCount: Int = 0,
    val favourited: Boolean = false,
    val reblogged: Boolean = false,
    val bookmarked: Boolean = false,
    val inReplyToId: String = "",
    val media: List<MastodonAttachment> = emptyList(),
    val mentions: List<MastodonMention> = emptyList(),
    val tags: List<MastodonTag> = emptyList(),
    val poll: MastodonPoll? = null,
    val reblog: MastodonStatus? = null,
) {
    val shown: MastodonStatus get() = reblog ?: this

    val segments: List<MastodonSegment> by lazy { MastodonHtml.segments(contentHtml, mentions, tags) }

    companion object {
        fun from(json: Map<String, Any?>): MastodonStatus? {
            val id = json.str("id").ifEmpty { return null }
            val account = MastodonAccount.from(json.obj("account")) ?: return null
            return MastodonStatus(
                id = id,
                url = MastodonServer.webLink(json.firstText("url", "uri")),
                account = account,
                contentHtml = json.str("content"),
                spoilerText = json.str("spoiler_text"),
                visibility = json.str("visibility").ifEmpty { "public" },
                sensitive = json.bool("sensitive"),
                createdAt = mastodonTime(json.str("created_at")),
                repliesCount = json.int("replies_count"),
                reblogsCount = json.int("reblogs_count"),
                favouritesCount = json.int("favourites_count"),
                favourited = json.bool("favourited"),
                reblogged = json.bool("reblogged"),
                bookmarked = json.bool("bookmarked"),
                inReplyToId = json.str("in_reply_to_id"),
                media = json.maps("media_attachments").mapNotNull(MastodonAttachment::from),
                mentions = json.maps("mentions").mapNotNull(MastodonMention::from),
                tags = json.maps("tags").mapNotNull(MastodonTag::from),
                poll = MastodonPoll.from(json.obj("poll")),
                reblog = json.obj("reblog").takeIf { it.isNotEmpty() }?.let(::from),
            )
        }

        fun list(value: Any?): List<MastodonStatus> = jsonMaps(value).mapNotNull(::from)
    }
}

data class MastodonNotification(
    val id: String,
    val type: String,
    val createdAt: Long,
    val account: MastodonAccount?,
    val status: MastodonStatus?,
) {
    val sentence: String
        get() {
            val name = account?.displayName.orEmpty().ifEmpty { "Someone" }
            return when (type) {
                "favourite" -> "$name favourited your post"
                "reblog" -> "$name boosted your post"
                "follow" -> "$name followed you"
                "follow_request" -> "$name requested to follow you"
                "mention" -> "$name mentioned you"
                "status" -> "$name posted"
                "update" -> "$name edited a post"
                "poll" -> "A poll you voted in has ended"
                else -> "$name: $type"
            }
        }

    companion object {
        fun from(json: Map<String, Any?>): MastodonNotification? {
            val id = json.str("id").ifEmpty { return null }
            return MastodonNotification(
                id,
                json.str("type"),
                mastodonTime(json.str("created_at")),
                MastodonAccount.from(json.obj("account")),
                json.obj("status").takeIf { it.isNotEmpty() }?.let(MastodonStatus::from),
            )
        }
    }
}

data class MastodonRelationship(
    val id: String,
    val following: Boolean = false,
    val followedBy: Boolean = false,
    val requested: Boolean = false,
    val blocking: Boolean = false,
    val muting: Boolean = false,
) {
    companion object {
        fun from(json: Map<String, Any?>): MastodonRelationship? {
            val id = json.str("id").ifEmpty { return null }
            return MastodonRelationship(
                id, json.bool("following"), json.bool("followed_by"), json.bool("requested"), json.bool("blocking"), json.bool("muting"),
            )
        }
    }
}

data class MastodonInstanceLimits(val maxCharacters: Int = Mastodon.DEFAULT_MAX_CHARACTERS, val maxMedia: Int = Mastodon.DEFAULT_MAX_MEDIA) {
    companion object {
        fun from(json: Map<String, Any?>): MastodonInstanceLimits {
            val statuses = json.obj("configuration").obj("statuses")
            return MastodonInstanceLimits(
                statuses.int("max_characters", Mastodon.DEFAULT_MAX_CHARACTERS).takeIf { it > 0 } ?: Mastodon.DEFAULT_MAX_CHARACTERS,
                statuses.int("max_media_attachments", Mastodon.DEFAULT_MAX_MEDIA).takeIf { it > 0 } ?: Mastodon.DEFAULT_MAX_MEDIA,
            )
        }
    }
}

data class MastodonThread(val ancestors: List<MastodonStatus>, val status: MastodonStatus, val descendants: List<MastodonStatus>) {
    val all: List<MastodonStatus> get() = ancestors + status + descendants
}

data class MastodonPage<out T>(val items: List<T>, val next: String?)

data class MastodonSearch(val accounts: List<MastodonAccount>, val hashtags: List<MastodonTag>, val statuses: List<MastodonStatus>)

data class MastodonApp(val clientId: String, val clientSecret: String)

data class MastodonSession(
    val server: String,
    val token: String,
    val clientId: String = "",
    val clientSecret: String = "",
    val accountId: String = "",
    val acct: String = "",
    val displayName: String = "",
    val avatar: String = "",
) {
    val handle: String get() = "@$acct@${MastodonServer.host(server)}"

    fun withAccount(account: MastodonAccount) =
        copy(accountId = account.id, acct = account.acct, displayName = account.displayName, avatar = account.avatar)

    fun toJson(): String = Json.stringify(
        jsonOf(
            "server" to server, "token" to token, "clientId" to clientId, "clientSecret" to clientSecret,
            "accountId" to accountId, "acct" to acct, "displayName" to displayName, "avatar" to avatar,
        ),
    )

    companion object {
        fun parse(text: String?): MastodonSession? {
            if (text.isNullOrEmpty()) return null
            val json = runCatching { Json.parseObject(text) }.getOrNull() ?: return null
            val server = MastodonServer.normalize(json.str("server")) ?: return null
            val token = json.str("token").ifEmpty { return null }
            return MastodonSession(
                server, token, json.str("clientId"), json.str("clientSecret"),
                json.str("accountId"), json.str("acct"), json.str("displayName"), json.str("avatar"),
            )
        }
    }
}

data class MastodonPending(
    val server: String,
    val clientId: String,
    val clientSecret: String,
    val state: String,
    val verifier: String,
    val redirect: String,
) {
    fun toJson(): String = Json.stringify(
        jsonOf(
            "server" to server, "clientId" to clientId, "clientSecret" to clientSecret,
            "state" to state, "verifier" to verifier, "redirect" to redirect,
        ),
    )

    companion object {
        fun parse(text: String?): MastodonPending? {
            if (text.isNullOrEmpty()) return null
            val json = runCatching { Json.parseObject(text) }.getOrNull() ?: return null
            val server = MastodonServer.normalize(json.str("server")) ?: return null
            val clientId = json.str("clientId").ifEmpty { return null }
            val state = json.str("state").ifEmpty { return null }
            return MastodonPending(server, clientId, json.str("clientSecret"), state, json.str("verifier"), json.str("redirect").ifEmpty { Mastodon.REDIRECT })
        }
    }
}
