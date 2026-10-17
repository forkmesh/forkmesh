import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest

@testable import FMChat

@MainActor
final class MeshDirectMessagesTests: XCTestCase {
    static let conversationId = String(repeating: "f", count: 32)
    static let room = "chat-direct:\(conversationId):v1"
    static let channel = "direct:\(conversationId)"

    struct Phone {
        let device: ChatDevice
        let dm: DirectMessageService
        let mesh: LocalMeshService
        let connection: FakeWebSocketConnection
        let kid: String
    }

    func api(other: String) -> FakeAccountApi {
        let api = FakeAccountApi()
        api.replies["GET /api/chat/direct-messages"] = [
            "conversations": [["id": .string(Self.conversationId), "otherUser": .string(other)]]
        ]
        api.replies["GET /api/chat/direct-messages/\(Self.conversationId)/room-access"] = [
            "room": .string(Self.room), "passphrase": "direct-pass",
            "webSocketUrl": .string("/api/chat/direct-messages/\(Self.conversationId)/ws?ticket=abc"),
        ]
        return api
    }

    func roomSeal(_ device: ChatDevice, as acct: String? = nil, node: String? = nil) throws -> RoomSeal {
        let name = acct ?? device.account
        return try RoomSeal.loadOrCreate(
            Self.room, scope: Self.channel, policy: .strict, acct: name, node: node ?? "\(name)-node",
            secureStore: device.secure)
    }

    func crypto() async -> RoomCrypto {
        await Task.detached { RoomCrypto.withPassphrase(MeshDirectMessagesTests.room, "direct-pass") }.value
    }

    func phone(
        _ name: String, other: String,
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        }
    ) async throws -> Phone {
        let device = ChatDevice(name: name)
        let ws = FakeWebSocketConnector()
        let dm = DirectMessageService(
            api: api(other: other), connector: ws, secureStore: device.secure,
            origin: { URL(string: "https://app.forkmesh.com") }, selfId: { "\(name)-node" }, displayName: { name },
            sleep: sleep)
        await dm.refresh()
        let opened = await dm.open(Self.channel)
        XCTAssertTrue(opened)
        let mesh = makeMesh(device)
        dm.attachMesh(mesh)
        let started = await mesh.start()
        XCTAssertTrue(started)
        return Phone(
            device: device, dm: dm, mesh: mesh, connection: try XCTUnwrap(ws.lastConnection), kid: mesh.sealKeyId)
    }

    func introduce(_ a: Phone, _ b: Phone) async throws {
        let crypto = await crypto()
        a.connection.receive(text: JSON.encode(try crypto.encrypt(try roomSeal(b.device).keyFrame())))
        b.connection.receive(text: JSON.encode(try crypto.encrypt(try roomSeal(a.device).keyFrame())))
        let learned = await waitUntil { a.dm.deviceKeys(Self.channel) == [b.kid] && b.dm.deviceKeys(Self.channel) == [a.kid] }
        XCTAssertTrue(learned, "each direct room knows the other phone's one device key")
    }

    func link(_ a: Phone, _ b: Phone) async {
        _ = await a.mesh.connectToPeer(.loopbackIPv4, port: b.mesh.listenPort)
        let linked = await waitUntil {
            a.mesh.directLinks()[b.kid] != nil && b.mesh.directLinks()[a.kid] != nil
                && a.mesh.links.contains { $0.peerId == b.kid && $0.keyKnown }
        }
        XCTAssertTrue(linked)
    }

    func relayChats(_ phone: Phone) -> [JSONObject] {
        phone.connection.sentJSON.filter { $0["notify"] == .bool(true) }
    }

    func relayCopy(_ phone: Phone, to reader: Phone, id: String) async -> Bool {
        let crypto = await crypto()
        guard let peek = try? roomSeal(reader.device) else { return false }
        return relayChats(phone).contains { envelope in
            guard let sealed = crypto.decrypt(envelope) else { return false }
            return peek.unseal(sealed)?["id"]?.text == id
        }
    }

    func testADirectMessageReachesTheOtherPhoneOverTheMeshWithTheRelayDown() async throws {
        let ann = try await phone("ann", other: "pat")
        let pat = try await phone("pat", other: "ann")
        let eve = try await phone("eve", other: "ann")
        try await introduce(ann, pat)
        await link(ann, pat)
        await link(eve, ann)
        await link(eve, pat)
        var patAlerts: [ChatMessage] = []
        pat.dm.onMessage = { patAlerts.append($0) }
        var eveSaw = 0
        eve.mesh.onDirectSealed = { _, _, _ in eveSaw += 1 }
        XCTAssertEqual(ann.dm.route(Self.channel), .wifi)

        ann.connection.failSends(with: WebSocketError.transport("offline"))
        let sent = await ann.dm.send(Self.channel, text: "the router is down")
        XCTAssertTrue(sent, "the send still succeeds over the mesh")
        XCTAssertEqual(ann.dm.messages(Self.channel).map(\.text), ["the router is down"])
        let arrived = await waitUntil { pat.dm.messages(Self.channel).contains { $0.text == "the router is down" } }
        XCTAssertTrue(arrived, "pat's phone shows it")
        XCTAssertEqual(pat.dm.messages(Self.channel).first?.senderName, "ann")
        XCTAssertEqual(pat.dm.messages(Self.channel).first?.senderId, "ann-node")
        XCTAssertEqual(pat.dm.messages(Self.channel).first?.isSelf, false)
        XCTAssertEqual(patAlerts.count, 1, "and raises one alert")
        let id = try XCTUnwrap(ann.dm.messages(Self.channel).first?.id)
        let acked = await waitUntil { ann.dm.receipt(id) == .delivered }
        XCTAssertTrue(acked, "the direct-ack marks it delivered")
        _ = await waitUntil(0.3) { false }
        XCTAssertEqual(eveSaw, 0, "the third phone on the mesh is never sent it")
        XCTAssertTrue(eve.mesh.messages.isEmpty && pat.mesh.messages.isEmpty, "and it never lands in #mesh")

        let forger = try roomSeal(eve.device, as: "ann", node: "ann-node")
        forger.acceptBundle(try roomSeal(pat.device).keyFrame())
        let forged = try forger.seal(
            ["type": "chat", "id": "eve-1", "senderId": "ann-node", "sender": "ann", "channel": .string(Self.channel),
             "text": "it is ann, honest", "ts": 1])
        let addressed = await eve.mesh.sendDirectSealed(forged, channel: Self.channel, to: pat.kid)
        XCTAssertTrue(addressed, "eve can address pat's device")
        _ = await waitUntil(0.5) { false }
        XCTAssertFalse(
            pat.dm.messages(Self.channel).contains { $0.text.contains("honest") },
            "but pat refuses a device the conversation never approved")
        for phone in [ann, pat, eve] { await phone.mesh.stop() }
    }

    func testWhileBothLookAtTheChatNothingReachesTheServer() async throws {
        let ann = try await phone("ann", other: "pat")
        let pat = try await phone("pat", other: "ann")
        try await introduce(ann, pat)
        await link(ann, pat)
        ann.dm.setFocus(Self.channel, true)
        pat.dm.setFocus(Self.channel, true)
        let directOnly = await waitUntil {
            ann.dm.route(Self.channel) == .directOnly && pat.dm.route(Self.channel) == .directOnly
        }
        XCTAssertTrue(directOnly)
        XCTAssertEqual(DirectRoute.directOnly.label, "Direct only")
        let before = relayChats(ann).count

        let sent = await ann.dm.send(Self.channel, text: "just us")
        XCTAssertTrue(sent)
        let arrived = await waitUntil { pat.dm.messages(Self.channel).contains { $0.text == "just us" } }
        XCTAssertTrue(arrived)
        let id = try XCTUnwrap(ann.dm.messages(Self.channel).last?.id)
        let delivered = await waitUntil { ann.dm.receipt(id) == .delivered }
        XCTAssertTrue(delivered)
        _ = await waitUntil(0.5) { false }
        XCTAssertEqual(relayChats(ann).count, before, "no relay copy while both have focus")

        pat.dm.setFocus(Self.channel, false)
        let left = await waitUntil { ann.dm.route(Self.channel) == .wifi }
        XCTAssertTrue(left, "the route drops back once the other side looks away")
        XCTAssertEqual(DirectRoute.wifi.label, "Direct \u{00B7} Wi-Fi")
        let second = await ann.dm.send(Self.channel, text: "now via both")
        XCTAssertTrue(second)
        let secondId = try XCTUnwrap(ann.dm.messages(Self.channel).last?.id)
        let copied = await waitUntil { self.relayChats(ann).count == before + 1 }
        XCTAssertTrue(copied, "and the relay copy keeps other devices in step")
        let both = await relayCopy(ann, to: pat, id: secondId)
        XCTAssertTrue(both)
        let once = await waitUntil { pat.dm.messages(Self.channel).filter { $0.id == secondId }.count == 1 }
        XCTAssertTrue(once)
        for phone in [ann, pat] { await phone.mesh.stop() }
    }

    func testWithNoDirectAckTheRelayCopyFollows() async throws {
        let quick: @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(min(max(0, seconds), 0.3) * 1_000_000_000))
        }
        let ann = try await phone("ann", other: "pat", sleep: quick)
        let pat = try await phone("pat", other: "ann")
        try await introduce(ann, pat)
        await link(ann, pat)
        pat.mesh.onDirectSealed = { _, _, _ in }
        ann.dm.setFocus(Self.channel, true)
        pat.dm.setFocus(Self.channel, true)
        let directOnly = await waitUntil { ann.dm.route(Self.channel) == .directOnly }
        XCTAssertTrue(directOnly)
        let sent = await ann.dm.send(Self.channel, text: "anyone?")
        XCTAssertTrue(sent)
        let id = try XCTUnwrap(ann.dm.messages(Self.channel).last?.id)
        let fellBack = await waitUntil(3) { self.relayChats(ann).count == 1 }
        XCTAssertTrue(fellBack, "after the ack timeout the relay copy goes out")
        let sameId = await relayCopy(ann, to: pat, id: id)
        XCTAssertTrue(sameId, "with the same message id")
        for phone in [ann, pat] { await phone.mesh.stop() }
    }

    func testALinkDropSendsTheRelayCopyAtOnce() async throws {
        let ann = try await phone("ann", other: "pat")
        let pat = try await phone("pat", other: "ann")
        try await introduce(ann, pat)
        await link(ann, pat)
        pat.mesh.onDirectSealed = { _, _, _ in }
        ann.dm.setFocus(Self.channel, true)
        pat.dm.setFocus(Self.channel, true)
        let directOnly = await waitUntil { ann.dm.route(Self.channel) == .directOnly }
        XCTAssertTrue(directOnly)
        let sent = await ann.dm.send(Self.channel, text: "still there?")
        XCTAssertTrue(sent)
        let id = try XCTUnwrap(ann.dm.messages(Self.channel).last?.id)
        await pat.mesh.stop()
        let fellBack = await waitUntil(1.5) { self.relayChats(ann).count == 1 }
        XCTAssertTrue(fellBack, "the link drop does not wait out the ack timeout")
        let sameId = await relayCopy(ann, to: pat, id: id)
        XCTAssertTrue(sameId)
        let none = await waitUntil { ann.dm.route(Self.channel) == .none }
        XCTAssertTrue(none)
        await ann.mesh.stop()
    }

    func testNoLinkedRecipientMeansTheSendStillNeedsTheRelay() async throws {
        let device = ChatDevice(name: "ann")
        let ws = FakeWebSocketConnector()
        let dm = DirectMessageService(
            api: api(other: "pat"), connector: ws, secureStore: device.secure,
            origin: { URL(string: "https://app.forkmesh.com") }, selfId: { "ann-node" }, displayName: { "ann" })
        await dm.refresh()
        let opened = await dm.open(Self.channel)
        XCTAssertTrue(opened)
        let mesh = makeMesh(device)
        dm.attachMesh(mesh)
        try XCTUnwrap(ws.lastConnection).failSends(with: WebSocketError.transport("offline"))
        let sent = await dm.send(Self.channel, text: "hello?")
        XCTAssertFalse(sent, "nothing reached anyone")
        XCTAssertEqual(dm.route(Self.channel), .none)
        XCTAssertTrue(dm.messages(Self.channel).isEmpty)
    }
}
