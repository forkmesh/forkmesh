// The ApiService-coupled mobile node cases, as in native/android MobileNodeApiTest.kt: the node
// registry (`ApiService.listDevices`) and a device_revoked 401 reaching AuthService through
// `onSessionInvalid`. Also the session list and revoke calls of the same settings screen
// (`listSessions` / `revokeSession`). The AuthSession parse, login-bind and pure revocation cases
// are FMAccount's (AuthServiceTests.swift); the settings-screen and UserPresence groups are
// widget/app tests.

import FMAccount
import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest
@testable import FMApi

/// `_boundPayload()`.
private func mobileBoundPayload() -> JSONObject {
    [
        "nodeName": "alice",
        "name": "alice",
        "email": "alice@example.com",
        "status": "active",
        "sessionToken": "mobile-session",
        "sessionKind": "mobile_node",
        "deviceKind": "mobile_node",
        "deviceKeyMatched": true,
        "desktopCapable": false,
        "capabilities": ["browse", "comment", "submit_issue", "submit_pr", "publish_repo", "owner_sign"],
        "canHost": false,
        "canPublish": true,
        "canOwnerSign": true,
        "deviceId": "dev-mobile-1",
        "devices": [],
    ]
}

@MainActor
final class ApiMobileNodeTests: XCTestCase {
    func testDeviceRevoked401ClearsTheSessionRotatesTheKeyAndArmsTheBanner() async throws {
        let prefs = InMemoryKeyValueStore([:])
        let secure = InMemorySecureStore([:])
        let settings = SettingsService.create(prefs: prefs, secureStore: secure)
        settings.setServerUrl("ws://test.local/ws")
        let identity = try Identity.loadOrCreate(secureStore: secure, prefs: prefs)
        let originalKey = identity.publicKeyB64url
        let originalSeed = secure.read(Identity.seedKey)
        let auth = AuthService(
            settings: settings, identity: identity, http: FakeHTTPClient { _ in portJSON([:]) },
            session: AuthSession.fromJSON(mobileBoundPayload()))
        let http = FakeHTTPClient { _ in portJSON(["error": "device_revoked"], 401) }
        let api = ApiService(settings: settings, http: http, clock: FixedClock(epochMillis: 1_700_000_000_000), sleep: { _ in })
        api.sessionTokenProvider = { auth.session?.sessionToken ?? "" }
        var codes: [String] = []
        api.onSessionInvalid = { code in
            codes.append(code)
            auth.handleUnauthorized(code: code)
        }

        let error = await portCaughtError { try await api.repositories() }

        XCTAssertTrue(error is ApiException, "\(String(describing: error))")
        XCTAssertEqual((error as? ApiException)?.message, "This device was signed out from the web. Please sign in again.")
        XCTAssertEqual(http.requests.first?.headers["Authorization"], "Bearer mobile-session")
        XCTAssertEqual(codes, ["device_revoked"])
        XCTAssertFalse(auth.isAuthenticated)
        XCTAssertTrue(auth.revokedBannerPending)
        // The banner is armed under the relay that revoked us, not globally: a revocation on
        // one relay says nothing about the others.
        XCTAssertEqual(prefs.bool(forKey: AuthService.revokedBannerKeyFor(settings.relays.activeId)), true)
        XCTAssertNotEqual(identity.publicKeyB64url, originalKey)
        let newSeed = secure.read(Identity.seedKey)
        XCTAssertNotNil(newSeed)
        XCTAssertNotEqual(newSeed, originalSeed)
        XCTAssertNil(secure.read(AuthService.sessionKey))

        auth.clearRevokedBanner()
        XCTAssertFalse(auth.revokedBannerPending)
    }

    func testListDevicesGetsTheRegistryWithTheBearerTokenAndParsesRows() async throws {
        let http = FakeHTTPClient { _ in
            portJSON([
                "devices": [
                    [
                        "id": "desk-1", "kind": "desktop_node", "label": "MacBook Pro",
                        "capabilities": ["host_repo", "mirror_repo", "publish_repo", "owner_sign"],
                        "enabled": true, "lastSeen": 100, "createdAt": 50, "current": false,
                    ],
                    [
                        "id": "mob-1", "kind": "mobile_node", "label": "iPhone 15 Pro · iOS 19.0",
                        "capabilities": ["publish_repo", "owner_sign"],
                        "enabled": true, "lastSeen": 200, "createdAt": 150, "current": true,
                    ],
                    [
                        "id": "mob-0", "kind": "mobile_node", "label": "Old phone", "capabilities": [],
                        "enabled": false, "lastSeen": 10, "createdAt": 5, "revokedAt": 20, "current": false,
                    ],
                ],
            ])
        }
        let api = ApiPortFixture.api(http, token: "tok", serverUrl: "ws://test.local/ws")

        let devices = try await api.listDevices()

        let seen = try XCTUnwrap(http.lastRequest)
        XCTAssertEqual(http.requests.count, 1)
        XCTAssertEqual(seen.method, "GET")
        XCTAssertEqual(portRawPath(seen), "/api/accounts/devices")
        XCTAssertEqual(seen.headers["Authorization"], "Bearer tok")
        XCTAssertEqual(devices.count, 3)
        guard devices.count == 3 else { return }
        XCTAssertTrue(devices[0].isDesktopNode)
        XCTAssertTrue(devices[0].canHost)
        XCTAssertTrue(devices[1].current)
        XCTAssertEqual(devices[1].createdAt, 150)
        XCTAssertEqual(devices[1].capabilitySummary, "Publishes · signs as owner")
        XCTAssertTrue(devices[2].isRevoked)
        XCTAssertEqual(devices[2].revokedAt, 20)
        XCTAssertFalse(devices[2].enabled)
    }

    /// A bare list is accepted too (`data is List`).
    func testListDevicesAcceptsABareList() async throws {
        let http = FakeHTTPClient { _ in portJSON([["id": "desk-1", "kind": "desktop_node"], "junk"]) }
        let devices = try await ApiPortFixture.api(http).listDevices()
        XCTAssertEqual(devices.map(\.id), ["desk-1"])
    }

    func testListSessionsReadsLiveSignInsAndDropsRowsWithoutAnId() async throws {
        let http = FakeHTTPClient { _ in
            portJSON([
                "sessions": [
                    ["id": "s1", "deviceLabel": "iPhone", "current": true, "lastSeenAt": 200],
                    ["id": "", "deviceLabel": "ghost"],
                    ["id": "s2", "deviceLabel": "Firefox"],
                ],
            ])
        }
        let api = ApiPortFixture.api(http, token: "tok")

        let sessions = try await api.listSessions()

        XCTAssertEqual(http.lastRequest.map(portMethodPath), "GET /api/accounts/sessions")
        XCTAssertEqual(http.lastRequest?.headers["Authorization"], "Bearer tok")
        XCTAssertEqual(sessions.map(\.id), ["s1", "s2"])
        XCTAssertEqual(sessions.first?.current, true)
    }

    func testRevokeSessionDeletesByEncodedIdAndReportsTheCurrentOne() async throws {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: ["ok": true, "currentRevoked": false])
        http.enqueue(status: 200, json: ["ok": true, "currentRevoked": true])
        let api = ApiPortFixture.api(http, token: "tok")

        let first = try await api.revokeSession("a b/c")
        let second = try await api.revokeSession("all")

        XCTAssertFalse(first)
        XCTAssertTrue(second)
        XCTAssertEqual(http.requests.map(portMethodPath), [
            "DELETE /api/accounts/sessions/a%20b%2Fc",
            "DELETE /api/accounts/sessions/all",
        ])
        XCTAssertEqual(http.requests.first?.bodyText, "{}")
        XCTAssertEqual(http.requests.first?.headers["Authorization"], "Bearer tok")
    }
}
