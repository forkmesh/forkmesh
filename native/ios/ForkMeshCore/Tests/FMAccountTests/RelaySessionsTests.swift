// An account is a fact about a relay, not about the phone.

import FMBase
import FMCrypto
import FMNet
import XCTest
@testable import FMAccount

final class RelaySessionsTests: XCTestCase {
    func payload(_ name: String, _ token: String) -> JSONObject {
        [
            "nodeName": .string(name),
            "name": .string(name),
            "email": .string("\(name)@example.com"),
            "status": "active",
            "sessionToken": .string(token),
            "sessionKind": "mobile_node",
            "deviceKind": "mobile_node",
            "deviceId": .string("dev-\(name)"),
            "capabilities": ["browse", "publish_repo", "owner_sign"],
        ]
    }

    /// A mutable answer for the fake relay.
    final class Next: @unchecked Sendable {
        var body: JSONObject
        var status = 200
        init(_ body: JSONObject) { self.body = body }
    }

    @MainActor
    func harness(_ extra: [String]) throws -> Harness {
        let h = Harness()
        for host in extra { h.settings.relays.add(try XCTUnwrap(RelayProfile.tryParse(host))) }
        return h
    }

    /// The switch the way the relay switch controller runs it: record it, then adopt.
    @MainActor
    func switchTo(_ h: Harness, _ auth: AuthService, _ relayId: String) {
        h.settings.relays.setActive(relayId)
        auth.adoptActiveRelay()
    }

    func client(_ next: Next) -> FakeHTTPClient {
        FakeHTTPClient { _ in jsonResponse(next.body, next.status) }
    }

    @MainActor
    func testTwoRelaysHoldTwoSessionsAndSwitchingRestoresEach() async throws {
        let h = try harness(["staging.forkmesh.com"])
        let next = Next(payload("alice", "token-prod"))
        let auth = h.auth(try h.identity(), http: client(next))

        try await auth.login(identifier: "alice", password: "pw")
        XCTAssertEqual(auth.session?.nodeName, "alice")
        XCTAssertEqual(auth.relayScope, AppEnv.prodHost)

        next.body = payload("bob", "token-staging")
        switchTo(h, auth, "staging.forkmesh.com")
        XCTAssertNil(auth.session, "never signed in on staging yet")

        try await auth.login(identifier: "bob", password: "pw")
        XCTAssertEqual(auth.session?.nodeName, "bob")
        XCTAssertEqual(auth.session?.sessionToken, "token-staging")

        switchTo(h, auth, AppEnv.prodHost)
        XCTAssertEqual(auth.session?.nodeName, "alice")
        XCTAssertEqual(auth.session?.sessionToken, "token-prod")

        switchTo(h, auth, "staging.forkmesh.com")
        XCTAssertEqual(auth.session?.nodeName, "bob")
    }

    @MainActor
    func testBothSessionsSurviveARelaunchEachUnderItsOwnRelay() async throws {
        let h = try harness(["staging.forkmesh.com"])
        let identity = try h.identity()
        let next = Next(payload("alice", "token-prod"))
        let auth = h.auth(identity, http: client(next))
        try await auth.login(identifier: "alice", password: "pw")
        next.body = payload("bob", "token-staging")
        switchTo(h, auth, "staging.forkmesh.com")
        try await auth.login(identifier: "bob", password: "pw")

        let restored = h.auth(identity)
        XCTAssertEqual(restored.relayScope, "staging.forkmesh.com")
        XCTAssertEqual(restored.session?.nodeName, "bob")
        XCTAssertEqual(restored.sessionForRelay(AppEnv.prodHost)?.nodeName, "alice")
        XCTAssertEqual(Set(restored.signedInRelayIds), ["staging.forkmesh.com", AppEnv.prodHost])
    }

    @MainActor
    func testSigningOutOfOneRelayLeavesTheOtherSignedIn() async throws {
        let h = try harness(["staging.forkmesh.com"])
        let next = Next(payload("alice", "token-prod"))
        let auth = h.auth(try h.identity(), http: client(next))
        try await auth.login(identifier: "alice", password: "pw")
        next.body = payload("bob", "token-staging")
        switchTo(h, auth, "staging.forkmesh.com")
        try await auth.login(identifier: "bob", password: "pw")

        await auth.logout()

        XCTAssertFalse(auth.isAuthenticated)
        XCTAssertEqual(auth.sessionForRelay(AppEnv.prodHost)?.nodeName, "alice")
        switchTo(h, auth, AppEnv.prodHost)
        XCTAssertEqual(auth.session?.nodeName, "alice")
    }

    @MainActor
    func testRemovingARelayForgetsItsSessionRatherThanOrphaningIt() async throws {
        let h = try harness(["staging.forkmesh.com"])
        let auth = h.auth(try h.identity(), http: client(Next(payload("bob", "token-staging"))))
        switchTo(h, auth, "staging.forkmesh.com")
        try await auth.login(identifier: "bob", password: "pw")
        switchTo(h, auth, AppEnv.prodHost)

        auth.forgetRelay("staging.forkmesh.com")
        h.settings.relays.remove("staging.forkmesh.com")

        XCTAssertNil(auth.sessionForRelay("staging.forkmesh.com"))
        XCTAssertNil(
            h.secure.read(AuthService.sessionKeyFor("staging.forkmesh.com")),
            "a removed relay must not leave a live token in the Keychain")
    }

    @MainActor
    func testGuestModeIsPerRelay() async throws {
        let h = try harness(["staging.forkmesh.com"])
        let auth = h.auth(try h.identity())

        auth.enterGuestMode()
        XCTAssertTrue(auth.isGuest)
        XCTAssertTrue(auth.hasAppAccess)

        switchTo(h, auth, "staging.forkmesh.com")
        XCTAssertFalse(auth.isGuest, "staging was never browsed as guest")
        XCTAssertFalse(auth.hasAppAccess)

        switchTo(h, auth, AppEnv.prodHost)
        XCTAssertTrue(auth.isGuest)
    }

    @MainActor
    func testIsSignedInOnRelayAnswersBeforeASwitchIsMade() async throws {
        let h = try harness(["staging.forkmesh.com"])
        let auth = h.auth(try h.identity(), http: client(Next(payload("alice", "tok"))))
        try await auth.login(identifier: "alice", password: "pw")
        XCTAssertTrue(auth.isSignedInOnRelay(AppEnv.prodHost))
        XCTAssertFalse(auth.isSignedInOnRelay("staging.forkmesh.com"))
    }

    @MainActor
    func testASessionWrittenBeforeRelaysExistedLandsOnTheActiveRelay() async throws {
        let h = Harness()
        let identity = try h.identity()
        h.secure.write(AuthService.sessionKey, JSON.encode(.object(payload("alice", "legacy-token"))))

        let auth = h.auth(identity)

        XCTAssertEqual(auth.session?.nodeName, "alice")
        XCTAssertEqual(auth.relayScope, AppEnv.prodHost)
        XCTAssertNil(h.secure.read(AuthService.sessionKey), "the unscoped slot is retired once migrated")
        XCTAssertNotNil(h.secure.read(AuthService.sessionKeyFor(AppEnv.prodHost)))
    }

    @MainActor
    func testAPlaintextSessionAnOlderBuildLeftInPreferencesIsMigrated() async throws {
        let h = Harness(prefs: [AuthService.sessionKey: .string(JSON.encode(.object(payload("alice", "old"))))])
        let auth = h.auth(try h.identity())
        XCTAssertEqual(auth.session?.sessionToken, "old")
        XCTAssertNil(h.prefs.string(forKey: AuthService.sessionKey))
    }

    // MARK: The phone has one device key across every relay

    @MainActor
    func testARevocationDoesNotRotateItWhileAnotherRelayIsSignedIn() async throws {
        let h = try harness(["staging.forkmesh.com"])
        let identity = try h.identity()
        let auth = h.auth(identity, http: client(Next(payload("alice", "tok"))))
        try await auth.login(identifier: "alice", password: "pw")
        switchTo(h, auth, "staging.forkmesh.com")
        try await auth.login(identifier: "bob", password: "pw")
        let key = identity.publicKeyB64url

        auth.handleUnauthorized(code: "device_revoked")

        XCTAssertFalse(auth.isAuthenticated)
        XCTAssertTrue(auth.revokedBannerPending)
        XCTAssertEqual(identity.publicKeyB64url, key, "rotating would break the device binding on production too")
        XCTAssertEqual(auth.sessionForRelay(AppEnv.prodHost)?.nodeName, "alice")
    }

    @MainActor
    func testItStillRotatesWhenThisIsTheOnlyRelaySignedIn() async throws {
        let h = Harness()
        let identity = try h.identity()
        let auth = h.auth(identity, http: client(Next(payload("alice", "tok"))))
        try await auth.login(identifier: "alice", password: "pw")
        let key = identity.publicKeyB64url

        auth.handleUnauthorized(code: "device_revoked")

        XCTAssertNotEqual(identity.publicKeyB64url, key)
    }

    @MainActor
    func testADeviceKeyConflictIsSurfacedRatherThanSilentlyRotating() async throws {
        let h = try harness(["staging.forkmesh.com"])
        let identity = try h.identity()
        let next = Next(payload("alice", "tok"))
        let auth = h.auth(identity, http: client(next))
        try await auth.login(identifier: "alice", password: "pw")
        let key = identity.publicKeyB64url

        next.body = ["error": "device_key_conflict"]
        next.status = 409
        switchTo(h, auth, "staging.forkmesh.com")

        do {
            try await auth.login(identifier: "bob", password: "pw")
            XCTFail("expected an AuthException")
        } catch let error as AuthException {
            XCTAssertTrue(error.message.contains("other relays"))
        }
        XCTAssertEqual(identity.publicKeyB64url, key)
        XCTAssertEqual(auth.sessionForRelay(AppEnv.prodHost)?.nodeName, "alice")
    }

    @MainActor
    func testTheRevokedBannerIsArmedPerRelay() async throws {
        let h = try harness(["staging.forkmesh.com"])
        let auth = h.auth(try h.identity(), http: client(Next(payload("bob", "tok"))))
        switchTo(h, auth, "staging.forkmesh.com")
        try await auth.login(identifier: "bob", password: "pw")

        auth.handleUnauthorized(code: "device_revoked")
        XCTAssertTrue(auth.revokedBannerPending)

        switchTo(h, auth, AppEnv.prodHost)
        XCTAssertFalse(auth.revokedBannerPending)
    }
}
