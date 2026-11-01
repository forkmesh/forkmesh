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

    func testTheHomeCountAsksForOneReportOnly() async throws {
        let body = portEncode(["ok": true, "status": "new", "counts": ["new": 7, "triaged": 2, "closed": 4], "reports": []])
        let relay = FakeBugRelay(status: 200, body: body)

        let inbox = await wire(relay).bugReportInbox(status: "new", limit: 1)

        XCTAssertEqual(inbox.counts["new"], 7)
        XCTAssertEqual(try XCTUnwrap(relay.http.requests.first).url.query, "status=new&limit=1")
    }

    func testTheInboxRefusalSaysItIsForAdministrators() async {
        let relay = FakeBugRelay(status: 403, body: portEncode(["error": "admin_required"]))
        let api = wire(relay)

        let inbox = await api.bugReportInbox()

        XCTAssertFalse(inbox.ok)
        XCTAssertTrue(inbox.reports.isEmpty)
        XCTAssertTrue(inbox.message.contains("administrators"), inbox.message)
    }

    func testAnAdministratorSetsAReportStatus() async throws {
        let relay = FakeBugRelay(status: 200, body: portEncode([
            "ok": true, "id": "r1", "status": "closed", "previous": "new", "counts": ["new": 0, "closed": 1],
        ]))
        let api = wire(relay)
        api.sessionTokenProvider = { "session-token" }

        let change = await api.setBugReportStatus(id: "r1", status: "closed")

        XCTAssertTrue(change.ok)
        XCTAssertEqual(change.status, "closed")
        XCTAssertEqual(change.counts["closed"], 1)
        let request = try XCTUnwrap(relay.http.requests.first)
        XCTAssertEqual(request.method, "POST")
        XCTAssertEqual(portRawPath(request), "/api/bug-reports/inbox")
        XCTAssertEqual(relay.submitted.first?["report"], "r1")
        XCTAssertEqual(relay.submitted.first?["status"], "closed")

        let refused = await wire(FakeBugRelay(status: 403, body: portEncode(["error": "admin_required"])))
            .setBugReportStatus(id: "r1", status: "closed")
        XCTAssertFalse(refused.ok)
        XCTAssertTrue(refused.message.contains("administrators"), refused.message)
    }

    func testAnAdministratorDownloadsAnAttachment() async throws {
        let http = FakeHTTPClient { _ in
            HTTPResponse(status: 200, headers: ["content-type": "image/png"], body: Data("pixels".utf8))
        }
        let api = ApiPortFixture.api(http, token: "")
        api.sessionTokenProvider = { "session-token" }

        let file = await api.bugReportAttachment(reportId: "r1", attachmentId: "a1")

        XCTAssertTrue(file.ok)
        XCTAssertEqual(file.data, Data("pixels".utf8))
        XCTAssertEqual(file.contentType, "image/png")
        let request = try XCTUnwrap(http.requests.first)
        XCTAssertEqual(portRawPath(request), "/api/bug-reports/inbox/attachment")
        XCTAssertEqual(request.url.query, "report=r1&attachment=a1")

        let gone = await wire(FakeBugRelay(status: 410, body: portEncode(["error": "attachment_unavailable"])))
            .bugReportAttachment(reportId: "r1", attachmentId: "a1")
        XCTAssertFalse(gone.ok)
        XCTAssertTrue(gone.message.contains("no longer available"), gone.message)
    }

    func testACancelledInboxLoadIsNotReportedAsUnreachable() async {
        for error in [HTTPError.cancelled as Error, CancellationError()] {
            let relay = FakeBugRelay()
            relay.offline.value = error
            let inbox = await wire(relay).bugReportInbox(status: "new")
            XCTAssertTrue(inbox.cancelled)
            XCTAssertFalse(inbox.ok)
            XCTAssertEqual(inbox.message, "")
        }
        let relay = FakeBugRelay()
        relay.offline.value = HTTPError.transport("no route to host")
        let offline = await wire(relay).bugReportInbox()
        XCTAssertFalse(offline.cancelled)
        XCTAssertTrue(offline.message.contains("Could not reach ForkMesh"), offline.message)
    }

    func testARelayWithoutTheInboxSaysToUpdateTheRelay() async {
        let notFound = portEncode(["error": "not_found"])
        let inbox = await wire(FakeBugRelay(status: 404, body: notFound)).bugReportInbox()
        XCTAssertTrue(inbox.message.contains("Update the relay"), inbox.message)
        let change = await wire(FakeBugRelay(status: 404, body: notFound)).setBugReportStatus(id: "r1", status: "closed")
        XCTAssertTrue(change.message.contains("Update the relay"), change.message)
        let file = await wire(FakeBugRelay(status: 404, body: notFound)).bugReportAttachment(reportId: "r1", attachmentId: "a1")
        XCTAssertTrue(file.message.contains("Update the relay"), file.message)

        let gone = await wire(FakeBugRelay(status: 404, body: portEncode(["error": "report_missing"])))
            .setBugReportStatus(id: "r1", status: "closed")
        XCTAssertTrue(gone.message.contains("no longer exists"), gone.message)
        let missing = await wire(FakeBugRelay(status: 404, body: portEncode(["error": "attachment_missing"])))
            .bugReportAttachment(reportId: "r1", attachmentId: "a1")
        XCTAssertTrue(missing.message.contains("no longer available"), missing.message)
    }

    func testAReporterListsTheirOwnReports() async throws {
        let body = portEncode([
            "ok": true, "offset": 50, "nextOffset": 100,
            "counts": ["new": 1, "triaged": 1, "closed": 0],
            "reports": [[
                "id": "r1", "status": "triaged", "severity": "high", "platform": "ios", "appVersion": "1.0.0+2",
                "createdAt": 1_783_463_371_000, "message": "Repos tab spins.", "screen": "code", "package": "",
                "confirmedFixedAt": 0,
                "attachments": [["name": "screen.png", "size": 6]],
            ]],
        ])
        let relay = FakeBugRelay(status: 200, body: body)
        let api = wire(relay)
        api.sessionTokenProvider = { "session-token" }

        let mine = await api.myBugReports(offset: 50)

        XCTAssertTrue(mine.ok)
        XCTAssertEqual(mine.counts["triaged"], 1)
        XCTAssertEqual(mine.nextOffset, 100)
        let report = try XCTUnwrap(mine.reports.first)
        XCTAssertEqual(report.id, "r1")
        XCTAssertEqual(report.status, "triaged")
        XCTAssertEqual(report.severity, "high")
        XCTAssertEqual(report.screen, "code")
        XCTAssertEqual(report.createdAt, 1_783_463_371_000)
        XCTAssertEqual(report.confirmedFixedAt, 0)
        XCTAssertFalse(report.confirmedFixed)
        XCTAssertEqual(report.attachments.first?.name, "screen.png")
        XCTAssertEqual(report.attachments.first?.size, 6)
        let request = try XCTUnwrap(relay.http.requests.first)
        XCTAssertEqual(request.method, "GET")
        XCTAssertEqual(portRawPath(request), "/api/bug-reports/mine")
        XCTAssertEqual(request.url.query, "offset=50")
        XCTAssertEqual(request.headers["Authorization"], "Bearer session-token")

        let last = await wire(FakeBugRelay(status: 200, body: portEncode([
            "ok": true, "counts": [:], "reports": [], "offset": 0, "nextOffset": nil,
        ]))).myBugReports()
        XCTAssertTrue(last.ok)
        XCTAssertNil(last.nextOffset)
    }

    func testTheFirstPageOfMyReportsSendsNoOffset() async throws {
        let relay = FakeBugRelay(status: 200, body: portEncode(["ok": true, "counts": [:], "reports": []]))

        _ = await wire(relay).myBugReports()

        let request = try XCTUnwrap(relay.http.requests.first)
        XCTAssertEqual(portRawPath(request), "/api/bug-reports/mine")
        XCTAssertNil(request.url.query)
    }

    func testMyReportsSignedOutSaysToSignIn() async {
        let relay = FakeBugRelay(status: 401, body: portEncode(["error": "sign_in_required"]))

        let mine = await wire(relay).myBugReports()

        XCTAssertFalse(mine.ok)
        XCTAssertTrue(mine.message.contains("Sign in"), mine.message)
        let confirm = await wire(FakeBugRelay(status: 401, body: portEncode(["error": "sign_in_required"])))
            .confirmBugReportFixed(id: "r1")
        XCTAssertFalse(confirm.ok)
        XCTAssertTrue(confirm.message.contains("Sign in"), confirm.message)
    }

    func testAReporterConfirmsAFix() async throws {
        let relay = FakeBugRelay(status: 200, body: portEncode([
            "ok": true, "previous": "triaged", "counts": ["new": 0, "triaged": 0, "closed": 1],
            "report": [
                "id": "r1", "status": "closed", "severity": "normal", "platform": "ios", "appVersion": "1.0.0+2",
                "createdAt": 1_783_463_371_000, "message": "Repos tab spins.", "screen": "", "package": "",
                "confirmedFixedAt": 1_783_500_000_000, "attachments": [],
            ],
        ]))
        let api = wire(relay)
        api.sessionTokenProvider = { "session-token" }

        let confirmation = await api.confirmBugReportFixed(id: "r1")

        XCTAssertTrue(confirmation.ok)
        XCTAssertEqual(confirmation.previous, "triaged")
        XCTAssertEqual(confirmation.counts["closed"], 1)
        let report = try XCTUnwrap(confirmation.report)
        XCTAssertEqual(report.status, "closed")
        XCTAssertEqual(report.confirmedFixedAt, 1_783_500_000_000)
        XCTAssertTrue(report.confirmedFixed)
        let request = try XCTUnwrap(relay.http.requests.first)
        XCTAssertEqual(request.method, "POST")
        XCTAssertEqual(portRawPath(request), "/api/bug-reports/mine")
        XCTAssertEqual(relay.submitted.first?["report"], "r1")
        XCTAssertEqual(relay.submitted.first?["fixed"], true)

        let missing = await wire(FakeBugRelay(status: 404, body: portEncode(["error": "report_missing"])))
            .confirmBugReportFixed(id: "r1")
        XCTAssertFalse(missing.ok)
        XCTAssertTrue(missing.message.contains("no longer exists"), missing.message)
        let old = await wire(FakeBugRelay(status: 404, body: portEncode(["error": "not_found"])))
            .confirmBugReportFixed(id: "r1")
        XCTAssertTrue(old.message.contains("Update the relay"), old.message)
    }

    func testACancelledLoadOfMyReportsIsNotReportedAsUnreachable() async {
        let relay = FakeBugRelay()
        relay.offline.value = HTTPError.cancelled
        let mine = await wire(relay).myBugReports()
        XCTAssertTrue(mine.cancelled)
        XCTAssertEqual(mine.message, "")
    }
}
