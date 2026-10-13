import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

private let accountA = String(repeating: "a", count: 32)
private let accountB = String(repeating: "b", count: 32)
private let accountC = String(repeating: "c", count: 32)
private let requestId = String(repeating: "d", count: 32)
private let loginId = String(repeating: "e", count: 32)
private let nowMs: Int64 = 1_770_000_000_000

private func accountsPayload(requestState: String? = nil, action: String = "activate") -> JSONValue {
    let requests: String = requestState.map {
        """
        [{"id":"\(requestId)","provider":"claude-code","action":"\(action)","account":"\(accountB)","state":"\($0)",
          "message":"\($0 == "failed" ? "login rejected" : "")","createdAt":20},
         {"id":"\(String(repeating: "f", count: 32))","provider":"claude-code","action":"activate","account":"\(accountB)",
          "state":"done","createdAt":10},
         {"id":"short","provider":"claude-code","action":"activate","account":"\(accountB)","state":"pending"}]
        """
    } ?? "[]"
    return try! JSON.parse("""
    {"ok":true,"runners":[
      {"name":"fmnext-r1","scope":"pool","status":"active","lastSeenAt":5,"availability":{
        "claude-code":{"binaryFound":true,"loginState":"available","accounts":[
          {"id":"\(accountA)","label":"Work","email":"w@x.test","plan":"Max","active":true,
           "usage":{"fetchedAt":7,"windows":[
             {"id":"five_hour","label":"5-hour session","usedPercent":41.6,"resetsAt":\(nowMs + 3_600_000),"windowMinutes":300},
             {"id":"seven_day","label":"Weekly","usedPercent":140,"resetsAt":1},
             {"label":"no id","usedPercent":5}]}},
          {"id":"\(accountB)","email":"p@x.test","plan":"Pro","usage":{"error":"token_expired","windows":[]}},
          {"id":"\(accountC)"},
          {"id":"NOTHEX","label":"Dropped"}]},
        "codex":{"binaryFound":false,"loginState":"missing"}},
       "logins":[{"id":"\(loginId)","provider":"codex","state":"pending"}],
       "accountRequests":\(requests)},
      {"name":"fmnext-r2","status":"active","availability":{
        "claude-code":{"binaryFound":false},"codex":{"binaryFound":true,"loginState":"missing"}}},
      {"name":"fmnext-r3","status":"active","availability":{"claude-code":{"binaryFound":false}}},
      {"name":"fmnext-r4","status":"revoked","availability":{
        "claude-code":{"binaryFound":true,"accounts":[{"id":"\(accountA)","active":true}]}}},
      {"name":"Bad Name","status":"active","availability":{"claude-code":{"binaryFound":true}}}
    ]}
    """)
}

final class RunnerAccountsParsingTests: XCTestCase {
    func testParsesAccountsUsageAndRequests() {
        let all = RunnerAccounts.parse(accountsPayload(requestState: "pending"))
        XCTAssertEqual(all.map(\.name), ["fmnext-r1", "fmnext-r2", "fmnext-r3", "fmnext-r4"])
        let r1 = all[0]
        XCTAssertEqual(r1.scope, "pool")
        XCTAssertEqual(r1.lastSeenAtMs, 5)
        let claude = r1.provider("claude-code")
        XCTAssertTrue(claude.binaryFound)
        XCTAssertEqual(claude.accounts.map(\.id), [accountA, accountB, accountC])
        XCTAssertEqual(claude.activeAccount?.id, accountA)
        let work = claude.accounts[0]
        XCTAssertEqual(work.displayName, "Work")
        XCTAssertEqual(work.plan, "Max")
        XCTAssertTrue(work.usageRead)
        XCTAssertEqual(work.usageFetchedAtMs, 7)
        XCTAssertEqual(work.usage.map(\.id), ["five_hour", "seven_day"])
        XCTAssertEqual(work.usage[0].windowMinutes, 300)
        XCTAssertEqual(work.usage[1].usedPercent, 100)
        XCTAssertEqual(claude.accounts[1].displayName, "p@x.test")
        XCTAssertEqual(claude.accounts[1].usageError, "token_expired")
        XCTAssertEqual(claude.accounts[2].displayName, "Account")
        XCTAssertFalse(claude.accounts[2].usageRead)
        XCTAssertEqual(r1.requests.map(\.id), [requestId, String(repeating: "f", count: 32)])
        XCTAssertTrue(r1.pending(provider: "claude-code", account: accountB))
        XCTAssertFalse(r1.pending(provider: "claude-code", account: accountA))
        XCTAssertEqual(r1.request(provider: "claude-code", account: accountB)?.id, requestId)
        XCTAssertTrue(RunnerAccounts.anyPending(all))
        XCTAssertEqual(r1.signInRequest("codex").openLoginId, loginId)
        XCTAssertEqual(r1.signInRequest("claude-code").openLoginId, "")
        XCTAssertEqual(r1.signInRequest("codex").connectPath, "/dashboard/runners?runner=fmnext-r1&provider=codex&connect=1")
    }

    func testListsActiveRunnersWithTheCliOrAccounts() {
        let listed = RunnerAccounts.listed(RunnerAccounts.parse(accountsPayload()))
        XCTAssertEqual(listed.map(\.name), ["fmnext-r1", "fmnext-r2"])
        XCTAssertEqual(listed[0].providers.map(\.provider), ["claude-code"])
        XCTAssertEqual(listed[1].providers.map(\.provider), ["codex"])
        XCTAssertEqual(listed[0].providers[0].signInLabel, "Add Claude Code account…")
        XCTAssertEqual(listed[1].providers[0].signInLabel, "Sign in Codex…")
        XCTAssertTrue(RunnerAccounts.hasAccounts(listed))
        XCTAssertFalse(RunnerAccounts.hasAccounts([listed[1]]))
        XCTAssertFalse(RunnerAccounts.anyPending(listed))
        XCTAssertEqual(RunnerAccounts.summary(listed), "fmnext-r1 Claude: Work")
        XCTAssertEqual(RunnerAccounts.summary([listed[1]]), "Choose the account each runner uses")
    }

    func testUsageTextMatchesTheDesktop() {
        let claude = RunnerAccounts.parse(accountsPayload())[0].provider("claude-code")
        XCTAssertEqual(
            claude.accounts[0].usageText(nowMs: nowMs), "5-hour session 42% (resets in 1 h 0 min) · Weekly 100%")
        XCTAssertEqual(claude.accounts[1].usageText(nowMs: nowMs), "usage refreshes after its next run")
        XCTAssertEqual(claude.accounts[2].usageText(nowMs: nowMs), "usage not read yet")
        XCTAssertEqual(
            RunnerAccount(id: accountA, usageRead: true).usageText(nowMs: nowMs), "no usage limits reported")
        XCTAssertEqual(
            RunnerAccount(
                id: accountA, usageRead: true, usageError: "unreachable",
                usage: [RunnerUsageWindow(id: "seven_day", usedPercent: 12.4)]).usageText(nowMs: nowMs),
            "seven_day 12% · usage service unreachable")
        XCTAssertEqual(RunnerAccounts.usageErrorText("unauthorized"), "the provider refused this login; sign it in again")
        XCTAssertEqual(RunnerAccounts.usageErrorText("no_oauth_login"), "usage limits need a subscription sign-in")
        XCTAssertEqual(RunnerAccounts.usageErrorText("missing_login"), "saved login missing; sign it in again")
        XCTAssertEqual(RunnerAccounts.usageErrorText("weird"), "usage unavailable (weird)")
        XCTAssertEqual(RunnerAccounts.durationText(10_000), "1 min")
        XCTAssertEqual(RunnerAccounts.durationText(59 * 60_000), "59 min")
        XCTAssertEqual(RunnerAccounts.durationText(47 * 3_600_000 + 5 * 60_000), "47 h 5 min")
        XCTAssertEqual(RunnerAccounts.durationText(60 * 3_600_000), "3 days")
    }

    func testPathAndBody() {
        XCTAssertEqual(RunnerAccounts.accountsPath(" FMNext-R1 "), "/api/runners/fmnext-r1/accounts")
        let body = RunnerAccounts.activateBody(provider: "codex", account: accountA)
        XCTAssertEqual(JSON.encode(body), "{\"provider\":\"codex\",\"account\":\"\(accountA)\",\"action\":\"activate\"}")
    }
}

final class RunnerAccountsTerminalTests: XCTestCase {
    func testReadsTerminalSupportAndOpenTerminals() throws {
        let payload: JSONValue = try JSON.parse("""
        {"ok":true,"runners":[{"name":"fmnext-r1","status":"active","features":["terminal","shell"],
          "terminals":[{"id":"\(loginId)","provider":"codex","state":"running"},
                       {"id":"short","provider":"claude-code","state":"running"},
                       {"id":"\(requestId)","provider":"claude-code","state":"failed"}],
          "availability":{}}]}
        """)
        let runner: RunnerAccountsRunner = try XCTUnwrap(RunnerAccounts.parse(payload).first)
        XCTAssertTrue(runner.terminals)
        XCTAssertEqual(runner.features, ["terminal"])
        XCTAssertEqual(runner.openTerminals, ["codex": loginId])
        XCTAssertFalse(RunnerAccounts.parse(accountsPayload())[0].terminals)
        XCTAssertEqual(RunnerAccounts.removeBody(provider: "codex", account: accountA), .object([
            "provider": .string("codex"), "account": .string(accountA), "action": .string("remove"),
        ]))
        XCTAssertEqual(RunnerSignIn.resizePath("fmnext-r1", loginId), "/api/runners/fmnext-r1/login/\(loginId)/resize")
        XCTAssertTrue(RunnerSignIn.terminalInputValid("\u{1b}[A\u{3}"))
        XCTAssertFalse(RunnerSignIn.terminalInputValid("a\u{0}b"))
        XCTAssertFalse(RunnerSignIn.terminalInputValid(""))
        XCTAssertFalse(RunnerSignIn.terminalInputValid(String(repeating: "x", count: 4097)))
        XCTAssertFalse(RunnerSignIn.inputValid("\u{1b}[A"))
    }

    func testUsageNoteSaysHowOldTheReadingIs() {
        let runner: RunnerAccountsRunner = RunnerAccounts.parse(accountsPayload())[0]
        let accounts: [RunnerAccount] = runner.provider("claude-code").accounts
        XCTAssertEqual(RunnerAccounts.usageNote(accounts[0], nowMs: 7 + 180_000), "Read 3 min ago")
        XCTAssertEqual(RunnerAccounts.usageNote(accounts[1], nowMs: nowMs), "Usage refreshes after its next run")
        XCTAssertEqual(RunnerAccounts.usageNote(accounts[2], nowMs: nowMs), "Usage not read yet")
        XCTAssertEqual(
            RunnerAccounts.usageNote(RunnerAccount(id: accountA, usageRead: true, usageFetchedAtMs: 1), nowMs: 1),
            "No usage limits reported")
    }

    func testTerminalStartAttachesToAnOpenTerminal() {
        let session = RunnerLoginSession(runner: "fmnext-r1", provider: "codex", kind: "terminal")
        XCTAssertEqual(session.applyStartReply(status: 409, body: [
            "error": .string("login_active"), "id": .string(loginId),
        ]), .failed)
        let again = RunnerLoginSession(runner: "fmnext-r1", provider: "codex", kind: "terminal")
        XCTAssertEqual(again.applyStartReply(status: 409, body: [
            "error": .string("terminal_active"), "id": .string(loginId), "session": .object(["state": .string("running")]),
        ]), .attached)
        XCTAssertTrue(again.open)
        again.applyOutputReply(["state": .string("succeeded"), "exitCode": .int(0), "message": .string("")])
        XCTAssertEqual(again.exitCode, 0)
        XCTAssertFalse(again.open)
    }
}

@MainActor
final class RunnerAccountsModelTests: XCTestCase {
    func testLoadListsAndHidesOnRefusal() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: accountsPayload())
        http.enqueue(status: 403, json: .object(["error": "forbidden"]))
        let clock = FixedClock(epochMillis: nowMs)
        let model = RunnerAccountsModel(api: makeTestApi(http, token: "tok", clock: clock), clock: clock, sleep: { _ in })
        await model.refresh()
        XCTAssertTrue(model.visible)
        XCTAssertEqual(model.runners.map(\.name), ["fmnext-r1", "fmnext-r2"])
        XCTAssertEqual(http.requests.first?.apiPath, "/api/runners")
        XCTAssertEqual(http.requests.first?.apiHeader("Authorization"), "Bearer tok")
        clock.advance(by: 60)
        await model.refresh()
        XCTAssertEqual(http.requests.count, 1)
        await model.load()
        XCTAssertEqual(http.requests.count, 2)
        XCTAssertFalse(model.visible)
        XCTAssertEqual(model.runners, [])
        XCTAssertEqual(model.message, "")
    }

    func testSignInSettleRereads() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/runners", status: 200, json: accountsPayload())
        let slept = ApiShared<[TimeInterval]>([])
        let model = RunnerAccountsModel(
            api: makeTestApi(http, token: "tok"), sleep: { seconds in slept.value.append(seconds) })
        let login = model.loginModel(for: model.runners.first?.signInRequest("codex")
            ?? RunnerSignInRequest(runner: "fmnext-r1", provider: "codex"))
        XCTAssertEqual(login.request.runner, "fmnext-r1")
        await model.settle()
        XCTAssertEqual(slept.value, [6, 14])
        XCTAssertEqual(http.requests.count, 3)
        XCTAssertEqual(model.summary, "fmnext-r1 Claude: Work")
    }

    func testSignedOutAndUnauthorizedShowNothing() async {
        let signedOut = RunnerAccountsModel(api: makeTestApi(FakeHTTPClient()), sleep: { _ in })
        await signedOut.load()
        XCTAssertFalse(signedOut.visible)
        XCTAssertEqual(signedOut.emptyText, "Sign in to your ForkMesh account to manage runner accounts.")
        let http = FakeHTTPClient()
        http.on(path: "/api/runners", status: 401, json: .object(["error": "unauthorized"]))
        let refused = RunnerAccountsModel(api: makeTestApi(http, token: "tok"), sleep: { _ in })
        await refused.load()
        XCTAssertFalse(refused.visible)
        XCTAssertEqual(refused.message, "")
    }

    func testActivatePostsThenRereadsWhilePending() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: accountsPayload())
        http.enqueue(status: 202, json: .object(["ok": true]))
        http.enqueue(status: 200, json: accountsPayload(requestState: "pending"))
        http.enqueue(status: 200, json: accountsPayload(requestState: "pending"))
        http.enqueue(status: 200, json: accountsPayload(requestState: "done"))
        let slept = ApiShared<[TimeInterval]>([])
        let model = RunnerAccountsModel(
            api: makeTestApi(http, token: "tok"), sleep: { seconds in slept.value.append(seconds) })
        await model.load()
        let runner = model.runners[0]
        let target = runner.provider("claude-code").accounts[1]
        await model.activate(runner: runner.name, provider: "claude-code", account: target)
        XCTAssertEqual(http.requests.map { "\($0.method) \($0.apiPath)" }, [
            "GET /api/runners", "POST /api/runners/fmnext-r1/accounts", "GET /api/runners", "GET /api/runners",
            "GET /api/runners",
        ])
        let post = http.requests[1]
        XCTAssertEqual(post.apiHeader("Authorization"), "Bearer tok")
        XCTAssertEqual(post.apiBodyJSON.str("provider"), "claude-code")
        XCTAssertEqual(post.apiBodyJSON.str("account"), accountB)
        XCTAssertEqual(post.apiBodyJSON.str("action"), "activate")
        XCTAssertEqual(slept.value, [2, 2])
        XCTAssertFalse(model.anyPending)
        XCTAssertEqual(model.sending, [])
        XCTAssertEqual(model.message, "fmnext-r1 now runs Claude Code as p@x.test.")
        XCTAssertFalse(model.messageIsError)
    }

    func testPendingRereadsStopAfterThirty() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/runners", status: 200, json: accountsPayload(requestState: "pending"))
        http.on("POST", path: "/api/runners/fmnext-r1/accounts", status: 202, json: .object(["ok": true]))
        let model = RunnerAccountsModel(api: makeTestApi(http, token: "tok"), sleep: { _ in })
        await model.load()
        XCTAssertTrue(model.isPending(model.runners[0], provider: "claude-code", account: accountB))
        let target = model.runners[0].provider("claude-code").accounts[2]
        await model.activate(runner: "fmnext-r1", provider: "claude-code", account: target)
        XCTAssertEqual(http.requests.filter { $0.method == "GET" }.count, 1 + 1 + RunnerAccounts.maxPendingReads)
        XCTAssertTrue(model.anyPending)
        XCTAssertEqual(model.message, "Switching fmnext-r1 to Account; the runner applies it on its next check-in.")
    }

    func testRemoveLogsTheAccountOut() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: accountsPayload())
        http.enqueue(status: 202, json: .object(["ok": true]))
        http.enqueue(status: 200, json: accountsPayload(requestState: "done", action: "remove"))
        http.enqueue(status: 404, json: .object(["error": "unknown_account", "message": "The runner does not report that account."]))
        let model = RunnerAccountsModel(api: makeTestApi(http, token: "tok"), sleep: { _ in })
        await model.load()
        let runner = model.runners[0]
        await model.remove(runner: runner.name, provider: "claude-code", account: runner.provider("claude-code").accounts[1])
        XCTAssertEqual(http.requests[1].apiBodyJSON.str("action"), "remove")
        XCTAssertEqual(http.requests[1].apiBodyJSON.str("account"), accountB)
        XCTAssertEqual(model.message, "p@x.test is logged out of Claude Code on fmnext-r1.")
        XCTAssertFalse(model.messageIsError)
        await model.remove(runner: runner.name, provider: "claude-code", account: runner.provider("claude-code").accounts[0])
        XCTAssertEqual(http.requests[3].apiBodyJSON.str("account"), accountA)
        XCTAssertEqual(model.message, "Could not log the account out: The runner does not report that account.")
        XCTAssertTrue(model.messageIsError)
    }

    func testFailuresShowAMessage() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: accountsPayload())
        http.enqueue(status: 404, json: .object(["error": "unknown_account", "message": "That account is not on this runner."]))
        http.enqueue(status: 429, json: .object(["error": "rate_limited"]))
        http.enqueue(status: 202, json: .object(["ok": true]))
        http.enqueue(status: 200, json: accountsPayload(requestState: "failed"))
        let model = RunnerAccountsModel(api: makeTestApi(http, token: "tok"), sleep: { _ in })
        await model.load()
        let target = model.runners[0].provider("claude-code").accounts[1]
        await model.activate(runner: "fmnext-r1", provider: "claude-code", account: target)
        XCTAssertEqual(model.message, "Could not change the runner account: That account is not on this runner.")
        XCTAssertTrue(model.messageIsError)
        await model.activate(runner: "fmnext-r1", provider: "claude-code", account: target)
        XCTAssertEqual(model.message, "Could not change the runner account: HTTP 429")
        XCTAssertEqual(http.requests.count, 3)
        await model.activate(runner: "fmnext-r1", provider: "claude-code", account: target)
        XCTAssertEqual(model.message, "Could not change the runner account: login rejected")
        XCTAssertTrue(model.messageIsError)
        await model.activate(runner: "fmnext-r1", provider: "claude-code", account: model.runners[0].provider("claude-code").accounts[0])
        await model.activate(runner: "fmnext-r1", provider: "gemini", account: target)
        XCTAssertEqual(http.requests.count, 5)
    }
}
