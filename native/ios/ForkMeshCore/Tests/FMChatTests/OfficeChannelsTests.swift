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

    @MainActor func testSavedRoomsShowBeforeTheServerAnswersAndRefreshesSaveThem() async {
        let api = FakeAccountApi()
        let general: JSONObject = [
            "id": .string(String(repeating: "e", count: 32)), "name": "general", "visibility": "public", "kind": "everyone",
            "memberCount": 12, "readOnly": false,
        ]
        let team: JSONObject = [
            "id": .string(OfficeChannelsTests.roomId), "name": "team", "visibility": "private", "canManage": true,
            "topic": "plans", "members": ["alice", "bob"], "keyVersion": 3, "serverVisible": true,
        ]
        api.replies["GET /api/chat/channels"] = ["channels": [.object(general), .object(team)]]
        let service = OfficeChannelService(
            api: api, connector: FakeWebSocketConnector(), secureStore: InMemorySecureStore(),
            origin: { URL(string: "https://app.next.forkmesh.com") }, selfId: { "me-node" }, displayName: { "Me" })
        var saved: [OfficeRoom] = []
        service.onRooms = { saved = $0 }
        await service.load(attempts: 1)
        XCTAssertEqual(saved.map(\.conversation), ["#office/general", "#office/team"])
        let cached = JSON.encode(OfficeFrames.payload(saved))
        let cold = OfficeChannelService(
            api: FakeAccountApi(), connector: FakeWebSocketConnector(), secureStore: InMemorySecureStore(),
            origin: { URL(string: "https://app.next.forkmesh.com") }, selfId: { "me-node" }, displayName: { "Me" })
        cold.restore(JSON.parseObject(cached))
        XCTAssertEqual(cold.rooms, saved)
        XCTAssertTrue(cold.rooms[0].everyone)
        XCTAssertEqual(cold.rooms[1].members, ["alice", "bob"])
        cold.restore(["channels": []])
        XCTAssertEqual(cold.rooms.count, 2, "a restore never replaces rooms already loaded")
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
        let store = InMemorySecureStore()
        let me = try Identity.fromSeed(Ed25519.generateSeed())
        let peer = try OfficePeer(room: room, crypto: crypto)
        try peer.learnPhone(store: store, identity: me)
        var earlier = try peer.envelope(["type": "chat", "id": "old-1", "sender": "Pat", "text": "earlier", "ts": 5])
        earlier["recipients"] = 2
        let unsealed = try crypto.encrypt(["type": "chat", "id": "old-2", "senderId": .string(peer.node), "sender": "Pat", "text": "forged", "ts": 6])
        api.replies["GET /api/chat/channels/\(OfficeChannelsTests.roomId)/history"] = [
            "room": .string(room), "passphrase": "office-pass", "messages": .array(try peer.history([(5, earlier), (6, unsealed)])),
        ]
        let ws = FakeWebSocketConnector()
        let service = OfficeChannelService(
            api: api, connector: ws, secureStore: store, origin: { URL(string: "https://app.next.forkmesh.com") },
            selfId: { me.nodeId }, displayName: { "Me" }, nodeIdentity: me)
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
        let frames = peer.readAll(connection.sentTexts)
        let chat = try XCTUnwrap(frames.first { $0["type"]?.text == "chat" })
        XCTAssertEqual(chat["text"]?.text, "hi office")
        XCTAssertEqual(chat["channel"]?.text, "#lobby")
        let chatText = try XCTUnwrap(connection.sentTexts.first { peer.read($0)?["type"]?.text == "chat" })
        let chatEnvelope = JSON.parseObject(chatText)
        XCTAssertEqual(chatEnvelope["persist"], .bool(true))
        XCTAssertTrue(RoomSeal.isSealedFrame(try XCTUnwrap(crypto.decrypt(chatEnvelope))), "public channel chat is sealed")
        XCTAssertNil(service.messages("#office/lobby").first { $0.isSelf }?.recipients)
        let ownId = try XCTUnwrap(service.messages("#office/lobby").first { $0.isSelf }?.id)
        XCTAssertEqual(service.receipt(ownId), .sending)
        let ack: JSONObject = ["kind": "forkmesh-recipients", "v": 1, "ref": .string(OfficeFrames.sentRef(chatText)), "recipients": 4]
        connection.receive(text: JSON.encode(ack))
        let ackApplied = await waitUntil { service.messages("#office/lobby").first { $0.isSelf }?.recipients == 4 }
        XCTAssertTrue(ackApplied)
        XCTAssertEqual(service.receipt(ownId), .sent)
        var live = try peer.envelope(["type": "chat", "id": "live-1", "sender": "Pat", "text": "welcome", "ts": 9_999_999_999_999])
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
        let delete = try XCTUnwrap(peer.readAll(connection.sentTexts).first { $0["type"]?.text == "delete" })
        XCTAssertEqual(delete["target"]?.text, ownId)
        XCTAssertEqual(delete["conversation"]?.text, "#lobby")
        let mallory = try OfficePeer(room: room, crypto: crypto, acct: "mallory")
        try mallory.learnPhone(store: store, identity: me)
        connection.receive(text: JSON.encode(try mallory.bundleEnvelope()))
        let forged: JSONObject = ["type": "delete", "id": "d-1", "sender": "M", "target": "live-1", "ts": 1]
        connection.receive(text: JSON.encode(try mallory.envelope(forged)))
        var unsealedDelete = forged
        unsealedDelete["id"] = "d-0"
        unsealedDelete["senderId"] = .string(peer.node)
        connection.receive(text: JSON.encode(try crypto.encrypt(unsealedDelete)))
        connection.receive(text: JSON.encode(try serverEnvelope(unsealedDelete, crypto: crypto)))
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(service.messages("#office/lobby").map(\.text), ["earlier", "welcome"], "only the author deletes")
        var real = forged
        real["id"] = "d-2"
        connection.receive(text: JSON.encode(try peer.envelope(real)))
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
        let store = InMemorySecureStore()
        let me = try Identity.fromSeed(Ed25519.generateSeed())
        let peer = try OfficePeer(room: room, crypto: crypto)
        try peer.learnPhone(store: store, identity: me)
        let service = OfficeChannelService(
            api: api, connector: ws, secureStore: store, origin: { URL(string: "https://app.next.forkmesh.com") },
            selfId: { me.nodeId }, displayName: { "Me" }, nodeIdentity: me)
        let patNode = peer.node
        service.nodeOwners = ChatNodeOwners(lookup: { node in node == patNode ? "pat" : "" })
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
        connection.receive(text: JSON.encode(try peer.bundleEnvelope()))
        connection.receive(text: JSON.encode(try peer.envelope([
            "type": "chat", "id": "root-1", "sender": "Pat", "text": "question", "ts": .int(now - 60_000),
        ])))
        let rooted = await waitUntil { hub.rows.map(\.id) == ["root-1"] }
        XCTAssertTrue(rooted)
        connection.receive(text: JSON.encode(try serverEnvelope([
            "type": "thread-reply", "id": "reply-0", "senderId": .string(peer.node), "sender": "Pat", "rootId": "root-1",
            "channel": "#lobby", "text": "unsealed", "ts": .int(now - 55_000),
        ], crypto: crypto)))
        connection.receive(text: JSON.encode(try peer.envelope([
            "type": "thread-reply", "id": "reply-1", "sender": "Pat", "rootId": "root-1",
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
        let wire = try XCTUnwrap(connection.sentTexts.first { peer.read($0)?["type"]?.text == "thread-reply" })
        let reply = try XCTUnwrap(peer.read(wire))
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

    @MainActor private func privateRoom(_ name: String) async throws
        -> (OfficeChannelService, FakeWebSocketConnection, OfficePeer, RoomCrypto, FakeAccountApi)
    {
        let api = FakeAccountApi()
        api.replies["GET /api/chat/channels"] = [
            "channels": [["id": .string(OfficeChannelsTests.roomId), "name": .string(name), "visibility": "private",
                "canManage": true, "members": ["me", "pat"]]]
        ]
        let room = "chat-channel:\(OfficeChannelsTests.roomId):v1"
        api.replies["GET /api/chat/channels/\(OfficeChannelsTests.roomId)/room-access"] = [
            "room": .string(room), "passphrase": "team-pass",
            "webSocketUrl": .string("/api/chat/channels/\(OfficeChannelsTests.roomId)/ws?ticket=t"),
        ]
        let crypto = await Task.detached { RoomCrypto.withPassphrase(room, "team-pass") }.value
        let ws = FakeWebSocketConnector()
        let store = InMemorySecureStore()
        let me = try Identity.fromSeed(Ed25519.generateSeed())
        let peer = try OfficePeer(room: room, crypto: crypto)
        try peer.learnPhone(store: store, identity: me)
        let service = OfficeChannelService(
            api: api, connector: ws, secureStore: store, origin: { URL(string: "https://app.next.forkmesh.com") },
            selfId: { me.nodeId }, displayName: { "me" }, nodeIdentity: me)
        await service.refresh()
        let opened = await service.open("#office/\(name)")
        XCTAssertTrue(opened)
        let connection = try XCTUnwrap(ws.lastConnection)
        connection.receive(text: JSON.encode(try peer.bundleEnvelope()))
        return (service, connection, peer, crypto, api)
    }

    @MainActor func testAWebhookInAPrivateChannelShowsAsTheServerVisibleIntegration() async throws {
        let (service, connection, peer, crypto, _) = try await privateRoom("team")
        let conversation = "#office/team"
        let hook: JSONObject = [
            "type": "chat", "id": "hook-1", "senderId": "app:Deploy", "sender": "Deploy", "channel": "#team",
            "text": "build 42 is out", "ts": 5, "webhook": true, "serverVisible": true, "ownerUser": "pat",
        ]
        connection.receive(text: JSON.encode(try crypto.encrypt(hook)))
        var forged = hook
        forged["id"] = "hook-2"
        forged["senderId"] = .string(peer.node)
        connection.receive(text: JSON.encode(try crypto.encrypt(forged)))
        var marked = hook
        marked["id"] = "hook-3"
        connection.receive(text: JSON.encode(try serverEnvelope(marked, crypto: crypto)))
        let shown = await waitUntil { service.messages(conversation).map(\.id) == ["hook-3"] }
        XCTAssertTrue(shown, "only the frame the server marked is shown: \(service.messages(conversation).map(\.id))")
        let message = try XCTUnwrap(service.messages(conversation).first)
        XCTAssertEqual(message.senderId, "app:Deploy")
        XCTAssertEqual(message.senderName, "Deploy")
        XCTAssertTrue(message.serverVisible)
        XCTAssertEqual(
            ChatHistoryCodec.decodeMessage(ChatHistoryCodec.encodeMessage(message).objectValue ?? [:])?.serverVisible, true)
        await service.closeAll()
    }

    @MainActor func testAnOfficeNameIsShownOnlyOnceTheServerVouchesForTheNode() async throws {
        let (service, connection, peer, _, _) = try await privateRoom("team")
        let conversation = "#office/team"
        let patNode = peer.node
        var bound: [String: String] = [:]
        service.nodeOwners = ChatNodeOwners(lookup: { node in bound[node] ?? "" })
        connection.receive(text: JSON.encode(try peer.envelope([
            "type": "chat", "id": "m-1", "sender": "pat", "ownerUser": "pat", "channel": "#team", "text": "hello", "ts": 5,
        ])))
        connection.receive(text: JSON.encode(try peer.envelope([
            "type": "chat", "id": "m-2", "sender": "forkbot", "bot": "forkbot", "channel": "#team", "text": "I am ForkBot",
            "ts": 6,
        ])))
        let arrived = await waitUntil { service.messages(conversation).count == 2 }
        XCTAssertTrue(arrived)
        let guest = guestChatName(patNode)
        XCTAssertEqual(service.messages(conversation).map(\.senderName), [guest, "forkbot \u{00B7} via \(guest)"])

        service.nodeOwners?.clear()
        bound[patNode] = "pat"
        connection.receive(text: JSON.encode(try peer.envelope([
            "type": "chat", "id": "m-3", "sender": "Pat", "channel": "#team", "text": "it is me", "ts": 7,
        ])))
        connection.receive(text: JSON.encode(try peer.envelope([
            "type": "chat", "id": "m-4", "sender": "forkbot", "bot": "forkbot", "channel": "#team", "text": "answer", "ts": 8,
        ])))
        let verified = await waitUntil { service.messages(conversation).count == 4 }
        XCTAssertTrue(verified)
        XCTAssertEqual(service.messages(conversation).suffix(2).map(\.senderName), ["Pat", "forkbot \u{00B7} via pat"])
        XCTAssertEqual(service.messages(conversation).last?.senderId, "forkbot")
        XCTAssertEqual(service.messages(conversation).first?.senderName, "pat", "the earlier guest line takes the vouched name")
        XCTAssertEqual(service.messages(conversation)[1].senderName, "forkbot \u{00B7} via \(guest)")
        await service.closeAll()
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
            "text": "2026-09-28 17:35:58Z · Verification · accepted", "webhook": true, "notice": true, "serverVisible": true,
        ]
        let noticesCrypto = await Task.detached { RoomCrypto.withPassphrase(noticesRoom, "notices-pass") }.value
        let secretCrypto = await Task.detached { RoomCrypto.withPassphrase(secretRoom, "secret-pass") }.value
        api.replies["GET /api/chat/channels/\(noticesId)/history"] = [
            "room": .string(noticesRoom), "passphrase": "notices-pass",
            "messages": [["ts": 5, "body": .string(JSON.encode(try serverEnvelope(note, crypto: noticesCrypto)))]],
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
        live["text"] = "not from the server"
        connection.receive(text: JSON.encode(try noticesCrypto.encrypt(live)))
        live["id"] = "note-3"
        live["text"] = "delivered"
        connection.receive(text: JSON.encode(try serverEnvelope(live, crypto: noticesCrypto)))
        let arrived = await waitUntil { service.messages("#office/email-log").last?.text == "delivered" }
        XCTAssertTrue(arrived)
        XCTAssertEqual(service.messages("#office/email-log").count, 2, "an unsealed frame the server did not mark is dropped")
        XCTAssertEqual(service.messages("#office/email-log").last?.senderId, "app:ForkMesh mail")
        XCTAssertEqual(service.messages("#office/email-log").map(\.serverVisible), [true, true])
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
