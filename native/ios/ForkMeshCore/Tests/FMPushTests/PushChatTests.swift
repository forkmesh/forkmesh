import FMBase
import FMCrypto
import Foundation
import XCTest

@testable import FMPush

final class PushChatTests: XCTestCase {
    static let directId = String(repeating: "d", count: 32)
    static let channelId = String(repeating: "c", count: 32)
    static let ref = String(repeating: "a", count: 40)
    static let room = "chat-direct:\(directId):v2"
    static let channelRoom = "chat-channel:\(channelId):v1"
    static let passphrase = "pending-pass"

    func directPayload(_ overrides: JSONObject = [:]) -> JSONObject {
        var payload: JSONObject = [
            "topic": "direct-messages", "conversation": .string("direct:\(PushChatTests.directId)"),
            "ref": .string(PushChatTests.ref), "keyVersion": 2,
        ]
        for (key, value) in overrides { payload[key] = value }
        return payload
    }

    func testRequestsNeedARefAndAConversationOrChannel() throws {
        let direct = try XCTUnwrap(PushChatRequest.parse(directPayload()))
        XCTAssertEqual(direct.kind, .direct(PushChatTests.directId))
        XCTAssertEqual(direct.keyVersion, 2)
        XCTAssertEqual(direct.trustScope, "direct:\(PushChatTests.directId)")
        XCTAssertEqual(
            direct.pendingPath, "/api/chat/direct-messages/\(PushChatTests.directId)/pending?ref=\(PushChatTests.ref)")
        XCTAssertEqual(
            direct.deliveredPath,
            "/api/chat/direct-messages/\(PushChatTests.directId)/delivered?ref=\(PushChatTests.ref)")
        XCTAssertEqual(direct.threadIdentifier, "direct:\(PushChatTests.directId)")
        let channel = try XCTUnwrap(
            PushChatRequest.parse(["topic": "chat", "channelId": .string(PushChatTests.channelId), "ref": .string(PushChatTests.ref)]))
        XCTAssertEqual(channel.kind, .channel(PushChatTests.channelId))
        XCTAssertEqual(channel.pendingPath, "/api/chat/channels/\(PushChatTests.channelId)/pending?ref=\(PushChatTests.ref)")
        XCTAssertNil(channel.deliveredPath)
        XCTAssertEqual(channel.threadIdentifier, "channel:\(PushChatTests.channelId)")
        XCTAssertNil(PushChatRequest.parse(directPayload(["ref": "ABC"])))
        XCTAssertNil(PushChatRequest.parse(directPayload(["ref": .string(String(repeating: "A", count: 40))])))
        XCTAssertNil(PushChatRequest.parse(directPayload(["conversation": "direct:../../x"])))
        XCTAssertNil(PushChatRequest.parse(["ref": .string(PushChatTests.ref), "channelId": "short"]))
        XCTAssertNil(PushChatRequest.parse(["topic": "chat"]))
    }

    func testPreviewModesParseWithNameAndMessageAsTheDefault() {
        XCTAssertEqual(PushPreviewMode.parse(nil), .nameAndMessage)
        XCTAssertEqual(PushPreviewMode.parse("bogus"), .nameAndMessage)
        XCTAssertEqual(PushPreviewMode.parse("name-only"), .nameOnly)
        XCTAssertEqual(PushPreviewMode.parse("none"), .hidden)
        XCTAssertEqual(PushPreviewMode.allCases.map(\.title), ["Name and message", "Name only", "No name or message"])
    }

    static var scope: String { "direct:\(directId)" }

    struct Pair {
        let mine: RoomSeal
        let peer: RoomSeal
        let store: InMemorySecureStore
    }

    func pair(introduced: Bool = true) throws -> Pair {
        let store = InMemorySecureStore()
        let mine = try RoomSeal.loadOrCreate(
            PushChatTests.room, scope: PushChatTests.scope, policy: .strict, acct: "me", node: "me-node", secureStore: store)
        let peer = try RoomSeal.loadOrCreate(
            PushChatTests.room, scope: PushChatTests.scope, policy: .strict, acct: "pat", node: "pat-node",
            secureStore: InMemorySecureStore())
        peer.acceptBundle(mine.keyFrame())
        if introduced { mine.acceptBundle(peer.keyFrame()) }
        return Pair(mine: mine, peer: peer, store: store)
    }

    func testASealedDirectMessageOpensOnlyWithThisDevicesKeys() throws {
        let keysPair = try pair()
        let plain: JSONObject = [
            "type": "chat", "id": "pat-1", "senderId": "pat-node", "sender": "Pat",
            "channel": .string("direct:\(PushChatTests.directId)"), "text": "see you at 6", "ts": 1,
        ]
        let key = RoomCrypto.derivedKey(PushChatTests.room, passphrase: PushChatTests.passphrase)
        let crypto = try XCTUnwrap(RoomCrypto.withKey(key))
        let envelope = try crypto.encrypt(try keysPair.peer.seal(plain))
        let pending: JSONObject = [
            "conversation": .string("direct:\(PushChatTests.directId)"), "room": .string(PushChatTests.room), "keyVersion": 2,
            "passphrase": .string(PushChatTests.passphrase), "frame": .string(JSON.encode(envelope)),
        ]
        let store = InMemorySharedItemStore()
        let keys = PushRoomKeys(store: store)
        keys.remember(key, room: PushChatTests.room, passphrase: PushChatTests.passphrase)
        let request = try XCTUnwrap(PushChatRequest.parse(directPayload()))
        let before = keysPair.store.readAll()
        let opened = try XCTUnwrap(PushChatOpener.open(pending, request: request, secureStore: keysPair.store, keys: keys).message)
        XCTAssertEqual(opened.id, "pat-1")
        XCTAssertEqual(opened.sender, "Pat")
        XCTAssertEqual(opened.senderId, "pat-node")
        XCTAssertEqual(opened.text, "see you at 6")
        XCTAssertFalse(opened.isReply)
        XCTAssertEqual(keysPair.store.readAll(), before, "the extension never writes the shared keychain")
        XCTAssertNotNil(
            PushChatOpener.open(pending, request: request, secureStore: keysPair.store, keys: keys).message,
            "so the app still opens the same frame afterwards")
        XCTAssertEqual(
            PushChatOpener.open(pending, request: request, secureStore: InMemorySecureStore(), keys: keys), .unavailable)
        XCTAssertEqual(PushChatOpener.open(pending, request: request, secureStore: nil, keys: keys), .unavailable)
        var wrongRoom = pending
        wrongRoom["conversation"] = .string("direct:" + String(repeating: "e", count: 32))
        XCTAssertEqual(PushChatOpener.open(wrongRoom, request: request, secureStore: keysPair.store, keys: keys), .unavailable)
        var unsealed = pending
        unsealed["frame"] = .string(JSON.encode(try crypto.encrypt(plain)))
        XCTAssertEqual(
            PushChatOpener.open(unsealed, request: request, secureStore: keysPair.store, keys: keys), .unavailable,
            "a direct message must be sealed")
        XCTAssertEqual(
            PushPreviewText.preview(opened, request: request, mode: .nameAndMessage), PushPreview(title: "Pat", body: "see you at 6"))
        XCTAssertEqual(
            PushPreviewText.preview(opened, request: request, mode: .nameOnly), PushPreview(title: "Pat", body: "New message"))
        XCTAssertEqual(
            PushPreviewText.preview(opened, request: request, mode: .hidden), PushPreview(title: "ForkMesh", body: "New message"))
        XCTAssertNil(PushPreviewText.preview(nil as PushChatMessage?, request: request, mode: .nameOnly), "the server names no one")
        XCTAssertNil(PushPreviewText.preview(nil as PushChatMessage?, request: request, mode: .nameAndMessage))
        let sealed = try XCTUnwrap(crypto.decrypt(envelope))
        XCTAssertEqual(sealed["v"], .int(ChatV4.version), "direct messages are v4 ratchet frames")
        XCTAssertEqual(keysPair.mine.open(sealed).plain?["text"], "see you at 6", "the app opens what the extension peeked")
        XCTAssertEqual(
            PushChatOpener.open(pending, request: request, secureStore: keysPair.store, keys: keys), .unavailable,
            "once the app used the key, the frame is a replay")
    }

    func testAFrameFromAnUnknownDeviceShowsNewMessage() throws {
        let keysPair = try pair(introduced: false)
        let key = RoomCrypto.derivedKey(PushChatTests.room, passphrase: PushChatTests.passphrase)
        let crypto = try XCTUnwrap(RoomCrypto.withKey(key))
        let plain: JSONObject = [
            "type": "chat", "id": "x-1", "senderId": "pat-node", "sender": "Pat",
            "channel": .string("direct:\(PushChatTests.directId)"), "text": "who am I", "ts": 1,
        ]
        let pending: JSONObject = [
            "conversation": .string("direct:\(PushChatTests.directId)"), "room": .string(PushChatTests.room),
            "passphrase": .string(PushChatTests.passphrase),
            "frame": .object(try crypto.encrypt(try keysPair.peer.seal(plain))),
        ]
        let request = try XCTUnwrap(PushChatRequest.parse(directPayload()))
        let opened = PushChatOpener.open(
            pending, request: request, secureStore: keysPair.store, keys: PushRoomKeys(store: InMemorySharedItemStore()))
        XCTAssertEqual(opened, .unknownSender)
        XCTAssertEqual(PushPreviewText.preview(opened, request: request, mode: .nameAndMessage), PushPreview(title: "New message", body: ""))
        XCTAssertEqual(PushPreviewText.preview(opened, request: request, mode: .nameOnly), PushPreviewText.unknownSender)
        XCTAssertEqual(
            PushPreviewText.preview(opened, request: request, mode: .hidden), PushPreview(title: "ForkMesh", body: "New message"))
        XCTAssertNil(PushPreviewText.preview(PushChatOpened.unavailable, request: request, mode: .nameAndMessage))
    }

    func testADirectMessageOpensFromThePushedFrameOnceTheRelayCopyIsGone() throws {
        let keysPair = try pair()
        let plain: JSONObject = [
            "type": "chat", "id": "pat-2", "senderId": "pat-node", "sender": "Pat",
            "channel": .string("direct:\(PushChatTests.directId)"), "text": "running late", "ts": 2,
        ]
        let key = RoomCrypto.derivedKey(PushChatTests.room, passphrase: PushChatTests.passphrase)
        let crypto = try XCTUnwrap(RoomCrypto.withKey(key))
        let envelope = try crypto.encrypt(try keysPair.peer.seal(plain))
        let gone: JSONObject = [
            "conversation": .string("direct:\(PushChatTests.directId)"), "room": .string(PushChatTests.room), "keyVersion": 2,
            "passphrase": .string(PushChatTests.passphrase), "frame": "",
        ]
        let payload = directPayload(["frame": .object(envelope)])
        let request = try XCTUnwrap(PushChatRequest.parse(payload))
        let keys = PushRoomKeys(store: InMemorySharedItemStore())
        XCTAssertEqual(PushChatOpener.open(gone, request: request, secureStore: keysPair.store, keys: keys), .unavailable)
        let merged = PushChatOpener.withPushedFrame(gone, pushed: payload)
        let opened = try XCTUnwrap(PushChatOpener.open(merged, request: request, secureStore: keysPair.store, keys: keys).message)
        XCTAssertEqual(opened.text, "running late")
        XCTAssertEqual(
            PushPreviewText.preview(opened, request: request, mode: .nameAndMessage), PushPreview(title: "Pat", body: "running late"))
        var stored = gone
        stored["frame"] = .string(JSON.encode(envelope))
        XCTAssertEqual(PushChatOpener.withPushedFrame(stored, pushed: directPayload(["frame": ["kind": "other"]])), stored)
        XCTAssertEqual(PushChatOpener.withPushedFrame(gone, pushed: directPayload()), gone)
    }

    func testAPushedFrameOpensOnThePhoneOnceTheRoomKeyIsCached() throws {
        let keysPair = try pair()
        let plain: JSONObject = [
            "type": "chat", "id": "pat-3", "senderId": "pat-node", "sender": "Pat",
            "channel": .string("direct:\(PushChatTests.directId)"), "text": "on my way", "ts": 3,
        ]
        let key = RoomCrypto.derivedKey(PushChatTests.room, passphrase: PushChatTests.passphrase)
        let crypto = try XCTUnwrap(RoomCrypto.withKey(key))
        let payload = directPayload(["frame": .object(try crypto.encrypt(try keysPair.peer.seal(plain)))])
        let request = try XCTUnwrap(PushChatRequest.parse(payload))
        XCTAssertEqual(request.directRoom, PushChatTests.room)
        let store = InMemorySharedItemStore()
        let keys = PushRoomKeys(store: store)
        XCTAssertEqual(
            PushChatOpener.openPushed(payload, request: request, secureStore: keysPair.store, keys: keys), .unavailable,
            "no cached key yet: the phone asks the relay")
        keys.remember(key, room: PushChatTests.room, passphrase: PushChatTests.passphrase)
        let opened = try XCTUnwrap(
            PushChatOpener.openPushed(payload, request: request, secureStore: keysPair.store, keys: keys).message)
        XCTAssertEqual(opened.text, "on my way")
        XCTAssertEqual(opened.sender, "Pat")
        let rotated = try XCTUnwrap(PushChatRequest.parse(directPayload(["keyVersion": 3, "frame": payload["frame"]!])))
        XCTAssertEqual(
            PushChatOpener.openPushed(payload, request: rotated, secureStore: keysPair.store, keys: keys), .unavailable,
            "a new key version is a new room")
        XCTAssertEqual(
            PushChatOpener.openPushed(directPayload(), request: request, secureStore: keysPair.store, keys: keys), .unavailable)
        let raw = String(decoding: try XCTUnwrap(store.data(PushRoomKeys.item)), as: UTF8.self)
        XCTAssertFalse(raw.contains(PushChatTests.directId), "the room is stored only as a hash")
    }

    func testPhotosVoiceAndViewOnceMessagesSayWhatArrived() throws {
        let request = try XCTUnwrap(PushChatRequest.parse(directPayload()))
        let channel = JSONValue.string("direct:\(PushChatTests.directId)")
        func body(_ extra: JSONObject) throws -> String {
            var plain: JSONObject = ["type": "chat", "id": "m", "channel": channel, "sender": "Pat", "text": ""]
            for (key, value) in extra { plain[key] = value }
            return PushPreviewText.body(try XCTUnwrap(PushChatOpener.message(plain, request: request)))
        }
        XCTAssertEqual(try body(["fileName": "IMG_1.jpg", "fileMime": "image/jpeg"]), "\u{1F4F7} Photo")
        XCTAssertEqual(try body(["fileName": "voice.m4a", "fileMime": "audio/mp4"]), "\u{1F3A4} Voice message")
        XCTAssertEqual(try body(["fileName": "notes.pdf", "fileMime": "application/pdf"]), "\u{1F4CE} notes.pdf")
        XCTAssertEqual(try body(["viewOnce": ["v": 1, "kind": "video"]]), "\u{1F3AC} Video message")
        XCTAssertEqual(try body(["viewOnce": ["v": 1, "kind": "voice"]]), "\u{1F3A4} Voice message")
        XCTAssertEqual(try body(["text": "look", "fileName": "a.jpg", "fileMime": "image/jpeg"]), "look")
        XCTAssertNil(PushChatOpener.message(["type": "chat", "id": "m", "channel": channel, "viewOnce": ["kind": "x"]], request: request))
    }

    func testAPublicChannelFrameIsPlainInsideTheRoomCipher() throws {
        let key = RoomCrypto.derivedKey(PushChatTests.channelRoom, passphrase: PushChatTests.passphrase)
        let crypto = try XCTUnwrap(RoomCrypto.withKey(key))
        let reply: JSONObject = [
            "type": "thread-reply", "id": "r-1", "senderId": "ann-node", "sender": "Ann", "channel": "#design",
            "rootId": "root-1", "text": "", "fileName": "mock.png", "ts": 2,
        ]
        let pending: JSONObject = [
            "channel": .string(PushChatTests.channelId), "room": .string(PushChatTests.channelRoom), "keyVersion": 1,
            "passphrase": .string(PushChatTests.passphrase), "frame": .object(try crypto.encrypt(reply)),
        ]
        let keys = PushRoomKeys(store: InMemorySharedItemStore())
        keys.remember(key, room: PushChatTests.channelRoom, passphrase: PushChatTests.passphrase)
        let request = try XCTUnwrap(
            PushChatRequest.parse(["topic": "chat", "channelId": .string(PushChatTests.channelId), "ref": .string(PushChatTests.ref)]))
        let opened = try XCTUnwrap(PushChatOpener.open(pending, request: request, secureStore: nil, keys: keys).message)
        XCTAssertTrue(opened.isReply)
        XCTAssertEqual(
            PushPreviewText.preview(opened, request: request, mode: .nameAndMessage),
            PushPreview(title: "Ann in #design", body: "\u{1F4CE} mock.png"))
        var keyFrame = pending
        let bundle = try RoomSeal.loadOrCreate(PushChatTests.channelRoom, acct: "ann", node: "ann-node", secureStore: InMemorySecureStore()).keyFrame()
        keyFrame["frame"] = .object(try crypto.encrypt(bundle))
        XCTAssertEqual(PushChatOpener.open(keyFrame, request: request, secureStore: nil, keys: keys), .unavailable)
        var otherChannel = pending
        otherChannel["channel"] = .string(String(repeating: "b", count: 32))
        XCTAssertEqual(PushChatOpener.open(otherChannel, request: request, secureStore: nil, keys: keys), .unavailable)
    }

    func testOnlyChatAndRepliesWithTextOrAFileBecomeMessages() throws {
        let request = try XCTUnwrap(PushChatRequest.parse(directPayload()))
        let channel = JSONValue.string("direct:\(PushChatTests.directId)")
        XCTAssertNil(PushChatOpener.message(["type": "reaction", "id": "x", "channel": channel, "text": "hi"], request: request))
        XCTAssertNil(PushChatOpener.message(["type": "chat", "id": "x", "channel": channel, "text": " "], request: request))
        XCTAssertNil(PushChatOpener.message(["type": "chat", "id": "x", "channel": "#general", "text": "hi"], request: request))
        XCTAssertNil(PushChatOpener.message(["type": "chat", "channel": channel, "text": "hi"], request: request))
        let long = String(repeating: "x", count: 1_500)
        let message = try XCTUnwrap(PushChatOpener.message(["type": "chat", "id": "x", "channel": channel, "text": .string(long)], request: request))
        XCTAssertEqual(PushPreviewText.body(message).count, PushPreviewText.maxBodyChars)
    }

    func testTheRoomKeyCacheKeepsTheNewestEntries() throws {
        let store = InMemorySharedItemStore()
        let keys = PushRoomKeys(store: store)
        for index in 0..<(PushRoomKeys.maxEntries + 3) {
            keys.remember(Array(repeating: UInt8(index), count: 32), room: "room-\(index)", passphrase: "p")
        }
        XCTAssertNil(keys.cached(room: "room-0", passphrase: "p"))
        XCTAssertNil(keys.cached(room: "room-2", passphrase: "p"))
        XCTAssertEqual(keys.cached(room: "room-3", passphrase: "p"), Array(repeating: 3, count: 32))
        XCTAssertNil(keys.cached(room: "room-3", passphrase: "other"))
        keys.remember([1, 2, 3], room: "short", passphrase: "p")
        XCTAssertNil(keys.cached(room: "short", passphrase: "p"))
        let raw = String(decoding: try XCTUnwrap(store.data(PushRoomKeys.item)), as: UTF8.self)
        XCTAssertFalse(raw.contains("room-3"), "the cache is keyed by a hash, never the room or passphrase")
        keys.clear()
        XCTAssertNil(store.data(PushRoomKeys.item))
    }

    func testTheDerivedKeyMatchesTheRoomCipher() throws {
        let room = "chat-direct:\(PushChatTests.directId):v9"
        let reference = RoomCrypto.withPassphrase(room, "match")
        let rebuilt = try XCTUnwrap(RoomCrypto.withKey(RoomCrypto.derivedKey(room, passphrase: "match")))
        let envelope = try reference.encrypt(["type": "chat", "id": "1"])
        XCTAssertEqual(rebuilt.decrypt(envelope)?["id"]?.text, "1")
        XCTAssertNil(RoomCrypto.withKey([0]))
    }
}
