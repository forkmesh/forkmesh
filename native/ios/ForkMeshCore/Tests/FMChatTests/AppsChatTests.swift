import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest

@testable import FMChat

@MainActor
final class AppsChatTests: XCTestCase {
    static let roomId = String(repeating: "d", count: 32)

    private func hub() async throws -> (ChatHub, RelayHarness, FakeAccountApi) {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        let hub = ChatHub(relay: h.relay, mesh: mesh)
        let api = FakeAccountApi()
        api.replies["GET /api/apps/commands"] = [
            "ok": true,
            "commands": [["command": "hello", "description": "Say hi", "app": ["name": "Hello App", "slug": "hello-app"]]],
            "mentionable": ["hello-app"],
        ]
        hub.apps = AppsClient { method, path, body in try await api.accountApiCall(method, path, body) }
        return (hub, h, api)
    }

    private func apiCalls(_ api: FakeAccountApi, _ path: String) -> [JSONObject?] {
        api.calls.filter { $0.1 == path }.map(\.2)
    }

    func testAppCommandsRunOffTheRecordAndRepliesStayOnThisPhone() async throws {
        let (hub, h, api) = try await hub()
        api.replies["POST /api/apps/commands/run"] = [
            "ok": true, "responseType": "ephemeral", "text": "Hi there",
            "app": ["name": "Hello App", "botName": "Hello App (app)", "slug": "hello-app"],
            "blocks": [["type": "actions", "elements": [["type": "button", "action_id": "wave", "text": "Wave", "value": "w"]]]],
            "appMessageId": .string(String(repeating: "e", count: 32)),
        ]
        let sent = await hub.sendChatText("/hello world")
        XCTAssertTrue(sent)
        XCTAssertEqual(apiCalls(api, "/api/apps/commands/run").first??["text"]?.text, "/hello world")
        XCTAssertEqual(apiCalls(api, "/api/apps/commands/run").first??["channelId"]?.text, "")
        XCTAssertTrue(h.openFrames().filter { $0["type"]?.text == "chat" }.isEmpty)
        let row = try XCTUnwrap(hub.rows.last)
        XCTAssertEqual(row.message.senderName, "Hello App (app)")
        XCTAssertEqual(row.text, "Hi there")
        XCTAssertEqual(row.message.app?.ephemeral, true)
        XCTAssertFalse(row.deletable)
        XCTAssertFalse(row.editable)
        XCTAssertEqual(hub.appSuggestions("/he").map(\.command), ["hello"])
        XCTAssertEqual(
            hub.slashCommands("/").map(\.command),
            ["/msg", "/dm", "/search", "/saved", "/mute", "/shrug", "/me", "/issue", "/hello", "/apps"])

        api.replies["POST /api/apps/interact"] = ["ok": true, "responseType": "ephemeral", "text": "Waved"]
        await hub.interact(row.message, actionId: "wave", value: "w")
        let interaction = try XCTUnwrap(apiCalls(api, "/api/apps/interact").first ?? nil)
        XCTAssertEqual(interaction["appMessageId"]?.text, String(repeating: "e", count: 32))
        XCTAssertEqual(interaction["actionId"]?.text, "wave")
        XCTAssertEqual(hub.rows.last?.text, "Waved")

        let listed = await hub.sendChatText("/apps")
        XCTAssertTrue(listed)
        XCTAssertEqual(hub.rows.last?.text, "Your app commands: /hello (Hello App)")

        api.failures["POST /api/apps/commands/run"] = OrgApiException("app_unavailable", "app unavailable")
        _ = await hub.sendChatText("/hello again")
        XCTAssertEqual(hub.notice?.text, "The app did not answer.")
        XCTAssertEqual(apiCalls(api, "/api/apps/commands").count, 1)
        await h.close()
    }

    func testUnknownCommandsAreSentAsText() async throws {
        let (hub, h, api) = try await hub()
        let sent = await hub.sendChatText("/wave okay")
        XCTAssertTrue(sent)
        XCTAssertTrue(apiCalls(api, "/api/apps/commands/run").isEmpty)
        let ok = await waitUntil { h.openFrames().contains { $0["text"]?.text == "/wave okay" } }
        XCTAssertTrue(ok)
        await h.close()
    }

    func testOfficeBotMessagesAreInteractiveAndMentionsAreForwarded() async throws {
        let (hub, h, api) = try await hub()
        let id = AppsChatTests.roomId
        api.replies["GET /api/chat/channels"] = ["channels": [["id": .string(id), "name": "lobby", "visibility": "public"]]]
        let room = "chat-channel:\(id):v1"
        api.replies["GET /api/chat/channels/\(id)/room-access"] = [
            "room": .string(room), "passphrase": "office-pass", "webSocketUrl": .string("/api/chat/channels/\(id)/ws?ticket=t"),
        ]
        let ws = FakeWebSocketConnector()
        let office = OfficeChannelService(
            api: api, connector: ws, secureStore: InMemorySecureStore(), origin: { URL(string: "https://app.forkmesh.com") },
            selfId: { "me-node" }, displayName: { "Me" })
        hub.office = office
        await office.refresh()
        hub.select("#office/lobby")
        XCTAssertEqual(hub.appsChannelId("#office/lobby"), id)
        XCTAssertEqual(hub.appsChannelId("#general"), "")

        api.replies["POST /api/apps/mentions"] = ["ok": true, "forwarded": 1]
        let sent = await hub.sendChatText("thanks @hello-app")
        XCTAssertTrue(sent)
        let forwarded = await waitUntil { !self.apiCalls(api, "/api/apps/mentions").isEmpty }
        XCTAssertTrue(forwarded)
        let mention = try XCTUnwrap(apiCalls(api, "/api/apps/mentions").first ?? nil)
        XCTAssertEqual(mention["channelId"]?.text, id)
        XCTAssertEqual(mention["text"]?.text, "thanks @hello-app")
        XCTAssertEqual(mention["messageId"]?.text, office.messages("#office/lobby").last?.id)

        _ = await hub.sendChatText("thanks @bob")
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(apiCalls(api, "/api/apps/mentions").count, 1)

        let connection = try XCTUnwrap(ws.lastConnection)
        let crypto = await Task.detached { RoomCrypto.withPassphrase(room, "office-pass") }.value
        let messageId = String(repeating: "f", count: 32)
        connection.receive(text: JSON.encode(try crypto.encrypt([
            "type": "chat", "id": .string(messageId), "senderId": "app:hello-app", "sender": "Hello App (app)",
            "text": "Pick one", "ts": 9_999_999_999_999, "appMessageId": .string(messageId),
            "app": ["name": "Hello App", "slug": "hello-app"],
            "blocks": [["type": "actions", "elements": [["type": "static_select", "action_id": "size",
                                                         "options": [["text": "Large", "value": "l"]]]]]],
        ])))
        let arrived = await waitUntil { hub.rows.last?.message.app != nil }
        XCTAssertTrue(arrived)
        let bot = try XCTUnwrap(hub.rows.last?.message)
        XCTAssertEqual(bot.app?.channelId, id)
        XCTAssertEqual(bot.app?.ephemeral, false)

        api.replies["POST /api/apps/interact"] = ["ok": true, "responseType": "in_channel", "messageId": "x"]
        await hub.interact(bot, actionId: "size", value: "l")
        let interaction = try XCTUnwrap(apiCalls(api, "/api/apps/interact").first ?? nil)
        XCTAssertEqual(interaction["channelId"]?.text, id)
        XCTAssertEqual(interaction["value"]?.text, "l")
        XCTAssertEqual(hub.rows.last?.message.id, messageId)
        await office.closeAll()
        await h.close()
    }

    func testGuestsAndTheMeshNeverReachApps() async throws {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        let hub = ChatHub(relay: h.relay, mesh: mesh, isGuest: { true })
        let api = FakeAccountApi()
        hub.apps = AppsClient { method, path, body in try await api.accountApiCall(method, path, body) }
        await hub.refreshAppCommands()
        XCTAssertTrue(hub.appSuggestions("/").isEmpty)
        _ = await hub.sendChatText("/hello")
        XCTAssertTrue(api.calls.isEmpty)
        await h.close()
    }
}
