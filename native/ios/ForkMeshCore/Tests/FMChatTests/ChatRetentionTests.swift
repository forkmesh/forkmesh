import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest

@testable import FMChat

private let hour: Int64 = 3_600_000
private let day: Int64 = 86_400_000

private func message(_ id: String, _ conversation: String, _ ts: Int64) -> ChatMessage {
    ChatMessage(id: id, conversation: conversation, senderId: "peer", senderName: "Pat", text: id, timestamp: Date(epochMillis: ts))
}

private func peerFrame(_ type: String, _ fields: JSONObject, id: String, ts: Int64) -> JSONObject {
    var frame: JSONObject = [
        "type": .string(type), "id": .string(id), "senderId": "peer-1", "sender": "Pat", "ts": .int(ts),
    ]
    for (key, value) in fields { frame[key] = value }
    return frame
}

final class ChatRetentionCodecTests: XCTestCase {
    func testBoundedDropsExpiredMessagesPerChat() {
        let now = Date(epochMillis: 1_770_000_000_000)
        let base = now.epochMillis
        let retention = ChatRetention(defaultMs: day, chats: ["#fast": hour])
        let kept = ChatHistoryCodec.bounded(
            [
                "#general": [message("g-old", "#general", base - 2 * day), message("g-new", "#general", base - 2 * hour)],
                "#fast": [message("f-old", "#fast", base - 2 * hour), message("f-new", "#fast", base - 60_000)],
                "direct:\(String(repeating: "a", count: 32))": [
                    message("d-edge", "direct:\(String(repeating: "a", count: 32))", base - day),
                    message("d-new", "direct:\(String(repeating: "a", count: 32))", base - day + 1),
                ],
                "@peer": [message("p-new", "@peer", base - 60_000)],
            ], retention: retention, now: now)
        XCTAssertEqual(Set(kept.map(\.id)), ["g-new", "f-new", "d-new"], "a relay @peer conversation is never saved")
        XCTAssertEqual(ChatHistoryCodec.bounded(["#general": [message("g-old", "#general", 0)]]).count, 1)
    }
}

@MainActor
final class ChatRetentionHubTests: XCTestCase {
    private func hub(clock: Clock = SystemClock()) async throws -> (ChatHub, RelayHarness, LocalMeshService) {
        let h = try await RelayHarness.start()
        let mesh = makeMesh(h.device)
        return (ChatHub(relay: h.relay, mesh: mesh, clock: clock), h, mesh)
    }

    func testIncomingMessagesPastTheCutoffNeverShow() async throws {
        let (hub, h, _) = try await hub()
        hub.applyRetention(ChatRetention(defaultMs: day))
        let now = Date().epochMillis
        try await h.sendProtected(peerFrame("chat", ["channel": "#general", "text": "stale"], id: "stale-1", ts: now - 2 * day))
        try await h.sendProtected(peerFrame("chat", ["channel": "#general", "text": "fresh"], id: "fresh-1", ts: now - 2 * hour))
        let arrived = await waitUntil { h.relay.messages("#general").contains { $0.id == "fresh-1" } }
        XCTAssertTrue(arrived)
        XCTAssertEqual(hub.rows.map(\.message.id), ["fresh-1"])
        await h.close()
    }

    func testAShorterPreferencePrunesAtOnce() async throws {
        let (hub, h, _) = try await hub()
        let now = Date().epochMillis
        try await h.sendProtected(peerFrame("chat", ["channel": "#general", "text": "root"], id: "root-1", ts: now - 2 * hour))
        try await h.sendProtected(
            peerFrame("thread-reply", ["channel": "#general", "rootId": "root-1", "text": "reply"], id: "reply-1", ts: now - 2 * hour))
        try await h.sendProtected(peerFrame("chat", ["channel": "#random", "text": "other"], id: "other-1", ts: now - 2 * hour))
        let arrived = await waitUntil {
            hub.rows.count == 1 && hub.extras.replies(to: "root-1").count == 1 && h.relay.messages("#random").count == 1
        }
        XCTAssertTrue(arrived)

        hub.applyRetention(ChatRetention(chats: ["#general": hour]))
        XCTAssertTrue(hub.rows.isEmpty)
        XCTAssertTrue(hub.extras.replies(to: "root-1").isEmpty)
        XCTAssertEqual(h.relay.messages("#random").map(\.id), ["other-1"])
        XCTAssertTrue(hub.historySnapshot().messages.allSatisfy { $0.conversation == "#random" })
        await h.close()
    }

    func testRestoredHistoryIsFilteredAndTheSnapshotWritesNothingExpired() async throws {
        let now = Date().epochMillis
        let dir = temporaryDirectory("chat-retention")
        defer { try? FileManager.default.removeItem(at: dir) }
        let store = ChatHistoryStore(directory: dir, delay: 0)
        var snapshot = ChatHistorySnapshot()
        snapshot.messages = [
            message("old", "#general", now - 3 * day), message("new", "#general", now - hour),
            message("dm-old", "@peer", now - 2 * hour),
        ]
        store.save(snapshot, scope: "scope-r")

        let (hub, h, _) = try await hub()
        hub.applyRetention(ChatRetention(defaultMs: day, chats: ["@peer": hour]))
        hub.attachHistory(store, scope: { "scope-r" })
        XCTAssertEqual(hub.rows.map(\.message.id), ["new"])
        XCTAssertTrue(h.relay.messages("@peer").isEmpty)
        XCTAssertEqual(hub.historySnapshot().messages.map(\.id), ["new"])
        await h.close()
    }

    func testThePeriodicPruneDropsMessagesAsTheyAge() async throws {
        let clock = FixedClock(Date())
        let (hub, h, mesh) = try await hub(clock: clock)
        hub.applyRetention(ChatRetention(defaultMs: day))
        _ = await mesh.start()
        let sent = await hub.relay.sendMessage("soon gone", to: "#general")
        XCTAssertTrue(sent)
        let meshId = await mesh.sendMessage("mesh note")
        XCTAssertFalse(meshId.isEmpty)
        XCTAssertEqual(hub.rows.count, 1)
        XCTAssertEqual(mesh.messages.count, 1)

        clock.advance(by: 2 * 86_400)
        hub.startRetentionPruning(every: 0.01, sleep: { seconds in
            try await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
        })
        let pruned = await waitUntil { hub.rows.isEmpty && mesh.messages.isEmpty }
        XCTAssertTrue(pruned)
        XCTAssertFalse(mesh.isQueued(meshId))
        hub.stopRetentionPruning()
        await mesh.stop()
        await h.close()
    }

    func testOfficeChannelsUseTheirRoomIdKey() async throws {
        let (hub, h, _) = try await hub()
        XCTAssertEqual(hub.retentionKey("#general"), "#general")
        XCTAssertEqual(hub.retentionKey("@abc"), "@abc")
        XCTAssertEqual(hub.retentionKey("#office/lobby"), "#office/lobby")

        let roomId = String(repeating: "a", count: 32)
        let api = FakeAccountApi()
        api.replies["GET /api/chat/channels"] = [
            "channels": [["id": .string(roomId), "name": "lobby", "visibility": "public", "canManage": true]]
        ]
        let room = "chat-channel:\(roomId):v1"
        let crypto = await Task.detached { RoomCrypto.withPassphrase(room, "office-pass") }.value
        let now = Date().epochMillis
        let store = InMemorySecureStore()
        let me = try Identity.fromSeed(Ed25519.generateSeed())
        let peer = try OfficePeer(room: room, crypto: crypto)
        try peer.learnPhone(store: store, identity: me)
        let older = try peer.envelope(["type": "chat", "id": "o-1", "sender": "Pat", "text": "older", "ts": .int(now - 2 * hour)])
        let recent = try peer.envelope(["type": "chat", "id": "o-2", "sender": "Pat", "text": "recent", "ts": .int(now - 60_000)])
        api.replies["GET /api/chat/channels/\(roomId)/history"] = [
            "room": .string(room), "passphrase": "office-pass",
            "messages": .array(try peer.history([(now - 2 * hour, older), (now - 60_000, recent)])),
        ]
        let office = OfficeChannelService(
            api: api, connector: FakeWebSocketConnector(), secureStore: store,
            origin: { URL(string: "https://app.forkmesh.com") }, selfId: { me.nodeId }, displayName: { "Me" }, nodeIdentity: me)
        hub.office = office
        await office.refresh()
        XCTAssertEqual(hub.retentionKey("#office/lobby"), "channel:\(roomId)")
        hub.applyRetention(ChatRetention(chats: ["channel:\(roomId)": hour]))
        await office.loadHistory("#office/lobby")
        XCTAssertEqual(office.messages("#office/lobby").map(\.id), ["o-2"])

        hub.applyRetention(ChatRetention(defaultMs: day))
        await office.loadHistory("#office/lobby")
        XCTAssertEqual(office.messages("#office/lobby").map(\.id), ["o-1", "o-2"])
        hub.applyRetention(ChatRetention(chats: ["#office/lobby": hour, "channel:\(roomId)": 7 * day]))
        XCTAssertEqual(office.messages("#office/lobby").map(\.id), ["o-1", "o-2"])
        hub.applyRetention(ChatRetention(chats: ["channel:\(roomId)": hour]))
        XCTAssertEqual(office.messages("#office/lobby").map(\.id), ["o-2"])
        await h.close()
    }
}
