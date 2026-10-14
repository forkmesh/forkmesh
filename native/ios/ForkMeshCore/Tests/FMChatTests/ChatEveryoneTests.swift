import FMApi
import FMBase
import FMNet
import Foundation
import XCTest

@testable import FMChat

@MainActor
final class ChatEveryoneTests: XCTestCase {
    static let general = String(repeating: "a", count: 32)
    static let random = String(repeating: "b", count: 32)
    static let welcome = String(repeating: "c", count: 32)
    static let ops = String(repeating: "d", count: 32)

    private func hub(store: KeyValueStore = InMemoryKeyValueStore()) async throws -> (ChatHub, RelayHarness, FakeAccountApi, OfficeChannelService) {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        let hub = ChatHub(relay: h.relay, mesh: mesh, isGuest: { false })
        hub.workspace = ChatWorkspace(store: store, scope: "next")
        let api = FakeAccountApi()
        api.replies["GET /api/chat/channels"] = [
            "channels": [
                ["id": .string(Self.ops), "name": "ops", "visibility": "public"],
                ["id": .string(Self.welcome), "name": "welcome", "visibility": "public", "kind": "everyone", "memberCount": 4],
                ["id": .string(Self.general), "name": "general", "visibility": "public", "kind": "everyone", "memberCount": 4],
                ["id": .string(Self.random), "name": "random", "visibility": "public", "kind": "everyone", "memberCount": 4],
            ],
        ]
        api.replies["GET /api/chat/channels/\(Self.general)/members"] = [
            "everyone": true,
            "members": [["username": "alice"], ["username": "bob"], ["username": "pat"], ["username": "Alice"]],
        ]
        let office = OfficeChannelService(
            api: api, connector: FakeWebSocketConnector(), secureStore: InMemorySecureStore(),
            origin: { URL(string: "https://app.forkmesh.com") }, selfId: { "me-node" }, displayName: { "Me" })
        hub.office = office
        await office.refresh()
        return (hub, h, api, office)
    }

    func testServerChannelsAreListedFirstAndTheSharedRoomIsWorld() async throws {
        let (hub, h, _, office) = try await hub()
        XCTAssertEqual(office.rooms.filter(\.everyone).map(\.name), ["welcome", "general", "random"])
        XCTAssertEqual(hub.listedChannels, ["#office/general", "#office/random", "#office/welcome", "#general", "#office/ops"])
        XCTAssertEqual(hub.conversationLabel("#office/general"), "#general")
        XCTAssertEqual(hub.conversationLabel("#general"), "World")
        XCTAssertEqual(hub.generalConversation, "#office/general")
        XCTAssertTrue(hub.isEveryoneChannel("#office/welcome"))
        XCTAssertFalse(hub.isEveryoneChannel("#office/ops"))
        XCTAssertFalse(hub.isEveryoneChannel("#general"))
        XCTAssertEqual(
            try ChatReplyTarget.resolve("general", channels: ["#general"], roster: [], offices: hub.officeChannels),
            "#office/general", "a reply to general goes to the server channel, not World")
        try h.sendPlain([
            "type": "hello", "id": "h1", "senderId": "old-desktop", "sender": "Old", "ts": 1000,
            "channels": ["#general", "#random", "#welcome"],
        ])
        _ = await waitUntil { h.relay.channels.contains("#random") }
        XCTAssertEqual(hub.channels, ["#general"], "an older client's relay labels stay out of the list")
        await h.close()
    }

    func testEveryMemberIsInTheServerChannels() async throws {
        let (hub, h, api, _) = try await hub()
        XCTAssertEqual(hub.memberCount("#office/general"), 4, "the server's count stands in until the list loads")
        try await h.sendProtected([
            "type": "presence", "id": "p1", "senderId": "peer-1", "sender": "Pat", "ownerUser": "pat", "ts": .int(Date().epochMillis),
        ])
        _ = await waitUntil { h.relay.roster().contains { $0.id == "peer-1" } }
        await hub.refreshMemberDirectory()
        XCTAssertEqual(hub.directoryMembers, ["alice", "bob", "pat"])
        for conversation in ["#office/general", "#office/random", "#office/welcome"] {
            let others = hub.channelMembers(conversation).filter { !$0.isSelf }
            XCTAssertEqual(Set(others.map { $0.account.lowercased() }), ["alice", "bob", "pat"], conversation)
            XCTAssertEqual(others.first { $0.account == "pat" }?.online, true)
            XCTAssertEqual(others.first { $0.account == "bob" }?.online, false)
        }
        XCTAssertFalse(hub.channelMembers("#office/ops").contains { $0.account == "alice" })
        await hub.refreshMemberDirectory()
        XCTAssertEqual(api.calls.filter { $0.1.hasSuffix("/members") }.count, 1, "not re-read within five minutes")
        await h.close()
    }

    func testThePhoneMovesToServerGeneralOnce() async throws {
        let store = InMemoryKeyValueStore()
        let (hub, h, _, _) = try await hub(store: store)
        XCTAssertEqual(hub.currentConversation, "#general")
        hub.adoptEveryoneGeneral()
        XCTAssertEqual(hub.currentConversation, "#office/general")
        hub.select("#general")
        hub.adoptEveryoneGeneral()
        XCTAssertEqual(hub.currentConversation, "#general", "World stays open once the member chose it")
        XCTAssertTrue(ChatWorkspace(store: store, scope: "next").movedToEveryone)
        await h.close()
    }

    func testTheServerWelcomeIsRecognised() {
        let welcome = ChatMessage(
            id: "w1", conversation: "#office/welcome", senderId: "system:welcome", senderName: "ForkMesh",
            text: "\u{1F389} Welcome to ForkMesh, @newbie! Everyone, come say hi \u{1F44B}", timestamp: Date())
        XCTAssertTrue(ChatEveryone.isWelcome(welcome))
        XCTAssertEqual(ChatEveryone.welcomeConversation, "#office/welcome")
        XCTAssertFalse(ChatEveryone.isWelcome(ChatMessage(
            id: "m1", conversation: "#office/welcome", senderId: "bob", senderName: "bob", text: "hi", timestamp: Date())))
    }
}
