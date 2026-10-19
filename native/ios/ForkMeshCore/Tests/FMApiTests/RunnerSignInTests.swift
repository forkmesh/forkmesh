import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

private let loginA = String(repeating: "a", count: 32)
private let loginB = String(repeating: "b", count: 32)

private func parse(_ text: String) -> JSONValue { try! JSON.parse(text) }

private func runnersPayload() -> JSONValue {
    parse("""
    {"ok":true,"runners":[
      {"name":"fmnext-r1","status":"active","availability":{
        "claude-code":{"binaryFound":true,"loginState":"expired"},
        "codex":{"binaryFound":true,"loginState":"available","auth":{"state":"expiring","expiresAt":1770000000000}}},
       "logins":[{"id":"\(loginA)","provider":"claude-code","state":"running"}]},
      {"name":"fmnext-r2","status":"active","availability":{
        "claude-code":{"binaryFound":true,"loginState":"available","auth":{"state":"expired","message":"job failed"}},
        "codex":{"binaryFound":false,"loginState":"expired"}}},
      {"name":"fmnext-r3","status":"revoked","availability":{
        "claude-code":{"binaryFound":true,"loginState":"expired"}}},
      {"name":"fmnext-r4","status":"active","availability":{
        "claude-code":{"binaryFound":true,"loginState":"missing"},
        "codex":{"binaryFound":true,"loginState":"available","auth":{"state":"ok"}}}},
      {"name":"Bad Name","status":"active","availability":{"claude-code":{"binaryFound":true,"loginState":"expired"}}}
    ]}
    """)
}

private let landedPayload = """
{"ok":true,"items":[
  {"runner":"fmnext-r1","provider":"claude-code","providerLabel":"Claude Code","state":"expired","scope":"pool",
   "canConnect":true,"online":true,"lastSeenAt":1,"expiresAt":0,"title":"fmnext-r1 needs Claude sign-in",
   "connectUrl":"/dashboard/runners?runner=fmnext-r1&provider=claude-code&connect=1","loginId":""},
  {"runner":"fmnext-r2","provider":"codex","providerLabel":"Codex","state":"expiring","scope":"personal",
   "canConnect":false,"online":true,"lastSeenAt":1,"expiresAt":1770000000000,"title":"fmnext-r2: Codex sign-in expires soon",
   "connectUrl":"https://evil.test/x","loginId":"\(String(repeating: "c", count: 32))"}
 ],"count":2,"expired":1,"runners":2,"checkedAt":1,"pollMs":120000}
"""

final class RunnerSignInParsingTests: XCTestCase {
    func testLandedAttentionPayload() {
        let items = RunnerSignIn.parseAttention(parse(landedPayload))
        XCTAssertEqual(items.map(\.id), ["fmnext-r1:claude-code", "fmnext-r2:codex"])
        XCTAssertEqual(items[0].title, "fmnext-r1 needs Claude sign-in")
        XCTAssertEqual(items[0].connectPath, "/dashboard/runners?runner=fmnext-r1&provider=claude-code&connect=1")
        XCTAssertTrue(items[0].canConnect)
        XCTAssertEqual(items[0].openLoginId, "")
        XCTAssertEqual(items[1].title, "fmnext-r2: Codex sign-in expires soon")
        XCTAssertEqual(items[1].connectPath, "")
        XCTAssertEqual(items[1].request.connectPath, "/dashboard/runners?runner=fmnext-r2&provider=codex&connect=1")
        XCTAssertFalse(items[1].canConnect)
        XCTAssertEqual(items[1].openLoginId, String(repeating: "c", count: 32))
        XCTAssertEqual(items[1].expiresAtMs, 1_770_000_000_000)
    }

    func testAPausedRunnerStillAsksForSignIn() {
        let row = parse("""
        {"name":"fmnext-r5","status":"paused","availability":{"claude-code":{"binaryFound":true,"loginState":"expired"}}}
        """).objectOrEmpty
        XCTAssertEqual(RunnerSignIn.derive(runner: row).map(\.id), ["fmnext-r5:claude-code"])
    }

    func testDerivesLikeTheWebBanner() {
        let items = RunnerSignIn.derive(runners: runnersPayload())
        XCTAssertEqual(items.map(\.id), ["fmnext-r1:claude-code", "fmnext-r2:claude-code", "fmnext-r1:codex"])
        XCTAssertEqual(items[0].state, .expired)
        XCTAssertEqual(items[0].openLoginId, loginA)
        XCTAssertEqual(items[1].state, .expired)
        XCTAssertEqual(items[1].message, "job failed")
        XCTAssertEqual(items[2].state, .expiring)
        XCTAssertEqual(items[2].expiresAtMs, 1_770_000_000_000)
        XCTAssertEqual(items[2].openLoginId, "")
    }

    func testFlatAttentionItems() {
        let items = RunnerSignIn.parseAttention(parse("""
        {"attention":[
          {"runner":"fmnext-r1","provider":"claude-code","state":"needs_login","loginId":"\(loginB)"},
          {"runner":"fmnext-r1","provider":"codex","state":"ok"},
          {"name":"fmnext-r2","provider":"codex","auth":{"state":"expiring","expiresAt":5}},
          {"runner":"fmnext-r2","provider":"gemini","state":"expired"},
          {"runner":"fmnext-r5","provider":"claude-code","loginState":"missing","login":{"id":"nothex"}}
        ]}
        """))
        XCTAssertEqual(items.map(\.id), ["fmnext-r1:claude-code", "fmnext-r5:claude-code", "fmnext-r2:codex"])
        XCTAssertEqual(items[0].openLoginId, loginB)
        XCTAssertEqual(items[1].state, .expired)
        XCTAssertEqual(items[1].openLoginId, "")
        XCTAssertEqual(items[2].state, .expiring)
        XCTAssertEqual(items[2].expiresAtMs, 5)
    }

    func testGroupedAndRunnerRowShapes() {
        let grouped = RunnerSignIn.parseAttention(parse("""
        {"items":[{"runner":"fmnext-r1","providers":[{"provider":"codex","state":"expired"},{"provider":"claude-code","state":"available"}],
                   "logins":[{"id":"\(loginA)","provider":"codex","state":"pending"}]}]}
        """))
        XCTAssertEqual(grouped.map(\.id), ["fmnext-r1:codex"])
        XCTAssertEqual(grouped[0].openLoginId, loginA)
        let rows = RunnerSignIn.parseAttention(runnersPayload())
        XCTAssertEqual(rows, RunnerSignIn.derive(runners: runnersPayload()))
        XCTAssertEqual(RunnerSignIn.parseAttention(parse("[]")), [])
        XCTAssertEqual(RunnerSignIn.parseAttention(parse("{\"ok\":true}")), [])
    }

    func testDedupeKeepsExpiredAndSortsExpiredFirst() {
        let items = RunnerSignIn.normalized([
            RunnerAttentionItem(runner: "b", provider: "codex", state: .expiring),
            RunnerAttentionItem(runner: "b", provider: "codex", state: .expired),
            RunnerAttentionItem(runner: "b", provider: "codex", state: .expiring),
            RunnerAttentionItem(runner: "a", provider: "codex", state: .expiring),
            RunnerAttentionItem(runner: "c", provider: "claude-code", state: .expired),
        ])
        XCTAssertEqual(items.map(\.id), ["b:codex", "c:claude-code", "a:codex"])
        XCTAssertEqual(items[0].state, .expired)
    }

    func testWording() {
        let expired = RunnerAttentionItem(runner: "fmnext-r1", provider: "claude-code", state: .expired)
        XCTAssertEqual(expired.title, "fmnext-r1 needs Claude sign-in")
        XCTAssertEqual(expired.detail(), "Jobs for Claude Code are not running on this runner.")
        XCTAssertEqual(expired.buttonLabel, "Sign in")
        let expiring = RunnerAttentionItem(
            runner: "fmnext-r1", provider: "codex", state: .expiring, expiresAtMs: 9, openLoginId: loginA)
        XCTAssertEqual(expiring.title, "fmnext-r1: Codex sign-in expires soon")
        XCTAssertEqual(expiring.detail(formatExpiry: { _ in "soon" }), "Sign in again before it expires. Expires soon.")
        XCTAssertEqual(expiring.buttonLabel, "Open sign-in")
        XCTAssertEqual(RunnerSignIn.summaryTitle([expired]), expired.title)
        XCTAssertEqual(RunnerSignIn.summaryTitle([expired, expiring]), "2 runner sign-ins needed")
        XCTAssertTrue(RunnerSignIn.anyExpired([expiring, expired]))
        XCTAssertFalse(RunnerSignIn.anyExpired([expiring]))
    }

    func testHrefAndInput() {
        XCTAssertEqual(
            RunnerSignIn.request(fromHref: "/dashboard/runners?runner=fmnext-r1&provider=claude-code"),
            RunnerSignInRequest(runner: "fmnext-r1", provider: "claude-code"))
        XCTAssertEqual(
            RunnerSignIn.request(fromHref: "/dashboard/runners?runner=fmnext-r1&provider=codex")?.connectPath,
            "/dashboard/runners?runner=fmnext-r1&provider=codex&connect=1")
        XCTAssertEqual(RunnerSignIn.connectPath("/dashboard/runners?runner=a&provider=codex&connect=1"),
                       "/dashboard/runners?runner=a&provider=codex&connect=1")
        XCTAssertEqual(RunnerSignIn.connectPath("https://evil.test/dashboard/runners"), "")
        XCTAssertEqual(RunnerSignIn.connectPath("/dashboard/runnersevil"), "")
        XCTAssertEqual(RunnerSignIn.connectPath("//evil.test/dashboard/runners"), "")
        XCTAssertNil(RunnerSignIn.request(fromHref: "/dashboard/runners?runner=fmnext-r1&provider=gemini"))
        XCTAssertNil(RunnerSignIn.request(fromHref: "/dashboard/other?runner=fmnext-r1&provider=codex"))
        XCTAssertNil(RunnerSignIn.request(fromHref: "http://evil.test/dashboard/runners?runner=a&provider=codex"))
        XCTAssertTrue(RunnerSignIn.inputValid("abc#DEF-123\r"))
        XCTAssertFalse(RunnerSignIn.inputValid(""))
        XCTAssertFalse(RunnerSignIn.inputValid("a\u{1b}[A"))
        XCTAssertFalse(RunnerSignIn.inputValid("a\u{7f}"))
        XCTAssertFalse(RunnerSignIn.inputValid(String(repeating: "x", count: 4097)))
        XCTAssertTrue(RunnerSignIn.inputValid(String(repeating: "x", count: 4096)))
        XCTAssertEqual(RunnerSignIn.codeSubmission("  code\n "), "code\r")
        XCTAssertEqual(RunnerSignIn.codeSubmission(" \r\n"), "")
    }

    func testTerminalTextAndUrls() {
        let raw = "\u{1b}[1mOpen\u{1b}[0m https://claude.ai/oauth/authorize?code=1&x=2.\r\nloading\rdone\n\u{1b}]0;title\u{07}http://plain.test"
        let text = RunnerSignIn.terminalText(raw)
        XCTAssertEqual(text, "Open https://claude.ai/oauth/authorize?code=1&x=2.\ndone\nhttp://plain.test")
        XCTAssertEqual(RunnerSignIn.loginUrls(text).map(\.absoluteString), ["https://claude.ai/oauth/authorize?code=1&x=2"])
    }

    func testSessionStartAttachAndChunks() {
        let session = RunnerLoginSession(runner: "FMNEXT-R1", provider: "claude-code")
        XCTAssertEqual(session.runner, "fmnext-r1")
        XCTAssertEqual(session.applyStartReply(status: 201, body: ["id": .string(loginA)]), .started)
        XCTAssertTrue(session.open)
        let busy = RunnerLoginSession(runner: "r", provider: "codex")
        XCTAssertEqual(
            busy.applyStartReply(status: 409, body: ["error": "login_active", "id": .string(loginB)]), .attached)
        XCTAssertEqual(busy.id, loginB)
        let refused = RunnerLoginSession(runner: "r", provider: "codex")
        XCTAssertEqual(refused.applyStartReply(status: 403, body: [:]), .failed)
        XCTAssertFalse(refused.open)
        XCTAssertTrue(refused.statusText.contains("not one of yours"))

        let b64 = { (text: String) in JSONValue.string(Data(text.utf8).base64EncodedString()) }
        let first = session.applyOutputReply([
            "state": "running",
            "chunks": .array([
                .object(["seq": 2, "data": b64("world")]),
                .object(["seq": 1, "data": b64("hello ")]),
                .object(["seq": 3, "data": "!!not base64!!"]),
            ]),
            "truncated": true, "more": true,
        ])
        XCTAssertEqual(String(decoding: first, as: UTF8.self), "\r\n[older sign-in output was trimmed]\r\nhello world")
        XCTAssertEqual(session.afterSeq, 2)
        XCTAssertTrue(session.more)
        let second = session.applyOutputReply(["state": "running", "chunks": .array([.object(["seq": 2, "data": b64("x")])]), "nextSeq": 7])
        XCTAssertTrue(second.isEmpty)
        XCTAssertEqual(session.afterSeq, 7)
        XCTAssertEqual(session.state, "running")
        session.applyOutputReply(["state": "succeeded", "message": "ok"])
        XCTAssertFalse(session.open)
        XCTAssertFalse(session.needsCancelOnClose)
        let cancelled = RunnerLoginSession(runner: "r", provider: "codex")
        cancelled.attach(loginA)
        cancelled.applyCancelReply([:])
        XCTAssertEqual(cancelled.state, "cancelled")
    }
}

@MainActor
final class RunnerAttentionModelTests: XCTestCase {
    func testLandedEndpointSetsFloorAndCanConnectFallsBackToBrowser() async {
        let http = FakeHTTPClient()
        http.on(path: "/api/runners/attention", status: 200, json: parse(landedPayload))
        let clock = FixedClock(epochMillis: apiTestEpochMillis)
        let model = RunnerAttentionModel(api: makeTestApi(http, token: "tok", clock: clock), clock: clock)
        await model.refresh()
        XCTAssertEqual(model.pollFloor, 120)
        XCTAssertEqual(model.title, "2 runner sign-ins needed")
        clock.advance(by: 100)
        await model.refresh()
        XCTAssertEqual(http.requests.count, 1)
        clock.advance(by: 21)
        await model.refresh()
        XCTAssertEqual(http.requests.count, 2)
        model.open(model.items[1])
        XCTAssertNil(model.request)
        XCTAssertEqual(
            model.fallbackURL?.absoluteString, "http://test.local/dashboard/runners?runner=fmnext-r2&provider=codex&connect=1")
        model.open(model.items[0])
        XCTAssertEqual(model.request?.runner, "fmnext-r1")
    }

    func testFallsBackToRunnersWhenAttentionIsMissing() async {
        let http = apiRouteByPath { path, _, _ in
            if path == "/api/runners" { return apiJSON(runnersPayload()) }
            return nil
        }
        let clock = FixedClock(epochMillis: apiTestEpochMillis)
        let api = makeTestApi(http, token: "tok", clock: clock)
        let model = RunnerAttentionModel(api: api, clock: clock)
        await model.refresh()
        XCTAssertEqual(model.items.count, 3)
        XCTAssertTrue(model.anyExpired)
        XCTAssertEqual(model.title, "3 runner sign-ins needed")
        XCTAssertFalse(model.usesAttentionEndpoint)
        XCTAssertEqual(http.requests.map(\.apiPath), ["/api/runners/attention", "/api/runners"])
        XCTAssertEqual(http.requests.first?.apiHeader("Authorization"), "Bearer tok")
        await model.refresh()
        XCTAssertEqual(http.requests.count, 2)
        clock.advance(by: 61)
        await model.refresh()
        XCTAssertEqual(http.requests.count, 2)
        clock.advance(by: 240)
        await model.refresh()
        XCTAssertEqual(http.requests.map(\.apiPath).last, "/api/runners")
        XCTAssertEqual(http.requests.count, 3)
    }

    func testUsesAttentionEndpointAndClearsWhenSignedIn() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: parse("{\"attention\":[{\"runner\":\"fmnext-r1\",\"provider\":\"claude-code\",\"state\":\"expired\"}]}"))
        http.enqueue(status: 200, json: parse("{\"attention\":[]}"))
        let clock = FixedClock(epochMillis: apiTestEpochMillis)
        let api = makeTestApi(http, token: "tok", clock: clock)
        let model = RunnerAttentionModel(api: api, clock: clock)
        await model.refresh()
        XCTAssertEqual(model.title, "fmnext-r1 needs Claude sign-in")
        await model.refresh(force: true)
        XCTAssertEqual(http.requests.count, 1)
        clock.advance(by: 6)
        await model.refresh(force: true)
        XCTAssertFalse(model.visible)
        XCTAssertEqual(http.requests.count, 2)
    }

    func testUnauthorizedAndSignedOutShowNothing() async {
        let http = FakeHTTPClient()
        http.on(path: "/api/runners/attention", status: 403, json: parse("{\"error\":\"forbidden\"}"))
        let model = RunnerAttentionModel(api: makeTestApi(http, token: "tok"), clock: FixedClock(epochMillis: 1))
        await model.refresh()
        XCTAssertEqual(model.items, [])
        let signedOut = RunnerAttentionModel(api: makeTestApi(FakeHTTPClient()), clock: FixedClock(epochMillis: 1))
        await signedOut.refresh()
        XCTAssertFalse(signedOut.visible)
        signedOut.open(RunnerSignInRequest(runner: "fmnext-r1", provider: "codex"))
        XCTAssertNil(signedOut.request)
        XCTAssertEqual(
            signedOut.fallbackURL?.absoluteString, "http://test.local/dashboard/runners?runner=fmnext-r1&provider=codex&connect=1")
    }

    func testRunnerAlertForcesRefreshAndHrefOpensSheet() async {
        let http = FakeHTTPClient()
        http.on(path: "/api/runners/attention", status: 200, json: parse("{\"attention\":[]}"))
        let clock = FixedClock(epochMillis: 1)
        let model = RunnerAttentionModel(api: makeTestApi(http, token: "tok"), clock: clock)
        await model.refresh()
        clock.advance(by: 6)
        let alert = ForkNotification(
            id: "p1", kind: "operational_alert", title: "fmnext-r1: Claude login expired — reconnect",
            href: "/dashboard/runners?runner=fmnext-r1&provider=claude-code", ts: 1)
        await model.noteNotifications([alert])
        XCTAssertEqual(http.requests.count, 2)
        clock.advance(by: 6)
        await model.noteNotifications([alert])
        XCTAssertEqual(http.requests.count, 2)
        XCTAssertTrue(model.openHref(alert.href))
        XCTAssertEqual(model.request, RunnerSignInRequest(runner: "fmnext-r1", provider: "claude-code"))
        XCTAssertEqual(model.request?.connectPath, "/dashboard/runners?runner=fmnext-r1&provider=claude-code&connect=1")
        XCTAssertFalse(model.openHref("/mona/forkmesh/issues/1"))
    }

    func testLoginModelRunsToSuccessAndSendsCode() async {
        let http = FakeHTTPClient()
        let b64 = Data("Open https://claude.ai/oauth/authorize?x=1\r\nPaste code: ".utf8).base64EncodedString()
        http.enqueue(status: 201, json: .object(["id": .string(loginA)]))
        http.enqueue(status: 200, json: .object([
            "state": "running", "chunks": .array([.object(["seq": 1, "data": .string(b64)])]), "nextSeq": 1,
        ]))
        let api = makeTestApi(http, token: "tok")
        var ended = 0
        let model = RunnerLoginModel(
            api: api, request: RunnerSignInRequest(runner: "fmnext-r1", provider: "claude-code"),
            sleep: { _ in }, onEnded: { ended += 1 })
        await model.start()
        XCTAssertTrue(model.open)
        http.enqueue(status: 200, json: .object(["seq": 1]))
        http.enqueue(status: 200, json: .object(["state": "succeeded", "chunks": .array([])]))
        _ = await model.pollOnce()
        XCTAssertEqual(model.urls.map(\.absoluteString), ["https://claude.ai/oauth/authorize?x=1"])
        model.code = " abc123 "
        await model.submitCode()
        XCTAssertEqual(model.code, "")
        await model.poll()
        XCTAssertEqual(model.state, "succeeded")
        XCTAssertEqual(ended, 1)
        let paths = http.requests.map { "\($0.method) \($0.apiPath)" }
        XCTAssertEqual(paths, [
            "POST /api/runners/fmnext-r1/login",
            "GET /api/runners/fmnext-r1/login/\(loginA)/output",
            "POST /api/runners/fmnext-r1/login/\(loginA)/input",
            "GET /api/runners/fmnext-r1/login/\(loginA)/output",
        ])
        XCTAssertEqual(http.requests[0].apiBodyJSON.str("provider"), "claude-code")
        XCTAssertEqual(http.requests[1].apiQuery["after"], "0")
        XCTAssertEqual(http.requests[2].apiBodyJSON.str("data"), "abc123\r")
        XCTAssertEqual(http.requests[3].apiQuery["after"], "1")
    }

    func testLoginModelAttachesOpenLoginAndCancelsOnClose() async {
        let http = FakeHTTPClient()
        http.on("POST", path: "/api/runners/fmnext-r1/login/\(loginB)/cancel", status: 200, json: .object(["state": "cancelled"]))
        let api = makeTestApi(http, token: "tok")
        var ended = 0
        let model = RunnerLoginModel(
            api: api, request: RunnerSignInRequest(runner: "fmnext-r1", provider: "codex", openLoginId: loginB),
            sleep: { _ in }, onEnded: { ended += 1 })
        await model.start()
        XCTAssertTrue(model.open)
        XCTAssertEqual(http.requests.count, 0)
        await model.close()
        XCTAssertEqual(model.state, "cancelled")
        XCTAssertEqual(ended, 1)
        XCTAssertEqual(http.requests.map(\.method), ["POST"])
    }

    func testLoginModelFallsBackToDashboardWhenRefused() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 403, json: .object(["error": "forbidden"]))
        let model = RunnerLoginModel(
            api: makeTestApi(http, token: "tok"), request: RunnerSignInRequest(runner: "fmnext-r1", provider: "codex"),
            sleep: { _ in })
        await model.run()
        XCTAssertTrue(model.failed)
        XCTAssertEqual(
            model.fallbackURL?.absoluteString, "http://test.local/dashboard/runners?runner=fmnext-r1&provider=codex&connect=1")
        XCTAssertEqual(http.requests.count, 1)
    }

    func testLoginModelStopsPollingOnNotFound() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 404, json: .object(["error": "not_found"]))
        var ended = 0
        let model = RunnerLoginModel(
            api: makeTestApi(http, token: "tok"),
            request: RunnerSignInRequest(runner: "fmnext-r1", provider: "codex", openLoginId: loginA),
            sleep: { _ in }, onEnded: { ended += 1 })
        await model.run()
        XCTAssertTrue(model.failed)
        XCTAssertEqual(model.status, "The sign-in is no longer available.")
        XCTAssertEqual(ended, 1)
        XCTAssertEqual(http.requests.count, 1)
    }
}
