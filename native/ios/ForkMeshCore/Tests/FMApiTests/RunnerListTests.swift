import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

private let accountA = String(repeating: "a", count: 32)
private let loginId = String(repeating: "e", count: 32)
private let nowMs: Int64 = 1_770_000_000_000

private func runnersPayload(r1Status: String = "active") -> JSONValue {
    try! JSON.parse("""
    {"ok":true,"providers":["claude-code","codex"],"now":\(nowMs),"runners":[
      {"name":"fmnext-r4","scope":"pool","status":"revoked","lastSeenAt":\(nowMs - 60_000),"activeLeases":0,
       "availability":{"claude-code":{"binaryFound":true,"loginState":"available"}}},
      {"name":"fmnext-r1","scope":"personal","status":"\(r1Status)","lastSeenAt":\(nowMs - 120_000),"activeLeases":2,
       "availability":{
        "claude-code":{"binaryFound":true,"loginState":"available","auth":{"state":"ok"},
          "accounts":[{"id":"\(accountA)","label":"Work","active":true}]},
        "codex":{"binaryFound":true,"loginState":"expired","auth":{"state":"expired"}}},
       "logins":[{"id":"\(loginId)","provider":"codex","state":"running"}]},
      {"name":"fmnext-r2","scope":"pool","status":"paused","lastSeenAt":\(nowMs - 3_600_000),"activeLeases":1,
       "availability":{
        "claude-code":{"binaryFound":true,"loginState":"available","auth":{"state":"expiring"}},
        "codex":{"binaryFound":false}}},
      {"name":"fmnext-r3","status":"active","lastSeenAt":0,
       "availability":{"claude-code":{"binaryFound":true,"loginState":"missing"}}},
      {"name":"Bad Name","status":"active"}
    ]}
    """)
}

final class RunnerListParsingTests: XCTestCase {
    func testParsesStatusLeasesAndProviderSignIn() {
        let all = RunnerList.parse(runnersPayload())
        XCTAssertEqual(all.map(\.name), ["fmnext-r1", "fmnext-r2", "fmnext-r3", "fmnext-r4"])
        XCTAssertEqual(all.map(\.status), [.active, .paused, .active, .revoked])
        let r1 = all[0]
        XCTAssertEqual(r1.scope, "personal")
        XCTAssertEqual(r1.scopeLabel, "Personal")
        XCTAssertEqual(r1.activeLeases, 2)
        XCTAssertEqual(r1.jobsText, "2 jobs")
        XCTAssertEqual(r1.provider("claude-code")?.signIn, .signedIn)
        XCTAssertEqual(r1.provider("claude-code")?.activeAccount, "Work")
        XCTAssertEqual(r1.provider("claude-code")?.detail, "Signed in as Work")
        XCTAssertEqual(r1.provider("codex")?.signIn, .expired)
        XCTAssertEqual(r1.provider("codex")?.detail, "Sign-in expired")
        XCTAssertEqual(r1.signInRequest("codex").openLoginId, loginId)
        XCTAssertEqual(r1.signInProviders.map(\.provider), ["claude-code", "codex"])
        XCTAssertTrue(r1.hasAccounts)
        let r2 = all[1]
        XCTAssertEqual(r2.status.label, "Paused")
        XCTAssertEqual(r2.jobsText, "1 job")
        XCTAssertEqual(r2.provider("claude-code")?.signIn, .expiring)
        XCTAssertEqual(r2.provider("codex")?.signIn, .notInstalled)
        XCTAssertEqual(r2.signInProviders.map(\.provider), ["claude-code"])
        XCTAssertFalse(r2.hasAccounts)
        let r3 = all[2]
        XCTAssertEqual(r3.scopeLabel, "")
        XCTAssertEqual(r3.jobsText, "No jobs")
        XCTAssertEqual(r3.provider("claude-code")?.signIn, .signedOut)
        XCTAssertEqual(r3.provider("codex")?.signIn, .unknown)
        XCTAssertEqual(r3.status.label, nil)
        let r4 = all[3]
        XCTAssertEqual(r4.status.label, "Revoked")
        XCTAssertTrue(r4.signInProviders.isEmpty)
        XCTAssertFalse(r4.canPause || r4.canResume || r4.canRevoke)
    }

    func testActionsFollowTheStatus() {
        let all = RunnerList.parse(runnersPayload())
        XCTAssertTrue(all[0].canPause)
        XCTAssertFalse(all[0].canResume)
        XCTAssertTrue(all[0].canRevoke)
        XCTAssertFalse(all[1].canPause)
        XCTAssertTrue(all[1].canResume)
        XCTAssertTrue(all[1].canRevoke)
    }

    func testOnlineWithinTenMinutes() {
        let all = RunnerList.parse(runnersPayload())
        XCTAssertTrue(all[0].online(nowMs: nowMs))
        XCTAssertEqual(all[0].seenText(nowMs: nowMs), "Online")
        XCTAssertFalse(all[0].online(nowMs: nowMs + RunnerList.onlineMs))
        XCTAssertFalse(all[1].online(nowMs: nowMs))
        XCTAssertEqual(all[1].seenText(nowMs: nowMs), "Last seen 1 h 0 min ago")
        XCTAssertFalse(all[2].online(nowMs: nowMs))
        XCTAssertEqual(all[2].seenText(nowMs: nowMs), "Never seen")
    }

    func testStatusAndActionPaths() {
        XCTAssertEqual(RunnerStatus(raw: "Paused"), .paused)
        XCTAssertEqual(RunnerStatus(raw: "active"), .active)
        XCTAssertEqual(RunnerStatus(raw: "something"), .revoked)
        XCTAssertEqual(RunnerAction.pause.path, "/api/runners/pause")
        XCTAssertEqual(RunnerAction.resume.path, "/api/runners/resume")
        XCTAssertEqual(RunnerAction.revoke.path, "/api/runners/revoke")
        XCTAssertEqual(RunnerList.body(name: " FMNext-R1 "), .object(["name": .string("fmnext-r1")]))
        XCTAssertEqual(RunnerList.path(), "/api/runners")
    }

    func testAccountsListKeepsPausedRunners() {
        let listed = RunnerAccounts.listed(RunnerAccounts.parse(runnersPayload()))
        XCTAssertEqual(listed.map(\.name), ["fmnext-r1", "fmnext-r2", "fmnext-r3"])
    }
}

final class RunnersModelTests: XCTestCase {
    @MainActor func testLoadListsRunners() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/runners", status: 200, json: runnersPayload())
        let model = RunnersModel(api: makeTestApi(http, token: "tok"))
        XCTAssertTrue(model.showsSpinner)
        await model.load()
        XCTAssertEqual(model.runners.map(\.name), ["fmnext-r1", "fmnext-r2", "fmnext-r3", "fmnext-r4"])
        XCTAssertFalse(model.showsSpinner)
        XCTAssertFalse(model.showsEmpty)
        XCTAssertFalse(model.signedOut)
        XCTAssertEqual(http.requests.first?.apiHeader("Authorization"), "Bearer tok")
        XCTAssertEqual(model.addRunnerURL?.path, "/dashboard/runners")
    }

    @MainActor func testSignedOutAndRefused() async {
        let signedOut = RunnersModel(api: makeTestApi(FakeHTTPClient()))
        await signedOut.load()
        XCTAssertTrue(signedOut.signedOut)
        XCTAssertFalse(signedOut.showsSpinner)
        let http = FakeHTTPClient()
        http.on(path: "/api/runners", status: 401, json: .object(["error": "unauthorized"]))
        let refused = RunnersModel(api: makeTestApi(http, token: "tok"))
        await refused.load()
        XCTAssertTrue(refused.signedOut)
        XCTAssertEqual(refused.runners, [])
    }

    @MainActor func testEmptyAndError() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 500, json: .object(["error": "boom", "message": "The database is busy."]))
        http.enqueue(status: 200, json: .object(["ok": true, "runners": .array([])]))
        let model = RunnersModel(api: makeTestApi(http, token: "tok"))
        await model.load()
        XCTAssertTrue(model.showsError)
        XCTAssertEqual(model.error, "Could not load runners: The database is busy.")
        await model.load()
        XCTAssertFalse(model.showsError)
        XCTAssertTrue(model.showsEmpty)
    }

    @MainActor func testPauseResumeAndRevokePostTheName() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: runnersPayload())
        http.enqueue(status: 200, json: .object(["ok": true, "name": "fmnext-r1", "status": "paused"]))
        http.enqueue(status: 200, json: runnersPayload(r1Status: "paused"))
        http.enqueue(status: 200, json: .object(["ok": true, "name": "fmnext-r1", "status": "active"]))
        http.enqueue(status: 200, json: runnersPayload())
        http.enqueue(status: 200, json: .object(["ok": true, "name": "fmnext-r1", "status": "revoked"]))
        http.enqueue(status: 200, json: runnersPayload(r1Status: "revoked"))
        let model = RunnersModel(api: makeTestApi(http, token: "tok"))
        await model.load()
        await model.pause(model.runners[0])
        XCTAssertEqual(model.message, "fmnext-r1 is paused. It finishes the jobs it holds and takes no new ones.")
        XCTAssertFalse(model.messageIsError)
        XCTAssertEqual(model.runners.first { $0.name == "fmnext-r1" }?.status, .paused)
        await model.resume(model.runners[0])
        XCTAssertEqual(model.runners.first { $0.name == "fmnext-r1" }?.status, .active)
        await model.revoke(model.runners[0])
        XCTAssertEqual(model.runners.last?.name, "fmnext-r1")
        XCTAssertEqual(model.runners.last?.status, .revoked)
        XCTAssertEqual(http.requests.map { "\($0.method) \($0.apiPath)" }, [
            "GET /api/runners", "POST /api/runners/pause", "GET /api/runners", "POST /api/runners/resume",
            "GET /api/runners", "POST /api/runners/revoke", "GET /api/runners",
        ])
        XCTAssertEqual(http.requests[1].apiBodyJSON.str("name"), "fmnext-r1")
        XCTAssertEqual(http.requests[1].apiHeader("Authorization"), "Bearer tok")
        XCTAssertEqual(model.busy, [])
    }

    @MainActor func testRefusalShowsAMessageAndRereads() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: runnersPayload())
        http.enqueue(status: 409, json: .object(["error": "runner_inactive", "message": "That runner is revoked."]))
        http.enqueue(status: 200, json: runnersPayload())
        http.enqueue(status: 404, json: .object(["error": "not_found"]))
        let model = RunnersModel(api: makeTestApi(http, token: "tok"))
        await model.load()
        await model.resume(model.runners[1])
        XCTAssertEqual(model.message, "Could not resume the runner: That runner is revoked.")
        XCTAssertTrue(model.messageIsError)
        XCTAssertEqual(http.requests.count, 3)
        await model.pause(model.runners[0])
        XCTAssertEqual(model.message, "Could not pause the runner: That runner is not one of yours.")
        XCTAssertEqual(http.requests.count, 4)
        model.dismissMessage()
        XCTAssertEqual(model.message, "")
    }
}
