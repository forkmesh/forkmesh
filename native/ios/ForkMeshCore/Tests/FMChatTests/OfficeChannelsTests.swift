import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest

@testable import FMChat

final class OfficeChannelsTests: XCTestCase {
    static let roomId = String(repeating: "a", count: 32)

    func testRoomsAreParsedAndBounded() {
        let payload: JSONObject = [
            "channels": [
                ["id": .string(OfficeChannelsTests.roomId), "name": "design", "visibility": "private", "canManage": true,
                 "topic": "  pixels   and   type ", "members": ["Alice", "alice", " bob "], "keyVersion": 2],
                ["id": "not-an-id", "name": "bad"],
                ["id": .string(String(repeating: "b", count: 32)), "name": "design"],
                ["id": .string(String(repeating: "c", count: 32)), "name": "lobby", "visibility": "public"],
            ]
        ]
        let rooms = OfficeFrames.rooms(from: payload)
        XCTAssertEqual(rooms.map(\.conversation), ["#office/design", "#office/lobby"])
        XCTAssertEqual(rooms[0].members, ["alice", "bob"])
        XCTAssertEqual(rooms[0].topic, "pixels and type")
        XCTAssertEqual(rooms[0].keyVersion, 2)
        XCTAssertTrue(rooms[0].isPrivate)
        XCTAssertTrue(rooms[1].members.isEmpty)
        XCTAssertTrue(OfficeFrames.validName("team-2"))
        XCTAssertFalse(OfficeFrames.validName("-x"))
        XCTAssertFalse(OfficeFrames.validName("Upper"))
    }

    func testChatMessagesAreBoundedLikeTheDesktop() {
        let plain: JSONObject = ["type": "chat", "id": "x1", "senderId": "me", "text": "hello", "ts": 1000]
        let message = OfficeFrames.chatMessage(plain, conversation: "#office/a", selfId: "me")
        XCTAssertEqual(message?.senderName, "Office visitor")
        XCTAssertEqual(message?.isSelf, true)
        var long = plain
        long["text"] = .string(String(repeating: "x", count: ChatLimits.maxTextChars + 1))
        XCTAssertNil(OfficeFrames.chatMessage(long, conversation: "#office/a", selfId: ""))
        XCTAssertNil(OfficeFrames.chatMessage(["type": "typing", "id": "x"], conversation: "#office/a", selfId: ""))
    }

    func testRecipientsComeOnlyFromTheOuterEnvelope() {
        XCTAssertEqual(OfficeFrames.recipients(["kind": "cipher", "recipients": 3]), 3)
        XCTAssertEqual(OfficeFrames.recipients(["recipients": 0]), 0)
        XCTAssertNil(OfficeFrames.recipients(["recipients": -1]))
        XCTAssertNil(OfficeFrames.recipients(["recipients": "3"]))
        XCTAssertNil(OfficeFrames.recipients(["recipients": 2.5]))
        XCTAssertNil(OfficeFrames.recipients(["kind": "cipher"]))
        XCTAssertEqual(OfficeFrames.sentRef("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a3")
        let ref = OfficeFrames.sentRef("frame")
        XCTAssertEqual(
            OfficeFrames.recipientsAck(["kind": "forkmesh-recipients", "v": 1, "ref": .string(ref), "recipients": 2])?.recipients, 2)
        XCTAssertNil(OfficeFrames.recipientsAck(["kind": "cipher", "ref": .string(ref), "recipients": 2]))
        XCTAssertNil(OfficeFrames.recipientsAck(["kind": "forkmesh-recipients", "ref": "short", "recipients": 2]))
        XCTAssertEqual(OfficeFrames.recipientsLabel(3), "to 3")
        XCTAssertEqual(OfficeFrames.recipientsDescription(1), "Delivered to 1 person")
        XCTAssertEqual(OfficeFrames.recipientsDescription(0), "Delivered to 0 people")
        XCTAssertNil(OfficeFrames.recipientsLabel(nil))
    }

    func testWebSocketURLStaysOnTheOrigin() {
        let origin = URL(string: "https://app.next.forkmesh.com/")!
        XCTAssertEqual(
            OfficeFrames.webSocketURL("/api/chat/channels/abc/ws?ticket=t%2B1", origin: origin)?.absoluteString,
            "wss://app.next.forkmesh.com/api/chat/channels/abc/ws?ticket=t%2B1")
        XCTAssertNil(OfficeFrames.webSocketURL("https://evil.example/ws", origin: origin))
        XCTAssertNil(OfficeFrames.webSocketURL("", origin: origin))
    }

    @MainActor func testOfficeRoomSendsAndReceivesEncryptedChat() async throws {
        let api = FakeAccountApi()
        api.replies["GET /api/chat/channels"] = [
            "channels": [["id": .string(OfficeChannelsTests.roomId), "name": "lobby", "visibility": "public", "canManage": true]]
        ]
        let room = "chat-channel:\(OfficeChannelsTests.roomId):v1"
        api.replies["GET /api/chat/channels/\(OfficeChannelsTests.roomId)/room-access"] = [
            "room": .string(room), "passphrase": "office-pass",
            "webSocketUrl": .string("/api/chat/channels/\(OfficeChannelsTests.roomId)/ws?ticket=abc"),
        ]
        let crypto = await Task.detached { RoomCrypto.withPassphrase(room, "office-pass") }.value
        var earlier = try crypto.encrypt(["type": "chat", "id": "old-1", "senderId": "peer", "sender": "Pat", "text": "earlier", "ts": 5])
        earlier["recipients"] = 2
        api.replies["GET /api/chat/channels/\(OfficeChannelsTests.roomId)/history"] = [
            "room": .string(room), "passphrase": "office-pass",
            "messages": [["ts": 5, "body": .string(JSON.encode(earlier))]],
        ]
        let ws = FakeWebSocketConnector()
        let service = OfficeChannelService(
            api: api, connector: ws, secureStore: InMemorySecureStore(), origin: { URL(string: "https://app.next.forkmesh.com") },
            selfId: { "me-node" }, displayName: { "Me" })
        await service.refresh()
        XCTAssertEqual(service.conversations, ["#office/lobby"])
        await service.loadHistory("#office/lobby")
        XCTAssertEqual(service.messages("#office/lobby").map(\.text), ["earlier"])
        XCTAssertEqual(service.messages("#office/lobby").first?.recipients, 2)
        let ok79 = await service.send("#office/lobby", text: "hi office")
        XCTAssertTrue(ok79)
        let connection = try XCTUnwrap(ws.lastConnection)
        XCTAssertEqual(connection.url.absoluteString, "wss://app.next.forkmesh.com/api/chat/channels/\(OfficeChannelsTests.roomId)/ws?ticket=abc")
        let ok82 = await waitUntil { connection.sentTexts.count >= 2 }
        XCTAssertTrue(ok82)
        let frames = connection.sentTexts.compactMap { crypto.decrypt(JSON.parseObject($0)) }
        let chat = try XCTUnwrap(frames.first { $0["type"]?.text == "chat" })
        XCTAssertEqual(chat["text"]?.text, "hi office")
        XCTAssertEqual(chat["channel"]?.text, "#lobby")
        let chatEnvelope = try XCTUnwrap(connection.sentTexts.map { JSON.parseObject($0) }.first { crypto.decrypt($0)?["type"]?.text == "chat" })
        XCTAssertEqual(chatEnvelope["persist"], .bool(true))
        let chatText = try XCTUnwrap(connection.sentTexts.first { crypto.decrypt(JSON.parseObject($0))?["type"]?.text == "chat" })
        XCTAssertNil(service.messages("#office/lobby").first { $0.isSelf }?.recipients)
        let ownId = try XCTUnwrap(service.messages("#office/lobby").first { $0.isSelf }?.id)
        XCTAssertEqual(service.receipt(ownId), .sending)
        let ack: JSONObject = ["kind": "forkmesh-recipients", "v": 1, "ref": .string(OfficeFrames.sentRef(chatText)), "recipients": 4]
        connection.receive(text: JSON.encode(ack))
        let ackApplied = await waitUntil { service.messages("#office/lobby").first { $0.isSelf }?.recipients == 4 }
        XCTAssertTrue(ackApplied)
        XCTAssertEqual(service.receipt(ownId), .sent)
        var live = try crypto.encrypt(["type": "chat", "id": "live-1", "senderId": "peer", "sender": "Pat", "text": "welcome", "ts": 9_999_999_999_999])
        live["recipients"] = 3
        connection.receive(text: JSON.encode(live))
        let ok89 = await waitUntil { service.messages("#office/lobby").last?.text == "welcome" }
        XCTAssertTrue(ok89)
        XCTAssertEqual(service.messages("#office/lobby").map(\.text), ["earlier", "hi office", "welcome"])
        XCTAssertEqual(service.messages("#office/lobby").map(\.recipients), [2, 4, 3])
        var removed: [String] = []
        service.onRemoved = { removed.append($1) }
        let deleted = await service.deleteForEveryone("#office/lobby", messageId: ownId)
        XCTAssertTrue(deleted)
        XCTAssertEqual(service.messages("#office/lobby").map(\.text), ["earlier", "welcome"])
        let delete = try XCTUnwrap(connection.sentTexts.compactMap { crypto.decrypt(JSON.parseObject($0)) }.first { $0["type"]?.text == "delete" })
        XCTAssertEqual(delete["target"]?.text, ownId)
        XCTAssertEqual(delete["conversation"]?.text, "#lobby")
        let forged: JSONObject = ["type": "delete", "id": "d-1", "senderId": "mallory", "sender": "M", "target": "live-1", "ts": 1]
        connection.receive(text: JSON.encode(try crypto.encrypt(forged)))
        var real = forged
        real["id"] = "d-2"
        real["senderId"] = "peer"
        connection.receive(text: JSON.encode(try crypto.encrypt(real)))
        let gone = await waitUntil { service.messages("#office/lobby").map(\.text) == ["earlier"] }
        XCTAssertTrue(gone)
        XCTAssertEqual(removed, [ownId, "live-1"])
        await service.closeAll()
    }

    @MainActor func testThreadRepliesInAnOfficeChannelGoBothWays() async throws {
        let api = FakeAccountApi()
        api.replies["GET /api/chat/channels"] = [
            "channels": [["id": .string(OfficeChannelsTests.roomId), "name": "lobby", "visibility": "public", "canManage": true]]
        ]
        let room = "chat-channel:\(OfficeChannelsTests.roomId):v1"
        api.replies["GET /api/chat/channels/\(OfficeChannelsTests.roomId)/room-access"] = [
            "room": .string(room), "passphrase": "office-pass",
            "webSocketUrl": .string("/api/chat/channels/\(OfficeChannelsTests.roomId)/ws?ticket=abc"),
        ]
        let crypto = await Task.detached { RoomCrypto.withPassphrase(room, "office-pass") }.value
        let ws = FakeWebSocketConnector()
        let service = OfficeChannelService(
            api: api, connector: ws, secureStore: InMemorySecureStore(), origin: { URL(string: "https://app.next.forkmesh.com") },
            selfId: { "me-node" }, displayName: { "Me" })
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        let hub = ChatHub(relay: h.relay, mesh: mesh)
        hub.office = service
        await service.refresh()
        let opened = await service.open("#office/lobby")
        XCTAssertTrue(opened)
        hub.select("#office/lobby")
        let connection = try XCTUnwrap(ws.lastConnection)
        let now = Date().epochMillis
        connection.receive(text: JSON.encode(try crypto.encrypt([
            "type": "chat", "id": "root-1", "senderId": "peer", "sender": "Pat", "text": "question", "ts": .int(now - 60_000),
        ])))
        let rooted = await waitUntil { hub.rows.map(\.id) == ["root-1"] }
        XCTAssertTrue(rooted)
        connection.receive(text: JSON.encode(try crypto.encrypt([
            "type": "thread-reply", "id": "reply-1", "senderId": "peer", "sender": "Pat", "rootId": "root-1",
            "channel": "#lobby", "text": "first", "ts": .int(now - 50_000),
        ])))
        let threaded = await waitUntil { hub.threadRows("root-1").map(\.text) == ["first"] }
        XCTAssertTrue(threaded)
        XCTAssertEqual(hub.rows.map(\.id), ["root-1"])
        XCTAssertEqual(hub.rows.first?.thread?.replyCount, 1)
        XCTAssertEqual(hub.threadRows("root-1").first?.message.conversation, "#office/lobby")

        let sent = await hub.replyInThread(channel: "#office/lobby", rootId: "root-1", text: "answer")
        XCTAssertTrue(sent)
        let mine = try XCTUnwrap(hub.threadRows("root-1").last)
        XCTAssertEqual(mine.text, "answer")
        XCTAssertTrue(mine.message.isSelf)
        XCTAssertEqual(mine.receipt, .sending)
        let wire = try XCTUnwrap(connection.sentTexts.first { crypto.decrypt(JSON.parseObject($0))?["type"]?.text == "thread-reply" })
        let reply = try XCTUnwrap(crypto.decrypt(JSON.parseObject(wire)))
        XCTAssertEqual(reply["rootId"]?.text, "root-1")
        XCTAssertEqual(reply["channel"]?.text, "#lobby")
        XCTAssertEqual(JSON.parseObject(wire)["notify"], .bool(true))
        connection.receive(text: JSON.encode(["kind": "forkmesh-recipients", "v": 1, "ref": .string(OfficeFrames.sentRef(wire)), "recipients": 2] as JSONObject))
        let acked = await waitUntil { hub.threadRows("root-1").last?.receipt == .sent }
        XCTAssertTrue(acked)
        XCTAssertEqual(hub.rows.first?.thread?.replyCount, 2)
        XCTAssertEqual(hub.listPreview("#office/lobby")?.text, "Pat: question")
        await service.closeAll()
        await h.close()
    }

    @MainActor func testAServerVisibleNoticesChannelShowsServerNotes() async throws {
        let noticesId = String(repeating: "e", count: 32)
        let api = FakeAccountApi()
        api.replies["GET /api/chat/channels"] = [
            "channels": [
                [
                    "id": .string(noticesId), "name": "email-log", "visibility": "private", "canManage": false,
                    "kind": "notices", "serverVisible": true, "readOnly": true,
                ],
                ["id": .string(OfficeChannelsTests.roomId), "name": "secret", "visibility": "private", "canManage": true],
            ]
        ]
        let noticesRoom = "chat-channel:\(noticesId):v1"
        let secretRoom = "chat-channel:\(OfficeChannelsTests.roomId):v1"
        let note: JSONObject = [
            "type": "chat", "id": "note-1", "senderId": "forkmesh-mail-log", "sender": "ForkMesh mail", "ts": 5,
            "text": "2026-09-28 17:35:58Z · Verification · accepted", "webhook": true, "notice": true,
        ]
        let noticesCrypto = await Task.detached { RoomCrypto.withPassphrase(noticesRoom, "notices-pass") }.value
        let secretCrypto = await Task.detached { RoomCrypto.withPassphrase(secretRoom, "secret-pass") }.value
        api.replies["GET /api/chat/channels/\(noticesId)/history"] = [
            "room": .string(noticesRoom), "passphrase": "notices-pass",
            "messages": [["ts": 5, "body": .string(JSON.encode(try noticesCrypto.encrypt(note)))]],
        ]
        api.replies["GET /api/chat/channels/\(noticesId)/room-access"] = [
            "room": .string(noticesRoom), "passphrase": "notices-pass",
            "webSocketUrl": .string("/api/chat/channels/\(noticesId)/ws?ticket=n"),
        ]
        api.replies["GET /api/chat/channels/\(OfficeChannelsTests.roomId)/history"] = [
            "room": .string(secretRoom), "passphrase": "secret-pass",
            "messages": [["ts": 5, "body": .string(JSON.encode(try secretCrypto.encrypt(note)))]],
        ]
        let ws = FakeWebSocketConnector()
        let service = OfficeChannelService(
            api: api, connector: ws, secureStore: InMemorySecureStore(), origin: { URL(string: "https://app.next.forkmesh.com") },
            selfId: { "me-node" }, displayName: { "Me" })
        await service.refresh()
        let notices = try XCTUnwrap(service.room(for: "#office/email-log"))
        XCTAssertTrue(notices.isPrivate && notices.serverVisible && notices.readOnly)
        XCTAssertEqual(service.room(for: "#office/secret")?.serverVisible, false)
        await service.loadHistory("#office/email-log")
        XCTAssertEqual(service.messages("#office/email-log").map(\.text), ["2026-09-28 17:35:58Z · Verification · accepted"])
        await service.loadHistory("#office/secret")
        XCTAssertTrue(service.messages("#office/secret").isEmpty)
        let opened = await service.open("#office/email-log")
        XCTAssertTrue(opened)
        let connection = try XCTUnwrap(ws.lastConnection)
        var live = note
        live["id"] = "note-2"
        live["ts"] = 6
        live["text"] = "delivered"
        connection.receive(text: JSON.encode(try noticesCrypto.encrypt(live)))
        let arrived = await waitUntil { service.messages("#office/email-log").last?.text == "delivered" }
        XCTAssertTrue(arrived)
        let sent = await service.send("#office/email-log", text: "hello")
        XCTAssertFalse(sent)
        XCTAssertFalse(connection.sentTexts.contains { noticesCrypto.decrypt(JSON.parseObject($0))?["type"]?.text == "chat" })
        await service.closeAll()
    }

    @MainActor func testManagingChannelsMapsServerErrors() async throws {
        let api = FakeAccountApi()
        api.failures["POST /api/chat/channels"] = OrgApiException("channel_name_taken", "taken")
        let service = OfficeChannelService(
            api: api, connector: FakeWebSocketConnector(), secureStore: InMemorySecureStore(), origin: { nil },
            selfId: { "me" }, displayName: { "Me" })
        do {
            _ = try await service.create(name: "taken", isPrivate: true, members: ["Bob"])
            XCTFail("expected a refusal")
        } catch let error as OfficeError {
            XCTAssertEqual(error, .refused("channel_name_taken"))
            XCTAssertEqual(error.description, "A channel with that name already exists.")
        }
        XCTAssertEqual(api.calls.first?.2?["members"], JSONValue(["bob"]))
        do {
            _ = try await service.create(name: "Bad Name", isPrivate: false, members: [])
            XCTFail("expected a local refusal")
        } catch let error as OfficeError {
            XCTAssertEqual(error, .refused("invalid_channel_name"))
        }
    }
}
