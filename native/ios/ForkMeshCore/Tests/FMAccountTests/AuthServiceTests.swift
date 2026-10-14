// The AuthService / AuthSession cases (Kotlin: api/MobileNodeAuthTest.kt, api/GuestAuthTest.kt).

import FMBase
import FMCrypto
import FMNet
import XCTest
@testable import FMAccount

func boundPayload(name: String = "alice", deviceId: String = "dev-mobile-1", token: String = "mobile-session", devices: [JSONValue] = []) -> JSONObject {
    [
        "nodeName": .string(name),
        "name": .string(name),
        "email": "alice@example.com",
        "status": "active",
        "sessionToken": .string(token),
        "sessionKind": "mobile_node",
        "deviceKind": "mobile_node",
        "deviceKeyMatched": true,
        "desktopCapable": false,
        "capabilities": ["browse", "comment", "submit_issue", "submit_pr", "publish_repo", "owner_sign"],
        "canHost": false,
        "canPublish": true,
        "canOwnerSign": true,
        "deviceId": .string(deviceId),
        "devices": .array(devices),
    ]
}

func plainPayload(name: String = "alice") -> JSONObject {
    [
        "nodeName": .string(name),
        "name": .string(name),
        "email": "alice@example.com",
        "status": "active",
        "sessionToken": "plain-session",
        "sessionKind": "account",
        "deviceKind": "web_or_mobile",
        "capabilities": ["browse", "comment", "submit_issue", "submit_pr"],
    ]
}

/// Verifies the device-bind proof the way the Worker does.
func verifyBind(_ body: JSONObject, account: String) -> Bool {
    let pubkey = body.str("pubkey")
    let ts = body.str("deviceTs")
    guard let pub = try? B64.decodeUnpadded(pubkey), let sig = try? B64.decodeUnpadded(body.str("deviceSig")) else { return false }
    let canonical = Array("forkmesh-device-bind-v1\n\(account)\n\(pubkey)\n\(ts)".utf8)
    return Ed25519.verify(publicKey: Array(pub), message: canonical, signature: Array(sig))
}

final class AuthServiceTests: XCTestCase {
    // MARK: AuthSession payload

    func testParsesDeviceIdSessionKindMobileDevicesAndIsMobileNode() {
        let session = AuthSession.fromJSON(boundPayload(devices: [
            [
                "id": "dev-desktop", "kind": "desktop_node", "label": "MacBook Pro",
                "capabilities": ["host_repo", "publish_repo", "owner_sign"], "enabled": true, "lastSeen": 1, "createdAt": 2,
            ],
            [
                "id": "dev-mobile-1", "kind": "mobile_node", "label": "iPhone 15 Pro · iOS 19.0",
                "capabilities": "browse,publish_repo,owner_sign", "enabled": true, "lastSeen": 3, "createdAt": 4,
                "current": true,
            ],
            [
                "id": "dev-mobile-old", "kind": "mobile_node", "label": "Old phone",
                "capabilities": ["publish_repo", "owner_sign"], "enabled": false, "revokedAt": 5,
            ],
        ]))

        XCTAssertEqual(session.sessionKind, "mobile_node")
        XCTAssertEqual(session.deviceId, "dev-mobile-1")
        XCTAssertTrue(session.canOwnerSign)
        XCTAssertTrue(session.canPublish)
        XCTAssertFalse(session.canHost)
        XCTAssertTrue(session.isMobileNode)
        XCTAssertEqual(session.desktopDevices.map(\.id), ["dev-desktop"])
        XCTAssertEqual(session.mobileDevices.map(\.id), ["dev-mobile-1"])
        XCTAssertEqual(session.thisDevice?.label, "iPhone 15 Pro · iOS 19.0")
        XCTAssertEqual(session.thisDevice?.current, true)
        XCTAssertEqual(session.thisDevice?.createdAt, 4)
        XCTAssertEqual(session.thisDevice?.capabilitySummary, "Publishes · signs as owner")

        let revoked = session.devices[2]
        XCTAssertTrue(revoked.isRevoked)
        XCTAssertEqual(revoked.revokedAt, 5)
        XCTAssertEqual(revoked.statusLabel, "Revoked")
        XCTAssertEqual(revoked.kindLabel, "Mobile node")

        // Round-trips through the persisted form.
        let restored = AuthSession.fromJSON(session.toJSON())
        XCTAssertEqual(restored, session)
        XCTAssertEqual(restored.devices[2].revokedAt, 5)
        XCTAssertTrue(restored.devices[1].current)
    }

    func testAPlainAccountSessionIsNotAMobileNode() {
        let session = AuthSession.fromJSON(plainPayload())
        XCTAssertFalse(session.isMobileNode)
        XCTAssertEqual(session.deviceId, "")
        XCTAssertNil(session.thisDevice)
    }

    func testAuthSessionParsesDesktopDeviceInventoryFromLoginPayload() {
        let session = AuthSession.fromJSON([
            "nodeName": "alice", "email": "alice@example.com", "status": "active", "pubkey": "desktop-primary-key",
            "emailVerified": true,
            "devices": [
                [
                    "id": "dev-1", "pubkey": "desktop-primary-key", "kind": "desktop_node", "label": "MacBook Pro",
                    "capabilities": ["host_repo", "publish_repo", "owner_sign"], "enabled": true, "lastSeen": 1_782_999_999_000,
                ],
                [
                    "id": "dev-2", "pubkey": "phone-key", "kind": "mobile_client", "label": "iPhone",
                    "capabilities": ["browse", "comment"], "enabled": true,
                ],
            ],
        ], now: 99)
        XCTAssertEqual(session.devices.count, 2)
        XCTAssertEqual(session.desktopDevices.first?.label, "MacBook Pro")
        XCTAssertTrue(session.desktopDevices.first?.canOwnerSign ?? false)
        XCTAssertTrue(session.desktopDevices.first?.capabilityLabel.contains("Host repo") ?? false)
        XCTAssertEqual(session.devices[1].capabilityLabel, "Browse · Comment")
        XCTAssertEqual(session.savedAt, 99, "a payload without `at` is stamped now")
        XCTAssertEqual(session.sessionKind, "account")
    }

    func testAccountDeviceAndSessionHelpers() {
        let device = AccountDevice.fromJSON([
            "id": "0123456789abcdef", "kind": "desktop_node", "label": "  ", "capabilities": "host_repo,,owner_sign",
            "enabled": 0, "last_seen": 7, "created_at": "8", "revoked_at": .null,
        ])
        XCTAssertFalse(device.enabled, "0 disables, as in Dart")
        XCTAssertEqual(device.capabilities, ["host_repo", "owner_sign"])
        XCTAssertEqual(device.lastSeen, 7)
        XCTAssertEqual(device.createdAt, 8)
        XCTAssertEqual(device.displayName, "Desktop node")
        XCTAssertEqual(device.statusLabel, "Disabled")
        XCTAssertEqual(device.shortId, "012345…cdef")
        XCTAssertEqual(device.capabilitySummary, "Hosts · signs as owner")
        XCTAssertEqual(AccountDevice.fromJSON(["enabled": .null]).enabled, true)

        let row = AccountSession.fromJSON(["id": "s1", "deviceLabel": "  ", "ipAddress": " 1.2.3.4 ", "current": true])
        XCTAssertEqual(row.deviceLabel, "Unknown device")
        XCTAssertEqual(row.ipAddress, "1.2.3.4")
        XCTAssertTrue(row.current)
    }

    func testDeviceLabels() {
        XCTAssertEqual(DeviceInfo(modelName: "iPhone 15 Pro", model: "iPhone", systemName: "iOS", systemVersion: "19.0").label,
                       "iPhone 15 Pro · iOS 19.0")
        XCTAssertEqual(DeviceInfo(model: "iPhone", systemName: "iOS", systemVersion: "19.0").label, "iPhone · iOS 19.0")
        XCTAssertEqual(DeviceInfo().label, "Mobile device")
        XCTAssertEqual(DeviceInfo(platform: "web").label, "Web browser")
        XCTAssertEqual(DeviceInfo(platform: "macos", model: "Mac15,3", systemVersion: "15.0").label, "Mac15,3 · macOS 15.0")
        XCTAssertEqual(DeviceInfo(modelName: String(repeating: "x", count: 200), systemName: "iOS", systemVersion: "19").label.utf16.count, 120)
    }

    func testNodeNamePattern() {
        for good in ["a", "alice", "a-1", "ab", String(repeating: "a", count: 63)] {
            XCTAssertTrue(AuthService.isValidNodeName(good), good)
        }
        for bad in ["", "1a", "a-", "Alice", "a_b", "alice@example.com", String(repeating: "a", count: 64)] {
            XCTAssertFalse(AuthService.isValidNodeName(bad), bad)
        }
    }

    // MARK: Login

    @MainActor
    func testNodeNameIdentifierBindsInOneCallWithAValidProof() async throws {
        let h = Harness()
        h.settings.setServerUrl("ws://test.local/ws")
        let identity = try h.identity()
        let http = FakeHTTPClient { _ in jsonResponse(boundPayload()) }
        let auth = h.auth(identity, http: http, deviceLabel: "iPhone 15 Pro · iOS 19.0", timestamp: "1760000000000")

        let session = try await auth.login(identifier: " Alice ", password: "password123", totp: " 123456 ")

        XCTAssertEqual(http.requests.map(\.url.path), ["/api/accounts/login"])
        XCTAssertEqual(http.requests.first?.url.absoluteString, "http://test.local/api/accounts/login")
        let body = bodyObject(try XCTUnwrap(http.lastRequest))
        XCTAssertEqual(body["identifier"], "alice")
        XCTAssertEqual(body["email"], "alice")
        XCTAssertEqual(body["password"], "password123")
        XCTAssertEqual(body["totp"], "123456")
        XCTAssertEqual(body["pubkey"], .string(identity.publicKeyB64url))
        XCTAssertEqual(body["deviceTs"], "1760000000000")
        XCTAssertEqual(body["deviceKind"], "mobile_node")
        XCTAssertEqual(body["deviceLabel"], "iPhone 15 Pro · iOS 19.0")
        XCTAssertFalse(body.str("deviceSig").isEmpty)
        XCTAssertFalse(body.str("deviceSig").contains("="))
        XCTAssertTrue(verifyBind(body, account: "alice"))
        XCTAssertEqual(body.keys, [
            "identifier", "email", "password", "totp", "pubkey", "deviceTs", "deviceSig", "deviceKind", "deviceLabel",
        ])
        XCTAssertEqual(http.lastRequest?.headers["content-type"], "application/json")

        XCTAssertTrue(session.isMobileNode)
        XCTAssertEqual(session.deviceId, "dev-mobile-1")
        XCTAssertEqual(auth.session?.sessionToken, "mobile-session")
        XCTAssertEqual(h.settings.displayName, "alice", "a sign-in names the profile")
    }

    @MainActor
    func testAPictureUploadedAfterLoginReachesTheSession() async throws {
        let h = Harness()
        h.settings.setServerUrl("ws://test.local/ws")
        let identity = try h.identity()
        final class Profile: @unchecked Sendable { var json: JSONObject = ["exists": true, "name": "alice"]; var reads = 0 }
        let profile = Profile()
        let http = FakeHTTPClient { request in
            if request.method == "GET" {
                XCTAssertEqual(request.url.path, "/api/accounts/alice")
                profile.reads += 1
                return jsonResponse(profile.json)
            }
            return jsonResponse(boundPayload())
        }
        let auth = h.auth(identity, http: http, deviceLabel: "iPhone 15 Pro · iOS 19.0", timestamp: "1760000000000")
        try await auth.login(identifier: "alice", password: "password123")
        XCTAssertEqual(auth.session?.avatarPng, "")

        // A profile without the field changes nothing.
        let notified = Counter()
        auth.addListener { notified.increment() }
        await auth.refreshAvatar()
        XCTAssertEqual(notified.value, 0)

        profile.json = ["exists": true, "name": "alice", "avatarPng": "cGljdHVyZQ==", "avatarUpdatedAt": 42]
        await auth.refreshAvatar()
        XCTAssertEqual(profile.reads, 2)
        XCTAssertEqual(notified.value, 1)
        XCTAssertEqual(auth.session?.avatarPng, "cGljdHVyZQ==")
        XCTAssertEqual(auth.session?.avatarUpdatedAt, 42)
        XCTAssertEqual(auth.session?.sessionToken, "mobile-session")
        XCTAssertTrue(auth.session?.isMobileNode ?? false)
        let restored = h.auth(identity, http: http)
        XCTAssertEqual(restored.session?.avatarPng, "cGljdHVyZQ==")

        // Unchanged picture: no write, no rebuild.
        await auth.refreshAvatar()
        XCTAssertEqual(notified.value, 1)
    }

    @MainActor
    func testEmailIdentifierLogsInFirstThenBindsWithTheAccountName() async throws {
        let h = Harness()
        h.settings.setServerUrl("ws://test.local/ws")
        let identity = try h.identity()
        let http = FakeHTTPClient { request in
            let body = bodyObject(request)
            if request.url.path == "/api/accounts/logout" { return jsonResponse(["ok": true]) }
            if body["pubkey"] == nil { return jsonResponse(plainPayload(name: "alice")) }
            return jsonResponse(boundPayload(name: "alice", token: "bound-session"))
        }
        let auth = h.auth(identity, http: http, deviceLabel: "Pixel 9 · Android 16", timestamp: "1760000000001")

        let session = try await auth.login(identifier: "Alice@Example.com", password: "password123")
        // Let the best-effort intermediate-session revocation run.
        for _ in 0..<50 where !http.requests.contains(where: { $0.url.path == "/api/accounts/logout" }) {
            try await Task.sleep(nanoseconds: 10_000_000)
        }

        let logins = http.requests.filter { $0.url.path == "/api/accounts/login" }.map(bodyObject)
        XCTAssertEqual(logins.count, 2)
        XCTAssertEqual(logins[0]["identifier"], "alice@example.com")
        XCTAssertNil(logins[0]["pubkey"])
        XCTAssertNil(logins[0]["deviceKind"])
        XCTAssertEqual(logins[1]["identifier"], "alice")
        XCTAssertEqual(logins[1]["pubkey"], .string(identity.publicKeyB64url))
        XCTAssertEqual(logins[1]["deviceKind"], "mobile_node")
        XCTAssertEqual(logins[1]["deviceLabel"], "Pixel 9 · Android 16")
        XCTAssertTrue(verifyBind(logins[1], account: "alice"))
        let logouts = http.requests.filter { $0.url.path == "/api/accounts/logout" }.map(bodyObject)
        XCTAssertEqual(logouts.map { $0.str("sessionToken") }, ["plain-session"])

        XCTAssertEqual(session.sessionToken, "bound-session")
        XCTAssertTrue(session.isMobileNode)
    }

    @MainActor
    func testAnEmailLoginWithoutANodeNameKeepsThePlainSession() async throws {
        let h = Harness()
        let http = FakeHTTPClient { _ in jsonResponse(["email": "x@example.com", "sessionToken": "t"]) }
        let auth = h.auth(try h.identity(), http: http)
        let session = try await auth.login(identifier: "x@example.com", password: "pw")
        XCTAssertEqual(http.requests.count, 1)
        XCTAssertEqual(session.email, "x@example.com")
        XCTAssertTrue(auth.isAuthenticated)
    }

    @MainActor
    func testDeviceKeyConflictRotatesTheSeedAndRetriesTheBindOnce() async throws {
        let h = Harness()
        h.settings.setServerUrl("ws://test.local/ws")
        let identity = try h.identity()
        let originalKey = identity.publicKeyB64url
        let pubkeys = Lines()
        let http = FakeHTTPClient { request in
            pubkeys.add(bodyObject(request).str("pubkey"))
            if pubkeys.all.count == 1 { return jsonResponse(["error": "device_key_conflict"], 409) }
            return jsonResponse(boundPayload())
        }
        let auth = h.auth(identity, http: http)

        let session = try await auth.login(identifier: "alice", password: "pw")

        XCTAssertEqual(pubkeys.all.count, 2)
        XCTAssertEqual(pubkeys.all.first, originalKey)
        XCTAssertNotEqual(pubkeys.all.last, originalKey)
        XCTAssertEqual(pubkeys.all.last, identity.publicKeyB64url)
        XCTAssertTrue(session.isMobileNode)
        // The new seed is persisted for the next launch.
        let stored = try XCTUnwrap(h.secure.read(Identity.seedKey))
        let reloaded = try Identity.fromSeed(Array(try B64.decode(stored)))
        XCTAssertEqual(reloaded.publicKeyB64url, identity.publicKeyB64url)
    }

    @MainActor
    func testASecondDeviceKeyConflictSurfacesAReadableError() async throws {
        let h = Harness()
        let counter = Counter()
        let http = FakeHTTPClient { _ in
            counter.increment()
            return jsonResponse(["error": "device_key_conflict"], 409)
        }
        let auth = h.auth(try h.identity(), http: http)
        do {
            try await auth.login(identifier: "alice", password: "pw")
            XCTFail("expected an AuthException")
        } catch let error as AuthException {
            XCTAssertEqual(error.code, "device_key_conflict")
            XCTAssertTrue(error.message.contains("another account"))
        }
        XCTAssertEqual(counter.value, 2)
        XCTAssertFalse(auth.isAuthenticated)
    }

    @MainActor
    func testDeviceProofRequiredAndInvalidDeviceKindAreReadable() async throws {
        let h = Harness()
        final class Code: @unchecked Sendable { var value = "device_proof_required" }
        let code = Code()
        let http = FakeHTTPClient { _ in jsonResponse(["error": .string(code.value)], 401) }
        let auth = h.auth(try h.identity(), http: http)
        await assertAuthError(auth, contains: "device clock")
        code.value = "invalid_device_kind"
        await assertAuthError(auth, contains: "mobile node")
    }

    @MainActor
    func testErrorsWithoutACodeUseTheStatus() async throws {
        let h = Harness()
        let auth = h.auth(try h.identity(), http: FakeHTTPClient { _ in HTTPResponse(status: 502, text: "<html>") })
        do {
            try await auth.login(identifier: "alice", password: "pw")
            XCTFail("expected an AuthException")
        } catch let error as AuthException {
            XCTAssertEqual(error.code, "http_502")
            XCTAssertEqual(error.message, "Could not complete the request. Please try again.")
        }
    }

    @MainActor
    func testWithoutADeviceKeyLoginFailsReadably() async {
        let h = Harness()
        let auth = h.auth(nil)
        do {
            try await auth.login(identifier: "alice", password: "pw")
            XCTFail("expected an AuthException")
        } catch let error as AuthException {
            XCTAssertEqual(error.code, "identity_unavailable")
        } catch {
            XCTFail("unexpected \(error)")
        }
    }

    @MainActor
    private func assertAuthError(_ auth: AuthService, contains text: String, file: StaticString = #filePath, line: UInt = #line) async {
        do {
            try await auth.login(identifier: "alice", password: "pw")
            XCTFail("expected an AuthException", file: file, line: line)
        } catch let error as AuthException {
            XCTAssertTrue(error.message.contains(text), error.message, file: file, line: line)
        } catch {
            XCTFail("unexpected \(error)", file: file, line: line)
        }
    }

    // MARK: Revocation

    @MainActor
    func testDeviceRevokedClearsTheSessionRotatesTheKeyAndArmsTheBanner() async throws {
        let h = Harness()
        h.settings.setServerUrl("ws://test.local/ws")
        let identity = try h.identity()
        let originalKey = identity.publicKeyB64url
        let originalSeed = h.secure.read(Identity.seedKey)
        let auth = h.plainAuth(identity, session: AuthSession.fromJSON(boundPayload()))

        auth.handleUnauthorized(code: "device_revoked")

        XCTAssertFalse(auth.isAuthenticated)
        XCTAssertTrue(auth.revokedBannerPending)
        // The banner is armed under the relay that revoked us, not globally.
        XCTAssertEqual(h.prefs.bool(forKey: AuthService.revokedBannerKeyFor(h.settings.relays.activeId)), true)
        XCTAssertNotEqual(identity.publicKeyB64url, originalKey)
        let newSeed = h.secure.read(Identity.seedKey)
        XCTAssertNotNil(newSeed)
        XCTAssertNotEqual(newSeed, originalSeed)
        XCTAssertNil(h.secure.read(AuthService.sessionKey))

        auth.clearRevokedBanner()
        XCTAssertFalse(auth.revokedBannerPending)
    }

    @MainActor
    func testAnInvalidSessionOnADeviceBoundSessionIsTreatedAsRevocation() async throws {
        let h = Harness()
        let identity = try h.identity()
        let originalKey = identity.publicKeyB64url
        let auth = h.plainAuth(identity, session: AuthSession.fromJSON(boundPayload()))
        auth.handleUnauthorized(code: "invalid_session")
        XCTAssertFalse(auth.isAuthenticated)
        XCTAssertTrue(auth.revokedBannerPending)
        XCTAssertNotEqual(identity.publicKeyB64url, originalKey)
    }

    @MainActor
    func testAPlainExpiredSessionOnlyClearsLocalAuth() async throws {
        let h = Harness()
        let identity = try h.identity()
        let originalKey = identity.publicKeyB64url
        let auth = h.plainAuth(identity, session: AuthSession.fromJSON(plainPayload()))
        auth.handleUnauthorized(code: "invalid_session")
        XCTAssertFalse(auth.isAuthenticated)
        XCTAssertFalse(auth.revokedBannerPending)
        XCTAssertEqual(identity.publicKeyB64url, originalKey)
    }

    @MainActor
    func testDeviceRevokedCodeRotatesEvenWithoutADeviceId() async throws {
        let h = Harness()
        let identity = try h.identity()
        let key = identity.publicKeyB64url
        let auth = h.plainAuth(identity, session: AuthSession.fromJSON(plainPayload()))
        auth.handleUnauthorized(code: "device_revoked")
        XCTAssertTrue(auth.revokedBannerPending)
        XCTAssertNotEqual(identity.publicKeyB64url, key)
    }

    @MainActor
    func testPreviewNeverReplacesARealSession() async throws {
        let h = Harness()
        let auth = h.plainAuth(try h.identity(), session: AuthSession.fromJSON(boundPayload()))
        auth.authenticatePreview()
        XCTAssertEqual(auth.session?.sessionKind, "mobile_node")
    }

    @MainActor
    func testGuestModeChangesNotifyListeners() async throws {
        let h = Harness()
        let auth = h.plainAuth(try h.identity())
        let counter = Counter()
        auth.addListener { counter.increment() }
        auth.enterGuestMode()
        auth.exitGuestMode()
        XCTAssertEqual(counter.value, 2)
    }

    @MainActor
    func testASuccessfulLoginConsumesAPendingBanner() async throws {
        let h = Harness(prefs: [AuthService.revokedBannerKey: true])
        h.settings.setServerUrl("ws://test.local/ws")
        let auth = h.auth(try h.identity(), http: FakeHTTPClient { _ in jsonResponse(boundPayload()) })
        // The unscoped banner migrated to the relay active at startup.
        XCTAssertEqual(h.prefs.bool(forKey: AuthService.revokedBannerKeyFor("test.local")), true)
        XCTAssertNil(h.prefs.bool(forKey: AuthService.revokedBannerKey))
        XCTAssertTrue(auth.revokedBannerPending)
        try await auth.login(identifier: "alice", password: "pw")
        XCTAssertFalse(auth.revokedBannerPending)
    }

    // MARK: Guest mode, preview, logout, deletion

    @MainActor
    func testGuestModeIsPersistedUntilTheUserLeavesIt() async throws {
        let h = Harness()
        let identity = try h.identity()
        let auth = h.plainAuth(identity)
        XCTAssertFalse(auth.isGuest)
        XCTAssertFalse(auth.hasAppAccess)

        auth.enterGuestMode()
        XCTAssertTrue(auth.isGuest)
        XCTAssertTrue(auth.hasAppAccess)
        XCTAssertTrue(h.plainAuth(identity).isGuest)

        auth.exitGuestMode()
        XCTAssertFalse(auth.isGuest)
        XCTAssertFalse(auth.hasAppAccess)
    }

    @MainActor
    func testASignedInUserIsNeverAGuestAndSigningInClearsIt() async throws {
        let h = Harness()
        let identity = try h.identity()
        let signedIn = h.plainAuth(identity, session: AuthSession.fromJSON(plainPayload()))
        signedIn.enterGuestMode()
        XCTAssertFalse(signedIn.isGuest)

        let auth = h.plainAuth(identity)
        auth.enterGuestMode()
        auth.authenticatePreview()
        XCTAssertFalse(auth.isGuest)
        XCTAssertFalse(h.prefs.contains(AuthService.guestModeKeyFor(AppEnv.prodHost)))
        XCTAssertFalse(h.prefs.contains(AuthService.guestModeKey))
        XCTAssertEqual(auth.session?.sessionKind, "preview")
        XCTAssertEqual(auth.session?.pubkey, identity.publicKeyB64url)
        // A stored preview session is never restored.
        XCTAssertNil(h.auth(identity).session)
    }

    @MainActor
    func testLogoutClearsLocallyFirstAndRevokesBestEffort() async throws {
        let h = Harness()
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: .object(boundPayload()))
        http.enqueue(error: HTTPError.timedOut)
        let auth = h.auth(try h.identity(), http: http)
        try await auth.login(identifier: "alice", password: "pw")
        XCTAssertNotNil(h.secure.read(AuthService.sessionKeyFor(AppEnv.prodHost)))

        await auth.logout()

        XCTAssertFalse(auth.isAuthenticated)
        XCTAssertNil(h.secure.read(AuthService.sessionKeyFor(AppEnv.prodHost)))
        let logout = try XCTUnwrap(http.lastRequest)
        XCTAssertEqual(logout.url.path, "/api/accounts/logout")
        XCTAssertEqual(logout.timeout, 5)
        XCTAssertEqual(bodyObject(logout)["sessionToken"], "mobile-session")
    }

    @MainActor
    func testDeleteAccountNeedsARealSessionAndAPassword() async throws {
        let h = Harness()
        let http = FakeHTTPClient { _ in jsonResponse(["ok": true]) }
        let preview = h.plainAuth(try h.identity(), http: http)
        preview.authenticatePreview()
        do {
            try await preview.deleteAccount(password: "pw")
            XCTFail("expected an AuthException")
        } catch let error as AuthException {
            XCTAssertEqual(error.code, "not_authenticated")
        }

        let auth = h.plainAuth(try h.identity(), session: AuthSession.fromJSON(boundPayload()), http: http)
        do {
            try await auth.deleteAccount(password: "  ")
            XCTFail("expected an AuthException")
        } catch let error as AuthException {
            XCTAssertEqual(error.code, "password_required")
        }
        try await auth.deleteAccount(password: " secret ")
        let body = bodyObject(try XCTUnwrap(http.lastRequest))
        XCTAssertEqual(http.lastRequest?.url.path, "/api/accounts/profile")
        XCTAssertEqual(body["password"], "secret")
        XCTAssertEqual(body["deleteAccount"], true)
        XCTAssertEqual(body["sessionToken"], "mobile-session")
        XCTAssertFalse(auth.isAuthenticated)
    }

    @MainActor
    func testCheckNodeName() async throws {
        let h = Harness()
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/accounts/taken", json: ["exists": true, "available": false, "name": "taken"])
        http.on("GET", path: "/api/accounts/free", json: ["exists": false, "available": true])
        let auth = h.auth(try h.identity(), http: http)

        let invalid = await auth.checkNodeName("1bad")
        XCTAssertFalse(invalid.ok)
        XCTAssertTrue(invalid.message.contains("lowercase letters"))
        let taken = await auth.checkNodeName(" Taken ")
        XCTAssertTrue(taken.ok)
        XCTAssertFalse(taken.available)
        XCTAssertEqual(taken.message, "That name is already taken.")
        let free = await auth.checkNodeName("free")
        XCTAssertTrue(free.available)
        XCTAssertEqual(free.name, "free")
        XCTAssertEqual(free.message, "“free” is available.")
        let offline = await auth.checkNodeName("offline")
        XCTAssertFalse(offline.ok)
        XCTAssertTrue(offline.available)
    }

    @MainActor
    func testSignupFollowsTheProductionRelayHost() async throws {
        let h = Harness()
        XCTAssertEqual(h.auth(try h.identity()).signupUrl?.absoluteString, "https://app.forkmesh.com/signup")
        XCTAssertEqual(
            h.auth(try h.identity()).forgotPasswordUrl?.absoluteString, "https://app.forkmesh.com/forgot-password")
    }

    @MainActor
    func testRequestsAreTrackedByThePerformanceMonitor() async throws {
        let h = Harness()
        let monitor = PerformanceMonitorService(logSink: { _ in }, elapsedForTest: { .milliseconds(700) })
        let auth = h.auth(try h.identity(), http: FakeHTTPClient { _ in jsonResponse(boundPayload()) }, monitor: monitor)
        try await auth.login(identifier: "alice", password: "pw")
        XCTAssertEqual(monitor.events.map(\.name), ["auth.POST /api/accounts/login"])
        XCTAssertEqual(monitor.events.first?.details["path"], "/api/accounts/login")
    }
}
