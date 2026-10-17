import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest

@testable import FMChat

final class FakeMediaServer: ChatAccountCalling {
    struct Row {
        var kind: String
        var size: Int
        var parts: Int
        var state: String
        var stored: [Int: [UInt8]] = [:]
    }

    var calls: [(String, String, JSONObject?)] = []
    var replies: [String: JSONObject] = [:]
    var rows: [String: Row] = [:]
    var role = "recipient"
    var nextId = 0
    var partBytes = ViewOnceFrames.partBytes
    var failConsume = false

    func accountApiCall(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject {
        calls.append((method, path, body))
        let bare = path.split(separator: "?").first.map(String.init) ?? path
        if let reply = replies["\(method) \(bare)"] { return reply }
        let pieces = bare.split(separator: "/").map(String.init)
        guard pieces.count >= 5, pieces[0] == "api", pieces[3] != "", pieces[4] == "media" else { return [:] }
        if pieces.count == 5 && method == "POST" {
            nextId += 1
            let id = String(format: "%032x", nextId)
            let size = body?["size"]?.intValue ?? 0
            let parts = body?["parts"]?.intValue ?? 0
            if parts != (size + partBytes - 1) / partBytes { throw OrgApiException("invalid_parts", "invalid parts") }
            rows[id] = Row(kind: body?["kind"]?.text ?? "", size: size, parts: parts, state: "uploading")
            return ["ok": true, "media": ["id": .string(id), "state": "uploading"]]
        }
        if pieces.count == 5 && method == "GET" {
            let ids = (path.split(separator: "=").last.map(String.init) ?? "").split(separator: ",").map(String.init)
            return ["media": .array(ids.compactMap { id in rows[id].map { status(id, $0) } })]
        }
        let id = pieces[5]
        guard var row = rows[id] else { throw OrgApiException("not_found", "not found") }
        defer { rows[id] = row }
        if pieces.count == 6 && method == "GET" { return ["media": status(id, row)] }
        if pieces.count == 6 && method == "DELETE" {
            if row.state == "uploading" || row.state == "ready" { row.state = "withdrawn" }
            row.stored = [:]
            return ["ok": true, "media": status(id, row)]
        }
        if pieces.count == 7 && pieces[6] == "finish" {
            guard row.stored.count == row.parts, row.stored.values.reduce(0, { $0 + $1.count }) == row.size else {
                throw OrgApiException("upload_incomplete", "upload incomplete")
            }
            row.state = "ready"
            return ["ok": true, "media": status(id, row)]
        }
        if pieces.count == 7 && pieces[6] == "consume" {
            if failConsume { throw OrgApiException("http_0", "offline") }
            if row.state != "ready" && row.state != "opened" { throw OrgApiException("media_gone", "media gone") }
            row.state = "opened"
            row.stored = [:]
            return ["ok": true, "media": status(id, row)]
        }
        if pieces.count == 8 && pieces[6] == "parts", let index = Int(pieces[7]) {
            if method == "PUT" {
                let data = Array(try B64.decode(body?["data"]?.text ?? ""))
                let expected = index < row.parts - 1 ? partBytes : row.size - partBytes * (row.parts - 1)
                if data.count != expected { throw OrgApiException("invalid_part_size", "invalid part size") }
                row.stored[index] = data
                return ["ok": true, "part": .int(Int64(index))]
            }
            if role != "recipient" { throw OrgApiException("recipient_only", "recipient only") }
            guard row.state == "ready", let data = row.stored[index] else {
                throw OrgApiException("media_gone", "media gone")
            }
            return ["index": .int(Int64(index)), "parts": .int(Int64(row.parts)), "data": .string(B64.encode(data))]
        }
        return [:]
    }

    func status(_ id: String, _ row: Row) -> JSONValue {
        ["id": .string(id), "state": .string(row.state), "role": .string(role), "size": .int(Int64(row.size)),
         "parts": .int(Int64(row.parts))]
    }

    func body(_ id: String) -> [UInt8] {
        guard let row = rows[id] else { return [] }
        return (0..<row.parts).flatMap { row.stored[$0] ?? [] }
    }

    func count(_ method: String, containing fragment: String) -> Int {
        calls.filter { $0.0 == method && $0.1.contains(fragment) }.count
    }
}

final class ViewOnceMediaTests: XCTestCase {
    static let conversationId = String(repeating: "d", count: 32)
    static let room = "chat-direct:\(conversationId):v1"
    static let mediaId = String(repeating: "a", count: 32)

    func replayed(_ envelope: JSONObject) -> JSONObject {
        var out = envelope
        out["historyReplay"] = true
        return out
    }

    func sample(_ count: Int) -> [UInt8] { (0..<count).map { UInt8(truncatingIfNeeded: $0 &* 31 &+ 7) } }

    func testSealSplitAndOpenRoundTrip() throws {
        let plain = sample(2_500)
        let sealed = try ViewOnceCrypto.seal(plain)
        XCTAssertEqual(sealed.key.count, 32)
        XCTAssertEqual(sealed.iv.count, 12)
        XCTAssertEqual(sealed.body.count, plain.count + 16)
        let parts = ViewOnceCrypto.split(sealed.body, partBytes: 1_000)
        XCTAssertEqual(parts.map(\.count), [1_000, 1_000, 516])
        XCTAssertEqual(ViewOnceFrames.parts(size: sealed.body.count, partBytes: 1_000), 3)
        XCTAssertEqual(try ViewOnceCrypto.open(parts.flatMap { $0 }, key: sealed.key, iv: sealed.iv), plain)
        var tampered = sealed.body
        tampered[5] ^= 1
        XCTAssertThrowsError(try ViewOnceCrypto.open(tampered, key: sealed.key, iv: sealed.iv)) {
            XCTAssertEqual($0 as? ViewOnceError, .corrupt)
        }
        XCTAssertThrowsError(try ViewOnceCrypto.open(sealed.body, key: CryptoBytes.random(32), iv: sealed.iv))
        XCTAssertThrowsError(try ViewOnceCrypto.open([1, 2, 3], key: sealed.key, iv: sealed.iv))
    }

    func testSealMatchesWebCryptoLayout() throws {
        let key = [UInt8](repeating: 0x11, count: 32)
        let iv = [UInt8](repeating: 0x22, count: 12)
        let sealed = try ViewOnceCrypto.seal(Array("hello".utf8), key: key, iv: iv)
        let box = try AesGcm.encrypt(Array("hello".utf8), key: key, nonce: iv)
        XCTAssertEqual(sealed.body, box.cipherText + box.mac)
    }

    func testPartCountsFollowTheServerRule() {
        XCTAssertEqual(ViewOnceFrames.parts(size: 0), 0)
        XCTAssertEqual(ViewOnceFrames.parts(size: 1), 1)
        XCTAssertEqual(ViewOnceFrames.parts(size: 524_288), 1)
        XCTAssertEqual(ViewOnceFrames.parts(size: 524_289), 2)
        XCTAssertEqual(ViewOnceFrames.parts(size: 12 * 1024 * 1024 + 16), 25)
    }

    func media(size: Int = 4_016, kind: ViewOnceKind = .video) -> ViewOnceMedia {
        ViewOnceMedia(
            kind: kind, media: ViewOnceMediaTests.mediaId, key: [UInt8](repeating: 7, count: 32),
            iv: [UInt8](repeating: 9, count: 12), mime: kind.defaultMime, durationMs: 4_200, size: size,
            parts: ViewOnceFrames.parts(size: size))
    }

    func testFrameFieldRoundTripsAndRejectsBadInput() throws {
        let original = media()
        let built = ViewOnceFrames.build(original)
        let object = try XCTUnwrap(built.objectValue)
        XCTAssertEqual(object["key"]?.text.contains("="), false)
        XCTAssertEqual(ViewOnceFrames.parse(built), original)
        let reparsed = ViewOnceFrames.parse(try JSON.parse(JSON.encode(built)))
        XCTAssertEqual(reparsed, original)
        var padded = object
        padded["key"] = .string(B64.encodeUrl(original.key))
        padded["iv"] = .string(B64.encode(original.iv))
        XCTAssertEqual(ViewOnceFrames.parse(.object(padded)), original)
        let breakers: [(String, JSONValue)] = [
            ("v", 2), ("kind", "photo"), ("media", "ABC"), ("media", .string(String(repeating: "A", count: 32))),
            ("key", .string(B64.encodeUrlNoPadding([UInt8](repeating: 1, count: 16)))), ("iv", "!!"),
            ("mime", "audio/mp4"), ("mime", ""), ("durationMs", 61_001), ("durationMs", -1),
            ("size", .int(Int64(12 * 1024 * 1024 + 2_000))), ("size", 10), ("parts", 2),
        ]
        for (field, value) in breakers {
            var broken = object
            broken[field] = value
            XCTAssertNil(ViewOnceFrames.parse(.object(broken)), field)
        }
        XCTAssertNil(ViewOnceFrames.parse(nil))
        XCTAssertNil(ViewOnceFrames.parse("text"))
        var voice = media(kind: .voice)
        voice = ViewOnceMedia(
            kind: .voice, media: voice.media, key: voice.key, iv: voice.iv, mime: "audio/webm", durationMs: 120_000,
            size: voice.size, parts: voice.parts)
        XCTAssertEqual(ViewOnceFrames.parse(ViewOnceFrames.build(voice))?.mime, "audio/webm")
        XCTAssertFalse(ViewOnceFrames.isPlayable("audio/webm"))
        XCTAssertTrue(ViewOnceFrames.isPlayable("video/mp4; codecs=avc1"))
        XCTAssertTrue(ViewOnceFrames.validMediaId(ViewOnceMediaTests.mediaId))
        XCTAssertFalse(ViewOnceFrames.validMediaId(String(repeating: "g", count: 32)))
        XCTAssertEqual(ViewOnceFrames.durationLabel(65_400), "1:05")
    }

    func testChatMessageIgnoresTextForViewOnceFrames() {
        let record = DirectConversation(id: ViewOnceMediaTests.conversationId, otherUser: "pat")
        let plain: JSONObject = [
            "type": "chat", "id": "pat-1", "senderId": "pat-node", "sender": "pat", "channel": .string(record.conversation),
            "text": "click here", "fileName": "x.mp4", "ts": 5, ViewOnceFrames.field: ViewOnceFrames.build(media()),
        ]
        let message = DirectMessageFrames.chatMessage(plain, record: record, selfId: "me-node")
        XCTAssertEqual(message?.text, "🎥 Video message")
        XCTAssertEqual(message?.fileName, "")
    }

    func testRelayFrameParsesOnlyFinalStates() {
        let id = ViewOnceMediaTests.mediaId
        XCTAssertEqual(
            ViewOnceFrames.relayUpdate(["kind": "forkmesh-media", "v": 1, "id": .string(id), "state": "opened"])?.state,
            .opened)
        XCTAssertEqual(
            ViewOnceFrames.relayUpdate(["kind": "forkmesh-media", "v": 1, "id": .string(id), "state": "expired"])?.state,
            .expired)
        XCTAssertNil(ViewOnceFrames.relayUpdate(["kind": "forkmesh-media", "v": 1, "id": .string(id), "state": "ready"]))
        XCTAssertNil(ViewOnceFrames.relayUpdate(["kind": "forkmesh-media", "v": 2, "id": .string(id), "state": "opened"]))
        XCTAssertNil(ViewOnceFrames.relayUpdate(["kind": "forkmesh-media", "v": 1, "id": "zz", "state": "opened"]))
        XCTAssertNil(ViewOnceFrames.relayUpdate(["kind": "cipher", "v": 1, "id": .string(id), "state": "opened"]))
    }

    @MainActor func testStoreDropsKeyOnceOpenedAndRemembersSpentIds() {
        let prefs = InMemoryKeyValueStore()
        let store = ViewOnceStore(prefs: prefs)
        let entry = store.register(messageId: "m1", conversation: "direct:x", media: media(), mine: false)
        XCTAssertTrue(entry.canPlay)
        XCTAssertNotNil(store.entry("m1")?.material)
        XCTAssertEqual(store.unresolved("direct:x"), [ViewOnceMediaTests.mediaId])
        XCTAssertTrue(store.apply(media: ViewOnceMediaTests.mediaId, state: .opened))
        XCTAssertNil(store.entry("m1")?.material)
        XCTAssertEqual(store.entry("m1")?.stateLabel, "Opened")
        XCTAssertFalse(store.apply(media: ViewOnceMediaTests.mediaId, state: .ready))
        XCTAssertEqual(store.unresolved("direct:x"), [])
        let stored = prefs.stringList(forKey: ViewOnceStore.spentKey) ?? []
        XCTAssertEqual(stored, ["\(ViewOnceMediaTests.mediaId):opened"])
        XCTAssertFalse(stored.joined().contains(B64.encodeUrlNoPadding(media().key)))
        let restarted = ViewOnceStore(prefs: prefs)
        let replayed = restarted.register(messageId: "m1", conversation: "direct:x", media: media(), mine: false)
        XCTAssertEqual(replayed.state, .opened)
        XCTAssertNil(replayed.material)
        let mine = restarted.register(
            messageId: "m2", conversation: "direct:x",
            media: ViewOnceMedia(
                kind: .voice, media: String(repeating: "b", count: 32), key: media().key, iv: media().iv,
                mime: "audio/mp4", durationMs: 1_000, size: 100, parts: 1),
            mine: true)
        XCTAssertNil(mine.material)
        XCTAssertFalse(mine.canPlay)
        XCTAssertEqual(mine.stateLabel, "Sent")
    }

    @MainActor func testTransferUploadsDownloadsAndConsumes() async throws {
        let server = FakeMediaServer()
        server.partBytes = 1_000
        let transfer = ViewOnceTransfer(api: server, conversationId: ViewOnceMediaTests.conversationId, partBytes: 1_000)
        let plain = sample(2_400)
        let sealed = try ViewOnceCrypto.seal(plain)
        var seen: [Double] = []
        let id = try await transfer.upload(kind: .voice, body: sealed.body, durationMs: 3_000) { seen.append($0) }
        XCTAssertEqual(seen.count, 3)
        XCTAssertEqual(server.rows[id]?.state, "ready")
        XCTAssertEqual(server.body(id), sealed.body)
        let create = try XCTUnwrap(server.calls.first)
        XCTAssertEqual(create.0, "POST")
        XCTAssertEqual(create.1, "/api/chat/direct-messages/\(ViewOnceMediaTests.conversationId)/media")
        XCTAssertEqual(create.2?["parts"]?.intValue, 3)
        XCTAssertEqual(create.2?["size"]?.intValue, 2_416)
        XCTAssertEqual(server.count("PUT", containing: "/parts/"), 3)
        let media = ViewOnceMedia(
            kind: .voice, media: id, key: sealed.key, iv: sealed.iv, mime: "audio/mp4", durationMs: 3_000,
            size: sealed.body.count, parts: 3)
        let downloaded = try await transfer.download(media)
        XCTAssertEqual(downloaded, plain)
        let consumed = try await transfer.consume(id)
        XCTAssertEqual(consumed, .opened)
        XCTAssertTrue(server.rows[id]?.stored.isEmpty == true)
        do {
            _ = try await transfer.download(media)
            XCTFail("downloaded after consume")
        } catch {
            XCTAssertEqual(error as? ViewOnceError, .gone(.expired))
        }
        let statuses = try await transfer.statuses([id, "nope"])
        XCTAssertEqual(statuses.map(\.state), [.opened])
    }

    @MainActor func testFailedUploadIsWithdrawn() async throws {
        let server = FakeMediaServer()
        let failing = FailingFinish(server)
        let failingTransfer = ViewOnceTransfer(api: failing, conversationId: ViewOnceMediaTests.conversationId)
        do {
            _ = try await failingTransfer.upload(kind: .video, body: sample(100), durationMs: 1_000)
            XCTFail("upload should fail")
        } catch {
            XCTAssertEqual(error as? ViewOnceError, .refused("upload_incomplete"))
        }
        XCTAssertEqual(server.rows.values.first?.state, "withdrawn")
    }

    @MainActor func testQuotaErrorsAreReadable() async {
        let server = FakeMediaServer()
        let failing = QuotaRefusal(server)
        let transfer = ViewOnceTransfer(api: failing, conversationId: ViewOnceMediaTests.conversationId)
        do {
            _ = try await transfer.upload(kind: .video, body: sample(100), durationMs: 1_000)
            XCTFail("upload should fail")
        } catch {
            XCTAssertEqual(error as? ViewOnceError, .refused("media_quota_count"))
            XCTAssertTrue("\(error)".contains("unopened"))
        }
    }

    @MainActor func makeService(_ api: ChatAccountCalling, _ ws: FakeWebSocketConnector, prefs: KeyValueStore? = nil)
        -> DirectMessageService
    {
        DirectMessageService(
            api: api, connector: ws, secureStore: InMemorySecureStore(),
            origin: { URL(string: "https://app.next.forkmesh.com") }, selfId: { "me-node" }, displayName: { "Me" },
            viewOncePrefs: prefs)
    }

    @MainActor func prepare(_ server: FakeMediaServer) {
        server.replies["GET /api/chat/direct-messages"] = [
            "conversations": [["id": .string(ViewOnceMediaTests.conversationId), "otherUser": "pat"]]
        ]
        server.replies["GET /api/chat/direct-messages/\(ViewOnceMediaTests.conversationId)/room-access"] = [
            "room": .string(ViewOnceMediaTests.room), "passphrase": "direct-pass",
            "webSocketUrl": .string("/api/chat/direct-messages/\(ViewOnceMediaTests.conversationId)/ws?ticket=abc"),
        ]
    }

    @MainActor func connectPeer(_ service: DirectMessageService, _ ws: FakeWebSocketConnector) async throws
        -> (FakeWebSocketConnection, RoomCrypto, RoomSeal, String)
    {
        await service.refresh()
        let conversation = service.conversations[0]
        _ = await service.open(conversation)
        let connection = try XCTUnwrap(ws.lastConnection)
        let crypto = await Task.detached { RoomCrypto.withPassphrase(ViewOnceMediaTests.room, "direct-pass") }.value
        let peer = try RoomSeal.loadOrCreate(
            ViewOnceMediaTests.room, scope: "direct:\(ViewOnceMediaTests.conversationId)", policy: .strict, acct: "pat",
            node: "pat-node", secureStore: InMemorySecureStore())
        connection.receive(text: JSON.encode(try crypto.encrypt(peer.keyFrame())))
        let learned = await waitUntil { service.deviceKeys(conversation).contains(peer.keyId) }
        XCTAssertTrue(learned)
        for text in connection.sentTexts {
            if let frame = crypto.decrypt(JSON.parseObject(text)), RoomSeal.isKeyFrame(frame) { _ = peer.acceptKeyFrame(frame) }
        }
        return (connection, crypto, peer, conversation)
    }

    @MainActor func testSendViewOnceUploadsThenSendsSealedFrameWithTheKey() async throws {
        let server = FakeMediaServer()
        server.role = "sender"
        prepare(server)
        let ws = FakeWebSocketConnector()
        let service = makeService(server, ws)
        let (connection, crypto, peer, conversation) = try await connectPeer(service, ws)
        let recording = Data(sample(3_000))
        try await service.sendViewOnce(conversation, kind: .video, mime: "video/mp4", data: recording, durationMs: 5_000)
        let frames = connection.sentTexts.compactMap { crypto.decrypt(JSON.parseObject($0)) }
            .filter { RoomSeal.isSealedFrame($0) }.compactMap { peer.unseal($0) }
        let plain = try XCTUnwrap(frames.first { $0[ViewOnceFrames.field] != nil })
        XCTAssertEqual(plain["text"]?.text, "🎥 Video message")
        let media = try XCTUnwrap(ViewOnceFrames.parse(plain[ViewOnceFrames.field]))
        XCTAssertEqual(media.size, 3_016)
        XCTAssertEqual(media.parts, 1)
        XCTAssertEqual(media.durationMs, 5_000)
        XCTAssertEqual(server.rows[media.media]?.state, "ready")
        XCTAssertEqual(try ViewOnceCrypto.open(server.body(media.media), key: media.key, iv: media.iv), Array(recording))
        let envelope = try XCTUnwrap(
            connection.sentTexts.map { JSON.parseObject($0) }.first {
                peer.unseal(crypto.decrypt($0) ?? [:])?[ViewOnceFrames.field] != nil
            })
        XCTAssertEqual(envelope["persist"], .bool(true))
        XCTAssertEqual(envelope["notify"], .bool(true))
        let message = try XCTUnwrap(service.messages(conversation).last)
        XCTAssertEqual(message.text, "🎥 Video message")
        let entry = try XCTUnwrap(service.viewOnce.entry(message.id))
        XCTAssertTrue(entry.mine)
        XCTAssertNil(entry.material)
        XCTAssertEqual(entry.stateLabel, "Sent")
        do {
            _ = try await service.playViewOnce(message.id)
            XCTFail("the sender cannot play")
        } catch {}
        connection.receive(json: ["kind": "forkmesh-media", "v": 1, "id": .string(media.media), "state": "opened"])
        let opened = await waitUntil { service.viewOnce.entry(message.id)?.state == .opened }
        XCTAssertTrue(opened)
        await service.closeAll()
    }

    @MainActor func testSendRefusesOversizeAndOverlongRecordings() async throws {
        let server = FakeMediaServer()
        prepare(server)
        let ws = FakeWebSocketConnector()
        let service = makeService(server, ws)
        await service.refresh()
        let conversation = service.conversations[0]
        do {
            try await service.sendViewOnce(
                conversation, kind: .voice, mime: "audio/mp4", data: Data(count: 4 * 1024 * 1024), durationMs: 1_000)
            XCTFail("oversize accepted")
        } catch {
            XCTAssertEqual(error as? ViewOnceError, .tooLarge)
        }
        do {
            try await service.sendViewOnce(conversation, kind: .video, mime: "video/mp4", data: Data([1]), durationMs: 60_001)
            XCTFail("overlong accepted")
        } catch {
            XCTAssertEqual(error as? ViewOnceError, .tooLong)
        }
        XCTAssertEqual(server.count("POST", containing: "/media"), 0)
    }

    @MainActor func peerViewOnce(_ server: FakeMediaServer, _ plainBytes: [UInt8], conversation: String, id: String)
        throws -> JSONObject
    {
        let sealed = try ViewOnceCrypto.seal(plainBytes)
        server.nextId += 1
        let media = String(format: "%032x", server.nextId)
        server.rows[media] = FakeMediaServer.Row(
            kind: "voice", size: sealed.body.count, parts: 1, state: "ready", stored: [0: sealed.body])
        let field = ViewOnceMedia(
            kind: .voice, media: media, key: sealed.key, iv: sealed.iv, mime: "audio/mp4", durationMs: 2_000,
            size: sealed.body.count, parts: 1)
        return [
            "type": "chat", "id": .string(id), "senderId": "pat-node", "sender": "pat", "channel": .string(conversation),
            "text": "🎤 Voice message", "ts": 1000, ViewOnceFrames.field: ViewOnceFrames.build(field),
        ]
    }

    @MainActor func testReceivedViewOncePlaysOnceThenDropsTheKey() async throws {
        let server = FakeMediaServer()
        prepare(server)
        let ws = FakeWebSocketConnector()
        let prefs = InMemoryKeyValueStore()
        let service = makeService(server, ws, prefs: prefs)
        var live: [ChatMessage] = []
        service.onMessage = { live.append($0) }
        let (connection, crypto, peer, conversation) = try await connectPeer(service, ws)
        let recording = sample(1_500)
        let plain = try peerViewOnce(server, recording, conversation: conversation, id: "pat-v1")
        connection.receive(text: JSON.encode(try crypto.encrypt(try peer.seal(plain))))
        let got = await waitUntil { service.viewOnce.entry("pat-v1") != nil }
        XCTAssertTrue(got)
        XCTAssertEqual(live.map(\.text), ["🎤 Voice message"])
        let entry = try XCTUnwrap(service.viewOnce.entry("pat-v1"))
        XCTAssertTrue(entry.canPlay)
        XCTAssertEqual(entry.stateLabel, "Tap to play once")
        let clip = try await service.playViewOnce("pat-v1")
        XCTAssertEqual(Array(clip.data), recording)
        XCTAssertEqual(clip.kind, .voice)
        XCTAssertNotNil(service.viewOnce.entry("pat-v1")?.material)
        await service.consumeViewOnce("pat-v1")
        XCTAssertEqual(server.count("POST", containing: "/consume"), 1)
        let after = try XCTUnwrap(service.viewOnce.entry("pat-v1"))
        XCTAssertEqual(after.state, .opened)
        XCTAssertNil(after.material)
        XCTAssertFalse(after.canPlay)
        do {
            _ = try await service.playViewOnce("pat-v1")
            XCTFail("played twice")
        } catch {
            XCTAssertEqual(error as? ViewOnceError, .gone(.opened))
        }
        XCTAssertEqual(server.count("GET", containing: "/parts/"), 1)
        let key = B64.encodeUrlNoPadding(ViewOnceFrames.parse(plain[ViewOnceFrames.field])?.key ?? [])
        XCTAssertFalse((prefs.stringList(forKey: ViewOnceStore.spentKey) ?? []).joined().contains(key))
        connection.receive(
            text: JSON.encode(replayed(try crypto.encrypt(try peer.seal(plain)))))
        try? await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertNil(service.viewOnce.entry("pat-v1")?.material)
        await service.closeAll()
    }

    @MainActor func testGoneMediaTakesItsServerState() async throws {
        let server = FakeMediaServer()
        prepare(server)
        let ws = FakeWebSocketConnector()
        let service = makeService(server, ws)
        let (connection, crypto, peer, conversation) = try await connectPeer(service, ws)
        let plain = try peerViewOnce(server, sample(200), conversation: conversation, id: "pat-v2")
        connection.receive(text: JSON.encode(try crypto.encrypt(try peer.seal(plain))))
        _ = await waitUntil { service.viewOnce.entry("pat-v2") != nil }
        let media = try XCTUnwrap(service.viewOnce.entry("pat-v2")?.mediaId)
        server.rows[media]?.state = "withdrawn"
        server.rows[media]?.stored = [:]
        do {
            _ = try await service.playViewOnce("pat-v2")
            XCTFail("withdrawn media played")
        } catch {
            XCTAssertEqual(error as? ViewOnceError, .gone(.withdrawn))
        }
        XCTAssertEqual(service.viewOnce.entry("pat-v2")?.state, .withdrawn)
        XCTAssertNil(service.viewOnce.entry("pat-v2")?.material)
        await service.closeAll()
    }

    @MainActor func testHistoryEndReadsStatusOnceAndRetriesConsume() async throws {
        let server = FakeMediaServer()
        prepare(server)
        let ws = FakeWebSocketConnector()
        let service = makeService(server, ws)
        let (connection, crypto, peer, conversation) = try await connectPeer(service, ws)
        let first = try peerViewOnce(server, sample(100), conversation: conversation, id: "pat-a")
        let second = try peerViewOnce(server, sample(120), conversation: conversation, id: "pat-b")
        for plain in [first, second] {
            connection.receive(
                text: JSON.encode(replayed(try crypto.encrypt(try peer.seal(plain)))))
        }
        _ = await waitUntil { service.viewOnce.entry("pat-b") != nil }
        let expiredId = try XCTUnwrap(service.viewOnce.entry("pat-a")?.mediaId)
        server.rows[expiredId]?.state = "expired"
        connection.receive(json: ["kind": "forkmesh-history-end"])
        let read = await waitUntil { service.viewOnce.entry("pat-a")?.state == .expired }
        XCTAssertTrue(read)
        XCTAssertNil(service.viewOnce.entry("pat-a")?.material)
        XCTAssertEqual(service.viewOnce.entry("pat-b")?.state, .ready)
        XCTAssertEqual(server.calls.filter { $0.0 == "GET" && $0.1.contains("/media?ids=") }.count, 1)
        connection.receive(json: ["kind": "forkmesh-history-end"])
        try? await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(server.calls.filter { $0.0 == "GET" && $0.1.contains("/media?ids=") }.count, 1)
        server.failConsume = true
        await service.consumeViewOnce("pat-b")
        XCTAssertEqual(service.viewOnce.entry("pat-b")?.state, .opened)
        XCTAssertNil(service.viewOnce.entry("pat-b")?.material)
        let pendingId = try XCTUnwrap(service.viewOnce.entry("pat-b")?.mediaId)
        XCTAssertTrue(service.viewOnce.pendingConsume.contains(pendingId))
        server.failConsume = false
        await service.refreshViewOnce(conversation)
        XCTAssertFalse(service.viewOnce.pendingConsume.contains(pendingId))
        XCTAssertEqual(server.rows[pendingId]?.state, "opened")
        await service.closeAll()
    }

    @MainActor func receipts(_ connection: FakeWebSocketConnection) -> [String] {
        connection.sentJSON.filter { $0["kind"]?.text == DirectMessageFrames.receivedKind }
            .flatMap { $0["refs"]?.arrayValue?.compactMap { $0.stringValue } ?? [] }
    }

    @MainActor func testReceiptIsHeldUntilTheMessageIsFinal() async throws {
        let server = FakeMediaServer()
        prepare(server)
        let ws = FakeWebSocketConnector()
        let prefs = InMemoryKeyValueStore()
        let service = makeService(server, ws, prefs: prefs)
        let (connection, crypto, peer, conversation) = try await connectPeer(service, ws)
        func ref(_ c: Character) -> String { String(repeating: c, count: 40) }
        func deliver(_ plain: JSONObject, _ ref: String, replay: Bool = false) throws {
            var envelope = try crypto.encrypt(try peer.seal(plain))
            envelope["ref"] = .string(ref)
            if replay { envelope["historyReplay"] = true }
            connection.receive(text: JSON.encode(envelope))
        }
        let text: JSONObject = [
            "type": "chat", "id": "pat-t", "senderId": "pat-node", "sender": "pat", "channel": .string(conversation),
            "text": "plain", "ts": 900,
        ]
        let play = try peerViewOnce(server, sample(300), conversation: conversation, id: "v-play")
        let expire = try peerViewOnce(server, sample(100), conversation: conversation, id: "v-expire")
        let withdraw = try peerViewOnce(server, sample(100), conversation: conversation, id: "v-withdraw")
        let offline = try peerViewOnce(server, sample(100), conversation: conversation, id: "v-offline")
        try deliver(text, ref("0"))
        try deliver(play, ref("1"))
        try deliver(expire, ref("2"))
        try deliver(withdraw, ref("3"), replay: true)
        try deliver(offline, ref("4"))
        let acked = await waitUntil { self.receipts(connection) == [ref("0")] }
        XCTAssertTrue(acked)
        XCTAssertEqual(service.heldReceiptCount, 4)
        try deliver(play, ref("5"))
        try? await Task.sleep(nanoseconds: 300_000_000)
        XCTAssertEqual(receipts(connection), [ref("0")])
        _ = try await service.playViewOnce("v-play")
        await service.consumeViewOnce("v-play")
        let played = await waitUntil { Set(self.receipts(connection)) == Set([ref("0"), ref("1"), ref("5")]) }
        XCTAssertTrue(played)
        let expireId = try XCTUnwrap(service.viewOnce.entry("v-expire")?.mediaId)
        connection.receive(json: ["kind": "forkmesh-media", "v": 1, "id": .string(expireId), "state": "expired"])
        let expired = await waitUntil { self.receipts(connection).contains(ref("2")) }
        XCTAssertTrue(expired)
        let withdrawId = try XCTUnwrap(service.viewOnce.entry("v-withdraw")?.mediaId)
        server.rows[withdrawId]?.state = "withdrawn"
        connection.receive(json: ["kind": "forkmesh-history-end"])
        let withdrawn = await waitUntil { self.receipts(connection).contains(ref("3")) }
        XCTAssertTrue(withdrawn)
        server.failConsume = true
        await service.consumeViewOnce("v-offline")
        XCTAssertEqual(service.viewOnce.entry("v-offline")?.state, .opened)
        XCTAssertEqual(service.heldReceiptCount, 1)
        try? await Task.sleep(nanoseconds: 300_000_000)
        XCTAssertFalse(receipts(connection).contains(ref("4")))
        let offlineId = try XCTUnwrap(service.viewOnce.entry("v-offline")?.mediaId)
        XCTAssertEqual(prefs.stringList(forKey: ViewOnceStore.consumeKey), [offlineId])
        server.failConsume = false
        await service.refreshViewOnce(conversation)
        let retried = await waitUntil { self.receipts(connection).contains(ref("4")) }
        XCTAssertTrue(retried)
        XCTAssertEqual(service.heldReceiptCount, 0)
        XCTAssertEqual(prefs.stringList(forKey: ViewOnceStore.consumeKey), [])
        try deliver(expire, ref("6"))
        let late = await waitUntil { self.receipts(connection).contains(ref("6")) }
        XCTAssertTrue(late)
        XCTAssertEqual(service.heldReceiptCount, 0)
        let restarted = ViewOnceStore(prefs: prefs)
        let unopened = ViewOnceMedia(
            kind: .voice, media: String(repeating: "c", count: 32), key: media().key, iv: media().iv,
            mime: "audio/mp4", durationMs: 1_000, size: 100, parts: 1)
        XCTAssertTrue(restarted.register(messageId: "again", conversation: conversation, media: unopened, mine: false).canPlay)
        XCTAssertTrue(restarted.holdsReceipt(media: unopened.media))
        await service.closeAll()
    }

    @MainActor func testOwnViewOnceFramesAreAckedAndCanBeUnsent() async throws {
        let server = FakeMediaServer()
        server.role = "sender"
        prepare(server)
        let ws = FakeWebSocketConnector()
        let service = makeService(server, ws)
        let (_, _, _, conversation) = try await connectPeer(service, ws)
        try await service.sendViewOnce(conversation, kind: .voice, mime: "audio/mp4", data: Data(sample(500)), durationMs: 900)
        let message = try XCTUnwrap(service.messages(conversation).last)
        XCTAssertEqual(service.viewOnce.entry(message.id)?.canUnsend, true)
        XCTAssertEqual(service.heldReceiptCount, 0)
        try await service.withdrawViewOnce(message.id)
        XCTAssertEqual(service.viewOnce.entry(message.id)?.state, .withdrawn)
        XCTAssertEqual(service.viewOnce.entry(message.id)?.canUnsend, false)
        XCTAssertEqual(server.count("DELETE", containing: "/media/"), 1)
        await service.closeAll()
    }
}

final class FailingFinish: ChatAccountCalling {
    let inner: FakeMediaServer

    init(_ inner: FakeMediaServer) { self.inner = inner }

    func accountApiCall(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject {
        if path.hasSuffix("/finish") { throw OrgApiException("upload_incomplete", "upload incomplete") }
        return try await inner.accountApiCall(method, path, body)
    }
}

final class QuotaRefusal: ChatAccountCalling {
    let inner: FakeMediaServer

    init(_ inner: FakeMediaServer) { self.inner = inner }

    func accountApiCall(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject {
        if method == "POST" && path.hasSuffix("/media") { throw OrgApiException("media_quota_count", "quota") }
        return try await inner.accountApiCall(method, path, body)
    }
}
