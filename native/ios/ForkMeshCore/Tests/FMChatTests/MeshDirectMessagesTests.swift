import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest

@testable import FMChat

final class MeshDirectMessagesTests: XCTestCase {
    static let conversationId = String(repeating: "f", count: 32)
    static let room = "chat-direct:\(conversationId):v1"
    static let channel = "direct:\(conversationId)"

    @MainActor func api(other: String) -> FakeAccountApi {
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

    @MainActor func service(_ device: ChatDevice, other: String, ws: FakeWebSocketConnector) -> DirectMessageService {
        let name = device.account
        return DirectMessageService(
            api: api(other: other), connector: ws, secureStore: device.secure,
            origin: { URL(string: "https://app.forkmesh.com") }, selfId: { "\(name)-node" }, displayName: { name })
    }

    @MainActor func teach(_ connection: FakeWebSocketConnection, keyOf device: ChatDevice) async throws {
        let crypto = await Task.detached { RoomCrypto.withPassphrase(MeshDirectMessagesTests.room, "direct-pass") }.value
        let other = try RoomSeal.loadOrCreate(Self.room, storageScope: "peer", secureStore: device.secure)
        connection.receive(text: JSON.encode(try crypto.encrypt(other.keyFrame())))
    }

    @MainActor func testADirectMessageReachesTheOtherPhoneOverTheMeshWithTheRelayDown() async throws {
        let annDevice = ChatDevice(name: "ann")
        let patDevice = ChatDevice(name: "pat")
        let eveDevice = ChatDevice(name: "eve")
        let annWs = FakeWebSocketConnector()
        let patWs = FakeWebSocketConnector()
        let ann = service(annDevice, other: "pat", ws: annWs)
        let pat = service(patDevice, other: "ann", ws: patWs)
        await ann.refresh()
        await pat.refresh()
        let annOpened = await ann.open(Self.channel)
        let patOpened = await pat.open(Self.channel)
        XCTAssertTrue(annOpened && patOpened)
        let annConnection = try XCTUnwrap(annWs.lastConnection)
        try await teach(annConnection, keyOf: patDevice)
        try await teach(try XCTUnwrap(patWs.lastConnection), keyOf: annDevice)

        let annMesh = makeMesh(annDevice)
        let patMesh = makeMesh(patDevice)
        let eveMesh = makeMesh(eveDevice)
        for mesh in [annMesh, patMesh, eveMesh] {
            let started = await mesh.start()
            XCTAssertTrue(started)
        }
        let learned = await waitUntil {
            ann.deviceKeys(Self.channel) == [patMesh.sealKeyId] && pat.deviceKeys(Self.channel) == [annMesh.sealKeyId]
        }
        XCTAssertTrue(learned, "each direct room knows the other phone's one device key")
        XCTAssertNotEqual(patMesh.sealKeyId, eveMesh.sealKeyId)
        _ = await annMesh.connectToPeer(.loopbackIPv4, port: patMesh.listenPort)
        _ = await eveMesh.connectToPeer(.loopbackIPv4, port: annMesh.listenPort)
        _ = await eveMesh.connectToPeer(.loopbackIPv4, port: patMesh.listenPort)
        let linked = await waitUntil { annMesh.peerCount == 2 && patMesh.peerCount == 2 && eveMesh.peerCount == 2 }
        XCTAssertTrue(linked, "the three phones link")
        _ = await waitUntil(0.3) { false }

        ann.meshSender = { plain, keys in await annMesh.sendDirect(plain, to: keys) }
        patMesh.onDirect = { channel, keyId, plain in _ = pat.acceptMeshDirect(channel, sealKeyId: keyId, plain: plain) }
        var eveSaw: [JSONObject] = []
        eveMesh.onDirect = { _, _, plain in eveSaw.append(plain) }
        var patAlerts: [ChatMessage] = []
        pat.onMessage = { patAlerts.append($0) }

        annConnection.failSends(with: WebSocketError.transport("offline"))
        let sent = await ann.send(Self.channel, text: "the router is down")
        XCTAssertTrue(sent, "the send still succeeds over the mesh")
        XCTAssertEqual(ann.messages(Self.channel).map(\.text), ["the router is down"])
        let arrived = await waitUntil { pat.messages(Self.channel).contains { $0.text == "the router is down" } }
        XCTAssertTrue(arrived, "pat's phone shows it")
        XCTAssertEqual(pat.messages(Self.channel).first?.senderName, "ann")
        XCTAssertEqual(pat.messages(Self.channel).first?.isSelf, false)
        XCTAssertEqual(patAlerts.count, 1, "and raises one alert")
        _ = await waitUntil(0.3) { false }
        XCTAssertTrue(eveSaw.isEmpty, "the third phone on the mesh is never sent it")
        XCTAssertTrue(eveMesh.messages.isEmpty && patMesh.messages.isEmpty, "and it never lands in #mesh")

        let forged: JSONObject = [
            "type": "direct", "id": "eve-1", "senderId": "ann-node", "sender": "ann",
            "channel": .string(Self.channel), "text": "it is ann, honest", "ts": 1,
        ]
        let addressed = await eveMesh.sendDirect(forged, to: [patMesh.sealKeyId])
        XCTAssertEqual(addressed, 1, "eve can address pat's device key")
        _ = await waitUntil(0.5) { false }
        XCTAssertFalse(
            pat.messages(Self.channel).contains { $0.text.contains("honest") },
            "but pat refuses a key the conversation never saw")

        let shown = try XCTUnwrap(pat.messages(Self.channel).first)
        XCTAssertFalse(
            pat.acceptMeshDirect(
                Self.channel, sealKeyId: annMesh.sealKeyId,
                plain: ["channel": .string(Self.channel), "id": .string(shown.id), "text": .string(shown.text)]),
            "a message already shown is not shown twice")

        for mesh in [annMesh, patMesh, eveMesh] { await mesh.stop() }
    }

    @MainActor func testNoLinkedRecipientMeansTheSendStillNeedsTheRelay() async throws {
        let device = ChatDevice(name: "ann")
        let ws = FakeWebSocketConnector()
        let dm = service(device, other: "pat", ws: ws)
        await dm.refresh()
        let opened = await dm.open(Self.channel)
        XCTAssertTrue(opened)
        try XCTUnwrap(ws.lastConnection).failSends(with: WebSocketError.transport("offline"))
        var asked = 0
        dm.meshSender = { _, _ in
            asked += 1
            return 0
        }
        let sent = await dm.send(Self.channel, text: "hello?")
        XCTAssertFalse(sent, "nothing reached anyone")
        XCTAssertEqual(asked, 0, "no device key is known, so the mesh is not even asked")
        XCTAssertTrue(dm.messages(Self.channel).isEmpty)
    }
}
