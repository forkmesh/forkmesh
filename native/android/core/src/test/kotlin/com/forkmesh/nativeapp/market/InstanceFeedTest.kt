package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class InstanceFeedTest {
    private val first = "0123456789abcdef0123456789abcdef"
    private val second = "fedcba9876543210fedcba9876543210"

    private data class Call(val method: String, val path: String, val query: List<Pair<String, String>>, val body: Map<String, Any?>?)

    private fun post(id: String, author: String, extra: Map<String, Any?> = emptyMap()): Map<String, Any?> = linkedMapOf<String, Any?>(
        "type" to "post", "id" to id, "author" to author, "authorName" to author.replaceFirstChar { it.uppercase() },
        "text" to "hello", "createdAt" to 1_789_561_860_000L, "replies" to 2, "boosts" to 1, "favourites" to 3,
    ).apply { putAll(extra) }

    private fun recording(reply: (Call) -> ApiJsonResult): Pair<PackagesClient, MutableList<Call>> {
        val calls = mutableListOf<Call>()
        val client = PackagesClient { method, path, query, body -> Call(method, path, query, body).also { calls += it }.let(reply) }
        return client to calls
    }

    fun testParsesTimelineWithBoostsAndProfile() {
        val (client, calls) = recording {
            ApiJsonResult(200, mapOf(
                "ok" to true, "installed" to true, "actor" to "alice", "admin" to false, "maxLength" to 500,
                "next" to "1789561860000-$first",
                "posts" to listOf(
                    post(first, "ada", mapOf("favourited" to true, "spoiler" to "cw", "canDelete" to true)),
                    mapOf("type" to "boost", "id" to second, "booster" to "grace", "boosterName" to "Grace",
                          "createdAt" to 5L, "post" to post(second, "lin", mapOf("boosted" to true, "replyTo" to first, "replyToAuthor" to "ada"))),
                    mapOf("type" to "post", "id" to "nope"),
                ),
                "profile" to mapOf("name" to "ada", "displayName" to "Ada", "posts" to 7, "since" to 9L),
            ))
        }
        val timeline = InstanceFeed.timeline(client, author = "@Ada", before = "1789561860000-$first")
        assertEq(Call("GET", "/api/instance-feed", listOf("author" to "ada", "before" to "1789561860000-$first"), null), calls.single())
        expect(timeline.installed)
        assertEq(2, timeline.entries.size)
        val head = timeline.entries[0]
        expect(!head.isBoost && head.post.favourited && head.post.canDelete)
        assertEq("cw", head.post.spoiler)
        assertEq(3, head.post.favourites)
        val boost = timeline.entries[1]
        expect(boost.isBoost && boost.post.boosted)
        assertEq("Grace", boost.boosterName)
        assertEq("ada", boost.post.replyToAuthor)
        assertEq(FeedProfile("ada", "Ada", 7, 9L), timeline.profile)
        assertEq("1789561860000-$first", timeline.next)
    }

    fun testNotInstalledTimelineAndInstallFirstWrites() {
        val (client, _) = recording { call ->
            if (call.method == "GET") ApiJsonResult(200, mapOf("ok" to true, "installed" to false, "actor" to "alice"))
            else ApiJsonResult(409, mapOf("error" to "plugin_not_installed", "canInstall" to true))
        }
        expect(!InstanceFeed.timeline(client).installed)
        val error = assertFails<PackagesException> { InstanceFeed.post(client, "hi") }
        assertEq("install_first", error.code)
    }

    fun testQueryDropsBadCursorsAndRefusesBadAuthors() {
        assertEq(emptyList<Pair<String, String>>(), InstanceFeed.timelineQuery(null, "drop table"))
        assertEq(listOf("author" to "lin"), InstanceFeed.timelineQuery(" lin ", ""))
        assertFails<PackagesException> { InstanceFeed.timelineQuery("bad name", null) }
    }

    fun testWritesUseTheRightMethodsPathsAndBodies() {
        val (client, calls) = recording { call ->
            when {
                call.method == "DELETE" && call.path.endsWith(first) -> ApiJsonResult(200, mapOf("ok" to true, "deleted" to true))
                call.path.endsWith("/posts/$first") -> ApiJsonResult(200, mapOf("ok" to true, "post" to post(first, "ada"),
                    "ancestors" to listOf(post(second, "lin")), "replies" to emptyList<Any>()))
                else -> ApiJsonResult(200, mapOf("ok" to true, "post" to post(first, "alice", mapOf("favourited" to true))))
            }
        }
        val created = InstanceFeed.post(client, "  Hello instance  ", spoiler = " cw ", replyTo = second)
        assertEq(first, created.id)
        assertEq(Call("POST", "/api/instance-feed/posts", emptyList(), mapOf("text" to "Hello instance", "spoiler" to "cw", "replyTo" to second)), calls.last())
        InstanceFeed.favourite(client, first, true)
        assertEq("POST" to "/api/instance-feed/posts/$first/favourite", calls.last().method to calls.last().path)
        InstanceFeed.favourite(client, first, false)
        assertEq("DELETE" to "/api/instance-feed/posts/$first/favourite", calls.last().method to calls.last().path)
        InstanceFeed.boost(client, first, true)
        assertEq("POST" to "/api/instance-feed/posts/$first/boost", calls.last().method to calls.last().path)
        val thread = InstanceFeed.thread(client, first)
        assertEq(listOf(second, first), thread.posts.map { it.id })
        InstanceFeed.delete(client, first)
        assertEq("DELETE" to "/api/instance-feed/posts/$first", calls.last().method to calls.last().path)
        val count = calls.size
        assertFails<PackagesException> { InstanceFeed.delete(client, "../x") }
        assertFails<PackagesException> { InstanceFeed.post(client, "   ") }
        assertFails<PackagesException> { InstanceFeed.post(client, "x".repeat(501)) }
        assertFails<PackagesException> { InstanceFeed.post(client, "hi", replyTo = "nope") }
        assertEq(count, calls.size)
    }

    fun testSpansFindLinksAndMentions() {
        assertEq(
            listOf(
                FeedSpan.Text("hi "), FeedSpan.Mention("ada", "@Ada"), FeedSpan.Text(", see "),
                FeedSpan.Link("https://forkmesh.com/x"), FeedSpan.Text(". mail me@bob.dev"),
            ),
            InstanceFeed.spans("hi @Ada, see https://forkmesh.com/x. mail me@bob.dev"),
        )
        assertEq(listOf(FeedSpan.Mention("lin", "@lin")), InstanceFeed.spans("@lin"))
        assertEq(listOf(FeedSpan.Text("plain")), InstanceFeed.spans("plain"))
    }
}
