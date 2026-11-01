import FMBase
import FMCrypto
import FMNet
import XCTest
@testable import FMPackages

final class MastodonCoreTests: XCTestCase {
    func testServerNormalize() {
        XCTAssertEqual(MastodonServer.normalize("mastodon.social"), "https://mastodon.social")
        XCTAssertEqual(MastodonServer.normalize("  https://Mastodon.Social/ "), "https://mastodon.social")
        XCTAssertEqual(MastodonServer.normalize("@me@fosstodon.org"), "https://fosstodon.org")
        XCTAssertEqual(MastodonServer.normalize("me@hachyderm.io"), "https://hachyderm.io")
        XCTAssertEqual(MastodonServer.normalize("social.example:8443"), "https://social.example:8443")
        XCTAssertNil(MastodonServer.normalize("http://mastodon.social"))
        XCTAssertNil(MastodonServer.normalize("https://mastodon.social/about"))
        XCTAssertNil(MastodonServer.normalize("https://a@mastodon.social"))
        XCTAssertNil(MastodonServer.normalize("https://mastodon.social/?x=1"))
        XCTAssertNil(MastodonServer.normalize("localhost"))
        XCTAssertNil(MastodonServer.normalize(""))
        XCTAssertNil(MastodonServer.normalize("mastodon social"))
    }

    func testPKCEAndAuthorizeURL() {
        let pkce = MastodonPKCE(verifier: "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk", state: "abc")
        XCTAssertEqual(pkce.challenge, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM")
        let made = MastodonPKCE.make()
        XCTAssertEqual(made.verifier.count, 43)
        XCTAssertEqual(made.state.count, 32)
        XCTAssertNotEqual(made.verifier, MastodonPKCE.make().verifier)
        let url = MastodonOAuth.authorizeURL(
            server: "https://mastodon.social", clientId: "id 1", redirect: MastodonOAuth.redirect, state: "s", challenge: "c")
        XCTAssertEqual(
            url?.absoluteString,
            "https://mastodon.social/oauth/authorize?response_type=code&client_id=id%201&redirect_uri=forkmesh%3A%2F%2Fmastodon%2Fcallback&scope=read%20write%20follow&state=s&code_challenge=c&code_challenge_method=S256")
    }

    func testCallbackCode() {
        XCTAssertEqual(MastodonOAuth.code(from: "forkmesh://mastodon/callback?code=abcd1234&state=s1", state: "s1"), "abcd1234")
        XCTAssertNil(MastodonOAuth.code(from: "forkmesh://mastodon/callback?code=abcd1234&state=other", state: "s1"))
        XCTAssertNil(MastodonOAuth.code(from: "forkmesh://mastodon/callback?error=access_denied&code=abcd", state: "s1"))
        XCTAssertEqual(MastodonOAuth.code(from: " Zx_9-codeValue \n", state: "s1"), "Zx_9-codeValue")
        XCTAssertEqual(MastodonOAuth.code(from: "https://mastodon.social/oauth/authorize/native?code=abcdef", state: "s1"), "abcdef")
        XCTAssertNil(MastodonOAuth.code(from: "not a code", state: "s1"))
        XCTAssertNil(MastodonOAuth.code(from: "abc", state: "s1"))
        XCTAssertEqual(MastodonOAuth.callbackError("forkmesh://mastodon/callback?error=access_denied"), "access_denied")
        XCTAssertTrue(MastodonOAuth.isCallback(URL(string: "forkmesh://mastodon/callback?code=x")!))
        XCTAssertFalse(MastodonOAuth.isCallback(URL(string: "forkmesh://mastodon")!))
    }

    func testTextLength() {
        XCTAssertEqual(MastodonText.length("hello"), 5)
        XCTAssertEqual(MastodonText.length("see https://example.com/a/very/long/path/that/goes/on"), 4 + 23)
        XCTAssertEqual(MastodonText.length("hi @alice@mastodon.social"), 9)
        XCTAssertEqual(MastodonText.length("👍🏽"), 2)
        XCTAssertEqual(MastodonText.remaining(limit: 500, text: "abc", spoiler: "cw", warning: true), 495)
        XCTAssertEqual(MastodonText.remaining(limit: 500, text: "abc", spoiler: "cw", warning: false), 497)
    }

    func testHTMLSegments() {
        let html = """
        <p>Hello <span class="h-card"><a href="https://mastodon.social/@alice" class="u-url mention">@<span>alice</span></a></span> &amp; friends</p>\
        <p>Read <a href="https://example.com/some/long/path" rel="nofollow noopener" target="_blank">\
        <span class="invisible">https://</span><span class="ellipsis">example.com/some/lo</span><span class="invisible">ng/path</span></a>\
        <br>tagged <a href="https://mastodon.social/tags/Swift" class="mention hashtag" rel="tag">#<span>Swift</span></a> &#x1F600;</p>
        """
        let mentions = [MastodonMention(id: "42", acct: "alice", url: "https://mastodon.social/@alice")]
        let segments = MastodonHTML.segments(html, mentions: mentions)
        XCTAssertEqual(segments, [
            .text("Hello "),
            .mention(label: "@alice", account: "42"),
            .text(" & friends\n\nRead "),
            .link(label: "example.com/some/lo…", url: "https://example.com/some/long/path"),
            .text("\ntagged "),
            .hashtag(label: "#Swift", name: "swift"),
            .text(" 😀"),
        ])
        XCTAssertEqual(MastodonHTML.text("<p>a</p><p>b &lt;3</p>"), "a\n\nb <3")
        XCTAssertEqual(MastodonHTML.text("<p>x <a href=\"javascript:alert(1)\">y</a></p>"), "x y")
        XCTAssertEqual(MastodonHTML.segments("<p>x <a href=\"javascript:alert(1)\">y</a></p>"), [.text("x y")])
    }

    func testPaging() {
        let header = "<https://mastodon.social/api/v1/timelines/home?max_id=10>; rel=\"next\", <https://mastodon.social/api/v1/timelines/home?min_id=20>; rel=\"prev\""
        XCTAssertEqual(MastodonPaging.next(header, server: "https://mastodon.social"), "https://mastodon.social/api/v1/timelines/home?max_id=10")
        XCTAssertNil(MastodonPaging.next(header, server: "https://other.example"))
        XCTAssertNil(MastodonPaging.next("<https://mastodon.social/x>; rel=\"prev\"", server: "https://mastodon.social"))
        XCTAssertNil(MastodonPaging.next(nil, server: "https://mastodon.social"))
    }

    func testModels() {
        let status = MastodonStatus(JSON.parseObject(MastodonFixtures.boost))
        XCTAssertEqual(status?.id, "200")
        XCTAssertEqual(status?.shown.id, "100")
        XCTAssertEqual(status?.shown.account.displayName, "Alice")
        XCTAssertEqual(status?.account.displayName, "bob")
        XCTAssertEqual(status?.shown.createdAt, 1_761_998_400_123)
        XCTAssertEqual(status?.shown.media.first?.previewUrl, "https://files.example/p.jpg")
        XCTAssertEqual(status?.shown.poll?.percent(status!.shown.poll!.options[0]), 75)
        XCTAssertEqual(status?.shown.plainText, "Hi there")
        var liked = status!.shown
        liked.favourited = true
        let replaced = status!.replacing(liked)
        XCTAssertEqual(replaced.shown.favourited, true)
        XCTAssertEqual(replaced.id, "200")
        let limits = MastodonLimits(JSON.parseObject(#"{"configuration":{"statuses":{"max_characters":1000,"max_media_attachments":6}}}"#))
        XCTAssertEqual(limits, MastodonLimits(maxCharacters: 1000, maxMedia: 6))
        XCTAssertEqual(MastodonLimits(JSONObject()), .standard)
        let note = MastodonNotification(JSON.parseObject(#"{"id":"9","type":"follow","created_at":"2025-11-01T12:00:00Z","account":{"id":"1","username":"carol","acct":"carol@x.example","display_name":""}}"#))
        XCTAssertEqual(note?.summary, "carol followed you")
        XCTAssertEqual(note?.account.handle(server: "https://mastodon.social"), "@carol@x.example")
        let session = MastodonSession(server: "https://mastodon.social", token: "t", accountId: "1", acct: "me")
        XCTAssertEqual(MastodonSession(session.json), session)
        XCTAssertEqual(session.handle, "@me@mastodon.social")
    }

    func testDraftBody() {
        let plain = MastodonDraft(text: "hi").json
        XCTAssertEqual(JSON.encode(plain), #"{"status":"hi","visibility":"public"}"#)
        let full = MastodonDraft(text: "hi", visibility: "private", spoiler: "cw", replyTo: "5", mediaIds: ["7"]).json
        XCTAssertEqual(full.str("spoiler_text"), "cw")
        XCTAssertEqual(full.bool("sensitive"), true)
        XCTAssertEqual(full.str("in_reply_to_id"), "5")
        XCTAssertEqual(full.strings("media_ids"), ["7"])
    }
}

enum MastodonFixtures {
    static let account = #"{"id":"1","username":"me","acct":"me","display_name":"Me","avatar":"https://files.example/a.png"}"#
    static let alice = #"{"id":"42","username":"alice","acct":"alice","display_name":"Alice"}"#
    static let original = #"""
    {"id":"100","url":"https://mastodon.social/@alice/100","created_at":"2025-11-01T12:00:00.123Z","content":"<p>Hi there</p>",
     "spoiler_text":"","visibility":"public","replies_count":1,"reblogs_count":2,"favourites_count":3,
     "favourited":false,"reblogged":false,"bookmarked":false,"account":\#(alice),
     "media_attachments":[{"id":"m1","type":"image","url":"https://files.example/f.jpg","preview_url":"https://files.example/p.jpg"}],
     "mentions":[],"tags":[],"poll":{"options":[{"title":"Yes","votes_count":3},{"title":"No","votes_count":1}],"votes_count":4,"expired":false}}
    """#
    static let boost = #"{"id":"200","created_at":"2025-11-01T13:00:00Z","content":"","account":{"id":"7","username":"bob","acct":"bob"},"reblog":\#(original)}"#
}

final class MastodonClientTests: XCTestCase {
    private let server = "https://mastodon.social"

    private func json(_ text: String) -> JSONValue { (try? JSON.parse(text)) ?? .null }

    @MainActor
    func testSignInFlowStoresSession() async throws {
        let http = FakeHTTPClient()
        http.on("POST", path: "/api/v1/apps", json: json(#"{"client_id":"cid","client_secret":"sec"}"#))
        http.on("POST", path: "/oauth/token", json: json(#"{"access_token":"tok"}"#))
        http.on("GET", path: "/api/v1/accounts/verify_credentials", json: json(MastodonFixtures.account))
        http.on("GET", path: "/api/v2/instance", json: json(#"{"configuration":{"statuses":{"max_characters":700}}}"#))
        let store = InMemorySecureStore()
        let hub = MastodonHub(store: store, http: http)
        hub.server = "@me@Mastodon.social"
        let url = await hub.start()
        let pending = try XCTUnwrap(hub.pending)
        XCTAssertEqual(url, pending.authorizeURL)
        XCTAssertEqual(pending.redirect, MastodonOAuth.redirect)
        XCTAssertNotNil(store.read("mastodon.pending.v1"))
        let register = try XCTUnwrap(http.requests.first)
        XCTAssertEqual(register.headers["Content-Type"], "application/x-www-form-urlencoded")
        XCTAssertEqual(register.bodyText, "client_name=ForkMesh&redirect_uris=forkmesh%3A%2F%2Fmastodon%2Fcallback%0Aurn%3Aietf%3Awg%3Aoauth%3A2.0%3Aoob&scopes=read%20write%20follow&website=https%3A%2F%2Fforkmesh.com")

        let reloaded = MastodonHub(store: store, http: http)
        await reloaded.finish("forkmesh://mastodon/callback?code=thecode&state=\(pending.state)")
        XCTAssertNil(reloaded.notice)
        XCTAssertEqual(reloaded.session?.token, "tok")
        XCTAssertEqual(reloaded.session?.acct, "me")
        XCTAssertEqual(reloaded.limits.maxCharacters, 700)
        XCTAssertNil(store.read("mastodon.pending.v1"))
        let exchange = try XCTUnwrap(http.requests.first { $0.url.path == "/oauth/token" })
        XCTAssertTrue(exchange.bodyText.contains("code=thecode"))
        XCTAssertTrue(exchange.bodyText.contains("code_verifier=\(pending.verifier)"))
        XCTAssertTrue(exchange.bodyText.contains("client_secret=sec"))
        let verify = try XCTUnwrap(http.requests.first { $0.url.path == "/api/v1/accounts/verify_credentials" })
        XCTAssertEqual(verify.headers["Authorization"], "Bearer tok")
        XCTAssertTrue(http.requests.allSatisfy { $0.url.host == "mastodon.social" })
        XCTAssertEqual(MastodonHub(store: store, http: http).session?.token, "tok")
    }

    @MainActor
    func testWrongStateIsRefused() async {
        let http = FakeHTTPClient()
        http.on("POST", path: "/api/v1/apps", json: json(#"{"client_id":"cid","client_secret":"sec"}"#))
        let hub = MastodonHub(store: InMemorySecureStore(), http: http)
        hub.server = "mastodon.social"
        _ = await hub.start(outOfBand: true)
        XCTAssertEqual(hub.pending?.redirect, MastodonOAuth.outOfBand)
        XCTAssertTrue(hub.showCode)
        await hub.finish("forkmesh://mastodon/callback?code=thecode&state=wrong")
        XCTAssertNil(hub.session)
        XCTAssertEqual(hub.notice?.isError, true)
        XCTAssertFalse(http.requests.contains { $0.url.path == "/oauth/token" })
    }

    @MainActor
    func testTokenSignInAndRevokedSession() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/v1/accounts/verify_credentials", json: json(MastodonFixtures.account))
        http.on("GET", path: "/api/v2/instance", status: 404, json: .object([:]))
        let store = InMemorySecureStore()
        let hub = MastodonHub(store: store, http: http)
        hub.server = "mastodon.social"
        await hub.signIn(token: " tok ")
        XCTAssertEqual(hub.session?.token, "tok")
        XCTAssertEqual(hub.limits, .standard)

        http.on("GET", path: "/api/v1/timelines/home", status: 401, json: json(#"{"error":"The access token was revoked"}"#))
        let feed = MastodonFeedModel(hub: hub, source: .timeline(.home))
        await feed.load()
        XCTAssertNil(hub.session)
        XCTAssertNil(store.read("mastodon.session.v1"))
        XCTAssertEqual(hub.server, "mastodon.social")
        XCTAssertEqual(hub.notice?.message, "Your Mastodon sign-in was revoked on mastodon.social. Sign in again.")
    }

    @MainActor
    func testTimelinePagingAndToggles() async throws {
        let http = FakeHTTPClient()
        let store = InMemorySecureStore(["mastodon.session.v1": JSON.encode(MastodonSession(server: server, token: "tok", accountId: "1", acct: "me").json)])
        let next = "\(server)/api/v1/timelines/home?limit=30&max_id=100"
        http.on("GET", where: { $0.url.path == "/api/v1/timelines/home" && $0.url.query == "limit=30" }) { _ in
            HTTPResponse(status: 200, headers: ["Link": "<\(next)>; rel=\"next\""], body: Data("[\(MastodonFixtures.boost)]".utf8))
        }
        http.on("GET", where: { $0.url.query?.contains("max_id=100") == true }) { _ in
            HTTPResponse(status: 200, body: Data("[\(MastodonFixtures.original)]".utf8))
        }
        let favourited = MastodonFixtures.original.replacingOccurrences(of: #""favourited":false"#, with: #""favourited":true"#)
        http.on("POST", path: "/api/v1/statuses/100/favourite") { _ in HTTPResponse(status: 200, body: Data(favourited.utf8)) }
        http.on("POST", path: "/api/v1/statuses/100/bookmark", status: 500, json: json(#"{"error":"Nope"}"#))
        let hub = MastodonHub(store: store, http: http)
        let feed = MastodonFeedModel(hub: hub, source: .timeline(.home))
        await feed.load()
        XCTAssertEqual(feed.statuses.map(\.id), ["200"])
        XCTAssertEqual(feed.next, next)
        await feed.load(append: true)
        XCTAssertEqual(feed.statuses.map(\.id), ["200", "100"])
        XCTAssertNil(feed.next)

        let shown = feed.statuses[0].shown
        await feed.toggleFavourite(shown)
        XCTAssertTrue(feed.statuses[0].shown.favourited)
        XCTAssertTrue(feed.statuses[1].favourited)
        await feed.toggleBookmark(shown)
        XCTAssertFalse(feed.statuses[0].shown.bookmarked)
        XCTAssertEqual(feed.notice?.message, "Nope")
    }

    @MainActor
    func testComposerPostsWithMediaAndReplyMentions() async throws {
        let http = FakeHTTPClient()
        let store = InMemorySecureStore(["mastodon.session.v1": JSON.encode(MastodonSession(server: server, token: "tok", accountId: "1", acct: "me").json)])
        http.on("POST", path: "/api/v2/media") { _ in HTTPResponse(status: 202, json: .object(["id": .string("m9"), "url": .null])) }
        http.on("GET", path: "/api/v1/media/m9", json: .object(["id": .string("m9"), "url": .string("https://files.example/m9.jpg")]))
        http.on("POST", path: "/api/v1/statuses") { _ in HTTPResponse(status: 200, body: Data(MastodonFixtures.original.utf8)) }
        let hub = MastodonHub(store: store, http: http)
        let parent = try XCTUnwrap(MastodonStatus(JSON.parseObject(MastodonFixtures.original)))
        let composer = MastodonComposer(hub: hub, replyTo: parent)
        XCTAssertEqual(composer.text, "@alice ")
        composer.text += "thanks"
        composer.warning = true
        composer.spoiler = "food"
        composer.add(MastodonUpload(data: Data([1, 2, 3]), filename: "a.jpg", mimeType: "image/jpeg", description: "a cat"))
        XCTAssertTrue(composer.canPost)
        let pausing = MastodonClient(server: server, token: "tok", http: http, pause: { _ in })
        let id = try await pausing.upload(MastodonUpload(data: Data([1]), filename: "b.jpg", mimeType: "image/jpeg"))
        XCTAssertEqual(id, "m9")
        http.reset()
        http.on("POST", path: "/api/v2/media", json: .object(["id": .string("m1"), "url": .string("https://files.example/m1.jpg")]))
        http.on("POST", path: "/api/v1/statuses") { _ in HTTPResponse(status: 200, body: Data(MastodonFixtures.original.utf8)) }
        let posted = await composer.submit()
        XCTAssertEqual(posted?.id, "100")
        let upload = try XCTUnwrap(http.requests.first { $0.url.path == "/api/v2/media" })
        XCTAssertTrue(upload.headers["Content-Type"]?.hasPrefix("multipart/form-data; boundary=") == true)
        XCTAssertTrue(upload.bodyText.contains("a cat"))
        let post = try XCTUnwrap(http.requests.first { $0.url.path == "/api/v1/statuses" })
        let body = JSON.parseObject(post.body ?? Data())
        XCTAssertEqual(body.str("status"), "@alice thanks")
        XCTAssertEqual(body.str("in_reply_to_id"), "100")
        XCTAssertEqual(body.str("spoiler_text"), "food")
        XCTAssertEqual(body.strings("media_ids"), ["m1"])
        XCTAssertFalse(post.headers["Idempotency-Key"]?.isEmpty ?? true)
    }

    @MainActor
    func testSignOutRevokes() async {
        let http = FakeHTTPClient()
        http.on("POST", path: "/oauth/revoke", json: .object([:]))
        let store = InMemorySecureStore(["mastodon.session.v1": JSON.encode(MastodonSession(server: server, token: "tok", clientId: "c", clientSecret: "s").json)])
        let hub = MastodonHub(store: store, http: http)
        await hub.signOut()
        XCTAssertNil(hub.session)
        XCTAssertNil(store.read("mastodon.session.v1"))
        XCTAssertEqual(http.lastRequest?.bodyText, "client_id=c&client_secret=s&token=tok")
    }

    @MainActor
    func testProfileFollow() async {
        let http = FakeHTTPClient()
        let store = InMemorySecureStore(["mastodon.session.v1": JSON.encode(MastodonSession(server: server, token: "tok", accountId: "1").json)])
        http.on("GET", path: "/api/v1/accounts/42", json: json(MastodonFixtures.alice))
        http.on("GET", path: "/api/v1/accounts/relationships", json: json(#"[{"id":"42","following":false,"followed_by":true}]"#))
        http.on("GET", path: "/api/v1/accounts/42/statuses", json: json("[\(MastodonFixtures.original)]"))
        http.on("POST", path: "/api/v1/accounts/42/follow", json: json(#"{"id":"42","following":true}"#))
        let profile = MastodonProfileModel(hub: MastodonHub(store: store, http: http), accountId: "42")
        await profile.load()
        XCTAssertEqual(profile.account?.displayName, "Alice")
        XCTAssertEqual(profile.followLabel, "Follow back")
        XCTAssertEqual(profile.feed.statuses.count, 1)
        await profile.toggleFollow()
        XCTAssertEqual(profile.followLabel, "Unfollow")
        XCTAssertTrue(http.requests.contains { $0.url.absoluteString.hasSuffix("relationships?id%5B%5D=42") })
    }
}
