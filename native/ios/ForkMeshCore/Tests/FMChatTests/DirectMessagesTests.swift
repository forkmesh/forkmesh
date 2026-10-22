import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest

@testable import FMChat

final class DirectMessagesTests: XCTestCase {
    static let conversationId = String(repeating: "d", count: 32)
    static let room = "chat-direct:\(conversationId):v1"

    func testConversationsAreParsedAndBounded() {
        let payload: JSONObject = [
            "conversations": [
                ["id": .string(DirectMessagesTests.conversationId), "otherUser": " Pat ", "updatedAt": 9, "keyVersion": 2,
                 "unreadCount": 3],
                ["id": "not-an-id", "otherUser": "bad"],
                ["id": .string(String(repeating: "e", count: 32)), "otherUser": ""],
                ["id": .string(DirectMessagesTests.conversationId), "otherUser": "dupe"],
            ],
            "nextCursor": "9.aa",
        ]
        let conversations = DirectMessageFrames.conversations(from: payload)
        XCTAssertEqual(conversations.count, 1)
        XCTAssertEqual(conversations[0].otherUser, "pat")
        XCTAssertEqual(conversations[0].unreadCount, 3)
        XCTAssertEqual(conversations[0].keyVersion, 2)
        XCTAssertEqual(conversations[0].conversation, "direct:\(DirectMessagesTests.conversationId)")
        XCTAssertTrue(DirectMessageFrames.isDirect(conversations[0].conversation))
        XCTAssertFalse(DirectMessageFrames.isDirect("direct:short"))
        XCTAssertFalse(DirectMessageFrames.isDirect("#general"))
        XCTAssertEqual(DirectMessageFrames.users(from: ["users": [["name": "Ann"], ["name": "ann"], ["name": ""]]]), ["ann"])
    }

    func testGroupChannelNamesFitTheChannelRule() {
        for names in [["alice", "bob", "carol"], ["Ann O'Really", "x", "very-long-name-exceeding"], [""], []] {
            let name = DirectMessageFrames.groupChannelName(names)
            XCTAssertTrue(OfficeFrames.validName(name), name)
            XCTAssertLessThanOrEqual(name.count, 40)
        }
        XCTAssertTrue(DirectMessageFrames.groupChannelName([]).hasPrefix("group-"))
    }

    @MainActor func makeService(
        api: FakeAccountApi, ws: FakeWebSocketConnector, store: SecureStore = InMemorySecureStore(),
        sleep: (@Sendable (TimeInterval) async throws -> Void)? = nil
    ) -> DirectMessageService {
        if let sleep {
            return DirectMessageService(
                api: api, connector: ws, secureStore: store,
                origin: { URL(string: "https://app.next.forkmesh.com") }, selfId: { "me-node" }, displayName: { "Me" },
                sleep: sleep)
        }
        return DirectMessageService(
            api: api, connector: ws, secureStore: store,
            origin: { URL(string: "https://app.next.forkmesh.com") }, selfId: { "me-node" }, displayName: { "Me" })
    }

    @MainActor func keyAdverts(_ connection: FakeWebSocketConnection, _ crypto: RoomCrypto) -> [JSONObject] {
        connection.sentJSON.filter { RoomSeal.isKeyFrame(crypto.decrypt($0) ?? [:]) }
    }

    @MainActor func teach(_ peer: RoomSeal, from connection: FakeWebSocketConnection, _ crypto: RoomCrypto) async -> Bool {
        let advertised = await waitUntil { !self.keyAdverts(connection, crypto).isEmpty }
        for envelope in keyAdverts(connection, crypto) {
            if let frame = crypto.decrypt(envelope) { _ = peer.acceptKeyFrame(frame) }
        }
        return advertised
    }

    @MainActor func sentOfKind(_ connection: FakeWebSocketConnection, _ kind: String) -> [JSONObject] {
        connection.sentJSON.filter { $0["kind"]?.text == kind }
    }

    @MainActor func roomCrypto() async -> RoomCrypto {
        await Task.detached { RoomCrypto.withPassphrase(DirectMessagesTests.room, "direct-pass") }.value
    }

    static let scope = "direct:\(conversationId)"

    @MainActor func peerSeal(_ label: String, acct: String = "pat", node: String = "pat-node") throws -> RoomSeal {
        try RoomSeal.loadOrCreate(
            DirectMessagesTests.room, scope: DirectMessagesTests.scope, policy: .strict, acct: acct, node: node,
            secureStore: InMemorySecureStore())
    }

    @MainActor func preparedApi() -> FakeAccountApi {
        let api = FakeAccountApi()
        api.replies["GET /api/chat/direct-messages"] = [
            "conversations": [
                ["id": .string(DirectMessagesTests.conversationId), "otherUser": "pat", "unreadCount": 2]
            ]
        ]
        api.replies["GET /api/chat/direct-messages/\(DirectMessagesTests.conversationId)/room-access"] = [
            "room": .string(DirectMessagesTests.room), "passphrase": "direct-pass",
            "webSocketUrl": .string("/api/chat/direct-messages/\(DirectMessagesTests.conversationId)/ws?ticket=abc"),
        ]
        return api
    }

    @MainActor func testDirectRoomSendsOnlySealedFrames() async throws {
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws)
        await service.refresh()
        XCTAssertEqual(service.conversations, ["direct:\(DirectMessagesTests.conversationId)"])
        XCTAssertEqual(service.unreadTotal, 2)
        let conversation = service.conversations[0]
        let opened = await service.open(conversation)
        XCTAssertTrue(opened)
        XCTAssertEqual(service.unreadTotal, 2)
        XCTAssertEqual(ws.connections.count, 1)
        let connection = try XCTUnwrap(ws.lastConnection)
        XCTAssertEqual(
            connection.url.absoluteString,
            "wss://app.next.forkmesh.com/api/chat/direct-messages/\(DirectMessagesTests.conversationId)/ws?ticket=abc")
        let crypto = await Task.detached { RoomCrypto.withPassphrase(DirectMessagesTests.room, "direct-pass") }.value
        let sent = await service.send(conversation, text: "hello pat")
        XCTAssertTrue(sent)
        let ok = await waitUntil { connection.sentTexts.count >= 2 }
        XCTAssertTrue(ok)
        let frames = connection.sentTexts.compactMap { crypto.decrypt(JSON.parseObject($0)) }
        XCTAssertTrue(frames.contains { RoomSeal.isKeyFrame($0) })
        let sealed = try XCTUnwrap(frames.first { RoomSeal.isSealedFrame($0) })
        XCTAssertNil(sealed["text"])
        let chatEnvelope = try XCTUnwrap(
            connection.sentTexts.map { JSON.parseObject($0) }
                .first { RoomSeal.isSealedFrame(crypto.decrypt($0) ?? [:]) })
        XCTAssertEqual(chatEnvelope["persist"], .bool(true))
        XCTAssertEqual(chatEnvelope["notify"], .bool(true))
        XCTAssertEqual(service.messages(conversation).map(\.text), ["hello pat"])
    }

    @MainActor func testPeersExchangeSealedChatAndCallFrames() async throws {
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws)
        var callFrames: [(JSONObject, String)] = []
        service.onCallFrame = { plain, conversation in callFrames.append((plain, conversation)) }
        var liveMessages: [ChatMessage] = []
        service.onMessage = { liveMessages.append($0) }
        await service.refresh()
        let conversation = service.conversations[0]
        _ = await service.open(conversation)
        let connection = try XCTUnwrap(ws.lastConnection)
        let crypto = await Task.detached { RoomCrypto.withPassphrase(DirectMessagesTests.room, "direct-pass") }.value
        let peerSeal = try self.peerSeal("peer")
        connection.receive(text: JSON.encode(try crypto.encrypt(peerSeal.keyFrame())))
        let advertised = await waitUntil {
            connection.sentTexts.contains { RoomSeal.isKeyFrame(crypto.decrypt(JSON.parseObject($0)) ?? [:]) }
        }
        XCTAssertTrue(advertised)
        for text in connection.sentTexts {
            if let frame = crypto.decrypt(JSON.parseObject(text)), RoomSeal.isKeyFrame(frame) {
                _ = peerSeal.acceptKeyFrame(frame)
            }
        }
        let plainChat: JSONObject = [
            "type": "chat", "id": "pat-1", "senderId": "pat-node", "sender": "pat", "accountKind": "user",
            "channel": .string(conversation), "text": "hi from pat", "ts": 1000,
        ]
        connection.receive(text: JSON.encode(try crypto.encrypt(try peerSeal.seal(plainChat))))
        let gotChat = await waitUntil { service.messages(conversation).map(\.text) == ["hi from pat"] }
        XCTAssertTrue(gotChat)
        XCTAssertEqual(liveMessages.map(\.text), ["hi from pat"])
        var unsealed = plainChat
        unsealed["id"] = "pat-2"
        unsealed["text"] = "leaky"
        connection.receive(text: JSON.encode(try crypto.encrypt(unsealed)))
        var wrongChannel = plainChat
        wrongChannel["id"] = "pat-3"
        wrongChannel["channel"] = .string("direct:" + String(repeating: "f", count: 32))
        connection.receive(text: JSON.encode(try crypto.encrypt(try peerSeal.seal(wrongChannel))))
        let invite: JSONObject = [
            "type": "call-invite", "id": "pat-4", "senderId": "pat-node", "sender": "pat",
            "channel": .string(conversation), "call": "c-1", "media": "audio", "ts": 1001,
        ]
        connection.receive(text: JSON.encode(try crypto.encrypt(try peerSeal.seal(invite))))
        let gotCall = await waitUntil { callFrames.count == 1 }
        XCTAssertTrue(gotCall)
        XCTAssertEqual(callFrames.first?.0["call"]?.text, "c-1")
        XCTAssertEqual(callFrames.first?.1, conversation)
        XCTAssertEqual(service.messages(conversation).map(\.text), ["hi from pat"])
        let signaled = await service.sendCallFrame(
            conversation, type: "call-accept", extra: ["call": "c-1", "to": "pat-node"])
        XCTAssertTrue(signaled)
        let callEnvelope = try XCTUnwrap(
            connection.sentTexts.map { JSON.parseObject($0) }.last { ($0["kind"]?.text ?? "") == "cipher" })
        XCTAssertNil(callEnvelope["persist"])
        XCTAssertNil(callEnvelope["notify"])
        let accepted = try XCTUnwrap(
            connection.sentTexts.compactMap { crypto.decrypt(JSON.parseObject($0)) }
                .compactMap { RoomSeal.isSealedFrame($0) ? peerSeal.unseal($0) : nil }
                .first { $0["type"]?.text == "call-accept" })
        XCTAssertEqual(accepted["to"]?.text, "pat-node")
        XCTAssertEqual(accepted["channel"]?.text, conversation)
        await service.closeAll()
    }

    @MainActor func testReceivedFramesAreAckedButKeyAdvertisementsAreNot() async throws {
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws)
        await service.refresh()
        let conversation = service.conversations[0]
        _ = await service.open(conversation)
        let connection = try XCTUnwrap(ws.lastConnection)
        XCTAssertNil(service.serverCopies(conversation))
        connection.receive(text: #"{"kind":"forkmesh-server-copies","v":1,"stored":2}"#)
        let shown = await waitUntil { service.serverCopies(conversation) == 2 }
        XCTAssertTrue(shown)
        XCTAssertEqual(DirectMessageFrames.serverCopiesLabel(2), "2 messages on the server")
        XCTAssertEqual(DirectMessageFrames.serverCopiesLabel(0), "Not stored on the server")
        let crypto = await roomCrypto()
        let peer = try peerSeal("peer")
        let taught = await teach(peer, from: connection, crypto)
        XCTAssertTrue(taught)
        var replayedKey = try crypto.encrypt(peer.keyFrame())
        replayedKey["ref"] = .string(String(repeating: "a", count: 40))
        replayedKey["historyReplay"] = true
        connection.receive(text: JSON.encode(replayedKey))
        var liveKey = try crypto.encrypt(peer.keyFrame())
        liveKey["ref"] = .string(String(repeating: "b", count: 40))
        connection.receive(text: JSON.encode(liveKey))
        var unreadable: JSONObject = ["kind": "cipher", "v": 1, "iv": "x", "data": "y"]
        unreadable["ref"] = .string(String(repeating: "c", count: 40))
        connection.receive(text: JSON.encode(unreadable))
        let chatRef = String(repeating: "d", count: 40)
        let plainChat: JSONObject = [
            "type": "chat", "id": "pat-1", "senderId": "pat-node", "sender": "pat",
            "channel": .string(conversation), "text": "hi", "ts": 1000,
        ]
        var chat = try crypto.encrypt(try peer.seal(plainChat))
        chat["ref"] = .string(chatRef)
        connection.receive(text: JSON.encode(chat))
        let acked = await waitUntil { !self.sentOfKind(connection, DirectMessageFrames.receivedKind).isEmpty }
        XCTAssertTrue(acked)
        try await Task.sleep(nanoseconds: 300_000_000)
        let receipts = sentOfKind(connection, DirectMessageFrames.receivedKind)
        XCTAssertEqual(receipts.count, 1)
        XCTAssertEqual(receipts[0]["refs"], .array([.string(chatRef)]))
        connection.receive(text: #"{"kind":"forkmesh-server-copies","v":1,"stored":0}"#)
        let cleared = await waitUntil { service.serverCopies(conversation) == 0 }
        XCTAssertTrue(cleared)
        await service.closeAll()
    }

    @MainActor func testAFrameNotWrappedForThisDeviceAsksForOneDurableAdvertisement() async throws {
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws)
        await service.refresh()
        let conversation = service.conversations[0]
        _ = await service.open(conversation)
        let connection = try XCTUnwrap(ws.lastConnection)
        let crypto = await roomCrypto()
        let advertised = await waitUntil { !self.keyAdverts(connection, crypto).isEmpty }
        XCTAssertTrue(advertised)
        let durableBefore = keyAdverts(connection, crypto).filter { $0["persist"] == .bool(true) }.count
        let stranger = try peerSeal("stranger")
        let plain: JSONObject = [
            "type": "chat", "id": "pat-1", "senderId": "pat-node", "sender": "pat",
            "channel": .string(conversation), "text": "sealed too early", "ts": 1000,
        ]
        var replayed = try crypto.encrypt(try stranger.seal(plain))
        replayed["ref"] = .string(String(repeating: "a", count: 40))
        replayed["historyReplay"] = true
        connection.receive(text: JSON.encode(replayed))
        let asked = await waitUntil {
            self.keyAdverts(connection, crypto).filter { $0["persist"] == .bool(true) }.count == durableBefore + 1
        }
        XCTAssertTrue(asked)
        var second = plain
        second["id"] = "pat-2"
        var live = try crypto.encrypt(try stranger.seal(second))
        live["ref"] = .string(String(repeating: "b", count: 40))
        connection.receive(text: JSON.encode(live))
        try await Task.sleep(nanoseconds: 300_000_000)
        XCTAssertEqual(
            keyAdverts(connection, crypto).filter { $0["persist"] == .bool(true) }.count, durableBefore + 1)
        XCTAssertTrue(sentOfKind(connection, DirectMessageFrames.receivedKind).isEmpty)
        XCTAssertTrue(service.messages(conversation).isEmpty)
        await service.closeAll()
    }

    @MainActor func testMessagesSealedBeforeThePeerKeyAreResealedAndSuperseded() async throws {
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws)
        await service.refresh()
        let conversation = service.conversations[0]
        _ = await service.open(conversation)
        let connection = try XCTUnwrap(ws.lastConnection)
        let crypto = await roomCrypto()
        let sent = await service.send(conversation, text: "before your key")
        XCTAssertTrue(sent)
        let originalText = try XCTUnwrap(connection.sentTexts.first { JSON.parseObject($0)["notify"] == .bool(true) })
        let originalRef = DirectMessageFrames.frameRef(originalText)
        XCTAssertEqual(originalRef.count, 40)
        let messageId = try XCTUnwrap(service.messages(conversation).first?.id)
        let peer = try peerSeal("peer")
        let taught = await teach(peer, from: connection, crypto)
        XCTAssertTrue(taught)
        let original = try XCTUnwrap(crypto.decrypt(JSON.parseObject(originalText)))
        XCTAssertNil(peer.unseal(original))
        let peerKey = JSON.encode(try crypto.encrypt(peer.keyFrame()))
        connection.receive(text: peerKey)
        let superseded = await waitUntil { !self.sentOfKind(connection, DirectMessageFrames.supersededKind).isEmpty }
        XCTAssertTrue(superseded)
        let resendText = try XCTUnwrap(connection.sentTexts.first { JSON.parseObject($0)["resend"] == .bool(true) })
        let resend = JSON.parseObject(resendText)
        XCTAssertEqual(resend["persist"], .bool(true))
        XCTAssertEqual(resend["notify"], .bool(true))
        let sealed = try XCTUnwrap(crypto.decrypt(resend))
        let opened = try XCTUnwrap(peer.unseal(sealed))
        XCTAssertEqual(opened["id"]?.text, messageId)
        XCTAssertEqual(opened["text"]?.text, "before your key")
        let first = sentOfKind(connection, DirectMessageFrames.supersededKind)
        XCTAssertEqual(first.count, 1)
        XCTAssertEqual(first[0]["refs"], .array([.string(originalRef)]))
        connection.receive(text: peerKey)
        try await Task.sleep(nanoseconds: 200_000_000)
        XCTAssertEqual(sentOfKind(connection, DirectMessageFrames.supersededKind).count, 1)
        let secondDevice = try peerSeal("peer-tablet", node: "pat-tablet")
        connection.receive(text: JSON.encode(try crypto.encrypt(secondDevice.keyFrame())))
        let pending = await waitUntil { service.pendingDevices(conversation).map(\.kid) == [secondDevice.keyId] }
        XCTAssertTrue(pending, "a second device of the same account waits for approval")
        try await Task.sleep(nanoseconds: 200_000_000)
        XCTAssertEqual(
            sentOfKind(connection, DirectMessageFrames.supersededKind).count, 1, "nothing is resealed to a pending device")
        await service.approveDevice(conversation, kid: secondDevice.keyId)
        let again = await waitUntil { self.sentOfKind(connection, DirectMessageFrames.supersededKind).count == 2 }
        XCTAssertTrue(again)
        XCTAssertEqual(
            sentOfKind(connection, DirectMessageFrames.supersededKind).last?["refs"],
            .array([.string(DirectMessageFrames.frameRef(resendText))]))
        connection.receive(text: #"{"kind":"forkmesh-server-copies","v":1,"stored":0}"#)
        let delivered = await waitUntil { service.serverCopies(conversation) == 0 }
        XCTAssertTrue(delivered)
        let thirdDevice = try peerSeal("peer-laptop", node: "pat-laptop")
        connection.receive(text: JSON.encode(try crypto.encrypt(thirdDevice.keyFrame())))
        let thirdPending = await waitUntil { service.pendingDevices(conversation).map(\.kid) == [thirdDevice.keyId] }
        XCTAssertTrue(thirdPending)
        await service.approveDevice(conversation, kid: thirdDevice.keyId)
        try await Task.sleep(nanoseconds: 200_000_000)
        XCTAssertEqual(sentOfKind(connection, DirectMessageFrames.supersededKind).count, 2)
        XCTAssertEqual(service.messages(conversation).map(\.text), ["before your key"])
        await service.closeAll()
    }

    @MainActor func testOwnFramesComingBackAreIgnored() async throws {
        let store = InMemorySecureStore()
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws, store: store)
        await service.refresh()
        let conversation = service.conversations[0]
        let earlier = try RoomSeal.loadOrCreate(
            DirectMessagesTests.room, scope: DirectMessagesTests.scope, policy: .strict, acct: "me", node: "me-node",
            secureStore: store)
        _ = await service.open(conversation)
        let connection = try XCTUnwrap(ws.lastConnection)
        let crypto = await roomCrypto()
        let peer = try peerSeal("peer")
        let taught = await teach(peer, from: connection, crypto)
        XCTAssertTrue(taught)
        connection.receive(text: JSON.encode(try crypto.encrypt(peer.keyFrame())))
        let known = await waitUntil { service.deviceKeys(conversation) == [peer.keyId] }
        XCTAssertTrue(known)
        let durableBefore = keyAdverts(connection, crypto).filter { $0["persist"] == .bool(true) }.count
        let sentBefore = connection.sentTexts.count
        let plain: JSONObject = [
            "type": "chat", "id": "me-node-1-1", "senderId": "me-node", "sender": "Me",
            "channel": .string(conversation), "text": "sent yesterday", "ts": .int(Date().epochMillis - 60_000),
        ]
        let sealed = try earlier.seal(plain)
        XCTAssertFalse(RoomSeal.wrappedKeyIds(sealed).contains(earlier.keyId), "a v4 frame has no entry for its sender")
        var own = try crypto.encrypt(sealed)
        own["persist"] = true
        own["notify"] = true
        own["ref"] = .string(String(repeating: "c", count: 40))
        own["historyReplay"] = true
        connection.receive(text: JSON.encode(own))
        var live = own
        live["ref"] = .string(String(repeating: "e", count: 40))
        live.removeValue(forKey: "historyReplay")
        connection.receive(text: JSON.encode(live))
        connection.receive(text: #"{"kind":"forkmesh-history-end"}"#)
        try await Task.sleep(nanoseconds: 400_000_000)
        XCTAssertTrue(sentOfKind(connection, DirectMessageFrames.receivedKind).isEmpty, "own frames are never acknowledged")
        XCTAssertTrue(sentOfKind(connection, DirectMessageFrames.supersededKind).isEmpty)
        XCTAssertEqual(
            keyAdverts(connection, crypto).filter { $0["persist"] == .bool(true) }.count, durableBefore,
            "and ask for no advertisement")
        XCTAssertFalse(
            connection.sentJSON.dropFirst(sentBefore).contains { RoomSeal.isSealedFrame(crypto.decrypt($0) ?? [:]) },
            "nothing is resealed from them")
        XCTAssertTrue(service.messages(conversation).isEmpty, "the device's own history already holds them")
        await service.closeAll()
    }

    @MainActor func testALostSessionSendsOneResetAndOneAdvertisement() async throws {
        let store = InMemorySecureStore()
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws, store: store)
        await service.refresh()
        let conversation = service.conversations[0]
        _ = await service.open(conversation)
        let connection = try XCTUnwrap(ws.lastConnection)
        let crypto = await roomCrypto()
        let peer = try peerSeal("peer")
        let taught = await teach(peer, from: connection, crypto)
        XCTAssertTrue(taught)
        connection.receive(text: JSON.encode(try crypto.encrypt(peer.keyFrame())))
        let known = await waitUntil { service.deviceKeys(conversation) == [peer.keyId] }
        XCTAssertTrue(known)
        func chat(_ id: String, _ text: String) -> JSONObject {
            [
                "type": "chat", "id": .string(id), "senderId": "pat-node", "sender": "pat",
                "channel": .string(conversation), "text": .string(text), "ts": .int(Date().epochMillis),
            ]
        }
        connection.receive(text: JSON.encode(try crypto.encrypt(try peer.seal(chat("pat-1", "one")))))
        let first = await waitUntil { service.messages(conversation).map(\.text) == ["one"] }
        XCTAssertTrue(first)
        let sent = await service.send(conversation, text: "reply")
        XCTAssertTrue(sent)
        let reply = try XCTUnwrap(
            connection.sentJSON.compactMap { crypto.decrypt($0) }.last { RoomSeal.isSealedFrame($0) })
        XCTAssertEqual(peer.open(reply).plain?["text"], "reply")
        let keyring = try ChatKeyring.shared(store)
        keyring.resetRatchet(DirectMessagesTests.room, peer.keyId)
        let durableBefore = keyAdverts(connection, crypto).filter { $0["persist"] == .bool(true) }.count
        var broken = try crypto.encrypt(try peer.seal(chat("pat-2", "two")))
        broken["ref"] = .string(String(repeating: "a", count: 40))
        connection.receive(text: JSON.encode(broken))
        let resets: () -> [JSONObject] = {
            connection.sentJSON.filter { envelope in
                guard let frame = crypto.decrypt(envelope), RoomSeal.isSealedFrame(frame) else { return false }
                return RoomSeal.wrappedKeyIds(frame) == [peer.keyId] && envelope["persist"] == .bool(true)
                    && envelope["notify"] == nil
            }
        }
        let asked = await waitUntil {
            resets().count == 1
                && self.keyAdverts(connection, crypto).filter { $0["persist"] == .bool(true) }.count == durableBefore + 1
        }
        XCTAssertTrue(asked, "one persisted reset sealed to that device and one persisted advertisement")
        var again = try crypto.encrypt(try peer.seal(chat("pat-3", "three")))
        again["ref"] = .string(String(repeating: "b", count: 40))
        connection.receive(text: JSON.encode(again))
        try await Task.sleep(nanoseconds: 300_000_000)
        XCTAssertEqual(resets().count, 1, "at most once a minute per device")
        XCTAssertTrue(sentOfKind(connection, DirectMessageFrames.receivedKind).isEmpty, "an unopened frame is not acked")
        let resetFrame = try XCTUnwrap(crypto.decrypt(try XCTUnwrap(resets().first)))
        let opened = peer.open(resetFrame)
        XCTAssertEqual(opened.plain?["type"], .string(ChatV4.resetType))
        XCTAssertEqual(opened.plain?["senderId"], "me-node")
        connection.receive(text: JSON.encode(try crypto.encrypt(try peer.seal(chat("pat-4", "four")))))
        let recovered = await waitUntil { service.messages(conversation).map(\.text).contains("four") }
        XCTAssertTrue(recovered, "the new session opens")
        XCTAssertFalse(service.messages(conversation).contains { $0.text.isEmpty })
        await service.closeAll()
    }

    @MainActor func testAnOldBrokenFrameReplayedAfterALaterOneOpenedStartsNoRecovery() async throws {
        let store = InMemorySecureStore()
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws, store: store)
        await service.refresh()
        let conversation = service.conversations[0]
        _ = await service.open(conversation)
        let connection = try XCTUnwrap(ws.lastConnection)
        let crypto = await roomCrypto()
        let peer = try peerSeal("peer")
        let taught = await teach(peer, from: connection, crypto)
        XCTAssertTrue(taught)
        connection.receive(text: JSON.encode(try crypto.encrypt(peer.keyFrame())))
        let known = await waitUntil { service.deviceKeys(conversation) == [peer.keyId] }
        XCTAssertTrue(known)
        let me = try ChatKeyring.shared(store).kid
        func chat(_ id: String, _ text: String) -> JSONObject {
            [
                "type": "chat", "id": .string(id), "senderId": "pat-node", "sender": "pat",
                "channel": .string(conversation), "text": .string(text), "ts": .int(Date().epochMillis),
            ]
        }
        connection.receive(text: JSON.encode(try crypto.encrypt(try peer.seal(chat("pat-1", "one")))))
        let first = await waitUntil { service.messages(conversation).map(\.text) == ["one"] }
        XCTAssertTrue(first)
        _ = await service.send(conversation, text: "reply")
        let reply = try XCTUnwrap(connection.sentJSON.compactMap { crypto.decrypt($0) }.last { RoomSeal.isSealedFrame($0) })
        XCTAssertEqual(peer.open(reply).plain?["text"], "reply")
        try ChatKeyring.shared(store).resetRatchet(DirectMessagesTests.room, peer.keyId)
        let broken = try peer.seal(chat("pat-2", "two"))
        peer.resetSessions(with: me)
        connection.receive(text: JSON.encode(try crypto.encrypt(try peer.seal(chat("pat-3", "three")))))
        let later = await waitUntil { service.messages(conversation).map(\.text).contains("three") }
        XCTAssertTrue(later, "a later frame on a new session opened")
        let durableBefore = keyAdverts(connection, crypto).filter { $0["persist"] == .bool(true) }.count
        let sentBefore = connection.sentTexts.count
        var replayed = try crypto.encrypt(broken)
        replayed["ref"] = .string(String(repeating: "a", count: 40))
        replayed["historyReplay"] = true
        connection.receive(text: JSON.encode(replayed))
        try await Task.sleep(nanoseconds: 400_000_000)
        XCTAssertEqual(
            keyAdverts(connection, crypto).filter { $0["persist"] == .bool(true) }.count, durableBefore,
            "no advertisement")
        XCTAssertFalse(
            connection.sentJSON.dropFirst(sentBefore).contains { RoomSeal.isSealedFrame(crypto.decrypt($0) ?? [:]) },
            "no reset frame")
        XCTAssertFalse(service.messages(conversation).contains { $0.text == "two" })
        connection.receive(text: JSON.encode(try crypto.encrypt(try peer.seal(chat("pat-4", "four")))))
        let still = await waitUntil { service.messages(conversation).map(\.text).contains("four") }
        XCTAssertTrue(still, "the working session was not reset")
        await service.closeAll()
    }

    @MainActor func testAResetResealsTheOutboxToThatDeviceOnly() async throws {
        let store = InMemorySecureStore()
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws, store: store)
        await service.refresh()
        let conversation = service.conversations[0]
        _ = await service.open(conversation)
        let connection = try XCTUnwrap(ws.lastConnection)
        let crypto = await roomCrypto()
        let peer = try peerSeal("peer")
        let taught = await teach(peer, from: connection, crypto)
        XCTAssertTrue(taught)
        connection.receive(text: JSON.encode(try crypto.encrypt(peer.keyFrame())))
        let known = await waitUntil { service.deviceKeys(conversation) == [peer.keyId] }
        XCTAssertTrue(known)
        let sent = await service.send(conversation, text: "lost on the way")
        XCTAssertTrue(sent)
        let messageId = try XCTUnwrap(service.messages(conversation).first?.id)
        let before = connection.sentTexts.count
        let reset = try peer.seal(["type": .string(ChatV4.resetType), "senderId": "pat-node"], for: [try ChatKeyring.shared(store).kid])
        var envelope = try crypto.encrypt(reset)
        envelope["ref"] = .string(String(repeating: "f", count: 40))
        connection.receive(text: JSON.encode(envelope))
        let resent = await waitUntil { connection.sentJSON.contains { $0["resend"] == .bool(true) } }
        XCTAssertTrue(resent)
        let resend = try XCTUnwrap(connection.sentJSON.first { $0["resend"] == .bool(true) })
        XCTAssertEqual(resend["persist"], .bool(true))
        XCTAssertEqual(resend["notify"], .bool(true))
        let frame = try XCTUnwrap(crypto.decrypt(resend))
        XCTAssertEqual(RoomSeal.wrappedKeyIds(frame), [peer.keyId])
        XCTAssertEqual(peer.open(frame).plain?["id"]?.text, messageId, "the same message id")
        try await Task.sleep(nanoseconds: 300_000_000)
        XCTAssertTrue(
            sentOfKind(connection, DirectMessageFrames.supersededKind).isEmpty,
            "the original still serves the other devices, so nothing is superseded")
        XCTAssertGreaterThan(connection.sentTexts.count, before)
        XCTAssertEqual(service.messages(conversation).map(\.text), ["lost on the way"], "the reset is not shown")
        await service.closeAll()
    }

    @MainActor func testAResentMessageIsShownOnce() async throws {
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws)
        var live: [ChatMessage] = []
        service.onMessage = { live.append($0) }
        await service.refresh()
        let conversation = service.conversations[0]
        _ = await service.open(conversation)
        let connection = try XCTUnwrap(ws.lastConnection)
        let crypto = await roomCrypto()
        let peer = try peerSeal("peer")
        let taught = await teach(peer, from: connection, crypto)
        XCTAssertTrue(taught)
        connection.receive(text: JSON.encode(try crypto.encrypt(peer.keyFrame())))
        let plain: JSONObject = [
            "type": "chat", "id": "pat-9", "senderId": "pat-node", "sender": "pat",
            "channel": .string(conversation), "text": "once", "ts": 1000,
        ]
        connection.receive(text: JSON.encode(try crypto.encrypt(try peer.seal(plain))))
        let shown = await waitUntil { service.messages(conversation).map(\.text) == ["once"] }
        XCTAssertTrue(shown)
        let resendRef = String(repeating: "e", count: 40)
        var resend = try crypto.encrypt(try peer.seal(plain))
        resend["resend"] = true
        resend["ref"] = .string(resendRef)
        connection.receive(text: JSON.encode(resend))
        let acked = await waitUntil {
            self.sentOfKind(connection, DirectMessageFrames.receivedKind).contains {
                $0["refs"] == .array([.string(resendRef)])
            }
        }
        XCTAssertTrue(acked)
        XCTAssertEqual(service.messages(conversation).map(\.text), ["once"])
        XCTAssertEqual(live.map(\.id), ["pat-9"])
        await service.closeAll()
    }

    @MainActor func testUnreadDirectMessagesGetASocketWithoutBeingOpened() async throws {
        let quiet = String(repeating: "e", count: 32)
        let api = preparedApi()
        api.replies["GET /api/chat/direct-messages"] = [
            "conversations": [
                ["id": .string(DirectMessagesTests.conversationId), "otherUser": "pat", "unreadCount": 1],
                ["id": .string(quiet), "otherUser": "sam", "unreadCount": 0],
            ]
        ]
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws)
        await service.refresh()
        let watched = await waitUntil { service.isConnected("direct:\(DirectMessagesTests.conversationId)") }
        XCTAssertTrue(watched)
        try await Task.sleep(nanoseconds: 200_000_000)
        XCTAssertEqual(ws.connections.count, 1)
        XCTAssertFalse(service.isConnected("direct:\(quiet)"))
        XCTAssertEqual(service.unreadTotal, 1)
        await service.closeAll()
    }

    @MainActor func testDroppedSocketsReconnectWithBackoffAndOnResume() async throws {
        let delays = DirectReconnectDelays()
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(
            api: api, ws: ws,
            sleep: { seconds in
                delays.add(seconds)
                try await Task.sleep(nanoseconds: 1_000_000)
            })
        await service.refresh()
        let conversation = service.conversations[0]
        _ = await service.open(conversation)
        XCTAssertEqual(ws.connections.count, 1)
        ws.failNext()
        ws.connections[0].serverClose()
        let back = await waitUntil { ws.connections.count == 2 && service.isConnected(conversation) }
        XCTAssertTrue(back)
        XCTAssertEqual(delays.values, [1, 2])
        XCTAssertEqual(api.calls.filter { $0.1.hasSuffix("/room-access") }.count, 3)
        service.appLifecycleChanged(.paused)
        ws.connections[1].fail()
        let lost = await waitUntil { !service.isConnected(conversation) }
        XCTAssertTrue(lost)
        try await Task.sleep(nanoseconds: 200_000_000)
        XCTAssertEqual(ws.connections.count, 2)
        service.appLifecycleChanged(.resumed)
        let resumed = await waitUntil { ws.connections.count == 3 && service.isConnected(conversation) }
        XCTAssertTrue(resumed)
        XCTAssertEqual(delays.values, [1, 2])
        await service.closeAll()
        ws.connections[2].serverClose()
        try await Task.sleep(nanoseconds: 200_000_000)
        XCTAssertEqual(ws.connections.count, 3)
    }

    @MainActor func testResumeReopensActiveDirectRoomsForHistoryReplay() async throws {
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws)
        await service.refresh()
        let conversation = service.conversations[0]
        _ = await service.open(conversation)
        XCTAssertEqual(ws.connections.count, 1)

        service.appLifecycleChanged(.paused)
        service.appLifecycleChanged(.resumed)

        let reopened = await waitUntil { ws.connections.count == 2 && service.isConnected(conversation) }
        XCTAssertTrue(reopened)
        XCTAssertEqual(api.calls.filter { $0.1.hasSuffix("/room-access") }.count, 2)
        await service.closeAll()
    }

    @MainActor func testAQuickInactiveStepKeepsDirectRoomsOpen() async throws {
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws)
        await service.refresh()
        let conversation = service.conversations[0]
        _ = await service.open(conversation)
        XCTAssertEqual(ws.connections.count, 1)

        service.appLifecycleChanged(.inactive)
        service.appLifecycleChanged(.resumed)
        service.appLifecycleChanged(.resumed)

        try await Task.sleep(nanoseconds: 100_000_000)
        XCTAssertEqual(ws.connections.count, 1, "closing Control Center does not reopen the rooms")
        XCTAssertTrue(service.isConnected(conversation))
        XCTAssertEqual(api.calls.filter { $0.1.hasSuffix("/room-access") }.count, 1)
        await service.closeAll()
    }

    @MainActor func testStartAndReadMarksUseTheServerContract() async throws {
        let api = FakeAccountApi()
        api.replies["POST /api/chat/direct-messages"] = [
            "ok": true,
            "conversation": ["id": .string(DirectMessagesTests.conversationId), "otherUser": "pat"],
        ]
        let service = makeService(api: api, ws: FakeWebSocketConnector())
        let started = try await service.start(username: " Pat ")
        XCTAssertEqual(started.otherUser, "pat")
        XCTAssertEqual(service.records.map(\.otherUser), ["pat"])
        XCTAssertEqual(api.calls.first?.2?["username"], "pat")
        service.bumpUnread(started.conversation)
        XCTAssertEqual(service.unreadTotal, 1)
        await service.markRead(started.conversation)
        XCTAssertEqual(service.unreadTotal, 0)
        XCTAssertTrue(
            api.calls.contains {
                $0.0 == "POST" && $0.1 == "/api/chat/direct-messages/\(DirectMessagesTests.conversationId)/read"
            })
        api.failures["POST /api/chat/direct-messages"] = OrgApiException("rate_limited", "slow down")
        do {
            _ = try await service.start(username: "sam")
            XCTFail("expected a refusal")
        } catch let error as DirectMessageError {
            XCTAssertEqual(error, .refused("rate_limited"))
            XCTAssertEqual(error.description, "Too many new conversations. Try again later.")
        }
    }

    @MainActor func testHubRoutesDirectConversations() async throws {
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
        XCTAssertEqual(hub.title, "@pat")
        XCTAssertTrue(hub.isPrivateRoom)
        XCTAssertTrue(hub.inDirect)
        XCTAssertEqual(hub.badge, "end-to-end")
        XCTAssertTrue(hub.headerDetail.hasPrefix("Direct message"))
        XCTAssertTrue(hub.introLine.contains("pat"))
        let opened = await waitUntil { service.isConnected(conversation) }
        XCTAssertTrue(opened)
        let sent = await hub.sendChatText("hello there")
        XCTAssertTrue(sent)
        let delivered = await waitUntil { hub.messages.map(\.text) == ["hello there"] }
        XCTAssertTrue(delivered)
        XCTAssertTrue(
            api.calls.contains {
                $0.0 == "POST" && $0.1 == "/api/chat/direct-messages/\(DirectMessagesTests.conversationId)/read"
            })
        await service.closeAll()
        await harness.close()
    }

    @MainActor func testTappingAPersonOpensTheirAccountDirectMessageNotTheRelayPeerRoom() async throws {
        let api = FakeAccountApi()
        api.replies["POST /api/chat/direct-messages"] = [
            "ok": true,
            "conversation": ["id": .string(DirectMessagesTests.conversationId), "otherUser": "pat"],
        ]
        let service = makeService(api: api, ws: FakeWebSocketConnector())
        let harness = try await RelayHarness.start()
        let mesh = LocalMeshService(
            settings: harness.device.settings, identity: harness.device.identity, secureStore: harness.device.secure)
        let hub = ChatHub(relay: harness.relay, mesh: mesh)
        hub.direct = service
        let group = MemberGroup(
            name: "Pat Smith", members: [Member(id: "pat-phone", name: "Pat Smith", online: true, owner: "Pat")])
        let person = ChatHub.recipient(for: group)
        XCTAssertEqual(person.account, "pat")
        XCTAssertEqual(person.peerId, "pat-phone")
        let opened = await hub.openPerson(person)
        XCTAssertTrue(opened)
        XCTAssertEqual(hub.currentConversation, service.conversations.first)
        XCTAssertTrue(DirectMessageFrames.isDirect(hub.currentConversation))
        XCTAssertEqual(api.calls.first { $0.0 == "POST" }?.2?["username"], "pat")
        await service.closeAll()
        await harness.close()
    }

    @MainActor func testMessagingAUsernameFromOutsideChatOpensTheirAccountDirectMessage() async throws {
        let api = FakeAccountApi()
        api.replies["POST /api/chat/direct-messages"] = [
            "ok": true,
            "conversation": ["id": .string(DirectMessagesTests.conversationId), "otherUser": "pat"],
        ]
        let service = makeService(api: api, ws: FakeWebSocketConnector())
        let harness = try await RelayHarness.start()
        let mesh = LocalMeshService(
            settings: harness.device.settings, identity: harness.device.identity, secureStore: harness.device.secure)
        let hub = ChatHub(relay: harness.relay, mesh: mesh)
        hub.direct = service
        hub.switchConversation("@Pat")
        let opened = await waitUntil { DirectMessageFrames.isDirect(hub.currentConversation) }
        XCTAssertTrue(opened)
        XCTAssertEqual(hub.title, "@pat")
        XCTAssertEqual(hub.revealRequests, 1)
        XCTAssertEqual(
            api.calls.filter { $0.0 == "POST" && $0.1 == "/api/chat/direct-messages" }.map { $0.2?["username"] }, ["pat"])
        hub.select("#general")
        hub.switchConversation("@pat")
        XCTAssertEqual(hub.currentConversation, service.conversations.first)
        XCTAssertEqual(api.calls.filter { $0.0 == "POST" && $0.1 == "/api/chat/direct-messages" }.count, 1)
        await service.closeAll()
        await harness.close()
    }

    @MainActor func testImagesTravelSealedInDirectMessages() async throws {
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
        XCTAssertTrue(hub.canSendMedia)
        let opened = await waitUntil { service.isConnected(conversation) }
        XCTAssertTrue(opened)
        let connection = try XCTUnwrap(ws.lastConnection)
        let crypto = await roomCrypto()
        let peer = try peerSeal("peer")
        connection.receive(text: JSON.encode(try crypto.encrypt(peer.keyFrame())))
        let taught = await teach(peer, from: connection, crypto)
        XCTAssertTrue(taught)
        let learned = await waitUntil { service.deviceKeys(conversation).contains(peer.keyId) }
        XCTAssertTrue(learned)

        let image = Data([0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 4])
        let sent = await hub.sendMedia(fileName: "photo.jpg", mime: "image/jpeg", data: image)
        XCTAssertTrue(sent)
        let mine = try XCTUnwrap(service.messages(conversation).last)
        XCTAssertTrue(mine.isSelf)
        XCTAssertEqual(mine.fileName, "photo.jpg")
        XCTAssertEqual(hub.extras.attachment(for: mine.id)?.data, image)
        let delivered = try XCTUnwrap(
            connection.sentTexts.compactMap { crypto.decrypt(JSON.parseObject($0)) }
                .compactMap { RoomSeal.isSealedFrame($0) ? peer.unseal($0) : nil }
                .first { $0["id"]?.text == mine.id })
        XCTAssertNil(delivered["text"])
        XCTAssertEqual(delivered["fileMime"]?.text, "image/jpeg")
        XCTAssertEqual(delivered["file"]?.text, image.base64EncodedString())

        let tooLarge = await hub.sendMedia(
            fileName: "big.jpg", mime: "image/jpeg", data: Data(count: DirectMessageFrames.maxFileBytes + 1))
        XCTAssertFalse(tooLarge)
        XCTAssertFalse(hub.sendAttachment(fileName: "notes.txt", mime: "text/plain", data: Data("hi".utf8)))

        let pasted = Data([0x89, 0x50, 0x4E, 0x47, 9, 9])
        let fromPat: JSONObject = [
            "type": "chat", "id": "pat-image", "senderId": "pat-node", "sender": "pat", "accountKind": "user",
            "channel": .string(conversation), "fileName": "pasted-image.png", "fileMime": "image/png",
            "file": .string(pasted.base64EncodedString()), "ts": .int(Int64(Date().timeIntervalSince1970 * 1000)),
        ]
        connection.receive(text: JSON.encode(try crypto.encrypt(try peer.seal(fromPat))))
        let received = await waitUntil { hub.extras.attachment(for: "pat-image") != nil }
        XCTAssertTrue(received)
        XCTAssertEqual(hub.extras.attachment(for: "pat-image")?.data, pasted)
        XCTAssertEqual(service.messages(conversation).first { $0.id == "pat-image" }?.fileName, "pasted-image.png")
        await service.closeAll()
        await harness.close()
    }

    @MainActor func testInjectedDevicesAreDroppedOrHeldUntilApproved() async throws {
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws)
        var events: [DirectTrustEvent] = []
        service.onTrustEvent = { events.append($0) }
        var alerts: [ChatMessage] = []
        service.onMessage = { alerts.append($0) }
        await service.refresh()
        let conversation = service.conversations[0]
        _ = await service.open(conversation)
        let connection = try XCTUnwrap(ws.lastConnection)
        let crypto = await roomCrypto()
        let pat = try peerSeal("peer")
        _ = await teach(pat, from: connection, crypto)
        connection.receive(text: JSON.encode(try crypto.encrypt(pat.keyFrame())))
        let known = await waitUntil { service.deviceKeys(conversation) == [pat.keyId] }
        XCTAssertTrue(known)
        let first = try XCTUnwrap(service.safetyNumber(conversation))
        XCTAssertEqual(first.digits.count, 60)
        XCTAssertFalse(first.changed)

        let outsider = try peerSeal("outsider", acct: "mallory", node: "mallory-node")
        connection.receive(text: JSON.encode(try crypto.encrypt(outsider.keyFrame())))
        let injected = try peerSeal("injected", node: "pat-new")
        _ = await teach(injected, from: connection, crypto)
        connection.receive(text: JSON.encode(try crypto.encrypt(injected.keyFrame())))
        let pending = await waitUntil { service.pendingDevices(conversation).map(\.kid) == [injected.keyId] }
        XCTAssertTrue(pending)
        XCTAssertFalse(service.devices(conversation).contains { $0.kid == outsider.keyId }, "an outsider is dropped")
        XCTAssertEqual(events, [.pendingDevice(conversation: conversation, kid: injected.keyId)])
        XCTAssertEqual(service.pendingDevices(conversation).first?.acct, "pat")
        XCTAssertEqual(service.devices(conversation).filter(\.isSelf).count, 1)

        let plain: JSONObject = [
            "type": "chat", "id": "new-1", "senderId": "pat-new", "sender": "pat", "channel": .string(conversation),
            "text": "trust me", "ts": .int(Date().epochMillis),
        ]
        connection.receive(text: JSON.encode(try crypto.encrypt(try injected.seal(plain))))
        _ = await waitUntil(0.3) { false }
        XCTAssertTrue(service.messages(conversation).isEmpty, "a pending device's frames are held, not shown")
        let reply = await service.send(conversation, text: "hi pat")
        XCTAssertTrue(reply)
        let sealedToInjected = connection.sentTexts.compactMap { crypto.decrypt(JSON.parseObject($0)) }
            .contains { RoomSeal.isSealedFrame($0) && RoomSeal.wrappedKeyIds($0).contains(injected.keyId) }
        XCTAssertFalse(sealedToInjected, "nothing is sealed to a pending device")

        await service.approveDevice(conversation, kid: injected.keyId)
        XCTAssertTrue(service.messages(conversation).contains { $0.text == "trust me" }, "approval opens the held frame")
        XCTAssertEqual(alerts.map(\.text), ["trust me"])
        let changed = try XCTUnwrap(service.safetyNumber(conversation))
        XCTAssertTrue(changed.changed)
        XCTAssertNotEqual(changed.digits, first.digits)
        XCTAssertTrue(events.contains(.safetyNumberChanged(conversation: conversation)))
        XCTAssertTrue(service.verifySafetyCode(conversation, scanned: changed.qr))
        XCTAssertEqual(service.safetyNumber(conversation)?.verified, true)
        XCTAssertEqual(service.safetyNumber(conversation)?.changed, false)

        await service.blockDevice(conversation, kid: injected.keyId)
        XCTAssertEqual(service.devices(conversation).first { $0.kid == injected.keyId }?.state, .blocked)
        XCTAssertFalse(service.deviceKeys(conversation).contains(injected.keyId))
        await service.closeAll()
    }

    @MainActor func testUnreadCountsFeedTheShellBadge() async throws {
        let api = preparedApi()
        let service = makeService(api: api, ws: FakeWebSocketConnector())
        await service.refresh()
        let harness = try await RelayHarness.start()
        let mesh = LocalMeshService(
            settings: harness.device.settings, identity: harness.device.identity, secureStore: harness.device.secure)
        let hub = ChatHub(relay: harness.relay, mesh: mesh)
        hub.direct = service
        XCTAssertEqual(hub.totalUnread, 2)
        XCTAssertEqual(hub.unreadConversations, ["direct:\(DirectMessagesTests.conversationId)"])
        await harness.close()
    }
}

final class DirectReconnectDelays: @unchecked Sendable {
    private let lock = NSLock()
    private var recorded: [TimeInterval] = []

    func add(_ value: TimeInterval) {
        lock.lock()
        recorded.append(value)
        lock.unlock()
    }

    var values: [TimeInterval] {
        lock.lock()
        defer { lock.unlock() }
        return recorded
    }
}
