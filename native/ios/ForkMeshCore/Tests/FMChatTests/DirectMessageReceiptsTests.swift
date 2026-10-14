import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest

@testable import FMChat

@MainActor
struct DirectRoomHelpers {
    func makeService(
        api: FakeAccountApi, ws: FakeWebSocketConnector,
        sleep: (@Sendable (TimeInterval) async throws -> Void)? = nil
    ) -> DirectMessageService {
        DirectMessageService(
            api: api, connector: ws, secureStore: InMemorySecureStore(),
            origin: { URL(string: "https://app.next.forkmesh.com") }, selfId: { "me-node" }, displayName: { "Me" },
            sleep: sleep ?? { seconds in try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000)) })
    }

    func keyAdverts(_ connection: FakeWebSocketConnection, _ crypto: RoomCrypto) -> [JSONObject] {
        connection.sentJSON.filter { RoomSeal.isKeyFrame(crypto.decrypt($0) ?? [:]) }
    }

    func teach(_ peer: RoomSeal, from connection: FakeWebSocketConnection, _ crypto: RoomCrypto) async -> Bool {
        let advertised = await waitUntil { !self.keyAdverts(connection, crypto).isEmpty }
        for envelope in keyAdverts(connection, crypto) {
            if let frame = crypto.decrypt(envelope) { _ = peer.acceptKeyFrame(frame) }
        }
        return advertised
    }

    func sentOfKind(_ connection: FakeWebSocketConnection, _ kind: String) -> [JSONObject] {
        connection.sentJSON.filter { $0["kind"]?.text == kind }
    }

    func roomCrypto() async -> RoomCrypto {
        await Task.detached { RoomCrypto.withPassphrase(DirectMessagesTests.room, "direct-pass") }.value
    }

    func peerSeal(_ scope: String) throws -> RoomSeal {
        try RoomSeal.loadOrCreate(DirectMessagesTests.room, storageScope: scope, secureStore: InMemorySecureStore())
    }

    func preparedApi() -> FakeAccountApi {
        let api = FakeAccountApi()
        api.replies["GET /api/chat/direct-messages"] = [
            "conversations": [["id": .string(DirectMessagesTests.conversationId), "otherUser": "pat"]]
        ]
        api.replies["GET /api/chat/direct-messages/\(DirectMessagesTests.conversationId)/room-access"] = [
            "room": .string(DirectMessagesTests.room), "passphrase": "direct-pass",
            "webSocketUrl": .string("/api/chat/direct-messages/\(DirectMessagesTests.conversationId)/ws?ticket=abc"),
        ]
        return api
    }
}

final class DirectMessageReceiptsTests: XCTestCase {
    @MainActor var base: DirectRoomHelpers { DirectRoomHelpers() }

    struct Room {
        let service: DirectMessageService
        let connection: FakeWebSocketConnection
        let crypto: RoomCrypto
        let peer: RoomSeal
        let conversation: String
    }

    @MainActor func openRoom(_ service: DirectMessageService? = nil, ws: FakeWebSocketConnector = FakeWebSocketConnector()) async throws
        -> Room
    {
        let service = service ?? base.makeService(api: base.preparedApi(), ws: ws)
        await service.refresh()
        let conversation = service.conversations[0]
        let opened = await service.open(conversation)
        XCTAssertTrue(opened)
        let connection = try XCTUnwrap(ws.lastConnection)
        let crypto = await base.roomCrypto()
        let peer = try base.peerSeal("peer")
        connection.receive(text: JSON.encode(try crypto.encrypt(peer.keyFrame())))
        let taught = await base.teach(peer, from: connection, crypto)
        XCTAssertTrue(taught)
        let known = await waitUntil { service.deviceKeys(conversation).contains(peer.keyId) }
        XCTAssertTrue(known)
        return Room(service: service, connection: connection, crypto: crypto, peer: peer, conversation: conversation)
    }

    @MainActor func peerSends(_ room: Room, _ plain: JSONObject, replay: Bool = false) throws {
        var envelope = try room.crypto.encrypt(try room.peer.seal(plain))
        if replay { envelope["historyReplay"] = true }
        room.connection.receive(text: JSON.encode(envelope))
    }

    func relayFrame(_ object: JSONObject) -> String { JSON.encode(object) }

    @MainActor func peerFrames(_ room: Room) -> [JSONObject] {
        room.connection.sentTexts.compactMap { room.crypto.decrypt(JSON.parseObject($0)) }
            .compactMap { RoomSeal.isSealedFrame($0) ? room.peer.unseal($0) : nil }
    }

    @MainActor func sealedRef(_ room: Room, type: String, id: String) -> String? {
        room.connection.sentTexts.first { text in
            guard let framed = room.crypto.decrypt(JSON.parseObject(text)), RoomSeal.isSealedFrame(framed),
                let plain = room.peer.unseal(framed)
            else { return false }
            return plain["type"]?.text == type && plain["id"]?.text == id
        }.map(DirectMessageFrames.frameRef)
    }

    func chat(_ id: String, _ text: String, conversation: String, sender: String = "pat-node", ts: Int64 = 1_000) -> JSONObject {
        [
            "type": "chat", "id": .string(id), "senderId": .string(sender), "sender": "pat", "accountKind": "user",
            "channel": .string(conversation), "text": .string(text), "ts": .int(ts),
        ]
    }

    @MainActor func testOwnMessagesGoFromSendingToReadThroughRelayReceipts() async throws {
        let room = try await openRoom()
        let sent = await room.service.send(room.conversation, text: "are you there?")
        XCTAssertTrue(sent)
        let message = try XCTUnwrap(room.service.messages(room.conversation).first)
        XCTAssertEqual(room.service.receipt(message.id), .sending)
        let ref = try XCTUnwrap(sealedRef(room, type: "chat", id: message.id))
        XCTAssertEqual(room.service.ref(of: message.id, in: room.conversation), ref)
        room.connection.receive(text: relayFrame(["kind": "forkmesh-delivered", "v": 1, "refs": [.string(String(repeating: "f", count: 40))]]))
        room.connection.receive(text: relayFrame(["kind": "forkmesh-recipients", "ref": .string(ref), "recipients": 1]))
        let acked = await waitUntil { room.service.receipt(message.id) == .sent }
        XCTAssertTrue(acked)
        room.connection.receive(text: relayFrame(["kind": "forkmesh-delivered", "v": 1, "refs": [.string(ref)]]))
        let delivered = await waitUntil { room.service.receipt(message.id) == .delivered }
        XCTAssertTrue(delivered)
        room.connection.receive(text: relayFrame(["kind": "forkmesh-recipients", "ref": .string(ref), "recipients": 1]))
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(room.service.receipt(message.id), .delivered, "a late ack never moves a mark back")
        let unsent = await room.service.send(room.conversation, text: "second")
        XCTAssertTrue(unsent)
        let second = try XCTUnwrap(room.service.messages(room.conversation).last)
        room.connection.receive(text: relayFrame(["kind": "forkmesh-read", "v": 1]))
        let read = await waitUntil { room.service.receipt(message.id) == .read }
        XCTAssertTrue(read)
        XCTAssertEqual(room.service.receipt(second.id), .sending, "read covers only what the relay already took")
        await room.service.closeAll()
    }

    @MainActor func testOwnFramesReplayedFromHistoryShowAsSent() async throws {
        let room = try await openRoom()
        var own = chat("me-node-1-1", "from my laptop", conversation: room.conversation, sender: "me-node")
        own["sender"] = "Me"
        var envelope = try room.crypto.encrypt(try room.peer.seal(own))
        envelope["historyReplay"] = true
        envelope["ref"] = .string(String(repeating: "b", count: 40))
        room.connection.receive(text: JSON.encode(envelope))
        let shown = await waitUntil { room.service.messages(room.conversation).count == 1 }
        XCTAssertTrue(shown)
        XCTAssertEqual(room.service.receipt("me-node-1-1"), .sent)
        room.connection.receive(text: relayFrame(["kind": "forkmesh-delivered", "v": 1, "refs": [.string(String(repeating: "b", count: 40))]]))
        let delivered = await waitUntil { room.service.receipt("me-node-1-1") == .delivered }
        XCTAssertTrue(delivered)
        await room.service.closeAll()
    }

    @MainActor func testAFailedSendTakesTheMessageBack() async throws {
        let api = base.preparedApi()
        api.failures["GET /api/chat/direct-messages/\(DirectMessagesTests.conversationId)/room-access"] =
            DirectMessageError.unavailable
        let service = base.makeService(api: api, ws: FakeWebSocketConnector(), sleep: { _ in throw CancellationError() })
        await service.refresh()
        let conversation = service.conversations[0]
        let sent = await service.send(conversation, text: "nobody home")
        XCTAssertFalse(sent)
        XCTAssertTrue(service.messages(conversation).isEmpty)
        XCTAssertTrue(service.receipts.isEmpty)
    }

    @MainActor func testDeleteForEveryoneRemovesTheMessageAndWithdrawsItsServerCopy() async throws {
        let room = try await openRoom()
        var removed: [(String, String)] = []
        room.service.onRemoved = { removed.append(($0, $1)) }
        let sent = await room.service.send(room.conversation, text: "oops")
        XCTAssertTrue(sent)
        let id = try XCTUnwrap(room.service.messages(room.conversation).first?.id)
        let ref = try XCTUnwrap(sealedRef(room, type: "chat", id: id))
        let deleted = await room.service.deleteForEveryone(room.conversation, messageId: id)
        XCTAssertTrue(deleted)
        XCTAssertTrue(room.service.messages(room.conversation).isEmpty)
        XCTAssertEqual(removed.map(\.1), [id])
        let frame = try XCTUnwrap(peerFrames(room).first { $0["type"]?.text == "delete" })
        XCTAssertEqual(frame["target"]?.text, id)
        XCTAssertEqual(frame["conversation"]?.text, room.conversation)
        XCTAssertNil(frame["channel"])
        let envelope = try XCTUnwrap(
            room.connection.sentTexts.map { JSON.parseObject($0) }.first {
                guard let framed = room.crypto.decrypt($0), let plain = room.peer.unseal(framed) else { return false }
                return plain["type"]?.text == "delete"
            })
        XCTAssertEqual(envelope["persist"], .bool(true))
        XCTAssertNil(envelope["notify"])
        let withdrawn = await waitUntil { !self.base.sentOfKind(room.connection, DirectMessageFrames.supersededKind).isEmpty }
        XCTAssertTrue(withdrawn)
        XCTAssertEqual(base.sentOfKind(room.connection, DirectMessageFrames.supersededKind).last?["refs"], .array([.string(ref)]))
        await room.service.closeAll()
    }

    @MainActor func testOnlyTheAuthorCanDeleteAndADeleteBeforeItsTargetStillWins() async throws {
        let room = try await openRoom()
        var gone: Set<String> = []
        room.service.isRemoved = { gone.contains($0) }
        room.service.onRemoved = { gone.insert($1) }
        try peerSends(room, chat("pat-1", "hello", conversation: room.conversation))
        let shown = await waitUntil { room.service.messages(room.conversation).count == 1 }
        XCTAssertTrue(shown)
        func delete(_ id: String, target: String, sender: String) -> JSONObject {
            [
                "type": "delete", "id": .string(id), "senderId": .string(sender), "sender": "x",
                "conversation": .string(room.conversation), "target": .string(target), "ts": 2_000,
            ]
        }
        try peerSends(room, delete("d-0", target: "pat-1", sender: "mallory"))
        var other = delete("d-1", target: "pat-1", sender: "pat-node")
        other["conversation"] = .string("direct:" + String(repeating: "e", count: 32))
        try peerSends(room, other)
        try peerSends(room, chat("pat-2", "still here", conversation: room.conversation, ts: 1_001))
        let both = await waitUntil { room.service.messages(room.conversation).count == 2 }
        XCTAssertTrue(both, "a delete from someone else, or for another conversation, is ignored")
        try peerSends(room, delete("d-2", target: "pat-1", sender: "pat-node"))
        let removed = await waitUntil { room.service.messages(room.conversation).map(\.id) == ["pat-2"] }
        XCTAssertTrue(removed)
        try peerSends(room, chat("pat-1", "hello", conversation: room.conversation), replay: true)
        try peerSends(room, delete("d-3", target: "pat-3", sender: "pat-node"), replay: true)
        try peerSends(room, chat("pat-3", "never shown", conversation: room.conversation, ts: 900), replay: true)
        try peerSends(room, chat("pat-4", "last", conversation: room.conversation, ts: 1_002))
        let settled = await waitUntil { room.service.messages(room.conversation).map(\.id) == ["pat-2", "pat-4"] }
        XCTAssertTrue(settled)
        XCTAssertEqual(gone, ["pat-1", "pat-3"])
        await room.service.closeAll()
    }

    @MainActor func hub(_ service: DirectMessageService) async throws -> (ChatHub, RelayHarness) {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        let hub = ChatHub(relay: h.relay, mesh: mesh)
        hub.direct = service
        return (hub, h)
    }

    @MainActor func testReactionsEditsRepliesAndDeletesWorkInADirectMessage() async throws {
        let ws = FakeWebSocketConnector()
        let service = base.makeService(api: base.preparedApi(), ws: ws)
        let (hub, h) = try await hub(service)
        var withdrawn: [String] = []
        hub.onMessageRemoved = { withdrawn.append($1) }
        let room = try await openRoom(service, ws: ws)
        hub.select(room.conversation)
        XCTAssertTrue(hub.inDirect)
        let now = Date().epochMillis
        try peerSends(room, chat("pat-1", "lunch?", conversation: room.conversation, ts: now - 60_000))
        let shown = await waitUntil { hub.rows.map(\.id) == ["pat-1"] }
        XCTAssertTrue(shown)
        let theirs = try XCTUnwrap(hub.rows.first)
        XCTAssertFalse(theirs.deletable)
        XCTAssertFalse(theirs.editable)
        XCTAssertNil(theirs.receipt)

        hub.toggleReaction(theirs.message, value: "like")
        let reacted = await waitUntil { hub.rows.first?.reactions.first?.mine == true }
        XCTAssertTrue(reacted)
        let reaction = try XCTUnwrap(peerFrames(room).first { $0["type"]?.text == "reaction" })
        XCTAssertEqual(reaction["target"]?.text, "pat-1")
        XCTAssertEqual(reaction["reactorId"]?.text, "me-node")
        XCTAssertEqual(reaction["added"], .bool(true))
        XCTAssertEqual(reaction["conversation"]?.text, room.conversation)

        let replied = await hub.replyInThread(channel: room.conversation, rootId: "pat-1", text: "yes, noon")
        XCTAssertTrue(replied)
        XCTAssertEqual(hub.threadRows("pat-1").map(\.text), ["yes, noon"])
        XCTAssertEqual(hub.threadRows("pat-1").first?.receipt, .sending)
        XCTAssertEqual(hub.rows.first?.thread?.replyCount, 1)
        XCTAssertEqual(hub.rootMessage("pat-1", in: room.conversation)?.text, "lunch?")
        let reply = try XCTUnwrap(peerFrames(room).first { $0["type"]?.text == "thread-reply" })
        XCTAssertEqual(reply["rootId"]?.text, "pat-1")
        XCTAssertEqual(reply["channel"]?.text, room.conversation)

        try peerSends(room, [
            "type": "edit", "id": "e-1", "senderId": "pat-node", "sender": "pat", "conversation": .string(room.conversation),
            "target": "pat-9", "text": "dinner?", "editedAt": .int(now - 30_000), "ts": .int(now - 30_000),
        ])
        try peerSends(room, [
            "type": "edit", "id": "e-0", "senderId": "mallory", "sender": "m", "conversation": .string(room.conversation),
            "target": "pat-1", "text": "forged", "ts": .int(now - 30_000),
        ])
        try peerSends(room, chat("pat-9", "lunch at 1?", conversation: room.conversation, ts: now - 50_000))
        let edited = await waitUntil { hub.rows.last?.text == "dinner?" }
        XCTAssertTrue(edited, "an edit that arrived before its message applies once the message is in")
        XCTAssertEqual(hub.rows.first?.text, "lunch?")

        _ = await hub.deliver("on my way", to: room.conversation)
        let mine = try XCTUnwrap(hub.rows.last)
        XCTAssertTrue(mine.deletable)
        XCTAssertTrue(mine.editable)
        XCTAssertEqual(mine.receipt, .sending)
        XCTAssertTrue(hub.edit(mine.message, text: "on my way!"))
        let ownEdit = await waitUntil { hub.rows.last?.text == "on my way!" }
        XCTAssertTrue(ownEdit)
        await hub.delete(mine.message)
        XCTAssertFalse(hub.rows.contains { $0.id == mine.id })
        XCTAssertTrue(hub.extras.isRemoved(mine.id))
        XCTAssertEqual(withdrawn, [mine.id])

        let ownReply = try XCTUnwrap(hub.threadRows("pat-1").first)
        hub.deleteReply(ownReply.message)
        let replyGone = await waitUntil { hub.threadRows("pat-1").isEmpty }
        XCTAssertTrue(replyGone)
        XCTAssertEqual(withdrawn, [mine.id, ownReply.id])

        try peerSends(room, [
            "type": "delete", "id": "d-1", "senderId": "pat-node", "sender": "pat", "conversation": .string(room.conversation),
            "target": "pat-1", "ts": .int(now),
        ])
        let theirsGone = await waitUntil { !hub.rows.contains { $0.id == "pat-1" } }
        XCTAssertTrue(theirsGone)
        XCTAssertTrue(hub.rows.allSatisfy { $0.reactions.isEmpty })
        let restored = try XCTUnwrap(ChatHistoryCodec.decode(ChatHistoryCodec.encode(hub.historySnapshot())))
        XCTAssertTrue(Set(restored.removedMessages).isSuperset(of: ["pat-1", mine.id]))
        await service.closeAll()
        await h.close()
    }

    @MainActor func testThreadRepliesFromEitherSideBuildTheDirectMessageThread() async throws {
        let ws = FakeWebSocketConnector()
        let service = base.makeService(api: base.preparedApi(), ws: ws)
        let (hub, h) = try await hub(service)
        let room = try await openRoom(service, ws: ws)
        hub.select(room.conversation)
        let now = Date().epochMillis
        try peerSends(room, chat("pat-1", "lunch?", conversation: room.conversation, ts: now - 60_000))
        let shown = await waitUntil { hub.rows.map(\.id) == ["pat-1"] }
        XCTAssertTrue(shown)
        XCTAssertEqual(hub.listPreview(room.conversation)?.text, "lunch?")

        try peerSends(room, [
            "type": "thread-reply", "id": "pat-r1", "senderId": "pat-node", "sender": "pat", "ts": .int(now - 50_000),
            "rootId": "pat-1", "channel": .string(room.conversation), "text": "or dinner",
        ])
        let threaded = await waitUntil { hub.threadRows("pat-1").map(\.id) == ["pat-r1"] }
        XCTAssertTrue(threaded)
        let theirs = try XCTUnwrap(hub.threadRows("pat-1").first)
        XCTAssertFalse(theirs.message.isSelf)
        XCTAssertFalse(theirs.deletable)
        XCTAssertNil(theirs.receipt)
        XCTAssertEqual(hub.rows.map(\.id), ["pat-1"], "a reply stays out of the conversation")
        XCTAssertEqual(hub.rows.first?.thread?.replyCount, 1)
        XCTAssertEqual(hub.listPreview(room.conversation)?.text, "lunch?")

        let own: JSONObject = [
            "type": "thread-reply", "id": "me-node-r0", "senderId": "me-node", "sender": "Me", "ts": .int(now - 40_000),
            "rootId": "pat-1", "channel": .string(room.conversation), "text": "from my laptop",
        ]
        var envelope = try room.crypto.encrypt(try room.peer.seal(own))
        envelope["historyReplay"] = true
        envelope["ref"] = .string(String(repeating: "c", count: 40))
        room.connection.receive(text: JSON.encode(envelope))
        let replayed = await waitUntil { hub.threadRows("pat-1").count == 2 }
        XCTAssertTrue(replayed)
        let laptop = try XCTUnwrap(hub.threadRows("pat-1").last)
        XCTAssertTrue(laptop.message.isSelf)
        XCTAssertTrue(laptop.deletable)
        XCTAssertEqual(laptop.receipt, .sent)

        let replied = await hub.replyInThread(channel: room.conversation, rootId: "pat-1", text: "noon then")
        XCTAssertTrue(replied)
        let mine = try XCTUnwrap(hub.threadRows("pat-1").last)
        XCTAssertEqual(mine.text, "noon then")
        XCTAssertEqual(mine.receipt, .sending)
        let ref = try XCTUnwrap(sealedRef(room, type: "thread-reply", id: mine.id))
        room.connection.receive(text: relayFrame(["kind": "forkmesh-recipients", "ref": .string(ref), "recipients": 1]))
        let acked = await waitUntil { hub.threadRows("pat-1").last?.receipt == .sent }
        XCTAssertTrue(acked)
        let notified = try XCTUnwrap(
            room.connection.sentTexts.map { JSON.parseObject($0) }.first {
                guard let framed = room.crypto.decrypt($0), let plain = room.peer.unseal(framed) else { return false }
                return plain["type"]?.text == "thread-reply" && plain["id"]?.text == mine.id
            })
        XCTAssertEqual(notified["notify"], .bool(true))
        XCTAssertEqual(hub.rows.first?.thread?.replyCount, 3)
        XCTAssertEqual(hub.listPreview(room.conversation)?.text, "lunch?")

        _ = await hub.deliver("see you", to: room.conversation)
        XCTAssertEqual(hub.listPreview(room.conversation)?.text, "You: see you")
        let sent = try XCTUnwrap(hub.rows.last)
        await hub.delete(sent.message)
        XCTAssertEqual(hub.listPreview(room.conversation)?.text, "lunch?")

        let restored = try XCTUnwrap(ChatHistoryCodec.decode(ChatHistoryCodec.encode(hub.historySnapshot())))
        XCTAssertEqual(Set(restored.replies.map(\.id)), ["pat-r1", "me-node-r0", mine.id])
        XCTAssertTrue(restored.replies.allSatisfy { $0.conversation == room.conversation })
        await service.closeAll()
        await h.close()
    }
}
