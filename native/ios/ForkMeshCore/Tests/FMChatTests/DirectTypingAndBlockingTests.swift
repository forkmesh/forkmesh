import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest

@testable import FMChat

extension DirectMessagesTests {
    @MainActor private func connectedHub() async throws
        -> (ChatHub, DirectMessageService, FakeWebSocketConnection, RoomCrypto, RoomSeal, RelayHarness)
    {
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws)
        await service.refresh()
        let harness = try await RelayHarness.start()
        let mesh = LocalMeshService(
            settings: harness.device.settings, identity: harness.device.identity, secureStore: harness.device.secure)
        let hub = ChatHub(relay: harness.relay, mesh: mesh)
        hub.direct = service
        let conversation = service.conversations[0]
        hub.select(conversation)
        let opened = await waitUntil { service.isConnected(conversation) }
        XCTAssertTrue(opened)
        let connection = try XCTUnwrap(ws.lastConnection)
        let crypto = await roomCrypto()
        let peer = try peerSeal("peer")
        connection.receive(text: JSON.encode(try crypto.encrypt(peer.keyFrame())))
        let taught = await teach(peer, from: connection, crypto)
        XCTAssertTrue(taught)
        return (hub, service, connection, crypto, peer, harness)
    }

    @MainActor private func typingFrame(_ conversation: String, sender: String, active: Bool, id: String) -> JSONObject {
        [
            "type": "typing", "id": .string(id), "senderId": .string("\(sender)-node"), "sender": .string(sender),
            "accountKind": "user", "conversation": .string(conversation), "active": .bool(active),
            "ts": .int(Date().epochMillis),
        ]
    }

    @MainActor func testDirectTypingTravelsSealedAndIsNeverStored() async throws {
        let (hub, service, connection, crypto, peer, harness) = try await connectedHub()
        let conversation = service.conversations[0]
        func typingEnvelopes() -> [(JSONObject, JSONObject)] {
            connection.sentTexts.map { JSON.parseObject($0) }.compactMap { envelope in
                guard let sealed = crypto.decrypt(envelope), RoomSeal.isSealedFrame(sealed), let plain = peer.unseal(sealed),
                    plain["type"]?.text == "typing"
                else { return nil }
                return (envelope, plain)
            }
        }
        hub.composerChanged("h")
        let sent = await waitUntil { typingEnvelopes().count == 1 }
        XCTAssertTrue(sent)
        let (envelope, plain) = try XCTUnwrap(typingEnvelopes().first)
        XCTAssertNil(envelope["persist"])
        XCTAssertNil(envelope["notify"])
        XCTAssertEqual(plain["active"], .bool(true))
        XCTAssertEqual(plain["conversation"]?.text, conversation)
        hub.composerChanged("he")
        try await Task.sleep(nanoseconds: 100_000_000)
        XCTAssertEqual(typingEnvelopes().count, 1)
        hub.composerChanged("")
        let stopped = await waitUntil { typingEnvelopes().last?.1["active"] == .bool(false) }
        XCTAssertTrue(stopped)
        await service.closeAll()
        await harness.close()
    }

    @MainActor func testTheOtherPersonTypingShowsAndClearsWhenTheirMessageArrives() async throws {
        let (hub, service, connection, crypto, peer, harness) = try await connectedHub()
        let conversation = service.conversations[0]
        connection.receive(
            text: JSON.encode(try crypto.encrypt(try peer.seal(typingFrame(conversation, sender: "Me", active: true, id: "t0")))))
        connection.receive(
            text: JSON.encode(try crypto.encrypt(try peer.seal(typingFrame(conversation, sender: "pat", active: true, id: "t1")))))
        let typing = await waitUntil { hub.conversationTyping == ["pat"] }
        XCTAssertTrue(typing)
        XCTAssertEqual(hub.typingLine, "pat is typing...")
        XCTAssertTrue(service.messages(conversation).isEmpty)
        let chat: JSONObject = [
            "type": "chat", "id": "pat-1", "senderId": "pat-node", "sender": "pat", "accountKind": "user",
            "channel": .string(conversation), "text": "hi", "ts": .int(Date().epochMillis),
        ]
        connection.receive(text: JSON.encode(try crypto.encrypt(try peer.seal(chat))))
        let cleared = await waitUntil { hub.conversationTyping.isEmpty }
        XCTAssertTrue(cleared)
        await service.closeAll()
        await harness.close()
    }

    @MainActor func testDeletingADirectConversationRemovesItUntilANewMessageArrives() async throws {
        let (hub, service, connection, crypto, peer, harness) = try await connectedHub()
        let conversation = service.conversations[0]
        let old: JSONObject = [
            "type": "chat", "id": "pat-1", "senderId": "pat-node", "sender": "pat", "accountKind": "user",
            "channel": .string(conversation), "text": "old", "ts": .int(Date().epochMillis - 60_000),
        ]
        connection.receive(text: JSON.encode(try crypto.encrypt(try peer.seal(old))))
        let got = await waitUntil { service.messages(conversation).count == 1 }
        XCTAssertTrue(got)
        XCTAssertEqual(hub.directRows.map(\.conversation), [conversation])
        hub.deleteConversation(conversation)
        XCTAssertTrue(hub.directRows.isEmpty)
        XCTAssertNotEqual(hub.currentConversation, conversation)
        XCTAssertEqual(service.record(for: conversation)?.unreadCount, 0)
        let fresh: JSONObject = [
            "type": "chat", "id": "pat-2", "senderId": "pat-node", "sender": "pat", "accountKind": "user",
            "channel": .string(conversation), "text": "new", "ts": .int(Date().epochMillis + 1_000),
        ]
        connection.receive(text: JSON.encode(try crypto.encrypt(try peer.seal(fresh))))
        let back = await waitUntil { hub.directRows.map(\.conversation) == [conversation] }
        XCTAssertTrue(back)
        hub.select(conversation)
        XCTAssertEqual(hub.rows.map(\.message.text), ["new"])
        await service.closeAll()
        await harness.close()
    }

    @MainActor func testBlockingSomeoneHidesTheirConversationTypingAndMessages() async throws {
        let (hub, service, connection, crypto, peer, harness) = try await connectedHub()
        let conversation = service.conversations[0]
        hub.setBlocked("Pat", true)
        XCTAssertTrue(hub.isBlocked("pat"))
        XCTAssertTrue(hub.directRows.isEmpty)
        XCTAssertNotEqual(hub.currentConversation, conversation)
        hub.select(conversation)
        connection.receive(
            text: JSON.encode(try crypto.encrypt(try peer.seal(typingFrame(conversation, sender: "pat", active: true, id: "t1")))))
        let chat: JSONObject = [
            "type": "chat", "id": "pat-1", "senderId": "pat-node", "sender": "pat", "accountKind": "user",
            "channel": .string(conversation), "text": "let me in", "ts": .int(Date().epochMillis),
        ]
        connection.receive(text: JSON.encode(try crypto.encrypt(try peer.seal(chat))))
        let arrived = await waitUntil { service.messages(conversation).count == 1 }
        XCTAssertTrue(arrived)
        XCTAssertTrue(hub.rows.isEmpty)
        XCTAssertTrue(hub.conversationTyping.isEmpty)
        hub.setBlocked("pat", false)
        XCTAssertEqual(hub.rows.map(\.message.text), ["let me in"])
        XCTAssertEqual(hub.directRows.map(\.conversation), [conversation])
        await service.closeAll()
        await harness.close()
    }
}

final class ChatWorkspaceBlockingTests: XCTestCase {
    @MainActor func testBlocksAndRemovedConversationsPersist() async {
        let store = InMemoryKeyValueStore()
        let workspace = ChatWorkspace(store: store, scope: "relay-1")
        workspace.setBlocked("@Pat ", true)
        let removedAt = Date(timeIntervalSince1970: 1_800_000_000)
        workspace.removeConversation("@peer-1", at: removedAt)
        let reloaded = ChatWorkspace(store: store, scope: "relay-1")
        XCTAssertTrue(reloaded.isBlocked("pat"))
        XCTAssertEqual(reloaded.blocked, ["pat"])
        XCTAssertTrue(reloaded.isRemoved("@peer-1", lastActivity: nil))
        XCTAssertTrue(reloaded.isRemoved("@peer-1", lastActivity: removedAt))
        XCTAssertFalse(reloaded.isRemoved("@peer-1", lastActivity: removedAt.addingTimeInterval(1)))
        reloaded.restoreConversation("@peer-1")
        reloaded.setBlocked("PAT", false)
        let again = ChatWorkspace(store: store, scope: "relay-1")
        XCTAssertFalse(again.isBlocked("pat"))
        XCTAssertFalse(again.isRemoved("@peer-1", lastActivity: nil))
    }

    @MainActor func testASignedInBrowserWithoutAnOwnerFieldJoinsTheAccountRow() async {
        let groups = RelayService.groups([
            Member(id: "phone", name: "jett", isSelf: true, online: true, owner: "jett"),
            Member(id: "browser", name: "Jett", online: true),
            Member(id: "guest", name: "World Guest ce483d", online: true),
        ])
        XCTAssertEqual(groups.map(\.name), ["jett", "World Guest ce483d"])
        XCTAssertEqual(groups.first?.members.map(\.id), ["phone", "browser"])
        XCTAssertEqual(groups.first?.isSelf, true)
    }
}
