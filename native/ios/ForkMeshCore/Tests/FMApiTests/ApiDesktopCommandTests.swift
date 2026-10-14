import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

@MainActor
final class ApiDesktopCommandTests: XCTestCase {
    func testTheRequestCarriesNoSessionTokenAndARefusalNeverSignsOut() async throws {
        let http = FakeHTTPClient { _ in apiJSON(["error": "invalid_signature"], status: 401) }
        let api = makeTestApi(http, token: "tok")
        var unauthorized = 0
        var codes: [String] = []
        api.onUnauthorized = { unauthorized += 1 }
        api.onSessionInvalid = { codes.append($0) }
        do {
            _ = try await api.desktopCommand(
                "alice", "repo", ownerAccount: "alice", command: "pull.merge", target: "pull:7", ts: "1", sig: "s")
            XCTFail("a refused request throws")
        } catch {
            XCTAssertEqual(
                "\(error)".contains("could not verify this device's signature"), true, "\(error)")
        }
        XCTAssertNil(http.lastRequest?.headers["Authorization"])
        XCTAssertEqual(http.lastRequest?.headers["Content-Type"], "application/json")
        XCTAssertEqual(unauthorized, 0)
        XCTAssertEqual(codes, [])
    }

    func testServerRefusalsReadAsSentences() {
        XCTAssertEqual(
            DesktopCommandResult.errorMessage(status: 401, code: "stale_request"),
            "This device's clock is more than five minutes off. Correct the time and try again.")
        XCTAssertEqual(DesktopCommandResult.errorMessage(status: 409, code: "replayed_request"), "That request was already sent.")
        XCTAssertEqual(
            DesktopCommandResult.errorMessage(status: 429, code: "command_queue_full"),
            "Fifty requests are already waiting for the desktop. Approve or decline them there first.")
        XCTAssertEqual(
            DesktopCommandResult.errorMessage(status: 403, code: "not_authorized"),
            "Only the repo owner or an organization member with write access can request desktop control for this repo.")
        XCTAssertEqual(
            DesktopCommandResult.errorMessage(status: 404, code: ""), "This server does not take desktop requests yet.")
        XCTAssertEqual(DesktopCommandResult.errorMessage(status: 500, code: ""), "Desktop request was not queued (HTTP 500).")
    }

    func testTheRouteProbeRecognisesTheServerAndItsAbsence() async {
        let served = FakeHTTPClient { _ in apiJSON(["error": "not_authorized"], status: 403) }
        let probeServed = makeTestApi(served, token: "tok")
        let answered = await probeServed.desktopCommandRouteAnswered("alice", "repo")
        XCTAssertEqual(answered, true)
        let request = try? XCTUnwrap(served.lastRequest)
        XCTAssertEqual(request?.method, "POST")
        XCTAssertEqual(request?.apiPath, "/api/repo/alice/repo/desktop-commands")
        XCTAssertEqual(request?.bodyText, "{}")
        XCTAssertNil(request?.headers["Authorization"])

        let missing = FakeHTTPClient { _ in apiText("not found", status: 404) }
        let absent = await makeTestApi(missing).desktopCommandRouteAnswered("alice", "repo")
        XCTAssertEqual(absent, false)

        let broken = FakeHTTPClient { _ in apiText("oops", status: 502) }
        let unknown = await makeTestApi(broken).desktopCommandRouteAnswered("alice", "repo")
        XCTAssertNil(unknown)

        XCTAssertEqual(DesktopCommandResult.routeAnswered(status: 403, code: "forbidden"), nil)
        XCTAssertEqual(DesktopCommandResult.routeAnswered(status: 400, code: "unsupported_command"), true)
        XCTAssertEqual(DesktopCommandResult.routeAnswered(status: 405, code: ""), false)
    }
}
