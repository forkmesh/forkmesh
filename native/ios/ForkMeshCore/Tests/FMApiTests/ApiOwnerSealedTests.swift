// Owner-sealed agent sessions, as in native/android OwnerSealedAgentsTest.kt: the relay stores
// agent sessions as opaque owner-sealed envelopes, so the app's list has to open what belongs to
// this device and stay honest about what does not. Uses the real OwnerCrypto (the Kotlin port used
// a stand-in); its legacy-row and enrollment-follows-the-session cases are included.

import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest
@testable import FMApi

private func sessionPayload(_ id: Int, _ status: String) -> JSONObject {
    [
        "kind": "forkmesh.agent-session",
        "id": .int(Int64(id)),
        "issueNumber": 7,
        "issueTitle": "Stabilize signed inbox",
        "status": .string(status),
        "provider": "claude-code",
        "model": "claude-sonnet-5",
        "branchName": "agent/issue-7",
        "numTurns": 4,
        "costUsd": 0.42,
        "transcript": "private transcript tail",
    ]
}

/// `hasNoPrivateMaterial`.
private func hasNoPrivateMaterial(_ body: JSONObject) -> Bool {
    let encoded = JSON.encode(.object(body)).lowercased()
    return !["priv", "secret", "seed"].contains { encoded.contains($0) }
}

@MainActor
final class ApiOwnerSealedTests: XCTestCase {
    private func seal(_ owner: OwnerCrypto, _ payload: JSONObject) throws -> JSONObject {
        try XCTUnwrap(try owner.sealJson(payload, to: [owner.publicBundle]))
    }

    private func listing(_ envelopes: [(Int, JSONObject)], extra: JSONObject = [:]) -> FakeHTTPClient {
        var body: JSONObject = [
            "ok": true,
            "agents": [],
            "encryptedAgents": .array(envelopes.map { .object(["id": .int(Int64($0.0)), "envelope": .object($0.1)]) }),
            "privacyBoundary": "owner-only-e2ee",
        ]
        body.merge(extra)
        let reply = body
        return FakeHTTPClient { request in
            XCTAssertEqual(portRawPath(request), "/api/repo/alice/proj/agents/list")
            return portJSON(.object(reply))
        }
    }

    func testSessionsSealedToThisDeviceAreOpenedAndListed() async throws {
        let owner = OwnerCrypto.generate()
        let envelope = try seal(owner, sessionPayload(42, "running"))
        let api = ApiPortFixture.api(listing([(42, envelope)]))
        api.ownerCrypto = owner

        let page = try await api.agentSessionPage("alice", "proj")
        XCTAssertEqual(page.sealedForOthers, 0)
        XCTAssertEqual(page.sessions.count, 1)
        XCTAssertEqual(page.sessions.first?.id, 42)
        XCTAssertEqual(page.sessions.first?.issueTitle, "Stabilize signed inbox")
        XCTAssertEqual(page.sessions.first?.statusLabel, "Running")
    }

    func testSessionsSealedToAnotherDeviceAreCountedNeverDroppedSilently() async throws {
        let owner = OwnerCrypto.generate()
        let otherDevice = OwnerCrypto.generate()
        let foreign = try seal(otherDevice, sessionPayload(7, "running"))
        let api = ApiPortFixture.api(listing([(7, foreign)]))
        api.ownerCrypto = owner

        let page = try await api.agentSessionPage("alice", "proj")
        XCTAssertTrue(page.sessions.isEmpty)
        XCTAssertEqual(page.sealedForOthers, 1)
    }

    func testADeviceWithNoOwnerIdentityReportsSealedRowsRatherThanNone() async throws {
        let owner = OwnerCrypto.generate()
        let envelope = try seal(owner, sessionPayload(42, "running"))
        // ownerCrypto deliberately unset: an un-enrolled install must not present an empty list
        // as though the repository had no agent work.
        let api = ApiPortFixture.api(listing([(42, envelope)]))

        let page = try await api.agentSessionPage("alice", "proj")
        XCTAssertTrue(page.sessions.isEmpty)
        XCTAssertEqual(page.sealedForOthers, 1)
    }

    /// Kotlin `testLegacyRowsHeldForOwnerMigrationCountAsSealed`.
    func testLegacyRowsHeldForOwnerMigrationCountAsSealed() async throws {
        let owner = OwnerCrypto.generate()
        let envelope = try seal(owner, sessionPayload(1, "done"))
        let http = listing([(0, envelope)], extra: [
            "legacyRowsHeldForOwnerMigration": 2,
            "agents": [.object(sessionPayload(9, "queued"))],
        ])
        let api = ApiPortFixture.api(http)
        api.ownerCrypto = owner

        let page = try await api.agentSessionPage("alice", "proj")
        XCTAssertEqual(page.sessions.map(\.id), [9, 1])
        XCTAssertEqual(page.sealedForOthers, 2)
    }

    func testAnEncryptedTranscriptIsOpenedOrFlaggedWhenItIsNotOurs() async throws {
        let owner = OwnerCrypto.generate()
        let mine = try seal(owner, ["status": "success", "transcript": "the agent edited three files"])
        let http = FakeHTTPClient { _ in
            portJSON(["ok": true, "encrypted": true, "envelope": .object(mine), "serverCanDecrypt": false])
        }

        var api = ApiPortFixture.api(http)
        api.ownerCrypto = owner
        let opened = try await api.agentTranscript("alice", "proj", 42)
        XCTAssertEqual(opened.transcript, "the agent edited three files")
        XCTAssertEqual(opened.status, "success")
        XCTAssertFalse(opened.sealedForOtherDevice)

        api = ApiPortFixture.api(http)
        api.ownerCrypto = OwnerCrypto.generate()
        let sealed = try await api.agentTranscript("alice", "proj", 42)
        XCTAssertTrue(sealed.sealedForOtherDevice)
        XCTAssertTrue(sealed.transcript.isEmpty)

        // Kotlin: no owner identity at all reads the same way.
        api.ownerCrypto = nil
        let unenrolled = try await api.agentTranscript("alice", "proj", 42)
        XCTAssertTrue(unenrolled.sealedForOtherDevice)
    }

    func testEnrollmentPostsThePublicBundleOnlyAndVerifiesTheReply() async throws {
        let owner = OwnerCrypto.generate()
        let keyId = owner.keyId
        let http = FakeHTTPClient { request in
            XCTAssertEqual(portRawPath(request), "/api/security/owner-keys")
            return portJSON([
                "ok": true, "keyId": .string(keyId), "custody": "owner-device-only", "privateKeysStored": false,
            ], 201)
        }
        let api = ApiPortFixture.api(http)
        api.ownerCrypto = owner

        let accepted = await api.registerOwnerKey(owner)
        XCTAssertEqual(accepted, owner.keyId)
        let sentBody = portBody(http.lastRequest)
        let bundle = try XCTUnwrap(sentBody["publicBundle"]?.objectValue)
        XCTAssertEqual(Set(bundle.keys), ["v", "x25519", "mlkem768"])
        // Nothing derived from a private half may appear in the request.
        XCTAssertTrue(hasNoPrivateMaterial(sentBody))
    }

    func testAnEnrollmentTheRelayDescribesDifferentlyIsRefused() async {
        let owner = OwnerCrypto.generate()
        let keyId = owner.keyId

        // Wrong key id: the relay is talking about a different device.
        var api = ApiPortFixture.api(FakeHTTPClient { _ in
            portJSON(["ok": true, "keyId": "some-other-device-key-id-value", "privateKeysStored": false], 201)
        })
        api.ownerCrypto = owner
        var accepted = await api.registerOwnerKey(owner)
        XCTAssertEqual(accepted, "")

        // A relay claiming to store private key material is never trusted.
        api = ApiPortFixture.api(FakeHTTPClient { _ in
            portJSON(["ok": true, "keyId": .string(keyId), "privateKeysStored": true], 201)
        })
        api.ownerCrypto = owner
        accepted = await api.registerOwnerKey(owner)
        XCTAssertEqual(accepted, "")

        // Kotlin: a failed request is no enrollment either.
        api = ApiPortFixture.api(FakeHTTPClient { _ in HTTPResponse(status: 500, text: "") })
        accepted = await api.registerOwnerKey(owner)
        XCTAssertEqual(accepted, "")
    }

    /// Kotlin `testOwnerEnrollmentFollowsTheSession`: one enrollment per session token, a failure
    /// retried, sign-out forgets.
    func testOwnerEnrollmentFollowsTheSession() async {
        let owner = OwnerCrypto.generate()
        let keyId = owner.keyId
        let http = FakeHTTPClient { _ in portJSON(["ok": true, "keyId": .string(keyId)], 201) }
        let api = ApiPortFixture.api(http, token: "")
        var token = ""
        api.sessionTokenProvider = { token }

        // No owner identity yet: nothing to enroll.
        await api.syncOwnerEnrollment()
        api.ownerCrypto = owner
        await api.syncOwnerEnrollment()
        XCTAssertEqual(http.requests.count, 0)

        token = "session-1"
        await api.syncOwnerEnrollment()
        await api.syncOwnerEnrollment()
        XCTAssertEqual(http.requests.count, 1)
        XCTAssertEqual(http.requests.first?.headers["Authorization"], "Bearer session-1")

        // A transient failure is retried on the next session change, not memoized.
        token = "session-2"
        http.enqueue(status: 503, text: "")
        await api.syncOwnerEnrollment()
        XCTAssertEqual(http.requests.count, 2)
        await api.syncOwnerEnrollment()
        XCTAssertEqual(http.requests.count, 3)
        await api.syncOwnerEnrollment()
        XCTAssertEqual(http.requests.count, 3)

        // Signing out clears the memo so the same token re-enrolls later.
        token = ""
        await api.syncOwnerEnrollment()
        token = "session-2"
        await api.syncOwnerEnrollment()
        XCTAssertEqual(http.requests.count, 4)
    }
}
