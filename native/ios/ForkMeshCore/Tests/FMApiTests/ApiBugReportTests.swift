// The API half of bug reports, as in native/android BugReportApiTest.kt:
// `POST /api/bug-reports`, usable signed out, failures as sentences. The app version comes
// from the bundle (AppBuild in FMBase).

import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

/// `_FakeRelay`: answers every report with `status` and `body` unless `offline` is set.
private final class FakeBugRelay: @unchecked Sendable {
    let http: FakeHTTPClient
    let offline: PortBox<Error?>

    init(status: Int = 201, body: String = portEncode(["ok": true, "id": .string(String(repeating: "a", count: 32))])) {
        let offline = PortBox<Error?>(nil)
        self.offline = offline
        http = FakeHTTPClient { _ in
            if let error = offline.value { throw error }
            return HTTPResponse(status: status, headers: ["content-type": "application/json"], text: body)
        }
    }

    var submitted: [JSONObject] { http.requests.filter { !$0.bodyText.isEmpty }.map { portBody($0) } }
}

@MainActor
final class ApiBugReportTests: XCTestCase {
    private func wire(_ relay: FakeBugRelay) -> ApiService {
        ApiPortFixture.api(relay.http, token: "")
    }

    func testAReportPostsToBugReportsWithItsBuildContext() async throws {
        let relay = FakeBugRelay()
        let api = wire(relay)

        let result = await api.submitBugReport(
            message: "  Repos tab spins forever.  ", contact: "reporter@example.com", screen: "code", severity: "high",
            platform: "ios", appVersion: "1.0.0+2", osVersion: "Version 26.1")

        XCTAssertTrue(result.ok)
        XCTAssertEqual(result.id, String(repeating: "a", count: 32))
        let request = try XCTUnwrap(relay.http.requests.first)
        XCTAssertEqual(relay.http.requests.count, 1)
        XCTAssertEqual(portRawPath(request), "/api/bug-reports")
        XCTAssertEqual(request.url.absoluteString, "http://relay.test/api/bug-reports")
        XCTAssertEqual(request.method, "POST")
        let sent = try XCTUnwrap(relay.submitted.first)
        XCTAssertEqual(sent["message"], "Repos tab spins forever.")
        XCTAssertEqual(sent["contact"], "reporter@example.com")
        XCTAssertEqual(sent["screen"], "code")
        XCTAssertEqual(sent["severity"], "high")
        XCTAssertEqual(sent["platform"], "ios")
        XCTAssertEqual(sent["appVersion"], "1.0.0+2")
        XCTAssertEqual(sent["osVersion"], "Version 26.1")
        // The Worker keys reports by client so it can rate-limit and triage them.
        XCTAssertEqual(sent["source"], "mobile")
        // Kotlin pins the whole body, in this key order.
        XCTAssertEqual(
            request.bodyText,
            #"{"message":"Repos tab spins forever.","contact":"reporter@example.com","screen":"code","severity":"high","#
                + #""source":"mobile","platform":"ios","appVersion":"1.0.0+2","osVersion":"Version 26.1"}"#)
    }

    func testAnOmittedContactIsLeftOutRatherThanSentEmpty() async throws {
        let relay = FakeBugRelay()
        let api = wire(relay)

        _ = await api.submitBugReport(message: "Something is broken.")
        _ = await api.submitBugReport(message: "Something is broken.", contact: "   ")

        XCTAssertEqual(relay.submitted.count, 2)
        for sent in relay.submitted {
            XCTAssertFalse(sent.contains("contact"))
            XCTAssertEqual(sent["severity"], "normal")
        }
    }

    func testFeedbackOnAPluginNamesItsMarketplacePackage() async throws {
        let relay = FakeBugRelay()
        let api = wire(relay)

        let result = await api.submitBugReport(message: "The face changer loses my outfit.", screen: "Face Changer", package: " face-changer ")

        XCTAssertTrue(result.ok)
        let sent = try XCTUnwrap(relay.submitted.first)
        XCTAssertEqual(sent["package"], "face-changer")
        XCTAssertEqual(sent["screen"], "Face Changer")
        XCTAssertTrue(try XCTUnwrap(relay.http.requests.first).bodyText.hasSuffix(#""package":"face-changer"}"#))
    }

    func testAReportWithoutAPluginLeavesThePackageOut() async {
        let relay = FakeBugRelay()
        let api = wire(relay)

        _ = await api.submitBugReport(message: "Something is broken.")
        _ = await api.submitBugReport(message: "Something is broken.", package: "  ")

        XCTAssertEqual(relay.submitted.count, 2)
        for sent in relay.submitted {
            XCTAssertFalse(sent.contains("package"))
        }
    }

    func testAnUnauthenticatedReportIsSentWithNoAuthorizationHeader() async {
        // The bug most worth hearing about is the one in the login flow, so this call must not
        // require, or wait for, a session.
        let relay = FakeBugRelay()
        let api = wire(relay)

        let result = await api.submitBugReport(message: "Login loops.")

        XCTAssertTrue(result.ok)
        XCTAssertNil(relay.http.lastRequest?.headers["Authorization"])
    }

    func testASignedInReportCarriesTheSessionBearerForAccountCredit() async {
        let relay = FakeBugRelay()
        let api = wire(relay)
        api.sessionTokenProvider = { "session-token" }

        _ = await api.submitBugReport(message: "Chat badge never clears.")

        XCTAssertEqual(relay.http.lastRequest?.headers["Authorization"], "Bearer session-token")
    }

    func testAnEmptyMessageNeverReachesTheNetwork() async {
        let relay = FakeBugRelay()
        let api = wire(relay)

        let result = await api.submitBugReport(message: "   ")

        XCTAssertFalse(result.ok)
        XCTAssertTrue(result.message.contains("Describe the problem"), result.message)
        XCTAssertTrue(relay.http.requests.isEmpty)
    }

    func testARateLimitIsReportedAsASentenceNotAStatusCode() async {
        let relay = FakeBugRelay(status: 429, body: portEncode(["error": "rate_limited"]))
        let result = await wire(relay).submitBugReport(message: "Still broken.")

        XCTAssertFalse(result.ok)
        XCTAssertTrue(result.message.contains("several reports"), result.message)
        XCTAssertFalse(result.message.contains("429"))
    }

    func testAnOversizedReportIsRefusedWithAnActionableSentence() async {
        let relay = FakeBugRelay(status: 413, body: portEncode(["error": "report_too_large"]))
        let result = await wire(relay).submitBugReport(message: String(repeating: "x", count: 40))

        XCTAssertFalse(result.ok)
        XCTAssertTrue(result.message.contains("too long"), result.message)
    }

    /// Kotlin: the remaining refusals, and a success without a JSON id.
    func testOtherRefusalsAreSentencesAndASuccessNeedsNoId() async {
        let detail = await wire(FakeBugRelay(status: 400)).submitBugReport(message: "x")
        XCTAssertTrue(detail.message.contains("more detail"), detail.message)
        let server = await wire(FakeBugRelay(status: 500)).submitBugReport(message: "x")
        XCTAssertTrue(server.message.contains("could not accept"), server.message)
        let noId = await wire(FakeBugRelay(status: 200, body: "ok")).submitBugReport(message: "x")
        XCTAssertTrue(noId.ok)
        XCTAssertEqual(noId.id, "")
    }

    func testAnUnreachableRelayFailsWithoutThrowing() async {
        // A thrown exception here would lose the paragraph the reporter typed.
        let relay = FakeBugRelay()
        relay.offline.value = HTTPError.transport("no route to host")
        let api = wire(relay)

        let result = await api.submitBugReport(message: "Nothing loads.")

        XCTAssertFalse(result.ok)
        XCTAssertTrue(result.message.contains("connection"), result.message)

        // Kotlin: a timeout says so.
        relay.offline.value = HTTPError.timedOut
        let timedOut = await api.submitBugReport(message: "Nothing loads.")
        XCTAssertTrue(timedOut.message.contains("timed out"), timedOut.message)
    }

    func testABugReportNeverBouncesAValidSessionToTheLoginScreen() async {
        // Reports are anonymous-capable, so a non-2xx here says nothing about the session.
        // Treating one as an expiry would sign people out for reporting. (Kotlin adds 401.)
        for status in [429, 401] {
            let relay = FakeBugRelay(status: status, body: portEncode(["error": "rate_limited"]))
            let api = wire(relay)
            api.sessionTokenProvider = { "session-token" }
            var signedOut = false
            api.onUnauthorized = { signedOut = true }
            api.onSessionInvalid = { (_: String) in signedOut = true }

            _ = await api.submitBugReport(message: "Still broken.")

            XCTAssertFalse(signedOut, "status \(status)")
        }
    }

    func testTheInboxListsReportsForAnAdministrator() async throws {
        let body = portEncode([
            "ok": true, "status": "new", "nextOffset": 50,
            "counts": ["new": 2, "triaged": 1, "closed": 0],
            "reports": [[
                "id": "r1", "status": "new", "severity": "crash", "platform": "ios", "appVersion": "1.0.0+2",
                "createdAt": 1_783_463_371_000, "account": "alice", "message": "Repos tab spins.",
                "contact": "reply@example.test",
                "attachments": [["id": "a1", "name": "screen.png", "contentType": "image/png", "size": 6]],
            ]],
        ])
        let relay = FakeBugRelay(status: 200, body: body)
        let api = wire(relay)
        api.sessionTokenProvider = { "session-token" }

        let inbox = await api.bugReportInbox(status: "new", offset: 50)

        XCTAssertTrue(inbox.ok)
        XCTAssertEqual(inbox.counts["new"], 2)
        XCTAssertEqual(inbox.nextOffset, 50)
        let report = try XCTUnwrap(inbox.reports.first)
        XCTAssertEqual(report.message, "Repos tab spins.")
        XCTAssertEqual(report.contact, "reply@example.test")
        XCTAssertEqual(report.createdAt, 1_783_463_371_000)
        XCTAssertEqual(report.attachments.first?.name, "screen.png")
        let request = try XCTUnwrap(relay.http.requests.first)
        XCTAssertEqual(request.method, "GET")
        XCTAssertEqual(portRawPath(request), "/api/bug-reports/inbox")
        XCTAssertEqual(request.url.query, "status=new&offset=50")
    }

    func testTheInboxRefusalSaysItIsForAdministrators() async {
        let relay = FakeBugRelay(status: 403, body: portEncode(["error": "admin_required"]))
        let api = wire(relay)

        let inbox = await api.bugReportInbox()

        XCTAssertFalse(inbox.ok)
        XCTAssertTrue(inbox.reports.isEmpty)
        XCTAssertTrue(inbox.message.contains("administrators"), inbox.message)
    }
}
