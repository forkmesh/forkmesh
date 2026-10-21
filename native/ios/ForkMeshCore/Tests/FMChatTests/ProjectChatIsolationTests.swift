import FMAccount
import FMApi
import FMBase
import FMNet
import Foundation
import XCTest

@testable import FMChat

@MainActor
final class ProjectChatIsolationTests: XCTestCase {
    private func peerFrame(_ fields: JSONObject, id: String) -> JSONObject {
        var frame: JSONObject = [
            "type": "chat", "id": .string(id), "senderId": "peer-1", "sender": "Pat", "ts": .int(Date().epochMillis),
        ]
        for (key, value) in fields { frame[key] = value }
        return frame
    }

    func testARelaySwitchForgetsTheChannelsOfTheRelayBeingLeft() async throws {
        let h = try await RelayHarness.start()
        try h.sendPlain(peerFrame(["channel": "#alpha-only", "text": "secret"], id: "a-1"))
        let arrived = await waitUntil { h.relay.channels.contains("#alpha-only") }
        XCTAssertTrue(arrived)
        h.relay.switchConversation("#alpha-only")

        await h.relay.disconnect()
        h.relay.resetForRelaySwitch()
        XCTAssertEqual(h.relay.channels, ["#general"])
        XCTAssertEqual(h.relay.currentConversation, "#general")
        XCTAssertTrue(h.relay.unreadConversations.isEmpty)
        XCTAssertTrue(h.relay.messages("#alpha-only").isEmpty)
        await h.close()
    }

    func testLeavingAProjectKeepsItsChatOutOfTheNextOne() async throws {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        let hub = ChatHub(relay: h.relay, mesh: mesh)
        let dir = temporaryDirectory("project-chat-isolation")
        defer { try? FileManager.default.removeItem(at: dir) }
        let store = ChatHistoryStore(directory: dir, delay: 0)
        var project = "a"
        hub.attachHistory(store, scope: { "project-\(project)" })

        try h.sendPlain(peerFrame(["channel": "#alpha-only", "text": "alpha secret"], id: "a-1"))
        let arrived = await waitUntil { hub.channels.contains("#alpha-only") }
        XCTAssertTrue(arrived)
        let secret = try XCTUnwrap(hub.extras.createPrivateChannel("alpha-private"))
        XCTAssertTrue(hub.channels.contains(secret))
        hub.extras.removeChannel("#alpha-hidden")

        await hub.leaveProject()
        await h.relay.disconnect()
        h.relay.resetForRelaySwitch()
        project = "b"
        hub.enterProject(workspace: ChatWorkspace())

        XCTAssertFalse(hub.channels.contains("#alpha-only"))
        XCTAssertFalse(hub.channels.contains(secret))
        XCTAssertTrue(hub.extras.hiddenChannels.isEmpty)
        XCTAssertTrue(hub.extras.privateChannels.isEmpty)
        XCTAssertTrue(h.relay.messages("#alpha-only").isEmpty)
        XCTAssertEqual(store.load(scope: "project-a")?.messages.map(\.id), ["a-1"])
        _ = await h.relay.sendMessage("beta note", to: "#general")
        await store.flush()
        let beta = store.load(scope: "project-b")?.messages ?? []
        XCTAssertFalse(beta.contains { $0.id == "a-1" || $0.conversation == "#alpha-only" })
        XCTAssertFalse(store.load(scope: "project-b")?.hiddenChannels.contains("#alpha-hidden") ?? false)
        XCTAssertFalse(store.load(scope: "project-b")?.privateChannels.contains(secret) ?? false)

        await hub.leaveProject()
        h.relay.resetForRelaySwitch()
        project = "a"
        hub.enterProject(workspace: ChatWorkspace())
        XCTAssertTrue(hub.channels.contains("#alpha-only"))
        XCTAssertTrue(hub.channels.contains(secret))
        XCTAssertEqual(h.relay.messages("#alpha-only").map(\.id), ["a-1"])
        XCTAssertFalse(h.relay.messages("#general").contains { $0.text == "beta note" })
        await h.close()
    }
}
