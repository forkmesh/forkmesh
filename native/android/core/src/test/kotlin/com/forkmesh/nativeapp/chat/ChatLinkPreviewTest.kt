package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.net.URI
import java.nio.file.Files

internal class ChatLinkFakeHttp : ChatLinkHttp {
    val requests = ArrayList<ChatLinkRequest>()
    private val routes = ArrayList<Pair<(URI) -> Boolean, (ChatLinkRequest) -> ChatLinkResponse>>()

    fun on(match: (URI) -> Boolean, reply: (ChatLinkRequest) -> ChatLinkResponse) {
        routes.add(match to reply)
    }

    fun onPath(path: String, json: Map<String, Any?>) = on({ it.path == path }) {
        ChatLinkResponse(200, mapOf("Content-Type" to "application/json"), Json.stringify(json).toByteArray())
    }

    fun hosts(): List<String> = requests.map { URI(it.url).host }

    override fun get(request: ChatLinkRequest): ChatLinkResponse? {
        requests.add(request)
        val url = URI(request.url)
        val route = routes.firstOrNull { it.first(url) } ?: return ChatLinkResponse(404)
        val reply = route.second(request)
        if (reply.body.size > request.maxBytes) {
            return ChatLinkResponse(reply.status, reply.headers, reply.body.copyOf(request.maxBytes), overflow = true)
        }
        return reply
    }
}

internal val publicDns = ChatHostResolver { listOf(byteArrayOf(93, 184.toByte(), 216.toByte(), 34)) }

internal fun html(text: String, status: Int = 200, type: String = "text/html; charset=utf-8") =
    ChatLinkResponse(status, mapOf("content-type" to type), text.toByteArray())

class ChatLinkPreviewTest {
    private fun url(raw: String) = URI(raw)

    fun testCatalogCoversFiftySitesWithDistinctKeys() {
        expect(ChatLinkSites.all.size >= 50, "${ChatLinkSites.all.size} sites")
        assertEq(ChatLinkSites.all.size, ChatLinkSites.all.map { it.key }.toSet().size)
        expect(ChatLinkSites.all.map { it.kind }.toSet().size >= 10)
        for (site in ChatLinkSites.all) {
            expect(site.hosts.isNotEmpty(), site.key)
            val entry = site.hosts[0]
            val host = entry.substringBefore('/')
            val path = if ('/' in entry) "/" + entry.substringAfter('/') else "/x"
            assertEq(site.key, ChatLinkSites.site(url("https://www.$host$path"))?.key, site.key)
        }
    }

    fun testSiteMatchingUsesHostSuffixAndPath() {
        assertEq("wikipedia", ChatLinkSites.site(url("https://en.m.wikipedia.org/wiki/Git"))?.key)
        assertEq("youtube", ChatLinkSites.site(url("https://youtu.be/aBcDeFgHi12"))?.key)
        assertEq("googlemaps", ChatLinkSites.site(url("https://google.com/maps/place/x"))?.key)
        assertEq(null, ChatLinkSites.site(url("https://google.com/search?q=x")))
        assertEq(null, ChatLinkSites.site(url("https://notyoutube.com/watch")))
        assertEq("substack", ChatLinkSites.site(url("https://someone.substack.com/p/post"))?.key)
    }

    fun testYouTubeIdsFromEveryLinkShape() {
        for (raw in listOf(
            "https://www.youtube.com/watch?v=aBcDeFgHi12&t=462s", "https://youtu.be/aBcDeFgHi12?t=4",
            "https://m.youtube.com/shorts/aBcDeFgHi12", "https://youtube.com/live/aBcDeFgHi12",
            "https://www.youtube-nocookie.com/embed/aBcDeFgHi12",
        )) {
            assertEq("aBcDeFgHi12", ChatLinkPreviewParsing.youtubeId(url(raw)), raw)
        }
        assertEq(null, ChatLinkPreviewParsing.youtubeId(url("https://www.youtube.com/@channel")))
        assertEq(null, ChatLinkPreviewParsing.youtubeId(url("https://www.youtube.com/watch?v=%3Cscript%3E")))
    }

    fun testOnlyPublicWebLinksArePreviewed() {
        for (blocked in listOf(
            "http://localhost/x", "https://127.0.0.1/", "https://10.0.0.5/a", "https://192.168.1.1/", "https://172.20.1.1",
            "https://169.254.169.254/latest", "https://[::1]/", "https://[fd00::1]/", "https://[fe80::1]/", "https://printer.local/",
            "https://router/", "ftp://example.com/", "javascript:alert(1)", "https://user:pw@example.com/", "https://example.com:8080/",
            "https://100.64.0.1/", "https://0x7f.0.0.1/", "https://2130706433/", "https://10.77.0.3/", "https://[::ffff:127.0.0.1]/",
            "https://nas.home.arpa/", "https://224.0.0.1/", "https://0.0.0.0/",
        )) {
            assertEq(null, ChatLinkPreviewParsing.previewable(blocked), blocked)
        }
        for (allowed in listOf("https://github.com/forkmesh", "http://example.org/page", "https://8.8.8.8/", "https://[2606:4700::1111]/")) {
            expect(ChatLinkPreviewParsing.previewable(allowed) != null, allowed)
        }
        val picked = ChatLinkPreviewParsing.candidates(
            listOf("https://a.com/1", "https://a.com/1", "mailto:x@y.z", "https://b.com", "https://c.com", "https://d.com"),
        )
        assertEq(listOf("https://a.com/1", "https://b.com", "https://c.com"), picked.map { it.toString() })
    }

    fun testLinksAreTakenFromTheMessageTextOutsideCodeFences() {
        val text = "see https://a.com/x), and <https://b.com/y>.\n```\nhttps://c.com/code\n```\n(https://d.com/(z)) [https://e.com/p|label]"
        assertEq(
            listOf("https://a.com/x", "https://b.com/y", "https://d.com/(z)", "https://e.com/p"),
            ChatLinkPreviewParsing.links(text),
        )
    }

    fun testOpenGraphParsingPrefersSocialTagsAndResolvesImages() {
        val page = """
            <html><head><title>Fallback &amp; title</title>
            <meta property="og:title" content="Fork &quot;Mesh&quot; &#8212; repos">
            <meta name='description' content='Plain description'>
            <meta property="og:description" content="Social
              description">
            <meta property="og:image" content="/img/card.png?x=1&amp;y=2">
            <meta property="og:site_name" content="ForkMesh">
            <meta name=author content=Jett>
            <meta property="og:type" content="video.other">
            </head><body><meta property="og:title" content="ignored"></body></html>
        """.trimIndent()
        val preview = ChatLinkPreviewParsing.parseHtml(page, url("https://forkmesh.com/a/b"))
        assertEq("Fork \"Mesh\" — repos", preview.title)
        assertEq("Social description", preview.detail)
        assertEq("https://forkmesh.com/img/card.png?x=1&y=2", preview.imageUrl)
        assertEq("ForkMesh", preview.siteName)
        assertEq("Jett", preview.author)
        assertEq(ChatLinkKind.Video, preview.kind)

        val bare = ChatLinkPreviewParsing.parseHtml(
            "<head><title>\n Just a title </title><meta name=\"twitter:image\" content=\"https://127.0.0.1/x.png\"></head>",
            url("https://example.org"),
        )
        assertEq("Just a title", bare.title)
        assertEq("", bare.imageUrl)
        assertEq("https://example.org/c.jpg", ChatLinkPreviewParsing.resolve("c.jpg", url("https://example.org")))
    }

    fun testTweetTextAndTruncation() {
        val tweet = "<blockquote class=\"twitter-tweet\"><p lang=\"en\">Shipping <a href=\"x\">#forkmesh</a> today &amp; tomorrow</p>&mdash; Jett</blockquote>"
        assertEq("Shipping #forkmesh today & tomorrow", ChatLinkPreviewParsing.tweetText(tweet))
        assertEq("aaaaaaaaa…", ChatLinkPreviewParsing.truncate("a".repeat(20), 10))
    }

    fun testLinkPathHelpers() {
        assertEq("@scope/pkg", ChatLinkPreviewParsing.packageName(url("https://www.npmjs.com/package/@scope/pkg"), "package"))
        assertEq("requests", ChatLinkPreviewParsing.packageName(url("https://pypi.org/project/requests/"), "project"))
        assertEq("6449999999", ChatLinkPreviewParsing.appStoreId(url("https://apps.apple.com/us/app/forkmesh/id6449999999")))
        assertEq("620", ChatLinkPreviewParsing.steamAppId(url("https://store.steampowered.com/app/620/Portal_2/")))
        assertEq(123L, ChatLinkPreviewParsing.hackerNewsId(url("https://news.ycombinator.com/item?id=123")))
        assertEq("927", ChatLinkPreviewParsing.xkcdNumber(url("https://xkcd.com/927/")))
        assertEq("Git", ChatLinkPreviewParsing.wikipediaPage(url("https://en.wikipedia.org/wiki/Git"))?.second)
        assertEq(null, ChatLinkPreviewParsing.wikipediaPage(url("https://en.wikipedia.org/wiki/Special:Random")))
        assertEq("unix" to "4", ChatLinkPreviewParsing.stackExchangeQuestion(url("https://unix.stackexchange.com/questions/4/x")))
    }

    fun testYouTubeUsesOEmbedAndThumbnail() {
        val http = ChatLinkFakeHttp()
        http.onPath("/oembed", mapOf("title" to "Example Video", "author_name" to "Example Channel"))
        val service = ChatLinkPreviewService(http, now = { 1_000_000L }, resolver = publicDns)
        val link = url("https://www.youtube.com/watch?v=aBcDeFgHi12&t=462s")
        service.load(link)
        val preview = service.preview(link)
        assertEq("Example Video", preview?.title)
        assertEq("Example Channel", preview?.author)
        assertEq("YouTube", preview?.siteName)
        assertEq("https://i.ytimg.com/vi/aBcDeFgHi12/hqdefault.jpg", preview?.imageUrl)
        assertEq(ChatLinkKind.Video, preview?.kind)
        assertEq(1, http.requests.size)
        expect(http.requests[0].url.contains("url=https%3A%2F%2Fwww.youtube.com%2Fwatch%3Fv%3DaBcDeFgHi12"), http.requests[0].url)
        service.load(link)
        assertEq(1, http.requests.size, "a fresh preview is not fetched again")
    }

    fun testGenericPageFollowsSafeRedirectsOnly() {
        val http = ChatLinkFakeHttp()
        http.on({ it.host == "short.example.com" }) { ChatLinkResponse(301, mapOf("Location" to "https://blog.example.com/post")) }
        http.on({ it.host == "blog.example.com" }) {
            html("<head><meta property=og:title content=\"A post\"><meta property=og:image content=\"/c.jpg\"></head>")
        }
        http.on({ it.host == "evil.example.com" }) { ChatLinkResponse(302, mapOf("location" to "http://192.168.1.1/admin")) }
        val service = ChatLinkPreviewService(http, resolver = publicDns)
        val short = url("https://short.example.com/x")
        service.load(short)
        assertEq("A post", service.preview(short)?.title)
        assertEq("https://blog.example.com/c.jpg", service.preview(short)?.imageUrl)
        assertEq("blog.example.com", service.preview(short)?.siteName)
        assertEq("https://short.example.com/x", service.preview(short)?.url)

        val evil = url("https://evil.example.com/")
        service.load(evil)
        assertEq(ChatLinkPreviewState.None, service.state(evil))
        expect("192.168.1.1" !in http.hosts(), "${http.hosts()}")
    }

    fun testRedirectLoopsStopAfterFourHops() {
        val http = ChatLinkFakeHttp()
        http.on({ it.host == "loop.example.com" }) { ChatLinkResponse(302, mapOf("location" to "/again")) }
        val service = ChatLinkPreviewService(http, resolver = publicDns)
        val loop = url("https://loop.example.com/start")
        service.load(loop)
        assertEq(ChatLinkPreviewState.None, service.state(loop))
        assertEq(2 * (ChatLinkPreviewService.MAX_REDIRECTS + 1), http.requests.size, "two agents, five hops each")
    }

    fun testOEmbedSitesAndWikipediaSummary() {
        val http = ChatLinkFakeHttp()
        http.onPath("/oembed", mapOf(
            "title" to "Track", "author_name" to "Artist", "thumbnail_url" to "https://i.scdn.co/image/abc", "provider_name" to "Spotify",
        ))
        http.onPath("/api/rest_v1/page/summary/Git", mapOf(
            "title" to "Git", "extract" to "Git is a distributed version control system.", "description" to "Version control software",
            "thumbnail" to mapOf("source" to "https://upload.wikimedia.org/git.png"),
        ))
        val service = ChatLinkPreviewService(http, resolver = publicDns)
        val track = url("https://open.spotify.com/track/123")
        service.load(track)
        assertEq("Track", service.preview(track)?.title)
        assertEq("https://i.scdn.co/image/abc", service.preview(track)?.imageUrl)
        assertEq(ChatLinkKind.Music, service.preview(track)?.kind)
        val wiki = url("https://en.wikipedia.org/wiki/Git")
        service.load(wiki)
        assertEq("Git is a distributed version control system.", service.preview(wiki)?.detail)
        assertEq("Wikipedia", service.preview(wiki)?.siteName)
        assertEq("https://upload.wikimedia.org/git.png", service.preview(wiki)?.imageUrl)
    }

    fun testTooLargeImagesAreRefusedAndDisabledServiceFetchesNothing() {
        val http = ChatLinkFakeHttp()
        http.on({ true }) { ChatLinkResponse(200, mapOf("content-type" to "image/png"), ByteArray(ChatLinkPreviewService.MAX_IMAGE_BYTES + 1)) }
        val service = ChatLinkPreviewService(http, resolver = publicDns)
        assertEq(null, service.imageData("https://img.example.com/huge.png"))
        val prefs = MemoryPrefStore()
        val off = ChatLinkPreviewService(http, prefs = prefs, resolver = publicDns)
        off.setEnabled(false)
        assertEq(false, prefs.getBool(ChatLinkPreviewService.ENABLED_KEY))
        assertEq(false, ChatLinkPreviewService(http, prefs = prefs, resolver = publicDns).enabled, "the switch survives a restart")
        val before = http.requests.size
        off.load(url("https://example.org/"))
        assertEq(null, off.imageData("https://img.example.com/small.png"))
        assertEq(before, http.requests.size)
    }

    fun testImagesAreCachedAndMustBeImages() {
        val http = ChatLinkFakeHttp()
        http.on({ it.path == "/a.png" }) { ChatLinkResponse(200, mapOf("content-type" to "image/png"), byteArrayOf(1, 2, 3)) }
        http.on({ it.path == "/page.png" }) { html("<html></html>") }
        val service = ChatLinkPreviewService(http, resolver = publicDns)
        assertEq(3, service.imageData("https://img.example.com/a.png")?.size)
        assertEq(3, service.imageData("https://img.example.com/a.png")?.size)
        assertEq(1, http.requests.size)
        assertEq(null, service.imageData("https://img.example.com/page.png"))
    }

    fun testRefusedCrawlerIsRetriedAsABrowserAndStackOverflowUsesTheApi() {
        val http = ChatLinkFakeHttp()
        http.on({ it.host == "shy.example.com" }) { request ->
            if (request.headers["User-Agent"].orEmpty().contains("ForkMeshBot")) {
                html("no bots", status = 403)
            } else {
                html("<html><head></head><body><meta property=\"og:title\" content=\"Late tags\"><meta property=\"og:image\" content=\"https://shy.example.com/i.png\"></body></html>")
            }
        }
        http.onPath("/2.3/questions/11227809", mapOf(
            "items" to listOf(mapOf(
                "title" to "Why is processing a sorted array &quot;faster&quot;?", "score" to 27000, "answer_count" to 26,
                "is_answered" to true, "owner" to mapOf("display_name" to "GManNickG"),
            )),
        ))
        val service = ChatLinkPreviewService(http, resolver = publicDns)
        val shy = url("https://shy.example.com/post")
        service.load(shy)
        assertEq("Late tags", service.preview(shy)?.title)
        assertEq(2, http.hosts().count { it == "shy.example.com" })
        val question = url("https://stackoverflow.com/questions/11227809/why-is-processing")
        service.load(question)
        assertEq("Why is processing a sorted array \"faster\"?", service.preview(question)?.title)
        assertEq("27000 votes · 26 answers · answered", service.preview(question)?.detail)
        assertEq("GManNickG", service.preview(question)?.author)
        assertEq("site=stackoverflow&filter=default", URI(http.requests.last().url).query)
    }

    fun testMissesAreNotRetriedForSixHours() {
        var clock = 10_000_000L
        val http = ChatLinkFakeHttp()
        val service = ChatLinkPreviewService(http, now = { clock }, resolver = publicDns)
        val gone = url("https://gone.example.com/")
        service.load(gone)
        assertEq(ChatLinkPreviewState.None, service.state(gone))
        val asked = http.requests.size
        clock += ChatLinkPreviewService.MISS_MS - 1
        service.load(gone)
        assertEq(asked, http.requests.size)
        clock += 2
        service.load(gone)
        expect(http.requests.size > asked, "a miss is retried after six hours")
    }

    fun testThePreviewCacheBelongsToOneProject() {
        val dir = Files.createTempDirectory("fm-link-projects").toFile()
        val http = ChatLinkFakeHttp()
        http.on({ true }) { html("<head><meta property=og:title content=\"Alpha\"></head>") }
        var project = "alpha"
        val service = ChatLinkPreviewService(
            http,
            fileSource = { File(dir, ChatLinkPreviewService.fileName(project)) },
            resolver = publicDns,
        )
        val link = url("https://example.org/post")
        service.load(link)
        assertEq("Alpha", service.preview(link)?.title)
        service.leaveProject()
        expect(File(dir, ChatLinkPreviewService.fileName("alpha")).isFile, "alpha's previews are saved")

        project = "beta"
        service.warm()
        assertEq(ChatLinkPreviewState.Idle, service.state(link), "beta never sees what alpha looked at")
        service.leaveProject()
        project = "alpha"
        service.warm()
        assertEq("Alpha", service.preview(link)?.title)
        expect(ChatLinkPreviewService.fileName("alpha") != ChatLinkPreviewService.fileName("beta"))
    }

    fun testOnlyOwnAndPrivateMessagesFetchPreviewsByThemselves() {
        val town = OfficeRoom("a".repeat(32), "town", isPrivate = false, canManage = false, topic = "", members = emptyList(), keyVersion = 1)
        val team = town.copy(id = "b".repeat(32), name = "team", isPrivate = true)
        val everyone = town.copy(id = "c".repeat(32), name = "general", isPrivate = true)
        assertEq(false, ChatLinkPreviewPolicy.autoLoads("#general", false, null), "world-general is everyone's")
        assertEq(false, ChatLinkPreviewPolicy.autoLoads(MESH_CONVERSATION, false, null))
        assertEq(false, ChatLinkPreviewPolicy.autoLoads("#office/town", false, town), "a public channel")
        assertEq(true, ChatLinkPreviewPolicy.autoLoads("#office/team", false, team))
        assertEq(false, ChatLinkPreviewPolicy.autoLoads("#office/general", false, everyone), "the whole organization's room")
        assertEq(false, ChatLinkPreviewPolicy.autoLoads("#office/team", false, null), "a room this phone does not know")
        assertEq(true, ChatLinkPreviewPolicy.autoLoads(DirectMessageFrames.PREFIX + "c".repeat(32), false, null))
        assertEq(true, ChatLinkPreviewPolicy.autoLoads("#general", true, null), "your own links load as before")
    }

    fun testCacheRoundTrip() {
        val file = File(Files.createTempDirectory("fm-link").toFile(), "previews.json")
        val saved = listOf(ChatLinkPreview(url = "https://a.com", title = "A", siteKey = "github", kind = ChatLinkKind.Code, fetchedAtMs = 50_000L))
        ChatLinkPreviewService.write(saved, file)
        assertEq(saved, ChatLinkPreviewService.read(file))
        assertEq(emptyList<ChatLinkPreview>(), ChatLinkPreviewService.read(File(file.parentFile, "missing.json")))
    }
}
