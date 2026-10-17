import FMApi
import FMBase
import Foundation
import XCTest

@testable import FMChat

@MainActor
final class ChatBubbleLifeTests: XCTestCase {
    private func message(_ conversation: String, isSelf: Bool, at: Date) -> ChatMessage {
        ChatMessage(id: "m1", conversation: conversation, senderId: "pat-node", senderName: "Pat", text: "hi", timestamp: at, isSelf: isSelf)
    }

    func testExpiryFollowsTheConversationRetention() async throws {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        let hub = ChatHub(relay: h.relay, mesh: mesh)
        let direct = DirectMessageFrames.prefix + String(repeating: "d", count: 32)
        hub.applyRetention(ChatRetention(defaultMs: 86_400_000, chats: [direct: 3_600_000]))
        let sent = Date(timeIntervalSince1970: 1_000)
        XCTAssertEqual(hub.lifetime("#general"), 86_400)
        XCTAssertEqual(hub.lifetime(direct), 3_600)
        XCTAssertEqual(hub.expiresAt(message(direct, isSelf: true, at: sent)), sent.addingTimeInterval(3_600))
        await h.close()
    }

    func testOnlyOwnStoredDirectMessagesAreOnTheServer() async throws {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        let hub = ChatHub(relay: h.relay, mesh: mesh)
        let direct = DirectMessageFrames.prefix + String(repeating: "d", count: 32)
        let now = Date()
        XCTAssertTrue(hub.isOnServer(message(direct, isSelf: true, at: now), receipt: .sent))
        XCTAssertFalse(hub.isOnServer(message(direct, isSelf: true, at: now), receipt: .delivered))
        XCTAssertFalse(hub.isOnServer(message(direct, isSelf: true, at: now), receipt: .sending))
        XCTAssertFalse(hub.isOnServer(message(direct, isSelf: false, at: now), receipt: .sent))
        XCTAssertFalse(hub.isOnServer(message("#general", isSelf: true, at: now), receipt: .sent))
        await h.close()
    }
}
