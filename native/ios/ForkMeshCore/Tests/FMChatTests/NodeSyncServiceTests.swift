// Node sync: the signed drain token, payload parsing, the
// ack-only-what-was-applied contract, the mode, and the foreground lifecycle. The widget groups
// ("node activity card", "agents degrade without a desktop node") are screen tests and belong
// with the SwiftUI views. `nodeSyncSettle` drains queued tasks; timers are the hand-driven
// `NodeSyncSleeper`.

import FMAccount
import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest
@testable import FMChat

@MainActor
final class NodeSyncServiceTests: XCTestCase {
    // MARK: Signed drain token

    func testTheCanonicalIsForkmeshIssuesPullV1OwnerTs() async {
        XCTAssertEqual(NodeSyncService.syncCanonical("alice", "1760000000000"), "forkmesh-issues-pull-v1\nalice\n1760000000000")
        // The Worker splits on "\n"; a stray separator would authorize a different owner.
        XCTAssertEqual(NodeSyncService.canonicalPrefix, "forkmesh-issues-pull-v1")
        XCTAssertEqual(NodeSyncService.syncCanonical("alice", "1").components(separatedBy: "\n"), ["forkmesh-issues-pull-v1", "alice", "1"])
    }

    func testTheSignatureVerifiesAgainstThisDeviceKey() async throws {
        let h = try NodeSyncHarness()
        defer { h.node.dispose() }

        let token = h.node.signedToken("alice", nowMs: 1_760_000_000_000)
        XCTAssertEqual(token.ts, "1760000000000")
        XCTAssertFalse(token.sig.contains("="))

        let message = Array("forkmesh-issues-pull-v1\nalice\n1760000000000".utf8)
        let publicKey = Array(try B64.decodeUnpadded(h.identity.publicKeyB64url))
        XCTAssertTrue(Ed25519.verify(publicKey: publicKey, message: message, signature: Array(try B64.decodeUnpadded(token.sig))))

        // A signature over a different owner must not verify for "alice".
        let other = h.node.signedToken("mallory", nowMs: 1_760_000_000_000)
        XCTAssertFalse(Ed25519.verify(publicKey: publicKey, message: message, signature: Array(try B64.decodeUnpadded(other.sig))))
    }

    func testGetApiSyncCarriesOwnerTsAndSig() async throws {
        let h = try NodeSyncHarness()
        defer { h.node.dispose() }

        let url = try XCTUnwrap(h.node.syncURL(nowMs: 1_760_000_000_000))
        XCTAssertEqual(url.path, "/api/sync")
        XCTAssertEqual(url.scheme, "http") // ws:// origin -> http://
        XCTAssertEqual(url.host, "relay.test")
        let query = nodeSyncQuery(url)
        XCTAssertEqual(query["owner"], "alice")
        XCTAssertEqual(query["ts"], "1760000000000")
        XCTAssertFalse((query["sig"] ?? "").isEmpty)
        // Query order: owner, ts, sig.
        XCTAssertTrue(url.absoluteString.hasPrefix("http://relay.test/api/sync?owner=alice&ts=1760000000000&sig="))
    }

    func testTheEventsSocketAuthenticatesWithTheSignedTokenNotATicket() async throws {
        let h = try NodeSyncHarness()
        defer { h.node.dispose() }

        let url = try XCTUnwrap(h.node.eventsURL(nowMs: 1_760_000_000_000))
        // `_node_events_authorization` takes ?ticket= only for browsers; a node presents the same
        // forkmesh-issues-pull-v1 proof as /api/sync.
        XCTAssertEqual(url.path, "/api/nodes/events")
        XCTAssertEqual(url.scheme, "ws")
        let query = nodeSyncQuery(url)
        XCTAssertNil(query["ticket"])
        XCTAssertEqual(query["owner"], "alice")
        XCTAssertEqual(query["ts"], "1760000000000")
        XCTAssertFalse((query["sig"] ?? "").isEmpty)
        XCTAssertFalse(url.absoluteString.contains("mobile-session"))
    }

    func testASessionThatCannotOwnerSignHasNoOwnerToSignAs() async throws {
        let h = try NodeSyncHarness(session: { nodeSyncPlainSession() })
        defer { h.node.dispose() }
        XCTAssertEqual(h.node.owner, "")
        XCTAssertEqual(h.node.mode, .disabled)
    }

    // MARK: Payload parsing

    func testEveryKindLandsOnItsRepositoryWithItsDrainId() async throws {
        let payload = NodeSyncPayload.parse(nodeSyncBody())

        XCTAssertEqual(payload.serverTime, 1_760_000_000_000)
        XCTAssertEqual(payload.repos.count, 1)
        let repo = try XCTUnwrap(payload.repos.first)
        XCTAssertEqual(repo.fullName, "alice/notes")
        XCTAssertEqual(repo.stateHash, "abc123")
        XCTAssertEqual(repo.source, "local-node")
        XCTAssertEqual(repo.agentPromptAckPath, "/api/repo/alice/notes/agents/ack")
        XCTAssertEqual(repo.mirrorMerges.first?["requestId"], "req-1")

        XCTAssertEqual(payload.countOf(.issues), 1)
        XCTAssertEqual(payload.countOf(.pulls), 1)
        XCTAssertEqual(payload.countOf(.discussions), 1)
        XCTAssertEqual(payload.countOf(.agentPrompts), 1)
        XCTAssertEqual(payload.countOf(.about), 1)

        XCTAssertEqual(repo.ofKind(.issues).first?.id, 11)
        XCTAssertEqual(repo.ofKind(.issues).first?.title, "Crash on launch")
        XCTAssertEqual(repo.ofKind(.issues).first?.numberOrZero, 4)
        XCTAssertEqual(repo.ofKind(.pulls).first?.id, 21)
        XCTAssertEqual(repo.ofKind(.discussions).first?.id, 31)
        // An agent prompt's queueId names a row the relay already deleted; it is never an ack
        // target, so it must not be carried as a drain id.
        XCTAssertEqual(repo.ofKind(.agentPrompts).first?.id, 0)
        XCTAssertEqual(repo.ofKind(.about).first?.id, 0)
        XCTAssertEqual(repo.ofKind(.about).first?.title, "A notes repository")
        XCTAssertEqual(repo.ofKind(.branchOps).first?.id, 7)
    }

    func testJunkEmptyAndUnknownShapesParseToNothingRatherThanThrow() async {
        XCTAssertTrue(NodeSyncPayload.parse(nil).repos.isEmpty)
        XCTAssertTrue(NodeSyncPayload.parse(.null).repos.isEmpty)
        XCTAssertTrue(NodeSyncPayload.parse("nope").repos.isEmpty)
        XCTAssertTrue(NodeSyncPayload.parse(["ok": true]).repos.isEmpty)
        let payload = NodeSyncPayload.parse([
            "repos": [
                ["owner": "alice"], // no name
                "garbage",
                ["owner": "alice", "name": "notes", "issues": ["garbage"], "futureKind": [["id": 1]]],
            ],
        ])
        XCTAssertEqual(payload.repos.count, 1)
        XCTAssertEqual(payload.repos.first?.items, [])
    }

    func testItemsRoundTripThroughTheCarriedItemCacheForm() async throws {
        let item = try XCTUnwrap(NodeSyncPayload.parse(nodeSyncBody()).items.first { $0.kind == .about })
        let restored = try XCTUnwrap(NodeSyncItem.fromJSON(item.toJSON()))
        XCTAssertEqual(restored.kind, .about)
        XCTAssertEqual(restored.repoFullName, "alice/notes")
        XCTAssertEqual(restored.title, "A notes repository")
        XCTAssertEqual(restored, item)
        XCTAssertNil(NodeSyncItem.fromJSON(["kind": "nonsense"]))
    }

    func testTitleAndNumberFallBackThroughTheFieldsThatExist() async {
        func item(_ data: JSONObject, kind: NodeSyncKind = .issues) -> NodeSyncItem {
            NodeSyncItem(owner: "a", repo: "b", kind: kind, id: 1, data: data)
        }
        XCTAssertEqual(item(["title": "  ", "titleIfNew": " New "]).title, "New")
        XCTAssertEqual(item(["event": ["title": "", "body": "Body text"]]).title, "Body text")
        XCTAssertEqual(item(["pullNumber": "12"]).title, "#12")
        XCTAssertEqual(item(["number": nil, "issueNumber": 7.9]).numberOrZero, 7)
        XCTAssertEqual(item(["number": " 5 "]).numberOrZero, 5)
        XCTAssertEqual(item(["number": "x"]).numberOrZero, 0)
        XCTAssertEqual(item([:], kind: .branchOps).title, "branch operation")
        XCTAssertEqual(NodeSyncKind.about.label(2), "About edits")
        XCTAssertEqual(NodeSyncKind.about.wireName, "aboutUpdate")
    }

    // MARK: Ack only what was applied

    func testWithNoApplierInstalledNothingIsEverDeleted() async throws {
        let h = try NodeSyncHarness()
        defer { h.node.dispose() }

        await h.node.syncNow()

        XCTAssertEqual(h.methods, ["GET"])
        XCTAssertEqual(h.node.ackedTotal, 0)
        XCTAssertEqual(h.node.pendingTotal, 6)
        XCTAssertEqual(h.node.counts[.issues], 1)
        XCTAssertEqual(h.node.counts[.branchOps], 1)
        XCTAssertEqual(h.requests.first?.headers["accept"], "application/json")
        XCTAssertEqual(h.requests.first?.timeout, 20)
    }

    func testExactlyTheAcceptedIssueRowsAreDeletedById() async throws {
        let h = try NodeSyncHarness(handler: { request in
            request.method == "DELETE"
                ? HTTPResponse(status: 200, json: ["ok": true, "drained": 1])
                : HTTPResponse(status: 200, json: nodeSyncBody())
        })
        defer { h.node.dispose() }

        var offered: [NodeSyncKind] = []
        h.node.applier = { item in
            offered.append(item.kind)
            return item.kind == .issues
        }

        await h.node.syncNow()

        // Only ackable kinds are ever offered: agent prompts need a host to run them, and About
        // edits are drained on read.
        XCTAssertEqual(Set(offered), [.issues, .pulls, .discussions])

        let deletes = h.requests.filter { $0.method == "DELETE" }
        XCTAssertEqual(deletes.count, 1)
        XCTAssertEqual(deletes.first?.url.path, "/api/repo/alice/notes/issues")
        let query = nodeSyncQuery(deletes.first?.url)
        XCTAssertEqual(query["ids"], "11")
        XCTAssertEqual(query["owner"], "alice")
        XCTAssertFalse((query["sig"] ?? "").isEmpty)
        XCTAssertEqual(h.node.ackedTotal, 1)
    }

    func testSeveralAcceptedRowsShareOneDeleteWithCommaSeparatedIds() async throws {
        let body: JSONValue = [
            "repos": [
                ["owner": "alice", "name": "notes", "issues": [["id": 11], ["id": 12]], "pulls": [["id": 21]]],
            ],
        ]
        let h = try NodeSyncHarness(handler: { request in
            request.method == "DELETE" ? HTTPResponse(status: 200, json: ["ok": true]) : HTTPResponse(status: 200, json: body)
        })
        defer { h.node.dispose() }
        h.node.applier = { _ in true }

        await h.node.syncNow()

        let deletes = h.requests.filter { $0.method == "DELETE" }
        XCTAssertEqual(deletes.map(\.url.path), ["/api/repo/alice/notes/issues", "/api/repo/alice/notes/pulls"])
        // The comma is percent-encoded.
        XCTAssertTrue(deletes.first?.url.absoluteString.hasSuffix("&ids=11%2C12") ?? false, deletes.first?.url.absoluteString ?? "")
        XCTAssertEqual(nodeSyncQuery(deletes.first?.url)["ids"], "11,12")
        XCTAssertEqual(h.node.ackedTotal, 3)
    }

    func testAnApplierThatRefusesOrThrowsLeavesTheRowOnTheRelay() async throws {
        let h = try NodeSyncHarness()
        defer { h.node.dispose() }

        struct NoCheckout: Error {}
        h.node.applier = { item in
            if item.kind == .issues { throw NoCheckout() }
            return false
        }

        await h.node.syncNow()

        XCTAssertTrue(h.requests.filter { $0.method == "DELETE" }.isEmpty)
        XCTAssertEqual(h.node.ackedTotal, 0)
    }

    func testAFailedAckNamesTheKindAndStatus() async throws {
        let h = try NodeSyncHarness(handler: { request in
            request.method == "DELETE" ? HTTPResponse(status: 500, text: "") : HTTPResponse(status: 200, json: nodeSyncBody())
        })
        defer { h.node.dispose() }
        h.node.applier = { $0.kind == .issues }

        await h.node.syncNow()

        XCTAssertEqual(h.node.lastError, "ack issues: HTTP 500")
        XCTAssertEqual(h.node.ackedTotal, 0)
    }

    func testOnlyTheIdScopedInboxKindsAreAckable() async {
        XCTAssertEqual(NodeSyncService.ackableKinds, [.issues, .pulls, .discussions])
        XCTAssertTrue(NodeSyncKind.issues.isAckable)
        XCTAssertTrue(NodeSyncKind.pulls.isAckable)
        XCTAssertTrue(NodeSyncKind.discussions.isAckable)
        XCTAssertFalse(NodeSyncKind.agentPrompts.isAckable)
        XCTAssertFalse(NodeSyncKind.about.isAckable)
        XCTAssertFalse(NodeSyncKind.branchOps.isAckable)
    }

    func testRelayDrainedKindsAreCarriedLocallySoAReadIsNotALoss() async throws {
        let h = try NodeSyncHarness()
        defer { h.node.dispose() }

        await h.node.syncNow()

        XCTAssertEqual(Set(h.node.carried.map(\.kind)), [.about, .agentPrompts, .branchOps])
        XCTAssertEqual(h.node.carried.first { $0.kind == .branchOps }?.data["branch"], "agent/old-work")
        let key = NodeSyncService.carriedPrefsKeyFor(h.settings.relays.activeId)
        XCTAssertEqual(key, "nodeSync/carried/relay.test")
        XCTAssertNotNil(h.prefs.string(forKey: key))

        // The cache survives a restart of the service.
        let again = try NodeSyncHarness(prefs: h.prefs, secure: h.secure, identity: h.identity)
        defer { again.node.dispose() }
        again.node.loadCarried()
        XCTAssertEqual(again.node.carried.count, 3)

        again.node.clearCarried()
        XCTAssertTrue(again.node.carried.isEmpty)
        XCTAssertNil(h.prefs.string(forKey: key))
    }

    func testTheCarryKeepsOnlyTheNewestHundred() async throws {
        let h = try NodeSyncHarness()
        defer { h.node.dispose() }
        for _ in 0..<40 {
            await h.node.syncNow()
        }
        // Three drained items per read, capped at the newest carriedLimit.
        XCTAssertEqual(h.node.carried.count, NodeSyncService.carriedLimit)
    }

    func testACarryBelongsToTheRelayItWasDrainedFrom() async throws {
        let h = try NodeSyncHarness()
        defer { h.node.dispose() }
        await h.node.syncNow()
        XCTAssertEqual(h.node.carried.count, 3)

        // These rows were deleted from relay.test on read, so this phone holds the only copy.
        // Moving to another relay must neither offer them there nor lose them.
        h.settings.relays.add(try XCTUnwrap(RelayProfile.tryParse("other.test")))
        h.settings.relays.setActive("other.test")
        h.node.loadCarried()
        XCTAssertTrue(h.node.carried.isEmpty)

        h.settings.relays.setActive(try XCTUnwrap(RelayProfile.normalizeOrigin("ws://relay.test/ws")))
        h.node.loadCarried()
        XCTAssertEqual(h.node.carried.count, 3)
    }

    func testTheLegacyUnscopedCarryMovesIntoTheActiveRelaySlot() async throws {
        let item = NodeSyncItem(owner: "alice", repo: "notes", kind: .about, id: 0, data: ["about": "Old"])
        let prefs = InMemoryKeyValueStore([
            "nodeSync/carried": .string(JSON.encode(.array([.object(item.toJSON())]))),
        ])
        let h = try NodeSyncHarness(prefs: prefs)
        defer { h.node.dispose() }

        h.node.loadCarried()

        XCTAssertEqual(h.node.carried, [item])
        XCTAssertNil(prefs.string(forKey: NodeSyncService.carriedPrefsKey))
        XCTAssertNotNil(prefs.string(forKey: "nodeSync/carried/relay.test"))
    }

    func testA401ReportsTheWorkerErrorCodeAndStops() async throws {
        let h = try NodeSyncHarness(handler: { _ in HTTPResponse(status: 401, json: ["error": "device_revoked"]) })
        defer { h.node.dispose() }

        var codes: [String] = []
        h.node.onUnauthorized = { codes.append($0) }
        await h.node.syncNow()

        XCTAssertEqual(codes, ["device_revoked"])
        XCTAssertEqual(h.node.lastError, "device_revoked")
        XCTAssertNil(h.node.lastSyncAt)
    }

    func testA403WithoutACodeReadsUnauthorizedAndOtherFailuresTheStatus() async throws {
        let status = NodeSyncBox(403)
        let h = try NodeSyncHarness(handler: { _ in HTTPResponse(status: status.value, text: "") })
        defer { h.node.dispose() }
        var codes: [String] = []
        h.node.onUnauthorized = { codes.append($0) }

        await h.node.syncNow()
        XCTAssertEqual(codes, [""])
        XCTAssertEqual(h.node.lastError, "unauthorized")

        status.value = 404
        await h.node.syncNow()
        XCTAssertEqual(h.node.lastError, "HTTP 404")
        XCTAssertNil(h.node.lastSyncAt)
        XCTAssertFalse(h.node.syncing)
    }

    // MARK: Mode

    func testADesktopNodeOnTheAccountPutsThisPhoneOnStandby() async throws {
        let h = try NodeSyncHarness(session: { nodeSyncMobileSession(withDesktop: true) })
        defer { h.node.dispose() }

        h.node.start()
        await nodeSyncSettle()

        // /api/sync claims inbox rows for five minutes and deletes About edits on read: two nodes
        // polling one account starve and rob each other.
        XCTAssertEqual(h.node.mode, .standby)
        XCTAssertTrue(h.node.hasDesktopNode)
        XCTAssertTrue(h.requests.isEmpty)
        XCTAssertEqual(h.node.connection, .offline)
        XCTAssertTrue(h.ws.attempts.isEmpty)

        // A user-driven "Sync now" is still allowed from standby.
        await h.node.syncNow()
        XCTAssertEqual(h.methods, ["GET"])
    }

    func testAPlainAccountSessionNeverSyncsAtAll() async throws {
        let h = try NodeSyncHarness(session: { nodeSyncPlainSession() })
        defer { h.node.dispose() }

        h.node.start()
        await nodeSyncSettle()
        await h.node.syncNow()

        XCTAssertEqual(h.node.mode, .disabled)
        XCTAssertTrue(h.requests.isEmpty)
    }

    func testServingAsANodeIsOffUntilTheUserTurnsItOn() async throws {
        let h = try NodeSyncHarness(serveAsNode: false)
        defer { h.node.dispose() }
        XCTAssertFalse(h.settings.serveAsNode)

        h.node.start()
        await nodeSyncSettle()
        await h.node.syncNow()
        XCTAssertEqual(h.node.mode, .disabled)
        XCTAssertTrue(h.requests.isEmpty)
        XCTAssertTrue(h.ws.connections.isEmpty)

        h.settings.setServeAsNode(true)
        let live = await nodeSyncEventually { h.node.connection == .live && !h.requests.isEmpty }
        XCTAssertTrue(live)
        XCTAssertEqual(h.node.mode, .active)
        XCTAssertEqual(h.ws.connections.count, 1)

        h.settings.setServeAsNode(false)
        await nodeSyncSettle()
        XCTAssertEqual(h.node.mode, .disabled)
        XCTAssertEqual(h.node.connection, .offline)
    }

    func testTheModeFollowsTheSessionWhenADesktopAppears() async throws {
        let session = NodeSyncBox(nodeSyncMobileSession())
        let h = try NodeSyncHarness(session: { session.value })
        defer { h.node.dispose() }

        h.node.start()
        await nodeSyncSettle()
        XCTAssertEqual(h.node.mode, .active)

        session.value = nodeSyncMobileSession(withDesktop: true)
        h.node.refreshSession()
        await nodeSyncSettle()
        XCTAssertEqual(h.node.mode, .standby)
        XCTAssertEqual(h.node.connection, .offline)
    }

    // MARK: Foreground lifecycle

    func testStartOpensTheSocketAndSyncsPauseTakesBothDown() async throws {
        let h = try NodeSyncHarness()
        defer { h.node.dispose() }

        h.node.start()
        let up = await nodeSyncEventually { h.node.connection == .live && h.node.lastSyncAt != nil }
        XCTAssertTrue(up)
        XCTAssertEqual(h.node.mode, .active)
        XCTAssertEqual(h.ws.connections.count, 1)
        XCTAssertFalse(h.requests.isEmpty)
        await nodeSyncSettle()

        // Contract §1: nothing runs in the background.
        h.node.appLifecycleChanged(.paused)
        await nodeSyncSettle()
        XCTAssertEqual(h.node.connection, .offline)
        XCTAssertNotNil(h.ws.connections.first?.closedByClient)
        XCTAssertEqual(h.sleeper.pending, [], "the poll timer is cancelled")

        let backgroundedAt = h.requests.count
        h.ws.connections.first?.receive(json: ["type": "event", "topic": "issues"])
        await nodeSyncSettle()
        XCTAssertEqual(h.requests.count, backgroundedAt)

        // Resume comes back at once rather than serving out a frozen backoff.
        h.node.appLifecycleChanged(.resumed)
        let back = await nodeSyncEventually { h.ws.connections.count == 2 && h.node.connection == .live }
        XCTAssertTrue(back)
        let synced = await nodeSyncEventually { h.requests.count > backgroundedAt }
        XCTAssertTrue(synced)
    }

    func testAnEventFrameTriggersOneSignedSyncHelloDoesNot() async throws {
        let h = try NodeSyncHarness()
        defer { h.node.dispose() }

        h.node.start()
        let live = await nodeSyncEventually { h.node.connection == .live }
        XCTAssertTrue(live)
        await nodeSyncSettle()
        let afterConnect = h.requests.count

        let server = try XCTUnwrap(h.ws.lastConnection)
        server.receive(json: ["type": "hello", "accepts": ["agent-status"]])
        await nodeSyncSettle()
        XCTAssertEqual(h.requests.count, afterConnect)

        server.receive(json: ["type": "event", "topic": "issues", "repo": "alice/notes"])
        let synced = await nodeSyncEventually { h.requests.count == afterConnect + 1 }
        XCTAssertTrue(synced)
        await nodeSyncSettle()
        XCTAssertEqual(h.requests.count, afterConnect + 1)
    }

    func testTheEventsSocketIsTheSignedURL() async throws {
        let h = try NodeSyncHarness()
        defer { h.node.dispose() }

        h.node.start()
        let live = await nodeSyncEventually { h.node.connection == .live }
        XCTAssertTrue(live)
        let url = try XCTUnwrap(h.ws.attempts.first?.url)
        XCTAssertEqual(url.scheme, "ws")
        XCTAssertEqual(url.path, "/api/nodes/events")
        XCTAssertEqual(nodeSyncQuery(url)["owner"], "alice")
    }

    func testAClosedSocketBacksOffWithGrowingJitteredDelays() async throws {
        // A deterministic jitter source keeps the delays exact for the assertions.
        let draws = NodeSyncBox<[Int]>([])
        let h = try NodeSyncHarness(random: { n in
            draws.value.append(n)
            return 0
        })
        defer { h.node.dispose() }
        h.ws.refuseConnections(WebSocketError.transport("relay down"))
        func reconnectDelays() -> [TimeInterval] { h.sleeper.pending.filter { $0 < 60 } }

        h.node.start()
        let first = await nodeSyncEventually { !reconnectDelays().isEmpty }
        XCTAssertTrue(first)
        XCTAssertEqual(h.ws.attempts.count, 1)
        XCTAssertEqual(h.node.connection, .offline)
        // First retry is 2 s ±25%: the draw spans 1.5 s ... 2.5 s (1001 ms values).
        XCTAssertEqual(reconnectDelays(), [1.5])
        XCTAssertEqual(draws.value, [1001])

        h.sleeper.fire { $0 < 60 }
        let second = await nodeSyncEventually { h.ws.attempts.count == 2 && !reconnectDelays().isEmpty }
        XCTAssertTrue(second)
        // The window doubles to 4 s ±25%.
        XCTAssertEqual(reconnectDelays(), [3])
        XCTAssertEqual(draws.value, [1001, 2001])

        // ... and keeps doubling up to the 30 s cap.
        for expected in [6.0, 12, 22.5, 22.5] {
            h.sleeper.fire { $0 < 60 }
            let next = await nodeSyncEventually { reconnectDelays() == [expected] }
            XCTAssertTrue(next, "\(reconnectDelays()) != [\(expected)]")
        }
        XCTAssertEqual(h.ws.attempts.count, 6)

        await h.node.stop()
    }

    func testTheJitterStaysWithinAQuarterEitherWay() async throws {
        let h = try NodeSyncHarness(random: { n in n - 1 })
        defer { h.node.dispose() }
        h.ws.refuseConnections(WebSocketError.transport("relay down"))

        h.node.start()
        let first = await nodeSyncEventually { h.sleeper.pending.contains { $0 < 60 } }
        XCTAssertTrue(first)
        XCTAssertEqual(h.sleeper.pending.filter { $0 < 60 }, [2.5])
        await h.node.stop()
    }

    func testAServerCloseSchedulesAReconnectAndTheNextOpenResetsTheBackoff() async throws {
        let h = try NodeSyncHarness()
        defer { h.node.dispose() }

        h.node.start()
        let live = await nodeSyncEventually { h.node.connection == .live }
        XCTAssertTrue(live)
        h.ws.lastConnection?.serverClose(code: 1001, reason: "deploy")
        let down = await nodeSyncEventually { h.node.connection == .offline && h.sleeper.pending.contains { $0 < 60 } }
        XCTAssertTrue(down)
        XCTAssertEqual(h.sleeper.pending.filter { $0 < 60 }, [2]) // random n/2: the centre

        h.sleeper.fire { $0 < 60 }
        let again = await nodeSyncEventually { h.ws.connections.count == 2 && h.node.connection == .live }
        XCTAssertTrue(again)
    }

    func testThePollTimerSyncsEveryInterval() async throws {
        let h = try NodeSyncHarness()
        defer { h.node.dispose() }

        h.node.start()
        let live = await nodeSyncEventually { h.node.connection == .live }
        XCTAssertTrue(live)
        await nodeSyncSettle()
        XCTAssertTrue(h.sleeper.pending.contains(60))
        let before = h.requests.count

        h.sleeper.fire { $0 == 60 }
        let polled = await nodeSyncEventually { h.requests.count == before + 1 }
        XCTAssertTrue(polled)
        // The timer is periodic: it parks again for the next tick.
        let parked = await nodeSyncEventually { h.sleeper.pending.contains(60) }
        XCTAssertTrue(parked)
    }

    func testStopCancelsThePollAndTheSocket() async throws {
        let h = try NodeSyncHarness()
        defer { h.node.dispose() }

        h.node.start()
        let live = await nodeSyncEventually { h.node.connection == .live }
        XCTAssertTrue(live)
        XCTAssertTrue(h.node.started)

        await h.node.stop()
        XCTAssertFalse(h.node.started)
        XCTAssertEqual(h.node.connection, .offline)
        XCTAssertNotNil(h.ws.connections.first?.closedByClient)
        await nodeSyncSettle()
        XCTAssertEqual(h.sleeper.pending, [])
    }

    func testOnNewItemsFiresForFreshlySeenRowsOnly() async throws {
        let body = NodeSyncBox(nodeSyncBody())
        let h = try NodeSyncHarness(handler: { _ in HTTPResponse(status: 200, json: body.value) })
        defer { h.node.dispose() }

        var fired = 0
        h.node.onNewItems = { fired += 1 }

        await h.node.syncNow()
        XCTAssertEqual(fired, 1)

        // The same rows again are not new.
        await h.node.syncNow()
        XCTAssertEqual(fired, 1)

        body.value = nodeSyncBody(issueId: 12)
        await h.node.syncNow()
        XCTAssertEqual(fired, 2)
    }

    func testListenersHearStateChanges() async throws {
        let h = try NodeSyncHarness()
        defer { h.node.dispose() }
        let count = NodeSyncBox(0)
        let token = h.node.addListener { count.value += 1 }

        await h.node.syncNow()
        XCTAssertGreaterThanOrEqual(count.value, 2) // syncing on, syncing off

        h.node.removeListener(token)
        let heard = count.value
        await h.node.syncNow()
        XCTAssertEqual(count.value, heard)
    }
}
