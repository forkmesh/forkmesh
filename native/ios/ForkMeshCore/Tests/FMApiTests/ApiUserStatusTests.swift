// ApiService user status, as in native/android UserStatusApiTest.kt: `POST
// /api/accounts/user-status` and the status a profile lookup carries. The model, ClearAfter and
// message cases are in UserStatusTests.swift; the UserStatusService case belongs with that
// service's tests.

import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

private func statusAnswer(_ status: JSONObject) -> HTTPResponse {
    portJSON(["ok": true, "nodeName": "alice", "userStatus": .object(status)])
}

@MainActor
final class ApiUserStatusTests: XCTestCase {
    private func statusApi(_ http: FakeHTTPClient, token: String = "session-token") -> ApiService {
        ApiPortFixture.api(http, token: token, serverUrl: "ws://test.local/ws")
    }

    func testPostsChangesWithTheBearerTokenAndParsesTheReply() async throws {
        let http = FakeHTTPClient { _ in
            portJSON([
                "ok": true,
                "nodeName": "ada",
                "userStatus": ["away": false, "emoji": "🚌", "text": "Commuting", "expiresAt": 1_760_000_000_000],
            ])
        }
        let api = statusApi(http)

        let status = try await api.updateUserStatus(["emoji": "🚌", "text": "Commuting", "expiresAt": 1_760_000_000_000])

        let sent = try XCTUnwrap(http.lastRequest)
        XCTAssertEqual(sent.method, "POST")
        XCTAssertEqual(portRawPath(sent), "/api/accounts/user-status")
        XCTAssertEqual(sent.headers["authorization"], "Bearer session-token")
        XCTAssertEqual(portBody(sent), ["emoji": "🚌", "text": "Commuting", "expiresAt": 1_760_000_000_000])
        XCTAssertEqual(sent.bodyText, #"{"emoji":"🚌","text":"Commuting","expiresAt":1760000000000}"#)
        XCTAssertEqual(status.emoji, "🚌")
        XCTAssertEqual(status.expiresAt, 1_760_000_000_000)
    }

    /// Kotlin: an empty body reads the account's own status.
    func testAnEmptyPayloadReadsTheOwnStatusWithTheBearerSession() async throws {
        let http = FakeHTTPClient { _ in
            statusAnswer(["away": true, "emoji": "🌴", "text": "Vacationing", "expiresAt": 0])
        }
        let api = statusApi(http, token: "tok")

        let status = try await api.updateUserStatus()

        let sent = try XCTUnwrap(http.lastRequest)
        XCTAssertEqual(http.requests.count, 1)
        XCTAssertEqual(portMethodPath(sent), "POST /api/accounts/user-status")
        XCTAssertEqual(sent.bodyText, "{}")
        XCTAssertEqual(status, UserStatus(away: true, emoji: "🌴", text: "Vacationing"))
        XCTAssertEqual(sent.headers["Authorization"], "Bearer tok")
        XCTAssertEqual(sent.headers["Content-Type"], "application/json")
    }

    /// Kotlin: the set and away bodies go out as built.
    func testSetAndAwayBodiesAreSentAsBuilt() async throws {
        let http = FakeHTTPClient { _ in
            statusAnswer(["away": false, "emoji": "📅", "text": "In a meeting", "expiresAt": 99])
        }
        let api = statusApi(http)

        try await api.updateUserStatus(["emoji": "📅", "text": "In a meeting", "expiresAt": 99])
        try await api.updateUserStatus(["away": false])

        XCTAssertEqual(http.requests.map(\.bodyText), [
            #"{"emoji":"📅","text":"In a meeting","expiresAt":99}"#,
            #"{"away":false}"#,
        ])
    }

    func testARefusalSurfacesTheServerCodeWithAReadableMessage() async {
        let http = FakeHTTPClient { _ in
            HTTPResponse(status: 400, text: #"{"error":"status_expiry_in_past"}"#)
        }
        let api = statusApi(http)

        let error = await portCaughtError { try await api.updateUserStatus(["expiresAt": 1]) }
        let refusal = error as? UserStatusException
        XCTAssertNotNil(refusal, "\(String(describing: error))")
        XCTAssertEqual(refusal?.code, "status_expiry_in_past")
        XCTAssertEqual(refusal?.message, userStatusErrorMessage("status_expiry_in_past"))
        XCTAssertEqual(refusal?.message, "Choose a clear-after time in the future.")

        // Kotlin: a body with no code reads as the status.
        let bare = await portCaughtError {
            try await self.statusApi(FakeHTTPClient { _ in HTTPResponse(status: 500, text: "oops") }).updateUserStatus()
        } as? UserStatusException
        XCTAssertEqual(bare?.code, "http_500")
    }

    /// Kotlin: a 401 signs the session out like every other bearer call.
    func testUnauthorizedSignsTheSessionOut() async {
        let http = FakeHTTPClient { _ in portJSON(["error": "unauthorized"], 401) }
        let api = statusApi(http, token: "tok")
        var unauthorized = 0
        var code = ""
        api.onUnauthorized = { unauthorized += 1 }
        api.onSessionInvalid = { code = $0 }

        let error = await portCaughtError { try await api.updateUserStatus(["away": true]) } as? UserStatusException

        XCTAssertEqual(error?.code, "unauthorized")
        XCTAssertEqual(unauthorized, 1)
        XCTAssertEqual(code, "unauthorized")
    }

    func testProfileLookupsCarryTheStatusAndShareTheAvatarRead() async {
        let http = FakeHTTPClient { request in
            if portRawPath(request) != "/api/accounts/ada" {
                return HTTPResponse(status: 404, text: "{}")
            }
            return portJSON([
                "kind": "user",
                "avatarPng": "cG5n",
                "userStatus": ["away": true, "emoji": "🤒", "text": "Out sick"],
            ])
        }
        let api = statusApi(http)

        let status = await api.accountUserStatus("Ada")
        XCTAssertTrue(status.away)
        XCTAssertEqual(status.emoji, "🤒")
        let avatar = await api.accountAvatarPng("ada")
        XCTAssertEqual(avatar, "cG5n")
        XCTAssertEqual(http.requests.count, 1)
        let first = await api.accountUserStatus("some-node")
        let second = await api.accountUserStatus("some-node")
        XCTAssertEqual(first, UserStatus.empty)
        XCTAssertEqual(second, UserStatus.empty)
        XCTAssertEqual(http.requests.count, 2, "a miss is remembered for the session")
    }
}
