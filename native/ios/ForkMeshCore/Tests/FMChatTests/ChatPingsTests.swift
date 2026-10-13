import FMApi
import FMBase
import Foundation
import XCTest

@testable import FMChat

@MainActor
final class ChatPingsTests: XCTestCase {
    private func hub() async throws -> (ChatHub, RelayHarness, LocalPingLog, InMemoryKeyValueStore) {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        let store = InMemoryKeyValueStore()
        let log = LocalPingLog(store: store)
        let hub = ChatHub(relay: h.relay, mesh: mesh)
        hub.pings = log
        return (hub, h, log, store)
    }

    private func message(
        _ id: String, _ conversation: String, text: String = "hello", age: TimeInterval = 1, isSelf: Bool = false,
        fileName: String = ""
    ) -> ChatMessage {
        ChatMessage(
            id: id, conversation: conversation, senderId: "pat-node", senderName: "Pat", text: text,
            timestamp: Date().addingTimeInterval(-age), isSelf: isSelf, fileName: fileName)
    }

    func testARelayMessageFromSomeoneElseLandsInPings() async throws {
        let (hub, h, log, _) = try await hub()
        h.relay.switchConversation("#random")
        try h.sendPlain([
            "type": "chat", "id": "m-live", "senderId": "pat-node", "sender": "Pat", "channel": "#general",
            "text": "lunch?", "ts": .int(Date().epochMillis),
        ])
        let filed = await waitUntil(2) { !log.pings.isEmpty }
        XCTAssertTrue(filed)
        let item = try XCTUnwrap(log.items.first)
        XCTAssertEqual(item.id, "local:chat:m-live")
        XCTAssertEqual(item.kind, "chat")
        XCTAssertEqual(item.title, "Pat in #general")
        XCTAssertEqual(item.body, "lunch?")
        XCTAssertEqual(item.route, .channel(id: "#general"))
        XCTAssertEqual(item.kindLabel, "Chat")
        XCTAssertEqual(item.icon, "chat_bubble_outline")
        XCTAssertTrue(item.unread)
        XCTAssertNotNil(hub.pings)
        await h.close()
    }

    func testTitlesFollowTheDesktop() async throws {
        let (hub, h, log, _) = try await hub()
        h.relay.switchConversation("#random")
        hub.filePing(message("m1", "@pat-node", text: "  are you there?  ", age: 3))
        hub.filePing(message("m2", DirectMessageFrames.prefix + String(repeating: "d", count: 32), text: "", age: 2, fileName: "plan.pdf"))
        hub.filePing(message("m3", "#welcome", text: "hi all"))
        XCTAssertEqual(log.pings.map(\.title), ["New user joined", "Pat sent you a message", "Pat sent you a message"])
        XCTAssertEqual(log.pings.map(\.message), ["Pat said hello in #welcome", "\u{1F4CE} plan.pdf", "are you there?"])
        await h.close()
    }

    func testOwnStaleAndRepeatedMessagesAreSkipped() async throws {
        let (hub, h, log, _) = try await hub()
        hub.filePing(message("m1", "#random", isSelf: true))
        hub.filePing(message("m2", "#random", age: ChatHub.pingFreshness + 1))
        hub.filePing(message("", "#random"))
        hub.filePing(message("m3", "#random"))
        hub.filePing(message("m3", "#random"))
        XCTAssertEqual(log.pings.map(\.id), ["chat:m3"])
        await h.close()
    }

    func testTheOpenConversationFilesReadAndOpeningMarksRead() async throws {
        let (hub, h, log, store) = try await hub()
        h.relay.switchConversation("#general")
        hub.filePing(message("m1", "#general", age: 2))
        hub.filePing(message("m2", "#random"))
        XCTAssertEqual(log.items.map(\.unread), [true, false])
        hub.select("#random")
        XCTAssertEqual(log.items.map(\.unread), [false, false])
        let reloaded = LocalPingLog(store: store).pings
        XCTAssertEqual(reloaded.map(\.kind), ["chat", "chat"])
        XCTAssertEqual(reloaded.map(\.conversation), ["#random", "#general"])
        await h.close()
    }
}
