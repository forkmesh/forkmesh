import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest

@testable import FMChat

extension DirectMessageReceiptsTests {
    @MainActor func testDirectHistoryAndDeliveredMarksSurviveARestart() async throws {
        let ws = FakeWebSocketConnector()
        let service = base.makeService(api: base.preparedApi(), ws: ws)
        let (hub, h) = try await hub(service)
        let room = try await openRoom(service, ws: ws)
        let now = Date().epochMillis
        try peerSends(room, chat("pat-1", "lunch?", conversation: room.conversation, ts: now - 60_000))
        let shown = await waitUntil { service.messages(room.conversation).count == 1 }
        XCTAssertTrue(shown)
        _ = await hub.deliver("noon works", to: room.conversation)
        let mine = try XCTUnwrap(service.messages(room.conversation).last)
        let ref = try XCTUnwrap(sealedRef(room, type: "chat", id: mine.id))
        room.connection.receive(text: relayFrame(["kind": "forkmesh-delivered", "v": 1, "refs": [.string(ref)]]))
        let delivered = await waitUntil { service.receipt(mine.id) == .delivered }
        XCTAssertTrue(delivered)

        let saved = try XCTUnwrap(ChatHistoryCodec.decode(ChatHistoryCodec.encode(hub.historySnapshot())))
        XCTAssertEqual(saved.messages.filter { $0.conversation == room.conversation }.map(\.id), ["pat-1", mine.id])
        XCTAssertEqual(saved.receipts, [mine.id: .delivered])

        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        let store = ChatHistoryStore(directory: directory)
        store.save(saved, scope: "restart")
        let restarted = base.makeService(api: base.preparedApi(), ws: FakeWebSocketConnector())
        let (fresh, h2) = try await self.hub(restarted)
        fresh.attachHistory(store) { "restart" }
        XCTAssertEqual(restarted.messages(room.conversation).map(\.id), ["pat-1", mine.id])
        XCTAssertEqual(restarted.receipt(mine.id), .delivered)
        XCTAssertTrue(fresh.relay.messages(room.conversation).isEmpty, "direct messages stay out of the relay history")
        store.erase(scope: "restart")
        await service.closeAll()
        await h.close()
        await h2.close()
    }

    @MainActor func testAReadReceiptMarksRestoredOwnMessagesRead() async throws {
        let ws = FakeWebSocketConnector()
        let service = base.makeService(api: base.preparedApi(), ws: ws)
        let conversation = "\(DirectMessageFrames.prefix)\(DirectMessagesTests.conversationId)"
        let earlier = ChatMessage(
            id: "me-node-1-1", conversation: conversation, senderId: "me-node", senderName: "Me", text: "hi",
            timestamp: Date(), isSelf: true)
        service.restore([earlier], receipts: [:])
        let room = try await openRoom(service, ws: ws)
        XCTAssertNil(service.receipt(earlier.id))
        room.connection.receive(text: relayFrame(["kind": "forkmesh-read", "v": 1]))
        let read = await waitUntil { service.receipt(earlier.id) == .read }
        XCTAssertTrue(read)
        await service.closeAll()
    }

    @MainActor func testADeliveredMessageIsNotResentToANewDevice() async throws {
        let room = try await openRoom()
        _ = await room.service.send(room.conversation, text: "first")
        let first = try XCTUnwrap(room.service.messages(room.conversation).last)
        _ = await room.service.send(room.conversation, text: "second")
        let second = try XCTUnwrap(room.service.messages(room.conversation).last)
        let firstRef = try XCTUnwrap(sealedRef(room, type: "chat", id: first.id))
        room.connection.receive(text: relayFrame(["kind": "forkmesh-delivered", "v": 1, "refs": [.string(firstRef)]]))
        let delivered = await waitUntil { room.service.receipt(first.id) == .delivered }
        XCTAssertTrue(delivered)

        let laptop = try base.peerSeal("laptop", node: "pat-laptop")
        room.connection.receive(text: JSON.encode(try room.crypto.encrypt(laptop.keyFrame())))
        let pending = await waitUntil { room.service.pendingDevices(room.conversation).map(\.kid) == [laptop.keyId] }
        XCTAssertTrue(pending, "pat's new laptop waits for approval")
        XCTAssertTrue(room.connection.sentJSON.filter { $0["resend"] == .bool(true) }.isEmpty)
        await room.service.approveDevice(room.conversation, kid: laptop.keyId)
        let taught = await base.teach(laptop, from: room.connection, room.crypto)
        XCTAssertTrue(taught)
        let resent: () -> [String] = {
            room.connection.sentJSON.filter { $0["resend"] == .bool(true) }.compactMap { envelope in
                guard let framed = room.crypto.decrypt(envelope), let plain = laptop.unseal(framed) else { return nil }
                return plain["id"]?.text
            }
        }
        let resealed = await waitUntil { !resent().isEmpty }
        XCTAssertTrue(resealed)
        XCTAssertEqual(resent(), [second.id])
        await room.service.closeAll()
    }

    @MainActor func testAReplyCarriesItsQuoteToThePeerAndIntoHistory() async throws {
        let ws = FakeWebSocketConnector()
        let service = base.makeService(api: base.preparedApi(), ws: ws)
        let (hub, h) = try await hub(service)
        let room = try await openRoom(service, ws: ws)
        hub.select(room.conversation)
        let now = Date().epochMillis
        try peerSends(room, chat("pat-1", "lunch at **noon**?", conversation: room.conversation, ts: now - 60_000))
        let shown = await waitUntil { hub.rows.map(\.id) == ["pat-1"] }
        XCTAssertTrue(shown)
        let theirs = try XCTUnwrap(hub.rows.first)

        hub.startReply(theirs.message, text: theirs.text)
        let quote = ChatReplyQuote(id: "pat-1", senderId: "pat-node", senderName: "pat", text: "lunch at noon?")
        XCTAssertEqual(hub.reply(in: room.conversation), quote)
        XCTAssertNil(hub.reply(in: "#general"))
        let sent = await hub.deliver("yes!", to: room.conversation)
        XCTAssertTrue(sent)
        XCTAssertNil(hub.replyingTo)
        let mine = try XCTUnwrap(service.messages(room.conversation).last)
        XCTAssertEqual(mine.replyTo, quote)
        let wire = try XCTUnwrap(peerFrames(room).first { $0["id"]?.text == mine.id })
        XCTAssertEqual(DirectMessageFrames.replyQuote(wire[DirectMessageFrames.replyField]), quote)
        XCTAssertEqual(hub.replyAuthor(quote, in: room.conversation), "pat")

        var answer = chat("pat-2", "great", conversation: room.conversation, ts: now)
        answer[DirectMessageFrames.replyField] = DirectMessageFrames.replyFields(
            ChatReplyQuote(id: mine.id, senderId: "me-node", senderName: "Me", text: "yes!"))
        try peerSends(room, answer)
        let quoted = await waitUntil {
            service.messages(room.conversation).first { $0.id == "pat-2" }?.replyTo?.id == mine.id
        }
        XCTAssertTrue(quoted)
        let back = try XCTUnwrap(service.messages(room.conversation).first { $0.id == "pat-2" }?.replyTo)
        XCTAssertEqual(hub.replyAuthor(back, in: room.conversation), "You")

        let saved = try XCTUnwrap(ChatHistoryCodec.decode(ChatHistoryCodec.encode(hub.historySnapshot())))
        XCTAssertEqual(saved.messages.first { $0.id == "pat-2" }?.replyTo?.id, mine.id)
        XCTAssertEqual(saved.messages.first { $0.id == mine.id }?.replyTo, quote)
        XCTAssertNil(saved.messages.first { $0.id == "pat-1" }?.replyTo)

        hub.startReply(theirs.message, text: theirs.text)
        hub.select(hub.generalConversation)
        XCTAssertNil(hub.replyingTo)
        await service.closeAll()
        await h.close()
    }

    @MainActor func testRecentConversationsPutTheNewestChatFirst() async throws {
        let ws = FakeWebSocketConnector()
        let service = base.makeService(api: base.preparedApi(), ws: ws)
        let (hub, h) = try await hub(service)
        let room = try await openRoom(service, ws: ws)
        try peerSends(room, chat("pat-1", "hello", conversation: room.conversation, ts: Date().epochMillis + 60_000))
        let shown = await waitUntil { service.messages(room.conversation).count == 1 }
        XCTAssertTrue(shown)
        XCTAssertEqual(hub.recentConversations(limit: 5).first, room.conversation)
        XCTAssertLessThanOrEqual(hub.recentConversations(limit: 5).count, 5)
        XCTAssertEqual(hub.recentConversations(limit: 0), [])
        XCTAssertEqual(Set(hub.recentConversations(limit: 50)).count, hub.recentConversations(limit: 50).count)
        await service.closeAll()
        await h.close()
    }
}
