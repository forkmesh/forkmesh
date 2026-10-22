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

    func testHelloKeepsPrivateAndHiddenChannelsToThisPhone() async throws {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        let hub = ChatHub(relay: h.relay, mesh: mesh)
        let secret = try XCTUnwrap(hub.extras.createPrivateChannel("alpha-private"))
        h.relay.addChannel("#alpha-hidden")
        hub.extras.removeChannel("#alpha-hidden")
        XCTAssertTrue(h.relay.channels.contains(secret))

        try await h.sendProtected(peerFrame(["type": "hello", "channels": ["#peer-room", "#alpha-hidden", "#office/team"]], id: "h-1"))
        let replied = await waitUntil {
            h.openFrames().contains { $0["type"]?.text == "hello" && $0["to"]?.text == "peer-1" }
        }
        XCTAssertTrue(replied)
        XCTAssertTrue(h.relay.channels.contains("#peer-room"))
        XCTAssertFalse(h.relay.channels.contains("#alpha-hidden"))
        XCTAssertFalse(h.relay.channels.contains("#office/team"))
        for hello in h.openFrames() where hello["type"]?.text == "hello" {
            let advertised = (hello["channels"]?.arrayValue ?? []).map(\.text)
            XCTAssertFalse(advertised.contains(secret), "a private channel name never leaves the phone")
            XCTAssertFalse(advertised.contains("#alpha-hidden"))
        }
        await h.close()
    }

    func testPrivateRoomsAndDirectMessagesStayWithTheirProject() async throws {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        let hub = ChatHub(relay: h.relay, mesh: mesh)
        let api = FakeAccountApi()
        let office = OfficeChannelService(
            api: api, connector: FakeWebSocketConnector(), secureStore: InMemorySecureStore(),
            origin: { URL(string: "https://app.forkmesh.com") }, selfId: { "me-node" }, displayName: { "Me" })
        let direct = DirectMessageService(
            api: api, connector: FakeWebSocketConnector(), secureStore: InMemorySecureStore(),
            origin: { URL(string: "https://app.forkmesh.com") }, selfId: { "me-node" }, displayName: { "Me" })
        hub.office = office
        hub.direct = direct
        let dir = temporaryDirectory("project-rooms-isolation")
        defer { try? FileManager.default.removeItem(at: dir) }
        let store = ChatHistoryStore(directory: dir, delay: 0)
        var project = "a"
        hub.attachHistory(store, scope: { "project-\(project)" })

        let alphaRoom = String(repeating: "a", count: 32)
        let alphaDirect = String(repeating: "1", count: 32)
        api.replies["GET /api/chat/channels"] = [
            "channels": [["id": .string(alphaRoom), "name": "alpha-team", "visibility": "private", "canManage": true]]
        ]
        api.replies["GET /api/chat/direct-messages"] = [
            "conversations": [["id": .string(alphaDirect), "otherUser": "pat", "unreadCount": 0]]
        ]
        await office.refresh()
        await direct.refresh()
        XCTAssertEqual(hub.officeChannels, ["#office/alpha-team"])
        XCTAssertEqual(direct.records.map(\.otherUser), ["pat"])

        await hub.leaveProject()
        await h.relay.disconnect()
        h.relay.resetForRelaySwitch()
        XCTAssertTrue(hub.officeChannels.isEmpty)
        XCTAssertTrue(direct.records.isEmpty)

        project = "b"
        let betaRoom = String(repeating: "b", count: 32)
        api.replies["GET /api/chat/channels"] = [
            "channels": [["id": .string(betaRoom), "name": "beta-team", "visibility": "private", "canManage": true]]
        ]
        api.replies["GET /api/chat/direct-messages"] = ["conversations": []]
        hub.enterProject(workspace: ChatWorkspace())
        await office.refresh()
        await direct.refresh()
        XCTAssertEqual(hub.officeChannels, ["#office/beta-team"])
        XCTAssertTrue(direct.records.isEmpty)
        XCTAssertFalse(hub.listedChannels.contains("#office/alpha-team"))
        await h.close()
    }
}
