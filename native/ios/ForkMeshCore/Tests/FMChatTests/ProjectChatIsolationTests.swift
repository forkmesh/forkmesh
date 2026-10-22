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
        try await h.sendProtected(peerFrame(["channel": "#alpha-only", "text": "secret"], id: "a-1"))
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

        try await h.sendProtected(peerFrame(["channel": "#alpha-only", "text": "alpha secret"], id: "a-1"))
        let arrived = await waitUntil { hub.channels.contains("#alpha-only") }
        XCTAssertTrue(arrived)
        hub.extras.removeChannel("#alpha-hidden")

        await hub.leaveProject()
        await h.relay.disconnect()
        h.relay.resetForRelaySwitch()
        project = "b"
        hub.enterProject(workspace: ChatWorkspace())

        XCTAssertFalse(hub.channels.contains("#alpha-only"))
        XCTAssertTrue(hub.extras.hiddenChannels.isEmpty)
        XCTAssertTrue(h.relay.messages("#alpha-only").isEmpty)
        XCTAssertEqual(store.load(scope: "project-a")?.messages.map(\.id), ["a-1"])
        _ = await h.relay.sendMessage("beta note", to: "#general")
        await store.flush()
        let beta = store.load(scope: "project-b")?.messages ?? []
        XCTAssertFalse(beta.contains { $0.id == "a-1" || $0.conversation == "#alpha-only" })
        XCTAssertFalse(store.load(scope: "project-b")?.hiddenChannels.contains("#alpha-hidden") ?? false)

        await hub.leaveProject()
        h.relay.resetForRelaySwitch()
        project = "a"
        hub.enterProject(workspace: ChatWorkspace())
        XCTAssertTrue(hub.channels.contains("#alpha-only"))
        XCTAssertEqual(h.relay.messages("#alpha-only").map(\.id), ["a-1"])
        XCTAssertFalse(h.relay.messages("#general").contains { $0.text == "beta note" })
        await h.close()
    }

    func testHelloKeepsHiddenChannelsToThisPhone() async throws {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        let hub = ChatHub(relay: h.relay, mesh: mesh)
        h.relay.addChannel("#alpha-hidden")
        hub.extras.removeChannel("#alpha-hidden")

        try await h.sendProtected(peerFrame(["type": "hello", "channels": ["#peer-room", "#alpha-hidden", "#office/team"]], id: "h-1"))
        let peer = h.node("peer-1")
        let replied = await waitUntil {
            h.openFrames().contains { $0["type"]?.text == "hello" && $0["to"]?.text == peer }
        }
        XCTAssertTrue(replied)
        XCTAssertTrue(h.relay.channels.contains("#peer-room"))
        XCTAssertFalse(h.relay.channels.contains("#alpha-hidden"))
        XCTAssertFalse(h.relay.channels.contains("#office/team"))
        for hello in h.openFrames() where hello["type"]?.text == "hello" {
            let advertised = (hello["channels"]?.arrayValue ?? []).map(\.text)
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

    func testFramesForOnePersonAreSealedToThemOnly() async {
        let edit: JSONObject = ["type": "edit", "conversation": "@peer-1", "to": "peer-1", "text": "new words"]
        XCTAssertEqual(RelayService.directRecipient(edit), "peer-1")
        XCTAssertEqual(RelayService.directRecipient(["type": "typing", "to": "peer-1"]), "peer-1")
        XCTAssertEqual(RelayService.directRecipient(["type": "invite", "to": "peer-1", "channel": "#team-x"]), "peer-1")
        XCTAssertNil(RelayService.directRecipient(["type": "hello", "to": "peer-1"]))
        XCTAssertNil(RelayService.directRecipient(["type": "chat", "channel": "#general"]))
    }

    func testNoRelayPrivateChannelReachesThisPhoneNotEvenItsOwn() async throws {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        let hub = ChatHub(relay: h.relay, mesh: mesh)
        let pings = LocalPingLog(store: InMemoryKeyValueStore())
        hub.pings = pings
        try await h.sendProtected(
            peerFrame(["channel": "#their-secret", "private": true, "ownerUser": "mallory", "text": "hidden"], id: "p-1"))
        try await h.sendProtected(
            peerFrame(["channel": "#own-other-device", "private": true, "ownerUser": "Mobile Tester", "text": "mine"], id: "p-3"))
        try await h.sendProtected(peerFrame(["channel": "#general", "text": "public"], id: "p-4"))
        let arrived = await waitUntil { !h.relay.messages("#general").isEmpty }
        XCTAssertTrue(arrived)
        for channel in ["#their-secret", "#own-other-device"] {
            XCTAssertTrue(h.relay.messages(channel).isEmpty, channel)
            XCTAssertFalse(h.relay.channels.contains(channel), channel)
        }
        XCTAssertFalse(pings.pings.contains { $0.message.contains("hidden") || $0.message.contains("mine") })
        await h.close()
    }

    func testInboxPingsBelongToOneProject() async {
        XCTAssertTrue(ProjectKeyPolicy.projectPreferenceKeys.contains(LocalPingLog.key))
        let store = InMemoryKeyValueStore()
        let pings = LocalPingLog(store: store)
        _ = pings.recordChat(
            messageId: "m-1", sender: "Pat", title: "Pat in #alpha", message: "alpha news", conversation: "#alpha",
            tsMs: Date().epochMillis, read: false)
        XCTAssertEqual(pings.pings.count, 1)
        store.remove(LocalPingLog.key)
        pings.reload()
        XCTAssertTrue(pings.pings.isEmpty)
    }

    func testRemovingAProjectErasesItsChatHistory() async {
        let dir = temporaryDirectory("project-history-erase")
        defer { try? FileManager.default.removeItem(at: dir) }
        let store = ChatHistoryStore(directory: dir, delay: 0)
        let scope = ChatHistoryCodec.projectScope(projectId: "local-beta-1", room: "world-general")
        store.save(ChatHistorySnapshot(), scope: scope)
        XCTAssertTrue(FileManager.default.fileExists(atPath: store.file(scope: scope).path))
        store.eraseProject("local-beta-1", room: "world-general")
        XCTAssertFalse(FileManager.default.fileExists(atPath: store.file(scope: scope).path))
    }
}
