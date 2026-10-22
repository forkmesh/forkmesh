// Account-level ApiService calls the service tests reach through fakes: the APNs registration,
// self-test and unregistration, checked here against their wire shapes; and the
// "ApiService requests the dedicated federated projection" case (the federated thread model
// cases are in ModelsTests.swift).

import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

@MainActor
final class ApiAccountTests: XCTestCase {
    // MARK: push registration

    func testRegisteringATokenBindsItToTheAccountAndReadsTheRelayFlag() async throws {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: ["ok": true, "pushConfigured": true])
        http.enqueue(status: 200, json: ["ok": true, "pushConfigured": false])
        http.enqueue(status: 200, json: ["ok": true])
        let api = ApiPortFixture.api(http)
        let token = String(repeating: "a", count: 64)

        let configured = await api.registerPushDevice(deviceToken: token, environment: "development")
        let unconfigured = await api.registerPushDevice(deviceToken: token, environment: "production")
        let predates = await api.registerPushDevice(deviceToken: token, environment: "production")

        XCTAssertEqual(configured, PushRegistration(ok: true, statusCode: 200, relayConfigured: true))
        XCTAssertEqual(unconfigured.relayConfigured, false)
        // A relay that predates the flag leaves the relay state unknown.
        XCTAssertTrue(predates.ok)
        XCTAssertNil(predates.relayConfigured)
        let request = try XCTUnwrap(http.requests.first)
        XCTAssertEqual(portMethodPath(request), "POST /api/accounts/register-device")
        XCTAssertEqual(request.headers["Authorization"], "Bearer session-token")
        XCTAssertEqual(request.headers["Content-Type"], "application/json")
        XCTAssertEqual(request.bodyText, #"{"deviceToken":"\#(token)","environment":"development"}"#)
    }

    func testRegisteringSendsTheVoipTokenBesideTheAlertToken() async throws {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: ["ok": true])
        let api = ApiPortFixture.api(http)
        let token = String(repeating: "a", count: 64)
        let voip = String(repeating: "b", count: 64)
        _ = await api.registerPushDevice(deviceToken: token, environment: "production", voipToken: voip)
        let body = JSON.parseObject(try XCTUnwrap(http.requests.first).bodyText)
        XCTAssertEqual(body["voipToken"]?.text, voip)
        XCTAssertEqual(body["deviceToken"]?.text, token)
    }

    func testRegistrationNeverThrows() async {
        let empty = FakeHTTPClient { _ in portJSON(["ok": true]) }
        let none = await ApiPortFixture.api(empty).registerPushDevice(deviceToken: "", environment: "production")
        XCTAssertEqual(none, PushRegistration(ok: false))
        XCTAssertTrue(empty.requests.isEmpty)

        let refused = await ApiPortFixture.api(FakeHTTPClient { _ in portJSON(["error": "forbidden"], 403) })
            .registerPushDevice(deviceToken: "abc", environment: "production")
        XCTAssertEqual(refused, PushRegistration(ok: false, statusCode: 403, relayConfigured: nil))

        let offline = await ApiPortFixture.api(FakeHTTPClient { _ in throw HTTPError.transport("offline") })
            .registerPushDevice(deviceToken: "abc", environment: "production")
        XCTAssertEqual(offline, PushRegistration(ok: false))
        XCTAssertEqual(offline.statusCode, 0)
    }

    func testATestPushGoesThroughTheRelayAndReadsAppleAnswers() async throws {
        let http = FakeHTTPClient { _ in
            portJSON([
                "ok": true, "configured": true, "devices": 1, "delivered": 1,
                "results": [["environment": "development", "delivered": true, "status": 200, "reason": ""]],
            ])
        }
        let api = ApiPortFixture.api(http)

        let result = await api.sendTestPush()

        XCTAssertEqual(http.lastRequest.map(portMethodPath), "POST /api/accounts/push-test")
        XCTAssertEqual(http.lastRequest?.bodyText, "{}")
        XCTAssertEqual(http.lastRequest?.headers["Authorization"], "Bearer session-token")
        XCTAssertEqual(result.statusCode, 200)
        XCTAssertEqual(result.configured, true)
        XCTAssertEqual(result.results, [PushTestDelivery(environment: "development", delivered: true, status: 200, reason: "")])
        XCTAssertTrue(result.summary.contains("Sent to 1 of 1 device"), result.summary)
        XCTAssertTrue(result.summary.contains("APNs sandbox"), result.summary)

        let unreachable = await ApiPortFixture.api(FakeHTTPClient { _ in throw HTTPError.timedOut }).sendTestPush()
        XCTAssertEqual(unreachable, PushTestResult.unreachable)
    }

    func testATestPushOnAnExpiredSessionSignsOut() async {
        let http = FakeHTTPClient { _ in portJSON(["error": "invalid_session"], 401) }
        let api = ApiPortFixture.api(http)
        var codes: [String] = []
        api.onSessionInvalid = { codes.append($0) }

        let result = await api.sendTestPush()

        XCTAssertEqual(result.statusCode, 401)
        XCTAssertEqual(result.summary, "The session has expired. Sign in again.")
        XCTAssertEqual(codes, ["invalid_session"])
    }

    func testSigningOutDropsTheRegistration() async throws {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: ["ok": true])
        http.enqueue(status: 500, text: "")
        let api = ApiPortFixture.api(http)

        let dropped = await api.unregisterPushDevice("tok-1")
        let failed = await api.unregisterPushDevice("tok-1")
        let empty = await api.unregisterPushDevice("")

        XCTAssertTrue(dropped)
        XCTAssertFalse(failed)
        XCTAssertFalse(empty)
        XCTAssertEqual(http.requests.count, 2)
        let request = try XCTUnwrap(http.requests.first)
        XCTAssertEqual(portMethodPath(request), "DELETE /api/accounts/register-device")
        XCTAssertEqual(request.bodyText, #"{"deviceToken":"tok-1"}"#)
        XCTAssertEqual(request.headers["content-type"], "application/json")
        XCTAssertEqual(request.headers["Authorization"], "Bearer session-token")

        let offline = await ApiPortFixture.api(FakeHTTPClient { _ in throw HTTPError.transport("offline") })
            .unregisterPushDevice("tok-1")
        XCTAssertFalse(offline)
    }

    // MARK: federated thread

    func testApiServiceRequestsTheDedicatedFederatedProjection() async throws {
        let http = FakeHTTPClient { _ in
            portJSON([
                "ok": true,
                "comments": [[
                    "remoteId": "https://lemmy.example/comment/10",
                    "author": "bob@lemmy.example",
                    "body": "hello from Lemmy",
                    "url": "https://lemmy.example/comment/10",
                    "sourceInstance": "lemmy.example",
                    "sourceSoftware": "lemmy",
                    "lifecycle": "active",
                    "nativeEvent": false,
                ]],
            ])
        }
        let api = ApiPortFixture.api(http, token: "")

        let replies = try await api.federatedReplies("forkmesh", "forkmesh", kind: "discussion", number: 9)

        let requested = try XCTUnwrap(http.lastRequest)
        XCTAssertEqual(portRawPath(requested), "/api/repo/forkmesh/forkmesh/fedi-comments")
        XCTAssertEqual(portQuery(requested)["kind"], "discussion")
        XCTAssertEqual(portQuery(requested)["number"], "9")
        XCTAssertEqual(replies.count, 1)
        XCTAssertEqual(replies.first?.body, "hello from Lemmy")
        XCTAssertEqual(replies.first?.nativeEvent, false)
    }
}
