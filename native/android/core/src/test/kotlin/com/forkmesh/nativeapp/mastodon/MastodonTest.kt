package com.forkmesh.nativeapp.mastodon

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.IOException

class MastodonTest {
    private val server = "https://social.example"

    private class Fake(private val reply: (HttpClient.Request) -> HttpClient.Response) : MastodonTransport {
        val requests = mutableListOf<HttpClient.Request>()

        override fun send(request: HttpClient.Request): HttpClient.Response {
            requests += request
            return reply(request)
        }
    }

    private fun response(status: Int, body: Any?, headers: Map<String, String> = emptyMap()) = HttpClient.Response(
        status,
        headers.mapKeys { it.key as String? }.mapValues { listOf(it.value) },
        (if (body is String) body else Json.stringify(body)).toByteArray(),
    )

    private fun account(id: String = "1", acct: String = "alice") = mapOf(
        "id" to id, "username" to acct.substringBefore('@'), "acct" to acct, "display_name" to "",
        "avatar" to "https://cdn.example/a.png", "avatar_static" to "https://cdn.example/a-static.png",
        "followers_count" to 3, "following_count" to 4, "statuses_count" to 5, "locked" to true,
    )

    private fun status(id: String = "100", extra: Map<String, Any?> = emptyMap()) = linkedMapOf<String, Any?>(
        "id" to id, "uri" to "$server/users/alice/statuses/$id", "url" to null, "account" to account(),
        "content" to "<p>Hello</p>", "created_at" to "2026-10-01T12:00:00.500Z", "visibility" to "unlisted",
        "replies_count" to 1, "reblogs_count" to 2, "favourites_count" to 3, "favourited" to true,
        "media_attachments" to listOf(mapOf("id" to "m1", "type" to "image", "url" to "$server/m1.png", "preview_url" to "$server/m1s.png", "description" to "A cat")),
        "mentions" to listOf(mapOf("id" to "7", "acct" to "bob@other.example", "url" to "https://other.example/@bob")),
        "tags" to listOf(mapOf("name" to "cats", "url" to "$server/tags/cats")),
    ).apply { putAll(extra) }

    fun testNormalizesServerAddresses() {
        assertEq("https://mastodon.social", MastodonServer.normalize("mastodon.social"))
        assertEq("https://mastodon.social", MastodonServer.normalize("  https://Mastodon.Social/ "))
        assertEq("https://mastodon.social", MastodonServer.normalize("@alice@mastodon.social"))
        assertEq("https://mastodon.social", MastodonServer.normalize("alice@mastodon.social"))
        assertEq("https://social.example:8443", MastodonServer.normalize("social.example:8443"))
        assertEq(null, MastodonServer.normalize("http://mastodon.social"))
        assertEq(null, MastodonServer.normalize("https://mastodon.social/about"))
        assertEq(null, MastodonServer.normalize("https://mastodon.social/?x=1"))
        assertEq(null, MastodonServer.normalize("https://me:pw@mastodon.social"))
        assertEq(null, MastodonServer.normalize("localhost"))
        assertEq(null, MastodonServer.normalize(""))
        assertEq("social.example:8443", MastodonServer.host("https://social.example:8443"))
        assertEq("Your Mastodon sign-in was revoked on social.example. Sign in again.", Mastodon.revokedNotice(server))
    }

    fun testPkceAndAuthorizeAddress() {
        assertEq("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM", MastodonAuth.challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"))
        val pkce = MastodonAuth.pkce()
        assertEq(43, pkce.verifier.length)
        expect(Regex("^[0-9a-f]{32}$").matches(pkce.state))
        assertEq(MastodonAuth.challenge(pkce.verifier), pkce.challenge)
        expect(MastodonAuth.pkce().state != pkce.state)
        assertEq(
            "$server/oauth/authorize?response_type=code&client_id=abc&redirect_uri=forkmesh%3A%2F%2Fmastodon%2Fcallback" +
                "&scope=read+write+follow&state=s1&code_challenge=ch&code_challenge_method=S256",
            MastodonAuth.authorizeUrl(server, "abc", Mastodon.REDIRECT, "s1", "ch"),
        )
    }

    fun testReadsCodesFromCallbacksAndPastes() {
        assertEq("xyz", MastodonAuth.code("forkmesh://mastodon/callback?code=xyz&state=s1", "s1"))
        assertEq("xyz", MastodonAuth.code("forkmesh://mastodon/callback?code=xyz", "s1"))
        assertEq(null, MastodonAuth.code("forkmesh://mastodon/callback?code=xyz&state=other", "s1"))
        assertEq(null, MastodonAuth.code("forkmesh://mastodon/callback?code=xyz&state=s1&error=access_denied", "s1"))
        assertEq("Ab_c-12345", MastodonAuth.code("  Ab_c-12345 ", "s1"))
        assertEq(null, MastodonAuth.code("not a code", "s1"))
        assertEq(null, MastodonAuth.code("abc", "s1"))
        assertEq(null, MastodonAuth.code("", "s1"))
    }

    fun testCountsCharactersLikeTheServer() {
        assertEq(5, MastodonText.length("hello"))
        assertEq(6 + 23, MastodonText.length("look: https://example.com/a/very/long/path/that/is/long"))
        assertEq(7, MastodonText.length("hi @bob@other.example"))
        assertEq(2, MastodonText.length("😀😀"))
        assertEq(500 - 5 - 4, MastodonText.remaining(500, "hello", "spoi", warning = true))
        assertEq(495, MastodonText.remaining(500, "hello", "spoi", warning = false))
    }

    fun testTurnsPostHtmlIntoSegments() {
        val mentions = listOf(MastodonMention("7", "bob@other.example", "https://other.example/@bob"))
        val html = "<p>Hi <span class=\"h-card\"><a href=\"https://other.example/@bob\" class=\"u-url mention\">@<span>bob</span></a></span> &amp; " +
            "<a href=\"https://social.example/tags/Cats\" class=\"mention hashtag\" rel=\"tag\">#<span>Cats</span></a></p>" +
            "<p>See <a href=\"https://example.com/a/long/path\" rel=\"nofollow\"><span class=\"invisible\">https://</span>" +
            "<span class=\"ellipsis\">example.com/a/lo</span><span class=\"invisible\">ng/path</span></a><br>bye&#33; &#x1F600;</p>\n"
        assertEq(
            listOf(
                MastodonSegment.Text("Hi "),
                MastodonSegment.Mention("@bob", "7"),
                MastodonSegment.Text(" & "),
                MastodonSegment.Hashtag("#Cats", "cats"),
                MastodonSegment.Text("\n\nSee "),
                MastodonSegment.Link("example.com/a/lo…", "https://example.com/a/long/path"),
                MastodonSegment.Text("\nbye! 😀"),
            ),
            MastodonHtml.segments(html, mentions),
        )
        assertEq("Hi @bob & #Cats\n\nSee example.com/a/lo…\nbye! 😀", MastodonHtml.text(html))
        assertEq(listOf(MastodonSegment.Text("x mailto")), MastodonHtml.segments("<p>x <a href=\"mailto:a@b\">mailto</a></p>"))
        assertEq("a < b \"c\" 'd' e f", MastodonHtml.text("a &lt; b &quot;c&quot; &#39;d&apos; e&nbsp;f"))
    }

    fun testFindsTheNextPageOnTheSameServer() {
        val header = "<$server/api/v1/timelines/home?max_id=9>; rel=\"next\", <$server/api/v1/timelines/home?min_id=20>; rel=\"prev\""
        assertEq("$server/api/v1/timelines/home?max_id=9", MastodonPaging.next(header, server))
        assertEq(null, MastodonPaging.next("<https://evil.example/x?max_id=9>; rel=\"next\"", server))
        assertEq(null, MastodonPaging.next("<$server/x>; rel=\"prev\"", server))
        assertEq(null, MastodonPaging.next(null, server))
    }

    fun testParsesStatusesNotificationsAndLimits() {
        val boosted = MastodonStatus.from(status("200", mapOf("reblog" to status("100"), "content" to "")))!!
        val inner = boosted.shown
        assertEq("100", inner.id)
        assertEq("$server/users/alice/statuses/100", inner.url)
        assertEq("alice", inner.account.displayName)
        assertEq("https://cdn.example/a-static.png", inner.account.avatar)
        assertEq(1_790_856_000_500L, inner.createdAt)
        assertEq("unlisted", inner.visibility)
        assertEq(listOf(MastodonAttachment("m1", "image", "$server/m1.png", "$server/m1s.png", "A cat")), inner.media)
        assertEq(listOf(MastodonSegment.Text("Hello")), inner.segments)
        val poll = MastodonPoll.from(mapOf("options" to listOf(mapOf("title" to "Yes", "votes_count" to 1), mapOf("title" to "No", "votes_count" to 2)), "votes_count" to 3, "expired" to true))!!
        assertEq(listOf(33, 67), poll.options.map(poll::percent))
        val note = MastodonNotification.from(mapOf("id" to "n1", "type" to "favourite", "created_at" to "2026-10-01T12:00:00Z", "account" to account(), "status" to status()))!!
        assertEq("alice favourited your post", note.sentence)
        assertEq("A poll you voted in has ended", note.copy(type = "poll").sentence)
        assertEq("alice: admin.sign_up", note.copy(type = "admin.sign_up").sentence)
        assertEq(MastodonInstanceLimits(1000, 6), MastodonInstanceLimits.from(mapOf("configuration" to mapOf("statuses" to mapOf("max_characters" to 1000, "max_media_attachments" to 6)))))
        assertEq(MastodonInstanceLimits(500, 4), MastodonInstanceLimits.from(emptyMap()))
    }

    fun testReplyPrefillsMentionsAndVisibility() {
        val parent = MastodonStatus.from(status())!!
        assertEq("@alice @bob@other.example ", MastodonCompose.replyText(parent, "carol"))
        assertEq("@bob@other.example ", MastodonCompose.replyText(parent, "alice"))
        assertEq("unlisted", MastodonCompose.replyVisibility(parent))
        assertEq("Followers only", MastodonCompose.visibilityLabel("private"))
    }

    fun testClientSendsTheRightRequests() {
        val fake = Fake { request ->
            when {
                request.url.endsWith("/api/v1/timelines/home?limit=30") ->
                    response(200, listOf(status()), mapOf("Link" to "<$server/api/v1/timelines/home?max_id=1>; rel=\"next\""))
                request.url.contains("/favourite") -> response(200, status(extra = mapOf("favourited" to true)))
                request.url.contains("/reblog") -> response(200, status("300", mapOf("reblog" to status("100", mapOf("reblogged" to true)))))
                request.url.endsWith("/api/v1/statuses") -> response(200, status("400"))
                request.url.contains("relationships") -> response(200, listOf(mapOf("id" to "7", "following" to true)))
                request.url.endsWith("/api/v2/instance") -> response(500, mapOf("error" to "down"))
                else -> response(200, emptyMap<String, Any?>())
            }
        }
        val client = MastodonClient(server, "tok", fake)
        val page = client.timeline(MastodonSource.Home)
        assertEq(1, page.items.size)
        assertEq("$server/api/v1/timelines/home?max_id=1", page.next)
        val request = fake.requests.last()
        assertEq("Bearer tok", request.headers["Authorization"])
        assertEq("application/json", request.headers["Accept"])
        client.timeline(MastodonSource.Home, "$server/api/v1/timelines/home?max_id=1")
        assertEq("$server/api/v1/timelines/home?max_id=1", fake.requests.last().url)
        client.timeline(MastodonSource.Home, "https://evil.example/api")
        assertEq("$server/api/v1/timelines/home?limit=30", fake.requests.last().url)
        assertEq("$server/api/v1/timelines/public?local=true&limit=30", run { client.timeline(MastodonSource.Local); fake.requests.last().url })
        assertEq("$server/api/v1/timelines/tag/caf%C3%A9?limit=30", run { client.timeline(MastodonSource.Tag("café")); fake.requests.last().url })
        assertEq(true, client.favourite("100", true).favourited)
        assertEq("POST", fake.requests.last().method)
        assertEq("$server/api/v1/statuses/100/favourite", fake.requests.last().url)
        val reblogged = client.reblog("100", true)
        assertEq("100", reblogged.id)
        assertEq(true, reblogged.reblogged)
        client.post("hi", "private", spoiler = "cw", replyTo = "100", mediaIds = listOf("m1"), idempotencyKey = "key-1")
        val posted = fake.requests.last()
        assertEq("key-1", posted.headers["Idempotency-Key"])
        assertEq(
            mapOf("status" to "hi", "visibility" to "private", "spoiler_text" to "cw", "sensitive" to true, "in_reply_to_id" to "100", "media_ids" to listOf("m1")),
            Json.parseObject(String(posted.body!!)),
        )
        client.post("plain", "public", idempotencyKey = "key-2")
        assertEq(mapOf("status" to "plain", "visibility" to "public"), Json.parseObject(String(fake.requests.last().body!!)))
        assertEq(true, client.relationship("7")?.following)
        assertEq("$server/api/v1/accounts/relationships?id%5B%5D=7", fake.requests.last().url)
        assertEq(MastodonInstanceLimits(), client.instance())
        client.delete("100")
        assertEq("DELETE", fake.requests.last().method)
    }

    fun testClientReportsErrors() {
        val unauthorized = MastodonClient(server, "tok", Fake { response(401, mapOf("error" to "The access token was revoked")) })
        assertFails<MastodonUnauthorized> { unauthorized.me() }
        assertFails<MastodonUnauthorized> { unauthorized.instance() }
        val failing = MastodonClient(server, "tok", Fake { response(422, mapOf("error" to "Validation failed: Text can't be blank")) })
        assertEq("Validation failed: Text can't be blank", assertFails<MastodonException> { failing.post("", "public") }.message)
        val bare = MastodonClient(server, "tok", Fake { response(503, "") })
        assertEq("Mastodon returned 503.", assertFails<MastodonException> { bare.me() }.message)
        val offline = MastodonClient(server, "tok", Fake { throw IOException("no route") })
        assertEq("Could not reach social.example.", assertFails<MastodonException> { offline.me() }.message)
    }

    fun testUploadWaitsForProcessing() {
        var polls = 0
        val fake = Fake { request ->
            if (request.url.endsWith("/api/v2/media")) {
                response(202, mapOf("id" to "m9", "type" to "image", "url" to null))
            } else {
                polls++
                response(if (polls < 3) 206 else 200, mapOf("id" to "m9", "type" to "image", "url" to if (polls < 3) null else "$server/m9.png"))
            }
        }
        val slept = mutableListOf<Long>()
        val attachment = MastodonClient(server, "tok", fake) { slept += it }.upload(MastodonUpload(byteArrayOf(1, 2, 3), "cat.png", "image/png", "A cat"))
        assertEq("$server/m9.png", attachment.url)
        assertEq(3, polls)
        assertEq(listOf(1000L, 1000L, 1000L), slept)
        val upload = fake.requests.first()
        expect(upload.headers["Content-Type"]!!.startsWith("multipart/form-data; boundary="))
        val body = String(upload.body!!, Charsets.ISO_8859_1)
        expect("name=\"file\"; filename=\"cat.png\"" in body)
        expect("name=\"description\"\r\n\r\nA cat\r\n" in body)
        assertEq("$server/api/v1/media/m9", fake.requests.last().url)
    }

    fun testSignInRoundTripKeepsTheSessionOnThePhone() {
        val fake = Fake { request ->
            when {
                request.url.endsWith("/api/v1/apps") -> response(200, mapOf("client_id" to "cid", "client_secret" to "sec"))
                request.url.endsWith("/oauth/token") -> response(200, mapOf("access_token" to "tok"))
                request.url.endsWith("/verify_credentials") -> response(200, account("1", "alice"))
                request.url.endsWith("/oauth/revoke") -> response(200, emptyMap<String, Any?>())
                else -> response(404, emptyMap<String, Any?>())
            }
        }
        val secrets = MemorySecretStore()
        val store = MastodonStore(secrets)
        val signIn = MastodonSignIn(store) { s, t -> MastodonClient(s, t, fake) }
        val authorize = signIn.start("@alice@Social.Example")
        val registration = MastodonForm.decode(String(fake.requests.first().body!!))
        assertEq("ForkMesh", registration["client_name"])
        assertEq("forkmesh://mastodon/callback\nurn:ietf:wg:oauth:2.0:oob", registration["redirect_uris"])
        assertEq("read write follow", registration["scopes"])
        assertEq("https://forkmesh.com", registration["website"])
        val pending = store.pending!!
        assertEq(server, pending.server)
        expect(authorize.startsWith("$server/oauth/authorize?") && "state=${pending.state}" in authorize)
        assertFails<MastodonException> { signIn.finish("forkmesh://mastodon/callback?code=abc&state=wrong") }
        val session = signIn.finish("forkmesh://mastodon/callback?code=abcd&state=${pending.state}")
        val exchange = MastodonForm.decode(String(fake.requests[1].body!!))
        assertEq("abcd", exchange["code"])
        assertEq(pending.verifier, exchange["code_verifier"])
        assertEq("forkmesh://mastodon/callback", exchange["redirect_uri"])
        assertEq("authorization_code", exchange["grant_type"])
        assertEq("@alice@social.example", session.handle)
        assertEq(null, store.pending)
        assertEq(session, MastodonStore(secrets).session)
        expect(secrets.read(MastodonStore.SESSION)!!.contains("\"token\":\"tok\""))
        signIn.signOut()
        assertEq("$server/oauth/revoke", fake.requests.last().url)
        assertEq(null, store.session)
        signIn.withToken("social.example", " tok2 ")
        assertEq("tok2", store.session?.token)
        assertFails<MastodonException> { signIn.start("http://insecure.example") }
    }
}
