import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest

@testable import FMChat

@MainActor
final class DirectLiveSent {
    var frames: [(String, JSONObject)] = []
}

final class DirectLiveTests: XCTestCase {
    static let conversation = "direct:" + String(repeating: "d", count: 32)
    static let quickSleep: @Sendable (TimeInterval) async throws -> Void = { _ in
        try await Task.sleep(nanoseconds: 1_000_000)
    }

    @MainActor func makeEngine(_ clock: FixedClock, _ sent: DirectLiveSent, interval: Int64 = 0) -> DirectLiveEngine {
        DirectLiveEngine(
            selfId: { "me-node" },
            send: { conversation, fields in
                sent.frames.append((conversation, fields))
                return true
            },
            interval: { _ in interval }, clock: clock, sleep: DirectLiveTests.quickSleep)
    }

    @MainActor func peerFrame(_ fields: JSONObject, sender: String = "pat-node", ts: Int64 = 10_000) -> JSONObject {
        var frame: JSONObject = [
            "type": "live", "id": "pat-live", "senderId": .string(sender), "sender": "pat",
            "channel": .string(DirectLiveTests.conversation), "ts": .int(ts),
        ]
        for (key, value) in fields { frame[key] = value }
        return frame
    }

    @MainActor func preparedApi() -> FakeAccountApi {
        let api = FakeAccountApi()
        let id = DirectMessagesTests.conversationId
        api.replies["GET /api/chat/direct-messages"] = ["conversations": [["id": .string(id), "otherUser": "pat"]]]
        api.replies["GET /api/chat/direct-messages/\(id)/room-access"] = [
            "room": .string(DirectMessagesTests.room), "passphrase": "direct-pass",
            "webSocketUrl": .string("/api/chat/direct-messages/\(id)/ws?ticket=abc"),
        ]
        return api
    }

    @MainActor func makeService(api: FakeAccountApi, ws: FakeWebSocketConnector) -> DirectMessageService {
        DirectMessageService(
            api: api, connector: ws, secureStore: InMemorySecureStore(),
            origin: { URL(string: "https://app.next.forkmesh.com") }, selfId: { "me-node" }, displayName: { "Me" })
    }

    @MainActor func roomCrypto() async -> RoomCrypto {
        await Task.detached { RoomCrypto.withPassphrase(DirectMessagesTests.room, "direct-pass") }.value
    }

    @MainActor func peerSeal(_ label: String, acct: String = "pat", node: String = "pat-node") throws -> RoomSeal {
        try RoomSeal.loadOrCreate(
            DirectMessagesTests.room, scope: "direct:\(DirectMessagesTests.conversationId)", policy: .strict, acct: acct,
            node: node, secureStore: InMemorySecureStore())
    }

    @MainActor func teach(_ peer: RoomSeal, from connection: FakeWebSocketConnection, _ crypto: RoomCrypto) async -> Bool {
        let advertised = await waitUntil {
            connection.sentJSON.contains { RoomSeal.isKeyFrame(crypto.decrypt($0) ?? [:]) }
        }
        for envelope in connection.sentJSON {
            if let frame = crypto.decrypt(envelope), RoomSeal.isKeyFrame(frame) { _ = peer.acceptKeyFrame(frame) }
        }
        return advertised
    }

    func testIntervalsFollowTheRelayBudget() {
        XCTAssertEqual(DirectLiveFrames.interval(perWindow: 150, windowMs: 10_000), 88)
        XCTAssertEqual(DirectLiveFrames.interval(perWindow: 100_000, windowMs: 10_000), DirectLiveFrames.minIntervalMs)
        XCTAssertEqual(DirectLiveFrames.interval(perWindow: 0, windowMs: 10_000), DirectLiveFrames.fallbackIntervalMs)
        XCTAssertNil(DirectLiveFrames.budget(["kind": "forkmesh-live", "perWindow": 0, "windowMs": 10]))
        let budget = DirectLiveFrames.budget(["kind": "forkmesh-live", "v": 1, "perWindow": 150, "windowMs": 10_000])
        XCTAssertEqual(budget?.perWindow, 150)
        XCTAssertEqual(budget?.windowMs, 10_000)
    }

    func testMotionSamplesGiveTiltAndInterpolate() {
        let upright = DirectLiveMotionSample(at: 0, gx: 0, gy: -1, gz: 0)
        XCTAssertEqual(upright.roll, 0, accuracy: 0.0001)
        XCTAssertEqual(upright.pitch, 0, accuracy: 0.0001)
        let flat = DirectLiveMotionSample(at: 0, gx: 0, gy: 0, gz: -1)
        XCTAssertEqual(flat.pitch, .pi / 2, accuracy: 0.0001)
        let right = DirectLiveMotionSample(at: 0, gx: 1, gy: 0, gz: 0)
        XCTAssertEqual(right.roll, .pi / 2, accuracy: 0.0001)
        XCTAssertEqual(right.steadyRoll, .pi / 2, accuracy: 0.0001)
        XCTAssertEqual(DirectLiveMotionSample(at: 0, gx: 0.01, gy: 0, gz: -1).steadyRoll, 0.0449, accuracy: 0.001)
        XCTAssertEqual(DirectLiveMotionSample(at: 0, gx: .nan, gy: 99, gz: 0).gx, 0)
        XCTAssertEqual(DirectLiveMotionSample(at: 0, gx: 0, gy: 99, gz: 0).gy, DirectLiveFrames.maxMotion)

        let track = DirectLiveMotionTrack(
            samples: [
                DirectLiveMotionSample(at: 100, gx: 0, gy: -1, gz: 0, ax: 0),
                DirectLiveMotionSample(at: 200, gx: 1, gy: 0, gz: 0, ax: 2),
            ], lastHeard: 200)
        XCTAssertEqual(track.sample(at: 50)?.gy, -1)
        let middle = track.sample(at: 150)
        XCTAssertEqual(middle?.gx ?? 0, 0.5, accuracy: 0.0001)
        XCTAssertEqual(middle?.ax ?? 0, 1, accuracy: 0.0001)
        XCTAssertEqual(track.sample(at: 900)?.gx, 1)
        XCTAssertTrue(track.isSharing(at: 200 + DirectLiveFrames.motionQuietMs - 1))
        XCTAssertFalse(track.isSharing(at: 200 + DirectLiveFrames.motionQuietMs))
    }

    @MainActor func testTapsAndStrokesAreBatchedIntoLiveFrames() async throws {
        let clock = FixedClock(epochMillis: 1_000_000)
        let sent = DirectLiveSent()
        let engine = makeEngine(clock, sent)
        let conversation = DirectLiveTests.conversation
        engine.tap(conversation, at: DirectLivePoint(x: 0.25, y: -0.5))
        XCTAssertEqual(engine.taps(in: conversation).map(\.isSelf), [true])
        let tapped = await waitUntil { sent.frames.count == 1 }
        XCTAssertTrue(tapped)
        XCTAssertEqual(sent.frames[0].1["taps"], .array([.array([.int(250), .int(-500)])]))

        let id = engine.beginStroke(conversation, at: DirectLivePoint(x: 0.1, y: 0.1), color: 0xFF453A, width: 6)
        engine.extendStroke(conversation, id: id, to: DirectLivePoint(x: 0.1001, y: 0.1))
        engine.extendStroke(conversation, id: id, to: DirectLivePoint(x: 0.2, y: 0.2))
        XCTAssertEqual(engine.strokes(in: conversation).first?.points.count, 2, "a point closer than the gap is skipped")
        let started = await waitUntil { sent.frames.contains { $0.1["ink"] != nil } }
        XCTAssertTrue(started)
        let first = try XCTUnwrap(sent.frames.compactMap { $0.1["ink"]?.arrayValue?.first?.objectValue }.first)
        XCTAssertEqual(first["s"]?.text, id)
        XCTAssertEqual(first["c"], .int(0xFF453A))
        XCTAssertEqual(first["w"], .int(60))
        engine.extendStroke(conversation, id: id, to: DirectLivePoint(x: 0.3, y: 0.3))
        engine.endStroke(conversation, id: id)
        let ended = await waitUntil {
            sent.frames.contains { $0.1["ink"]?.arrayValue?.first?.objectValue?["e"] == .bool(true) }
        }
        XCTAssertTrue(ended)
        let chunks = sent.frames.compactMap { $0.1["ink"]?.arrayValue?.first?.objectValue }
        let points = chunks.flatMap { $0["p"]?.arrayValue ?? [] }
        XCTAssertEqual(points, [.int(100), .int(100), .int(200), .int(200), .int(300), .int(300)])
        XCTAssertNil(chunks.last?["c"], "colour and width go with the first chunk only")

        engine.clearInk(conversation)
        XCTAssertTrue(engine.strokes(in: conversation).isEmpty)
        let cleared = await waitUntil { sent.frames.last?.1["clear"] == .bool(true) }
        XCTAssertTrue(cleared)
    }

    @MainActor func testLongStrokesAreSplitAcrossFrames() async throws {
        let clock = FixedClock(epochMillis: 1_000_000)
        let sent = DirectLiveSent()
        let engine = makeEngine(clock, sent)
        let conversation = DirectLiveTests.conversation
        let id = engine.beginStroke(conversation, at: DirectLivePoint(x: 0, y: 0), color: 0xFFFFFF, width: 4)
        for step in 1..<400 {
            engine.extendStroke(
                conversation, id: id, to: DirectLivePoint(x: 0.5 + Double(step % 2) / 100, y: Double(step) / 400))
        }
        engine.endStroke(conversation, id: id)
        let done = await waitUntil {
            sent.frames.contains { $0.1["ink"]?.arrayValue?.first?.objectValue?["e"] == .bool(true) }
        }
        XCTAssertTrue(done)
        let counts = sent.frames.compactMap { $0.1["ink"]?.arrayValue?.first?.objectValue?["p"]?.arrayValue?.count }
        XCTAssertTrue(counts.allSatisfy { $0 <= DirectLiveFrames.maxInkPointsPerFrame * 2 })
        XCTAssertEqual(counts.reduce(0, +), 800)
    }

    @MainActor func testPeerFramesBecomeTapsInkAndMotion() async throws {
        let clock = FixedClock(epochMillis: 50_000)
        let sent = DirectLiveSent()
        let engine = makeEngine(clock, sent)
        let conversation = DirectLiveTests.conversation
        var felt: [DirectLiveTap] = []
        engine.onTap = { _, tap in felt.append(tap) }
        var drawn: [String] = []
        engine.onStroke = { _, stroke in drawn.append(stroke.id) }

        engine.receive(peerFrame(["taps": [[500, 0]]], sender: "me-node"), conversation: conversation)
        engine.receive(peerFrame(["taps": [[500, 0]]]), conversation: "direct:" + String(repeating: "e", count: 32))
        XCTAssertTrue(felt.isEmpty, "own frames and other conversations are ignored")

        engine.receive(
            peerFrame([
                "taps": [[500, 9000]],
                "ink": [["s": "s-1", "c": 0x64D2FF, "w": 80, "p": [100, 100, 200, 200]], ["s": "bad id!", "p": [1, 1]]],
                "motion": [[-80, 0, -100, 0, 0, 0, 0], [0, 100, 0, 0, 50, 0, 0]],
            ]), conversation: conversation)
        XCTAssertEqual(felt.count, 1)
        XCTAssertEqual(felt.first?.point, DirectLivePoint(x: 0.5, y: DirectLiveFrames.maxY))
        XCTAssertFalse(felt.first?.isSelf ?? true)
        XCTAssertEqual(drawn, ["pat-node|s-1"])
        let stroke = try XCTUnwrap(engine.strokes(in: conversation).first)
        XCTAssertEqual(stroke.color, 0x64D2FF)
        XCTAssertEqual(stroke.width, 8)
        XCTAssertEqual(stroke.points, [DirectLivePoint(x: 0.1, y: 0.1), DirectLivePoint(x: 0.2, y: 0.2)])

        engine.receive(peerFrame(["ink": [["s": "s-1", "p": [300, 300], "e": true]]]), conversation: conversation)
        XCTAssertEqual(engine.strokes(in: conversation).first?.points.count, 3)
        XCTAssertEqual(engine.strokes(in: conversation).first?.ended, true)

        let track = try XCTUnwrap(engine.partnerMotion(conversation))
        let newest = 50_000 + DirectLiveFrames.motionPlaybackMarginMs
        XCTAssertEqual(track.samples.map(\.at), [newest - 80, newest])
        XCTAssertEqual(track.sample(at: newest - 40)?.gx ?? 0, 0.5, accuracy: 0.0001)

        engine.receive(peerFrame(["motionOff": true]), conversation: conversation)
        XCTAssertNil(engine.partnerMotion(conversation))

        engine.receive(peerFrame(["clear": true]), conversation: conversation)
        XCTAssertTrue(engine.strokes(in: conversation).isEmpty)
        XCTAssertTrue(sent.frames.isEmpty)
    }

    @MainActor func testPartnerMotionPlaysTheNewestSampleOneMarginBehind() throws {
        let clock = FixedClock(epochMillis: 60_000)
        let engine = makeEngine(clock, DirectLiveSent())
        let conversation = DirectLiveTests.conversation
        let margin = DirectLiveFrames.motionPlaybackMarginMs
        engine.receive(
            peerFrame(["motion": [[-66, 0, -100, 0], [-33, 20, -100, 0], [0, 40, -100, 0]]], ts: 5_000),
            conversation: conversation)
        var track = try XCTUnwrap(engine.partnerMotion(conversation))
        XCTAssertEqual(track.samples.last?.at, 60_000 + margin)
        XCTAssertEqual(track.sample(at: 60_000 + margin)?.gx ?? 0, 0.4, accuracy: 0.0001)

        clock.advance(by: 0.088)
        let newest = engine.now + margin
        engine.receive(
            peerFrame(["motion": [[-55, 60, -100, 0], [-22, 80, -100, 0], [0, 100, -100, 0]]], ts: 5_088),
            conversation: conversation)
        track = try XCTUnwrap(engine.partnerMotion(conversation))
        XCTAssertEqual(track.samples.last?.at, newest)
        XCTAssertEqual(track.samples.map(\.at), track.samples.map(\.at).sorted())
        XCTAssertEqual(track.sample(at: 60_000 + margin)?.gx ?? 0, 0.4, accuracy: 0.0001)
        XCTAssertEqual(track.sample(at: newest)?.gx ?? 0, 1, accuracy: 0.0001)
        let between = try XCTUnwrap(track.sample(at: 60_000 + margin + 10)?.gx)
        XCTAssertGreaterThan(between, 0.4)
        XCTAssertLessThan(between, 0.6)
    }

    @MainActor func testInkFadesAndIsPruned() {
        let clock = FixedClock(epochMillis: 1_000)
        let engine = makeEngine(clock, DirectLiveSent())
        let conversation = DirectLiveTests.conversation
        engine.receive(peerFrame(["ink": [["s": "a", "p": [0, 0, 10, 10], "e": true]], "taps": [[1, 1]]]), conversation: conversation)
        let stroke = try? XCTUnwrap(engine.strokes(in: conversation).first)
        XCTAssertEqual(stroke?.opacity(at: 1_000), 1)
        let fadeStart = 1_000 + DirectLiveFrames.inkLifeMs - DirectLiveFrames.inkFadeMs
        XCTAssertEqual(stroke?.opacity(at: fadeStart + DirectLiveFrames.inkFadeMs / 2) ?? 0, 0.5, accuracy: 0.001)
        clock.advance(by: 2)
        engine.prune()
        XCTAssertTrue(engine.taps(in: conversation).isEmpty)
        XCTAssertEqual(engine.strokes(in: conversation).count, 1)
        clock.advance(by: Double(DirectLiveFrames.inkLifeMs) / 1000)
        XCTAssertFalse(engine.prune())
        XCTAssertTrue(engine.strokes(in: conversation).isEmpty)
    }

    @MainActor func testSharingMotionSendsSamplesAndAStop() async throws {
        let clock = FixedClock(epochMillis: 2_000)
        let sent = DirectLiveSent()
        let engine = makeEngine(clock, sent)
        let conversation = DirectLiveTests.conversation
        engine.addMotion(DirectLiveMotionSample(at: 1_990, gx: 0, gy: -1, gz: 0))
        try await Task.sleep(nanoseconds: 20_000_000)
        XCTAssertTrue(sent.frames.isEmpty, "nothing is sent until sharing is on")
        engine.setSharingMotion(conversation, true)
        XCTAssertTrue(engine.isSharingMotion(conversation))
        engine.addMotion(DirectLiveMotionSample(at: 1_950, gx: 0.5, gy: -0.5, gz: 0, ax: 0.25))
        let shared = await waitUntil { sent.frames.contains { $0.1["motion"] != nil } }
        XCTAssertTrue(shared)
        XCTAssertEqual(sent.frames.last?.1["motion"], .array([.array([.int(-50), .int(50), .int(-50), .int(0), .int(25), .int(0), .int(0)])]))
        engine.stopSharingMotion()
        XCTAssertFalse(engine.isSharingMotion(conversation))
        let stopped = await waitUntil { sent.frames.last?.1["motionOff"] == .bool(true) }
        XCTAssertTrue(stopped)
    }

    @MainActor func testLiveFramesTravelSealedAndUnstoredThroughTheDirectRoom() async throws {
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws)
        await service.refresh()
        let conversation = service.conversations[0]
        let early = await service.sendLive(conversation, fields: ["taps": [[1, 1]]])
        XCTAssertFalse(early, "no socket, nothing is sent")
        _ = await service.open(conversation)
        let connection = try XCTUnwrap(ws.lastConnection)
        XCTAssertEqual(service.liveInterval(conversation), DirectLiveFrames.fallbackIntervalMs)
        connection.receive(text: #"{"kind":"forkmesh-live","v":1,"perWindow":150,"windowMs":10000}"#)
        let budgeted = await waitUntil { service.liveInterval(conversation) == 88 }
        XCTAssertTrue(budgeted)

        let crypto = await roomCrypto()
        let peer = try peerSeal("peer")
        connection.receive(text: JSON.encode(try crypto.encrypt(peer.keyFrame())))
        let taught = await teach(peer, from: connection, crypto)
        XCTAssertTrue(taught)
        let learned = await waitUntil { service.deviceKeys(conversation).contains(peer.keyId) }
        XCTAssertTrue(learned)

        let engine = service.makeLiveEngine()
        var felt = 0
        engine.onTap = { _, _ in felt += 1 }
        engine.tap(conversation, at: DirectLivePoint(x: 0.5, y: 0))
        let liveSent = await waitUntil {
            connection.sentTexts.map { JSON.parseObject($0) }.contains { $0["live"] == .bool(true) }
        }
        XCTAssertTrue(liveSent)
        let envelope = try XCTUnwrap(connection.sentTexts.map { JSON.parseObject($0) }.first { $0["live"] == .bool(true) })
        XCTAssertNil(envelope["persist"])
        XCTAssertNil(envelope["notify"])
        let plain = try XCTUnwrap(crypto.decrypt(envelope).flatMap { peer.unseal($0) })
        XCTAssertEqual(plain["type"]?.text, "live")
        XCTAssertEqual(plain["channel"]?.text, conversation)
        XCTAssertEqual(plain["taps"], .array([.array([.int(500), .int(0)])]))

        let fromPat: JSONObject = [
            "type": "live", "id": "pat-live-1", "senderId": "pat-node", "sender": "pat",
            "channel": .string(conversation), "ts": 1000, "taps": [[100, 100]],
        ]
        connection.receive(text: JSON.encode(try crypto.encrypt(try peer.seal(fromPat))))
        var replayed = try crypto.encrypt(try peer.seal(fromPat))
        replayed["historyReplay"] = true
        connection.receive(text: JSON.encode(replayed))
        let got = await waitUntil { felt == 1 }
        XCTAssertTrue(got)
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(felt, 1, "a replayed live frame is not played again")
        XCTAssertTrue(service.messages(conversation).isEmpty)
        await service.closeAll()
    }

    @MainActor func testADirectBackgroundIsSharedWithThePeer() async throws {
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
        XCTAssertTrue(hub.canChangeLook)
        let opened = await waitUntil { service.isConnected(conversation) }
        XCTAssertTrue(opened)
        let connection = try XCTUnwrap(ws.lastConnection)
        let crypto = await roomCrypto()
        let peer = try peerSeal("peer")
        connection.receive(text: JSON.encode(try crypto.encrypt(peer.keyFrame())))
        _ = await teach(peer, from: connection, crypto)
        let learned = await waitUntil { service.deviceKeys(conversation).contains(peer.keyId) }
        XCTAssertTrue(learned)

        let changed = await hub.setLook(ChatLookChange(field: .background, preset: "ocean"))
        XCTAssertTrue(changed)
        XCTAssertEqual(hub.conversationLook.backgroundPreset, "ocean")
        let envelope = try XCTUnwrap(
            connection.sentTexts.map { JSON.parseObject($0) }.first {
                guard let sealed = crypto.decrypt($0), RoomSeal.isSealedFrame(sealed) else { return false }
                return peer.unseal(sealed)?["type"]?.text == "look"
            })
        XCTAssertEqual(envelope["persist"], .bool(true))
        XCTAssertNil(envelope["notify"], "a background change does not ring the other phone")
        let plain = try XCTUnwrap(crypto.decrypt(envelope).flatMap { peer.unseal($0) })
        XCTAssertEqual(plain["preset"]?.text, "ocean")

        let fromPat: JSONObject = [
            "type": "look", "id": "pat-look", "senderId": "pat-node", "sender": "pat", "field": "background",
            "preset": "forest", "channel": .string(conversation), "ts": .int(Int64(Date().timeIntervalSince1970 * 1000) + 5_000),
        ]
        connection.receive(text: JSON.encode(try crypto.encrypt(try peer.seal(fromPat))))
        let applied = await waitUntil { hub.conversationLook.backgroundPreset == "forest" }
        XCTAssertTrue(applied)
        XCTAssertTrue(service.messages(conversation).isEmpty)
        await service.closeAll()
        await harness.close()
    }
}
