import Foundation
import XCTest

@testable import FMChat

@MainActor
final class PromptGatedBluetooth: MeshNativeTransport {
    let events = AsyncStream<MeshBluetoothEvent> { _ in }
    private(set) var listening = false
    private(set) var opened = 0
    var unavailable: Bool { false }

    func startBluetooth() async -> Bool {
        listening = opened > 0
        return listening
    }

    func stopBluetooth() async { listening = false }
    func sendBluetooth(_ linkId: Int, _ frame: Data) async -> Bool { false }
    func closeBluetoothLink(_ linkId: Int) async {}
    func roomOpened() async { opened += 1 }
    func acquireMulticastLock() async -> Bool { false }
    func releaseMulticastLock() async {}
}

final class MeshBleTests: XCTestCase {
    func testUuidsMatchTheDesktop() async {
        XCTAssertEqual(meshBleServiceUuid.lowercased(), "7f9e5c14-4d3b-4a21-9c6f-2b7ad1e05f11")
        XCTAssertEqual(meshBleInboxUuid.lowercased(), "7f9e5c15-4d3b-4a21-9c6f-2b7ad1e05f11")
        XCTAssertEqual(meshBleOutboxUuid.lowercased(), "7f9e5c16-4d3b-4a21-9c6f-2b7ad1e05f11")
        XCTAssertNotEqual(meshBleServiceUuid.lowercased(), meshBluetoothServiceUuid)
    }

    func testAdvertisedTokenRoundTrips() async {
        let token = MeshBle.newToken()
        XCTAssertEqual(token.count, 8)
        XCTAssertEqual(MeshBle.token(fromAdvertisedName: MeshBle.advertisedName(token: token)), token)
        XCTAssertNil(MeshBle.token(fromAdvertisedName: nil))
        XCTAssertNil(MeshBle.token(fromAdvertisedName: "Jett's iPhone"))
        XCTAssertNil(MeshBle.token(fromAdvertisedName: "FM-"))
    }

    func testExactlyOneOfTwoPhonesDials() async {
        let low = "0000beef"
        let high = "ffff0000"
        XCTAssertTrue(MeshBle.shouldDial(ownToken: high, peerName: MeshBle.advertisedName(token: low)))
        XCTAssertFalse(MeshBle.shouldDial(ownToken: low, peerName: MeshBle.advertisedName(token: high)))
        XCTAssertTrue(MeshBle.shouldDial(ownToken: low, peerName: nil))
        XCTAssertTrue(MeshBle.shouldDial(ownToken: low, peerName: "MacBook"))
    }

    func testChunkSizeStaysInsideAttLimits() async {
        XCTAssertEqual(MeshBle.chunkBytes(0), 20)
        XCTAssertEqual(MeshBle.chunkBytes(182), 182)
        XCTAssertEqual(MeshBle.chunkBytes(4096), 512)
    }

    func testLongWritesJoinInOffsetOrder() async {
        let joined = MeshBle.joinWrites([
            (offset: 0, value: Data([1, 2])), (offset: 2, value: Data([3])), (offset: 3, value: Data([4, 5])),
        ])
        XCTAssertEqual(joined, Data([1, 2, 3, 4, 5]))
        XCTAssertNil(MeshBle.joinWrites([(offset: 1, value: Data([1]))]))
        XCTAssertNil(MeshBle.joinWrites([(offset: 0, value: Data([1])), (offset: 3, value: Data([2]))]))
        XCTAssertEqual(MeshBle.joinWrites([(offset: 0, value: Data())]), Data())
    }

    func testOutboxResendsTheHeadUntilAdvanced() async {
        var outbox = MeshBleOutbox()
        XCTAssertTrue(outbox.isEmpty)
        XCTAssertNil(outbox.peek(max: 4))
        outbox.enqueue(Data([1, 2, 3, 4, 5, 6]))
        XCTAssertEqual(outbox.peek(max: 4), Data([1, 2, 3, 4]))
        XCTAssertEqual(outbox.peek(max: 2), Data([1, 2, 3, 4]))
        outbox.advance()
        outbox.enqueue(Data([7]))
        XCTAssertEqual(outbox.peek(max: 4), Data([5, 6, 7]))
        outbox.advance()
        XCTAssertTrue(outbox.isEmpty)
    }

    func testOutboxCarriesALargeFrameInOrder() async {
        var outbox = MeshBleOutbox()
        let frame = Data((0..<100_000).map { UInt8($0 % 251) })
        outbox.enqueue(frame.prefix(40_000))
        var received = Data()
        var rounds = 0
        while let chunk = outbox.peek(max: 182) {
            received.append(chunk)
            outbox.advance()
            rounds += 1
            if rounds == 10 { outbox.enqueue(frame.suffix(from: 40_000)) }
        }
        XCTAssertEqual(received, frame)
    }

    @MainActor
    func testBluetoothWaitsForTheMeshRoomToBeOpened() async {
        let device = ChatDevice(name: "ada")
        let radio = PromptGatedBluetooth()
        let mesh = LocalMeshService(
            settings: device.settings, identity: device.identity, secureStore: device.secure, transport: radio)
        mesh.networkEnabled = false
        let atLaunch = await mesh.start()
        XCTAssertFalse(atLaunch, "no Bluetooth prompt before the user opens #mesh")
        XCTAssertFalse(mesh.running)

        await mesh.roomOpened()
        XCTAssertEqual(radio.opened, 1)
        XCTAssertTrue(mesh.running)
        XCTAssertTrue(mesh.bluetoothActive)
        await mesh.stop()
    }
}
