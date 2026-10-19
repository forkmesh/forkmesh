import FMApi
import FMBase
import XCTest

@testable import FMTools

@MainActor
final class InstanceFeedTests: XCTestCase {
    let a = String(repeating: "a", count: 32)
    let b = String(repeating: "b", count: 32)
    let c = String(repeating: "c", count: 32)

    func post(_ id: String, author: String = "ada", text: String = "hello", extra: JSONObject = [:]) -> JSONObject {
        var json: JSONObject = [
            "type": "post", "id": .string(id), "author": .string(author), "authorName": .string(author.capitalized),
            "text": .string(text), "createdAt": 1_789_561_860_000, "replies": 2, "boosts": 1, "favourites": 3,
        ]
        json.merge(extra)
        return json
    }

    func timeline(_ posts: [JSONObject], next: String = "") -> JSONObject {
        ["ok": true, "installed": true, "maxLength": 500, "next": .string(next), "posts": .array(posts.map { .object($0) })]
    }

    func testTimelineParsesPostsAndBoosts() async {
        let api = FakePackageApi()
        api.replies["GET "] = timeline([
            post(a, extra: ["favourited": true, "spoiler": "cw"]),
            ["type": "boost", "id": .string(c), "booster": "grace", "boosterName": "Grace", "post": .object(post(b, author: "lin"))],
            ["type": "post", "id": "not-an-id"],
        ], next: "5-" + b)
        let model = InstanceFeedModel(api: InstanceFeedAPI(call: api.call))
        await model.load()
        XCTAssertTrue(model.installed)
        XCTAssertEqual(model.entries.map(\.id), [a, c])
        XCTAssertEqual(model.entries[0].post.authorName, "Ada")
        XCTAssertTrue(model.entries[0].post.favourited)
        XCTAssertEqual(model.entries[0].post.spoiler, "cw")
        XCTAssertTrue(model.entries[1].isBoost)
        XCTAssertEqual(model.entries[1].boosterName, "Grace")
        XCTAssertEqual(model.entries[1].post.author, "lin")
        XCTAssertEqual(model.next, "5-" + b)

        api.replies["GET ?before=5-" + b] = timeline([post(String(repeating: "d", count: 32))])
        await model.load(append: true)
        XCTAssertEqual(model.entries.count, 3)
        XCTAssertEqual(model.next, "")
    }

    func testProfileAndThreadPaths() async {
        let api = FakePackageApi()
        var profile = timeline([post(a)])
        profile["profile"] = ["name": "ada", "displayName": "Ada", "posts": 4, "since": 1]
        api.replies["GET ?author=ada"] = profile
        api.replies["GET /posts/" + a] = [
            "ok": true, "post": .object(post(a)), "ancestors": [],
            "replies": [.object(post(b, author: "grace", extra: ["replyTo": .string(a), "replyToAuthor": "ada"]))],
        ]
        let model = InstanceFeedModel(api: InstanceFeedAPI(call: api.call))
        await model.showProfile("Ada")
        XCTAssertEqual(model.view, .profile("ada"))
        XCTAssertEqual(model.profile?.displayName, "Ada")
        XCTAssertEqual(model.profile?.posts, 4)
        await model.showThread(a)
        XCTAssertEqual(model.thread?.post.id, a)
        XCTAssertEqual(model.thread?.replies.first?.replyToAuthor, "ada")
        XCTAssertTrue(model.entries.isEmpty)
        let before = api.calls.count
        await model.showThread("../etc")
        await model.showProfile("bad name")
        XCTAssertEqual(api.calls.count, before)
        XCTAssertEqual(InstanceFeed.timelineSuffix(author: "ada", before: "5-" + b), "?author=ada&before=5-" + b)
        XCTAssertEqual(InstanceFeed.timelineSuffix(author: "Bad", before: nil), "")
    }

    func testComposeRepliesAndToggles() async {
        let api = FakePackageApi()
        api.replies["GET "] = timeline([post(a)])
        api.replies["POST /posts"] = ["ok": true, "post": .object(post(c, author: "alice", text: "Hi all"))]
        api.replies["POST /posts/\(a)/favourite"] = ["ok": true, "post": .object(post(a, extra: ["favourited": true, "favourites": 4]))]
        let model = InstanceFeedModel(api: InstanceFeedAPI(call: api.call))
        await model.load()
        XCTAssertFalse(model.canPost)
        model.text = "Hi all"
        model.showSpoiler = true
        model.spoiler = "  spoilers "
        XCTAssertEqual(model.remaining, 494)
        await model.submit()
        XCTAssertEqual(api.bodies("POST", "/posts").last, ["text": "Hi all", "spoiler": "spoilers", "replyTo": ""])
        XCTAssertEqual(model.entries.first?.id, c)
        XCTAssertEqual(model.text, "")
        XCTAssertFalse(model.showSpoiler)

        await model.toggleFavourite(model.entries[1].post)
        XCTAssertTrue(model.entries[1].post.favourited)
        XCTAssertEqual(model.entries[1].post.favourites, 4)
        await model.toggleFavourite(model.entries[1].post)
        XCTAssertEqual(api.calls.last?.method, "DELETE")
        XCTAssertEqual(api.calls.last?.suffix, "/posts/\(a)/favourite")
        await model.toggleBoost(model.entries[1].post)
        XCTAssertTrue(api.calls.contains { $0.method == "POST" && $0.suffix == "/posts/\(a)/boost" })
        XCTAssertEqual(api.calls.last?.suffix, "")

        XCTAssertEqual(model.entries.map(\.id), [a])
        model.startReply(model.entries[0].post)
        XCTAssertEqual(model.text, "@ada ")
        model.text += "thanks"
        api.replies["GET /posts/" + a] = ["ok": true, "post": .object(post(a, extra: ["canDelete": true])), "ancestors": [], "replies": []]
        await model.submit()
        XCTAssertEqual(api.bodies("POST", "/posts").last?["replyTo"], .string(a))
        XCTAssertEqual(model.view, .thread(a))
        XCTAssertNil(model.replyTo)

        await model.delete(model.thread!.post)
        XCTAssertTrue(api.calls.contains { $0.method == "DELETE" && $0.suffix == "/posts/" + a })
        XCTAssertEqual(model.view, .timeline)
    }

    func testNotInstalledAndErrors() async {
        let api = FakePackageApi()
        api.replies["GET "] = ["ok": true, "installed": false, "actor": "alice"]
        let model = InstanceFeedModel(api: InstanceFeedAPI(call: api.call))
        await model.load()
        XCTAssertTrue(model.showsInstall)
        let blocked = FakePackageApi()
        blocked.failures["GET /posts/" + a] = OrgApiException("plugin_not_installed", "plugin not installed")
        let other = InstanceFeedModel(api: InstanceFeedAPI(call: blocked.call), view: .thread(a))
        await other.load()
        XCTAssertFalse(other.installed)
        XCTAssertTrue(other.showsInstall)
        let broken = FakePackageApi()
        broken.failures["GET "] = OrgApiException("http_500", "The server answered 500.")
        let third = InstanceFeedModel(api: InstanceFeedAPI(call: broken.call))
        await third.load()
        XCTAssertTrue(third.showsError)
        XCTAssertEqual(third.error, "The server answered 500.")
    }

    func testSegmentsMirrorTheWebRules() {
        XCTAssertEqual(InstanceFeed.segments("hi @Ada, see https://forkmesh.com/x. mail me@bob.dev"), [
            .text("hi "), .mention("ada"), .text(", see "), .link("https://forkmesh.com/x"), .text(". mail me@bob.dev"),
        ])
        XCTAssertEqual(InstanceFeed.segments("@lin first"), [.mention("lin"), .text(" first")])
        XCTAssertEqual(InstanceFeed.segments("plain"), [.text("plain")])
        XCTAssertEqual(InstanceFeed.segments("http://not.secure"), [.text("http://not.secure")])
        XCTAssertTrue(InstanceFeed.validPostId(a))
        XCTAssertFalse(InstanceFeed.validPostId(a.uppercased()))
        XCTAssertTrue(InstanceFeed.validAuthor("ada-l"))
        XCTAssertFalse(InstanceFeed.validAuthor("-ada"))
    }

    func testUserFacingNamesSayFeed() {
        XCTAssertEqual(InstanceFeedModel.title, "Feed")
        XCTAssertEqual(InstanceFeedModel.notInstalledTitle, "Install Feed")
        XCTAssertEqual(InstanceFeedModel.unavailableTitle, "The feed is unavailable")
    }
}
