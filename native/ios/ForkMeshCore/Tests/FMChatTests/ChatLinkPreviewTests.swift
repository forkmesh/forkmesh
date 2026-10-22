import FMBase
import FMNet
import Foundation
import XCTest

@testable import FMChat

let publicDNS: ChatHostResolver = { _ in [[93, 184, 216, 34]] }

final class ChatLinkPreviewTests: XCTestCase {
    func testCatalogCoversFiftySitesWithDistinctKeys() {
        XCTAssertGreaterThanOrEqual(ChatLinkSites.all.count, 50)
        XCTAssertEqual(Set(ChatLinkSites.all.map(\.key)).count, ChatLinkSites.all.count)
        XCTAssertGreaterThanOrEqual(Set(ChatLinkSites.all.map(\.kind)).count, 10)
        for site in ChatLinkSites.all {
            XCTAssertFalse(site.hosts.isEmpty, site.key)
            let host = site.hosts[0].split(separator: "/").first.map(String.init) ?? ""
            let path = site.hosts[0].contains("/") ? "/" + (site.hosts[0].split(separator: "/", maxSplits: 1).last.map(String.init) ?? "") : "/x"
            let url = URL(string: "https://www.\(host)\(path)")!
            XCTAssertEqual(ChatLinkSites.site(for: url)?.key, site.key, site.key)
        }
    }

    func testSiteMatchingUsesHostSuffixAndPath() {
        XCTAssertEqual(ChatLinkSites.site(for: URL(string: "https://en.m.wikipedia.org/wiki/Git")!)?.key, "wikipedia")
        XCTAssertEqual(ChatLinkSites.site(for: URL(string: "https://youtu.be/aBcDeFgHi12")!)?.key, "youtube")
        XCTAssertEqual(ChatLinkSites.site(for: URL(string: "https://google.com/maps/place/x")!)?.key, "googlemaps")
        XCTAssertNil(ChatLinkSites.site(for: URL(string: "https://google.com/search?q=x")!))
        XCTAssertNil(ChatLinkSites.site(for: URL(string: "https://notyoutube.com/watch")!))
        XCTAssertEqual(ChatLinkSites.site(for: URL(string: "https://someone.substack.com/p/post")!)?.key, "substack")
    }

    func testYouTubeIdsFromEveryLinkShape() {
        let cases = [
            "https://www.youtube.com/watch?v=aBcDeFgHi12&t=462s", "https://youtu.be/aBcDeFgHi12?t=4",
            "https://m.youtube.com/shorts/aBcDeFgHi12", "https://youtube.com/live/aBcDeFgHi12",
            "https://www.youtube-nocookie.com/embed/aBcDeFgHi12",
        ]
        for raw in cases {
            XCTAssertEqual(ChatLinkPreviewParsing.youtubeId(URL(string: raw)!), "aBcDeFgHi12", raw)
        }
        XCTAssertNil(ChatLinkPreviewParsing.youtubeId(URL(string: "https://www.youtube.com/@channel")!))
        XCTAssertNil(ChatLinkPreviewParsing.youtubeId(URL(string: "https://www.youtube.com/watch?v=<script>")!))
    }

    func testOnlyPublicWebLinksArePreviewed() {
        for blocked in [
            "http://localhost/x", "https://127.0.0.1/", "https://10.0.0.5/a", "https://192.168.1.1/", "https://172.20.1.1",
            "https://169.254.169.254/latest", "https://[::1]/", "https://[fd00::1]/", "https://[fe80::1]/", "https://printer.local/",
            "https://router/", "ftp://example.com/", "javascript:alert(1)", "https://user:pw@example.com/", "https://example.com:8080/",
            "https://100.64.0.1/", "https://0x7f.0.0.1/", "https://2130706433/", "https://10.77.0.3/", "https://[::ffff:127.0.0.1]/",
            "https://nas.home.arpa/",
        ] {
            XCTAssertNil(ChatLinkPreviewParsing.previewable(blocked), blocked)
        }
        for allowed in ["https://github.com/forkmesh", "http://example.org/page", "https://8.8.8.8/", "https://[2606:4700::1111]/"] {
            XCTAssertNotNil(ChatLinkPreviewParsing.previewable(allowed), allowed)
        }
        let picked = ChatLinkPreviewParsing.candidates(
            ["https://a.com/1", "https://a.com/1", "mailto:x@y.z", "https://b.com", "https://c.com", "https://d.com"])
        XCTAssertEqual(picked.map(\.absoluteString), ["https://a.com/1", "https://b.com", "https://c.com"])
    }

    func testOpenGraphParsingPrefersSocialTagsAndResolvesImages() {
        let html = """
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
            """
        let preview = ChatLinkPreviewParsing.parseHTML(html, page: URL(string: "https://forkmesh.com/a/b")!)
        XCTAssertEqual(preview.title, "Fork \"Mesh\" \u{2014} repos")
        XCTAssertEqual(preview.detail, "Social description")
        XCTAssertEqual(preview.imageUrl, "https://forkmesh.com/img/card.png?x=1&y=2")
        XCTAssertEqual(preview.siteName, "ForkMesh")
        XCTAssertEqual(preview.author, "Jett")
        XCTAssertEqual(preview.kind, .video)

        let bare = ChatLinkPreviewParsing.parseHTML(
            "<head><title>\n Just a title </title><meta name=\"twitter:image\" content=\"https://127.0.0.1/x.png\"></head>",
            page: URL(string: "https://example.org")!)
        XCTAssertEqual(bare.title, "Just a title")
        XCTAssertEqual(bare.imageUrl, "")
    }

    func testTweetTextAndTruncation() {
        let html = "<blockquote class=\"twitter-tweet\"><p lang=\"en\">Shipping <a href=\"x\">#forkmesh</a> today &amp; tomorrow</p>&mdash; Jett</blockquote>"
        XCTAssertEqual(ChatLinkPreviewParsing.tweetText(html), "Shipping #forkmesh today & tomorrow")
        XCTAssertEqual(ChatLinkPreviewParsing.truncate(String(repeating: "a", count: 20), 10), "aaaaaaaaa\u{2026}")
    }

    func testLinkPathHelpers() {
        XCTAssertEqual(ChatLinkPreviewParsing.packageName(URL(string: "https://www.npmjs.com/package/@scope/pkg")!, after: "package"), "@scope/pkg")
        XCTAssertEqual(ChatLinkPreviewParsing.packageName(URL(string: "https://pypi.org/project/requests/")!, after: "project"), "requests")
        XCTAssertEqual(ChatLinkPreviewParsing.appStoreId(URL(string: "https://apps.apple.com/us/app/forkmesh/id6449999999")!), "6449999999")
        XCTAssertEqual(ChatLinkPreviewParsing.steamAppId(URL(string: "https://store.steampowered.com/app/620/Portal_2/")!), "620")
        XCTAssertEqual(ChatLinkPreviewParsing.hackerNewsId(URL(string: "https://news.ycombinator.com/item?id=123")!), 123)
        XCTAssertEqual(ChatLinkPreviewParsing.xkcdNumber(URL(string: "https://xkcd.com/927/")!), "927")
        XCTAssertEqual(ChatLinkPreviewParsing.wikipediaPage(URL(string: "https://en.wikipedia.org/wiki/Git")!)?.title, "Git")
        XCTAssertNil(ChatLinkPreviewParsing.wikipediaPage(URL(string: "https://en.wikipedia.org/wiki/Special:Random")!))
    }

    @MainActor
    func testYouTubeUsesOEmbedAndThumbnail() async {
        let http = FakeHTTPClient()
        http.on(path: "/oembed", json: .object(["title": "Example Video", "author_name": "Example Channel"]))
        let service = ChatLinkPreviewService(http: http, clock: FixedClock(Date(timeIntervalSince1970: 1_000)), resolver: publicDNS)
        let url = URL(string: "https://www.youtube.com/watch?v=aBcDeFgHi12&t=462s")!
        await service.load(url)
        let preview = service.preview(for: url)
        XCTAssertEqual(preview?.title, "Example Video")
        XCTAssertEqual(preview?.author, "Example Channel")
        XCTAssertEqual(preview?.siteName, "YouTube")
        XCTAssertEqual(preview?.imageUrl, "https://i.ytimg.com/vi/aBcDeFgHi12/hqdefault.jpg")
        XCTAssertEqual(preview?.kind, .video)
        XCTAssertEqual(http.requests.count, 1)
        XCTAssertFalse(http.requests[0].followRedirects)
        await service.load(url)
        XCTAssertEqual(http.requests.count, 1)
    }

    @MainActor
    func testGenericPageFollowsSafeRedirectsOnly() async {
        let http = FakeHTTPClient()
        http.on(where: { $0.url.host == "short.example.com" }) { _ in
            HTTPResponse(status: 301, headers: ["location": "https://blog.example.com/post"], text: "")
        }
        http.on(where: { $0.url.host == "blog.example.com" }) { _ in
            HTTPResponse(
                status: 200, headers: ["content-type": "text/html; charset=utf-8"],
                text: "<head><meta property=og:title content=\"A post\"><meta property=og:image content=\"/c.jpg\"></head>")
        }
        http.on(where: { $0.url.host == "evil.example.com" }) { _ in
            HTTPResponse(status: 302, headers: ["location": "http://192.168.1.1/admin"], text: "")
        }
        let service = ChatLinkPreviewService(http: http, resolver: publicDNS)
        let short = URL(string: "https://short.example.com/x")!
        await service.load(short)
        XCTAssertEqual(service.preview(for: short)?.title, "A post")
        XCTAssertEqual(service.preview(for: short)?.imageUrl, "https://blog.example.com/c.jpg")
        XCTAssertEqual(service.preview(for: short)?.siteName, "blog.example.com")

        let evil = URL(string: "https://evil.example.com/")!
        await service.load(evil)
        XCTAssertEqual(service.state(for: evil), .none)
        XCTAssertFalse(http.requests.contains { $0.url.host == "192.168.1.1" })
    }

    @MainActor
    func testOEmbedSitesAndWikipediaSummary() async {
        let http = FakeHTTPClient()
        http.on(path: "/oembed", json: .object([
            "title": "Track", "author_name": "Artist", "thumbnail_url": "https://i.scdn.co/image/abc", "provider_name": "Spotify",
        ]))
        http.on(path: "/api/rest_v1/page/summary/Git", json: .object([
            "title": "Git", "extract": "Git is a distributed version control system.", "description": "Version control software",
            "thumbnail": .object(["source": "https://upload.wikimedia.org/git.png"]),
        ]))
        let service = ChatLinkPreviewService(http: http, resolver: publicDNS)
        let track = URL(string: "https://open.spotify.com/track/123")!
        await service.load(track)
        XCTAssertEqual(service.preview(for: track)?.title, "Track")
        XCTAssertEqual(service.preview(for: track)?.imageUrl, "https://i.scdn.co/image/abc")
        XCTAssertEqual(service.preview(for: track)?.kind, .music)
        let wiki = URL(string: "https://en.wikipedia.org/wiki/Git")!
        await service.load(wiki)
        XCTAssertEqual(service.preview(for: wiki)?.detail, "Git is a distributed version control system.")
        XCTAssertEqual(service.preview(for: wiki)?.siteName, "Wikipedia")
    }

    @MainActor
    func testTooLargeImagesAreRefusedAndDisabledServiceFetchesNothing() async {
        let http = FakeHTTPClient()
        http.on(where: { _ in true }) { _ in
            HTTPResponse(status: 200, headers: ["content-type": "image/png"], body: Data(count: ChatLinkPreviewService.maxImageBytes + 1))
        }
        let service = ChatLinkPreviewService(http: http, resolver: publicDNS)
        let data = await service.imageData("https://img.example.com/huge.png")
        XCTAssertNil(data)
        let store = InMemoryKeyValueStore()
        let off = ChatLinkPreviewService(http: http, store: store, resolver: publicDNS)
        off.setEnabled(false)
        XCTAssertEqual(store.bool(forKey: ChatLinkPreviewService.enabledKey), false)
        let before = http.requests.count
        await off.load(URL(string: "https://example.org/")!)
        XCTAssertEqual(http.requests.count, before)
    }

    @MainActor
    func testRefusedCrawlerIsRetriedAsABrowserAndStackOverflowUsesTheApi() async {
        let http = FakeHTTPClient()
        http.on(where: { $0.url.host == "shy.example.com" }) { request in
            if (request.headers["User-Agent"] ?? "").contains("ForkMeshBot") { return HTTPResponse(status: 403, text: "no bots") }
            return HTTPResponse(
                status: 200, headers: ["content-type": "text/html"],
                text: "<html><head></head><body><meta property=\"og:title\" content=\"Late tags\"><meta property=\"og:image\" content=\"https://shy.example.com/i.png\"></body></html>")
        }
        http.on(path: "/2.3/questions/11227809", json: .object([
            "items": .array([.object([
                "title": "Why is processing a sorted array &quot;faster&quot;?", "score": 27000, "answer_count": 26,
                "is_answered": true, "owner": .object(["display_name": "GManNickG"]),
            ])]),
        ]))
        let service = ChatLinkPreviewService(http: http, resolver: publicDNS)
        let shy = URL(string: "https://shy.example.com/post")!
        await service.load(shy)
        XCTAssertEqual(service.preview(for: shy)?.title, "Late tags")
        XCTAssertEqual(http.requests.filter { $0.url.host == "shy.example.com" }.count, 2)
        let question = URL(string: "https://stackoverflow.com/questions/11227809/why-is-processing")!
        await service.load(question)
        XCTAssertEqual(service.preview(for: question)?.title, "Why is processing a sorted array \"faster\"?")
        XCTAssertEqual(service.preview(for: question)?.detail, "27000 votes \u{00B7} 26 answers \u{00B7} answered")
        XCTAssertEqual(service.preview(for: question)?.author, "GManNickG")
        XCTAssertEqual(http.requests.last?.url.query, "site=stackoverflow&filter=default")
        XCTAssertEqual(
            ChatLinkPreviewParsing.stackExchangeQuestion(URL(string: "https://unix.stackexchange.com/questions/4/x")!)?.site, "unix")
    }

    func testResolvedAddressesMustAllBePublic() {
        func v6(_ text: String) -> [UInt8] { MeshAddress(text)!.rawAddress }
        XCTAssertTrue(ChatLinkAddresses.isPublic([93, 184, 216, 34]))
        for privateV4: [UInt8] in [[10, 0, 0, 1], [127, 0, 0, 1], [169, 254, 169, 254], [192, 168, 1, 1], [172, 16, 0, 1],
            [100, 64, 0, 1], [0, 0, 0, 0], [224, 0, 0, 1], [255, 255, 255, 255]]
        {
            XCTAssertFalse(ChatLinkAddresses.isPublic(privateV4), "\(privateV4)")
        }
        XCTAssertTrue(ChatLinkAddresses.isPublic(v6("2606:4700::1111")))
        for text in ["::1", "::", "fe80::1", "fd00::1", "fc00::1", "ff02::1", "fec0::1", "2001:db8::1", "::ffff:10.0.0.1",
            "64:ff9b::a00:1", "2002:0a00:0001::1", "2001::1"]
        {
            XCTAssertFalse(ChatLinkAddresses.isPublic(v6(text)), text)
        }
        XCTAssertTrue(ChatLinkAddresses.isPublic(v6("::ffff:93.184.216.34")))
        XCTAssertTrue(ChatLinkAddresses.isPublic(v6("64:ff9b::5db8:d822")))
        XCTAssertFalse(ChatLinkAddresses.allPublic([]), "a name that does not resolve is refused")
        XCTAssertFalse(ChatLinkAddresses.allPublic([[93, 184, 216, 34], [10, 0, 0, 1]]), "one private answer is enough to refuse")
        XCTAssertFalse(ChatLinkAddresses.isPublic([1, 2, 3]))
    }

    @MainActor
    func testANameThatResolvesPrivatelyIsNeverFetchedNorRedirectedTo() async {
        let http = FakeHTTPClient()
        http.on(where: { _ in true }) { request in
            if request.url.host == "jump.example.com" {
                return HTTPResponse(status: 302, headers: ["location": "https://rebind.example.com/admin"], text: "")
            }
            return HTTPResponse(
                status: 200, headers: ["content-type": "text/html"], text: "<head><meta property=og:title content=\"Inside\"></head>")
        }
        let addresses: [String: [[UInt8]]] = [
            "intranet.example.com": [[10, 0, 0, 5]],
            "rebind.example.com": [[127, 0, 0, 1]],
            "jump.example.com": [[93, 184, 216, 34]],
            "v6local.example.com": [MeshAddress("fe80::1")!.rawAddress],
            "mixed.example.com": [[93, 184, 216, 34], MeshAddress("::1")!.rawAddress],
            "public.example.com": [MeshAddress("2606:4700::1111")!.rawAddress],
        ]
        let service = ChatLinkPreviewService(http: http, resolver: { host in addresses[host] ?? [] })
        for host in ["intranet", "v6local", "mixed", "unknown"] {
            let url = URL(string: "https://\(host).example.com/")!
            await service.load(url)
            XCTAssertEqual(service.state(for: url), .none, host)
        }
        let jump = URL(string: "https://jump.example.com/go")!
        await service.load(jump)
        XCTAssertEqual(service.state(for: jump), .none)
        XCTAssertTrue(http.requests.contains { $0.url.host == "jump.example.com" })
        XCTAssertFalse(
            http.requests.contains { ["intranet.example.com", "rebind.example.com", "v6local.example.com", "mixed.example.com"].contains($0.url.host ?? "") },
            "\(http.requests.map(\.url))")
        let fine = URL(string: "https://public.example.com/")!
        await service.load(fine)
        XCTAssertEqual(service.preview(for: fine)?.title, "Inside")
        let image = await service.imageData("https://intranet.example.com/a.png")
        XCTAssertNil(image)
    }

    @MainActor
    func testThePreviewCacheBelongsToOneProject() async throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("fm-link-projects-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: dir) }
        let http = FakeHTTPClient()
        http.on(where: { _ in true }) { _ in
            HTTPResponse(status: 200, headers: ["content-type": "text/html"], text: "<head><meta property=og:title content=\"Alpha\"></head>")
        }
        var project = "alpha"
        let service = ChatLinkPreviewService(
            http: http, file: { dir.appendingPathComponent(ChatLinkPreviewService.fileName(project: project)) },
            resolver: publicDNS)
        let url = URL(string: "https://example.org/post")!
        await service.load(url)
        XCTAssertEqual(service.preview(for: url)?.title, "Alpha")
        service.leaveProject()
        let alphaFile = dir.appendingPathComponent(ChatLinkPreviewService.fileName(project: "alpha"))
        let saved = await waitUntil { FileManager.default.fileExists(atPath: alphaFile.path) }
        XCTAssertTrue(saved)

        project = "beta"
        service.warm()
        XCTAssertEqual(service.state(for: url), .idle, "beta never sees what alpha looked at")
        service.leaveProject()
        project = "alpha"
        service.warm()
        XCTAssertEqual(service.preview(for: url)?.title, "Alpha")
        XCTAssertNotEqual(ChatLinkPreviewService.fileName(project: "alpha"), ChatLinkPreviewService.fileName(project: "beta"))
    }

    @MainActor
    func testOnlyOwnAndPrivateMessagesFetchPreviewsByThemselves() async throws {
        let h = try await RelayHarness.start()
        let hub = ChatHub(relay: h.relay, mesh: makeMesh(h.device))
        let office = OfficeChannelService(
            api: FakeAccountApi(), connector: FakeWebSocketConnector(), secureStore: InMemorySecureStore(),
            origin: { URL(string: "https://app.forkmesh.com") }, selfId: { "me-node" }, displayName: { "Me" })
        office.restore([
            "channels": [
                ["id": .string(String(repeating: "a", count: 32)), "name": "town", "visibility": "public"],
                ["id": .string(String(repeating: "b", count: 32)), "name": "team", "visibility": "private"],
            ]
        ])
        hub.office = office
        XCTAssertFalse(hub.autoLoadsLinkPreviews(in: "#general", fromSelf: false), "world-general is everyone's")
        XCTAssertFalse(hub.autoLoadsLinkPreviews(in: meshConversation, fromSelf: false))
        XCTAssertFalse(hub.autoLoadsLinkPreviews(in: "#office/town", fromSelf: false), "a public channel")
        XCTAssertTrue(hub.autoLoadsLinkPreviews(in: "#office/team", fromSelf: false))
        XCTAssertTrue(hub.autoLoadsLinkPreviews(in: "direct:" + String(repeating: "c", count: 32), fromSelf: false))
        XCTAssertTrue(hub.autoLoadsLinkPreviews(in: "#general", fromSelf: true), "your own links load as before")
    }

    func testCacheRoundTrip() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("fm-link-\(UUID().uuidString)")
        let file = dir.appendingPathComponent("previews.json")
        let saved = [ChatLinkPreview(url: "https://a.com", title: "A", siteKey: "github", kind: .code, fetchedAt: Date(timeIntervalSince1970: 50))]
        ChatLinkPreviewService.write(saved, to: file)
        XCTAssertEqual(ChatLinkPreviewService.read(file), saved)
    }
}
