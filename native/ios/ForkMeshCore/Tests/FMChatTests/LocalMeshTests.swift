// These cases pin the wire format the desktop client also
// speaks (desktop/src/LocalMeshNode.cpp) and prove the room itself: two devices link, talk,
// backfill a late joiner, reach a device neither of them dialled, and do all of it over Bluetooth
// when there is no network at all. The room cases use real loopback sockets.

import FMApi
import FMBase
import FMCrypto
import Foundation
import XCTest

@testable import FMChat

/// Stands in for a Bluetooth plugin: two of these, crossed over, are a Bluetooth link with no
/// Bluetooth.
@MainActor
final class FakeBluetooth: MeshNativeTransport {
    let label: String
    weak var peer: FakeBluetooth?
    let events: AsyncStream<MeshBluetoothEvent>
    private let continuation: AsyncStream<MeshBluetoothEvent>.Continuation
    private var started = false
    private(set) var duplicateCloses = 0

    init(_ label: String) {
        self.label = label
        var cont: AsyncStream<MeshBluetoothEvent>.Continuation!
        events = AsyncStream { cont = $0 }
        continuation = cont
    }

    static func pair(_ a: String, _ b: String) -> (FakeBluetooth, FakeBluetooth) {
        let left = FakeBluetooth(a)
        let right = FakeBluetooth(b)
        left.peer = right
        right.peer = left
        return (left, right)
    }

    var listening: Bool { started }
    var unavailable: Bool { false }

    func startBluetooth() async -> Bool {
        started = true
        return true
    }

    func stopBluetooth() async { started = false }

    func acquireMulticastLock() async -> Bool { false }

    func releaseMulticastLock() async {}

    /// Both sides see a link open, as a completed RFCOMM connect would.
    func connect() {
        continuation.yield(MeshBluetoothEvent(kind: .linkOpened, linkId: 1, outbound: true))
        peer?.continuation.yield(MeshBluetoothEvent(kind: .linkOpened, linkId: 1, outbound: false))
    }

    func sendBluetooth(_ linkId: Int, _ frame: Data) async -> Bool {
        peer?.continuation.yield(MeshBluetoothEvent(kind: .data, linkId: linkId, bytes: frame))
        return true
    }

    func closeBluetoothLink(_ linkId: Int) async {
        await closeBluetoothLink(linkId, duplicate: false)
    }

    func closeBluetoothLink(_ linkId: Int, duplicate: Bool) async {
        if duplicate { duplicateCloses += 1 }
        peer?.continuation.yield(MeshBluetoothEvent(kind: .linkClosed, linkId: linkId))
    }
}

final class MeshWireTests: XCTestCase {
    private func address(_ text: String) -> MeshAddress { MeshAddress(text)! }

    // MARK: what may reach the room

    func testOnlyAddressesThatCannotLeaveTheLocalNetwork() {
        for text in ["192.168.1.20", "10.4.4.4", "172.20.0.9", "169.254.7.7", "127.0.0.1", "::1", "fd12::1", "fe80::1"] {
            XCTAssertTrue(isLocalNetworkAddress(address(text)), text)
        }
    }

    func testARoutableAddressIsNeverAPeer() {
        XCTAssertFalse(isLocalNetworkAddress(address("8.8.8.8")))
        XCTAssertFalse(isLocalNetworkAddress(address("172.32.0.1")))
        // Carrier-grade NAT belongs to an ISP, not to a LAN.
        XCTAssertFalse(isLocalNetworkAddress(address("100.64.0.1")))
        XCTAssertFalse(isLocalNetworkAddress(address("2606:4700::1")))
    }

    func testAnIPv4MappedAddressIsJudgedAsTheIPv4ItCarries() {
        XCTAssertTrue(isLocalNetworkAddress(address("::ffff:192.168.5.5")))
        XCTAssertFalse(isLocalNetworkAddress(address("::ffff:8.8.8.8")))
    }

    func testAddressesParseAndPrintLikeDart() {
        XCTAssertEqual(address("192.168.1.20").address, "192.168.1.20")
        XCTAssertEqual(address("fe80::1").address, "fe80::1")
        XCTAssertEqual(address("127.0.0.1").rawAddress, [127, 0, 0, 1])
        XCTAssertNil(MeshAddress("relay.example.com"))
        XCTAssertNil(MeshAddress(rawAddress: [1, 2, 3]))
    }

    // MARK: wire format

    func testAFrameComesBackByteForByte() throws {
        let payload = Array(#"{"hello":"mesh"}"#.utf8)
        let buffer = MeshByteBuffer()
        buffer.add(meshFrameBytes(payload))
        buffer.add(meshFrameBytes(payload))
        XCTAssertEqual(try meshTakeFrame(buffer).map(Array.init), payload)
        XCTAssertEqual(try meshTakeFrame(buffer).map(Array.init), payload)
        XCTAssertNil(try meshTakeFrame(buffer))
    }

    func testAHalfArrivedFrameWaitsInTheBuffer() throws {
        let framed = Array(meshFrameBytes(Array(#"{"a":1}"#.utf8)))
        let buffer = MeshByteBuffer()
        buffer.add(framed[0..<(framed.count - 2)])
        XCTAssertNil(try meshTakeFrame(buffer))
        buffer.add(framed[(framed.count - 2)...])
        XCTAssertNotNil(try meshTakeFrame(buffer))
    }

    func testAnImpossibleLengthIsRefusedRatherThanBuffered() {
        let buffer = MeshByteBuffer()
        buffer.add([0x7f, 0xff, 0xff, 0xff])
        XCTAssertThrowsError(try meshTakeFrame(buffer))
    }

    func testABufferSurvivesManyFramesTakenFromTheFront() throws {
        let buffer = MeshByteBuffer()
        for i in 0..<500 {
            buffer.add(meshFrameBytes(Array("frame-\(i)".utf8)))
            XCTAssertEqual(try meshTakeFrame(buffer).map { String(decoding: $0, as: UTF8.self) }, "frame-\(i)")
        }
        XCTAssertEqual(buffer.count, 0)
    }

    func testABeaconCarriesOnlyTheKeyIdAndHowToReachIt() {
        let beacon = MeshBeacon(nodeId: "7N0av-G4pgHJPeGe", port: 4242, timestampMs: 1_750_000_000_000)
        let parsed = MeshBeacon.parse(beacon.toJSON())
        XCTAssertEqual(parsed?.nodeId, "7N0av-G4pgHJPeGe")
        XCTAssertEqual(parsed?.port, 4242)
        XCTAssertEqual(parsed?.timestampMs, 1_750_000_000_000)
        XCTAssertEqual(parsed?.control, false)
        XCTAssertEqual(Set(beacon.toJSON().keys), ["t", "v", "id", "port", "ts"])
    }

    func testTheBeaconWireOrderIsTheV3Shape() {
        let json = JSON.encode(.object(MeshBeacon(nodeId: "a", port: 1, reply: true, control: true).toJSON()))
        XCTAssertEqual(json, #"{"t":"forkmesh-lan-mesh","v":3,"id":"a","port":1,"ts":0,"reply":true,"control":true}"#)
        var withNode = MeshBeacon(nodeId: "a", port: 1, control: true).toJSON()
        withNode["node"] = "desk-node-key"
        let parsed = try? XCTUnwrap(MeshBeacon.parse(withNode))
        XCTAssertEqual(parsed?.control, true)
        XCTAssertFalse(JSON.encode(.object(parsed?.toJSON() ?? [:])).contains("desk-node-key"), "a beacon names no node key")
    }

    func testABeaconIdIsRandomAndNeverTheKeyId() {
        let first = MeshBeacon.randomId()
        XCTAssertEqual(first.count, 16)
        XCTAssertNotEqual(first, MeshBeacon.randomId())
    }

    func testAReplyBeaconSaysSoAndAnOrdinaryOneDoesNot() {
        let reply = MeshBeacon(nodeId: "a", port: 1, reply: true).toJSON()
        XCTAssertEqual(reply["reply"], .bool(true))
        XCTAssertEqual(MeshBeacon.parse(reply)?.reply, true)
        let plain = MeshBeacon(nodeId: "a", port: 1).toJSON()
        XCTAssertFalse(plain.contains("reply"))
        XCTAssertEqual(MeshBeacon.parse(plain)?.reply, false)
    }

    func testASubnetSweepStaysOnTheLocal24AndOffThisDevice() {
        let hosts = sweepAddresses(address("192.168.1.228"))
        XCTAssertEqual(hosts.count, 253)
        XCTAssertEqual(hosts.first?.address, "192.168.1.1")
        XCTAssertEqual(hosts.last?.address, "192.168.1.254")
        XCTAssertFalse(hosts.contains { $0.address == "192.168.1.228" })
        XCTAssertEqual(sweepAddresses(address("10.4.0.9")).count, 253)
        XCTAssertTrue(sweepAddresses(address("8.8.8.8")).isEmpty)
        XCTAssertTrue(sweepAddresses(address("127.0.0.1")).isEmpty)
        XCTAssertTrue(sweepAddresses(address("169.254.3.4")).isEmpty)
        XCTAssertTrue(sweepAddresses(address("fe80::1")).isEmpty)
    }

    func testTheBroadcastBeaconGoesToTheSwept24Only() {
        XCTAssertEqual(sweepBroadcastAddress(address("192.168.1.228"))?.address, "192.168.1.255")
        XCTAssertNil(sweepBroadcastAddress(address("8.8.8.8")))
        XCTAssertNil(sweepBroadcastAddress(address("127.0.0.1")))
        XCTAssertNil(sweepBroadcastAddress(address("169.254.3.4")))
    }

    func testAPersonalHotspotSweepCoversItsSlash28() {
        let hosts = sweepAddresses(address("172.20.10.1"), prefixLength: 28)
        XCTAssertEqual(hosts.map(\.address), (2...14).map { "172.20.10.\($0)" })
        XCTAssertEqual(sweepBroadcastAddress(address("172.20.10.1"), prefixLength: 28)?.address, "172.20.10.15")
        XCTAssertEqual(sweepAddresses(address("172.20.10.4"), prefixLength: 28).first?.address, "172.20.10.1")
        XCTAssertEqual(sweepAddresses(address("10.4.0.9"), prefixLength: 16).count, 253)
        XCTAssertEqual(sweepBroadcastAddress(address("10.4.0.9"), prefixLength: 16)?.address, "10.4.0.255")
        XCTAssertEqual(sweepAddresses(address("192.168.1.228"), prefixLength: -1).count, 253)
        XCTAssertTrue(sweepAddresses(address("192.168.1.228"), prefixLength: 31).isEmpty)
        XCTAssertNil(sweepBroadcastAddress(address("192.168.1.228"), prefixLength: 32))
    }

    func testTetheredComputersAreSweptButCellularAndVpnAreNot() {
        for name in ["en0", "en2", "bridge100", "wlan0", "eth0"] {
            XCTAssertTrue(isSweptInterfaceName(name), name)
        }
        for name in ["pdp_ip0", "utun3", "ipsec0", "awdl0", "llw0", "lo0", "anpi0"] {
            XCTAssertFalse(isSweptInterfaceName(name), name)
        }
    }

    func testInterfacesCarryEachAddressPrefixLength() throws {
        let loopback = try MeshInterfaces.list(includeLoopback: true).first {
            $0.addresses.contains { $0.address == "127.0.0.1" }
        }
        let entry = try XCTUnwrap(loopback)
        XCTAssertEqual(entry.prefixLengths.count, entry.addresses.count)
        let index = try XCTUnwrap(entry.addresses.firstIndex { $0.address == "127.0.0.1" })
        XCTAssertEqual(entry.prefixLengths[index], 8)
        XCTAssertEqual(MeshInterface(name: "x", addresses: [address("10.0.0.1")]).prefixLengths, [-1])
    }

    func testAnythingThatIsNotABeaconWeSpeakIsDropped() {
        let good = MeshBeacon(nodeId: "a", port: 1).toJSON()
        var other = good
        other["t"] = "other"
        XCTAssertNil(MeshBeacon.parse(other))
        var future = good
        future["v"] = 99
        XCTAssertNil(MeshBeacon.parse(future))
        var noPort = good
        noPort["port"] = 0
        XCTAssertNil(MeshBeacon.parse(noPort))
        XCTAssertNil(MeshBeacon.parse([:]))
        // A version written as a double still parses.
        var double = good
        double["v"] = .double(3)
        XCTAssertNotNil(MeshBeacon.parse(double))
        var old = good
        old["v"] = 1
        XCTAssertNil(MeshBeacon.parse(old), "a v1 beacon is ignored")
    }

    func testNamesInABeaconAreIgnored() {
        var beacon = MeshBeacon(nodeId: "a", port: 1).toJSON()
        beacon["name"] = "ada"
        beacon["ownerUser"] = "ada"
        let parsed = try? XCTUnwrap(MeshBeacon.parse(beacon))
        XCTAssertEqual(parsed?.toJSON().contains("name"), false)
        XCTAssertEqual(parsed?.toJSON().contains("ownerUser"), false)
    }

    func testTheBeaconOnTheWireIsCiphertextNotAnAdvertisement() async throws {
        let beacon = MeshBeacon(nodeId: "node-ada-1", port: 4242)
        let crypto = await Task.detached { RoomCrypto.shared(meshRoomName) }.value
        let envelope = try crypto.encrypt(beacon.toJSON())
        let datagram = JSON.encode(.object(envelope))
        XCTAssertFalse(datagram.contains("node-ada-1"))
        XCTAssertFalse(datagram.contains(meshBeaconTag))
        let back = try XCTUnwrap(crypto.decrypt(envelope))
        XCTAssertEqual(MeshBeacon.parse(back)?.nodeId, "node-ada-1")
    }
}

@MainActor
final class LocalMeshRoomTests: XCTestCase {
    func testTwoDevicesOnThisNetworkTalkWithNoRelay() async throws {
        let ada = makeMesh(ChatDevice(name: "ada"))
        let grace = makeMesh(ChatDevice(name: "grace"))
        // Discovery is the host's business; these link directly so the room does not depend on
        // this machine allowing multicast.
        let adaStarted = await ada.start()
        let graceStarted = await grace.start()
        XCTAssertTrue(adaStarted)
        XCTAssertTrue(graceStarted)
        XCTAssertNotEqual(ada.listenPort, 0)

        let dialled = await ada.connectToPeer(.loopbackIPv4, port: grace.listenPort)
        XCTAssertTrue(dialled)
        let linked = await waitUntil { ada.peerCount == 1 && grace.peerCount == 1 }
        XCTAssertTrue(linked, "both devices end up in the same room")

        let sent = await ada.sendMessage("anyone here?")
        XCTAssertFalse(sent.isEmpty)
        let crossed = await waitUntil { !grace.messages.isEmpty }
        XCTAssertTrue(crossed, "the message crosses the local link")
        XCTAssertEqual(grace.messages.first?.text, "anyone here?")
        XCTAssertEqual(grace.messages.first?.conversation, meshConversation)
        XCTAssertEqual(grace.messages.first?.isSelf, false)
        XCTAssertEqual(grace.members().count, 2)
        XCTAssertEqual(grace.members()[1].online, true)
        XCTAssertEqual(ada.messages.filter { $0.id == sent }.count, 1)
        XCTAssertEqual(grace.unread, 1)
        grace.markRead()
        XCTAssertEqual(grace.unread, 0)

        // The debug bar counts what just happened on both ends.
        let delivered = await waitUntil { !ada.isQueued(sent) }
        XCTAssertTrue(delivered)
        XCTAssertTrue(ada.diagnosticsLine().contains("dial 1 ok 1 fail 0"), ada.diagnosticsLine())
        XCTAssertTrue(ada.diagnosticsLine().contains("links 1 keyed 1"), ada.diagnosticsLine())
        XCTAssertTrue(ada.diagnosticsLine().contains("chat in 0 out 1 queued 0"), ada.diagnosticsLine())
        XCTAssertTrue(grace.diagnosticsLine().contains("fail 0 · in 1"), grace.diagnosticsLine())
        XCTAssertTrue(grace.diagnosticsLine().contains("chat in 1 out 0"), grace.diagnosticsLine())
        XCTAssertTrue(grace.diagnosticsDetail().contains("peer Guest "), grace.diagnosticsDetail())

        await ada.stop()
        await grace.stop()
        XCTAssertEqual(ada.diagnosticsLine(), "mesh not running")
    }

    func testAPeerMessageIsAnnouncedOnceAndOwnMessagesAreNot() async throws {
        let ada = makeMesh(ChatDevice(name: "ada"))
        let grace = makeMesh(ChatDevice(name: "grace"))
        var heard: [String] = []
        var unreadSeen: [Int] = []
        var echoed: [String] = []
        grace.onIncoming = { (message: ChatMessage, unread: Int) in
            heard.append(message.text)
            unreadSeen.append(unread)
        }
        ada.onIncoming = { (message: ChatMessage, _: Int) in echoed.append(message.text) }
        let adaStarted = await ada.start()
        let graceStarted = await grace.start()
        XCTAssertTrue(adaStarted && graceStarted)
        let dialled = await ada.connectToPeer(.loopbackIPv4, port: grace.listenPort)
        XCTAssertTrue(dialled)
        let linked = await waitUntil { ada.peerCount == 1 && grace.peerCount == 1 }
        XCTAssertTrue(linked)

        _ = await ada.sendMessage("lunch is here")
        let crossed = await waitUntil { !heard.isEmpty }
        XCTAssertTrue(crossed)
        _ = await grace.sendMessage("coming")
        let replied = await waitUntil { !echoed.isEmpty }
        XCTAssertTrue(replied)
        XCTAssertEqual(heard, ["lunch is here"])
        XCTAssertEqual(unreadSeen, [1])
        XCTAssertEqual(echoed, ["coming"])

        await ada.stop()
        await grace.stop()
    }

    func testAMessageSentAloneWaitsSurvivesARestartAndReachesLaterDevices() async throws {
        let dir = temporaryDirectory("mesh-history")
        defer { try? FileManager.default.removeItem(at: dir) }
        let historyFile = dir.appendingPathComponent("history.json")
        let device = ChatDevice(name: "ada")

        let alone = makeMesh(device, historyFile: historyFile)
        let started = await alone.start()
        XCTAssertTrue(started)
        let sent = await alone.sendMessage("anyone out there?")
        XCTAssertTrue(alone.isQueued(sent), "nobody is linked, so the message waits on this device")
        await alone.stop()
        XCTAssertTrue(FileManager.default.fileExists(atPath: historyFile.path))

        // The same device after a restart: still there, still queued.
        let ada = makeMesh(device, historyFile: historyFile)
        let restarted = await ada.start()
        XCTAssertTrue(restarted)
        XCTAssertEqual(ada.messages.map(\.id), [sent])
        XCTAssertTrue(ada.isQueued(sent))
        XCTAssertEqual(ada.unread, 0, "your own history is not news")

        let grace = makeMesh(ChatDevice(name: "grace"))
        let carol = makeMesh(ChatDevice(name: "carol"))
        _ = await grace.start()
        _ = await carol.start()
        _ = await ada.connectToPeer(.loopbackIPv4, port: grace.listenPort)
        let taken = await waitUntil { !ada.isQueued(sent) && grace.messages.contains { $0.id == sent } }
        XCTAssertTrue(taken, "the first device to join takes the queued message")

        _ = await carol.connectToPeer(.loopbackIPv4, port: grace.listenPort)
        _ = await waitUntil { carol.peerCount == 1 }
        let later = await ada.sendMessage("and now?")
        let reached = await waitUntil { carol.messages.contains { $0.id == later } }
        XCTAssertTrue(reached, "carol hears what is said after she joins, through grace")
        XCTAssertFalse(
            carol.messages.contains { $0.id == sent }, "a message grace already took is not sealed again to carol's new key")

        await carol.stop()
        await grace.stop()
        await ada.stop()
    }

    func testARefusedSendDoesNotDeafenDiscovery() async throws {
        // iOS refuses every multicast send; the device must go on hearing beacons, and answering
        // them, after that.
        let desk = LocalMeshService(
            settings: ChatDevice(name: "desk").settings, identity: ChatDevice().identity, secureStore: InMemorySecureStore())
        desk.discoveryPort = 49742
        desk.addressSweepEnabled = false
        desk.bluetoothEnabled = false
        desk.loopbackIsLocalNetwork = true
        let started = await desk.start()
        XCTAssertTrue(started)

        // Larger than any datagram can be: refused with "message too long".
        XCTAssertEqual(desk.sendDiscoveryDatagram([UInt8](repeating: 0, count: 70_000), to: .loopbackIPv4, port: 49742), -1)
        XCTAssertTrue(desk.diagnosticsLine().contains("udp 49742 bound"), desk.diagnosticsLine())
        XCTAssertFalse(desk.diagnosticsDetail().contains("send buffer full"))

        let crypto = await Task.detached { RoomCrypto.shared(meshRoomName) }.value
        let wire = try MeshUDPSocket.bind(address: .loopbackIPv4, port: 0, reuse: false)
        defer { wire.close() }
        let answers = AnswerCounter()
        let listener = Task {
            for await _ in wire.datagrams { await answers.add() }
        }
        defer { listener.cancel() }
        let envelope = try crypto.encrypt(MeshBeacon(nodeId: "someone-else", port: 1).toJSON())
        _ = wire.send(Array(JSON.encode(.object(envelope)).utf8), to: .loopbackIPv4, port: 49742)
        var heard = false
        for _ in 0..<400 where !heard {
            heard = await answers.count > 0
            if !heard { try await Task.sleep(nanoseconds: 10_000_000) }
        }
        XCTAssertTrue(heard, "the beacon is heard and answered")
        await desk.stop()
    }

    func testADeviceThatNeverHearsMulticastIsAnsweredAndLinked() async throws {
        // desk hears beacons; phone stands in for an iPhone with no multicast and a dial that may
        // never land, so only desk's answer and desk's own dial can bring them together.
        let deskDevice = ChatDevice(name: "desk")
        let desk = LocalMeshService(settings: deskDevice.settings, identity: deskDevice.identity, secureStore: deskDevice.secure)
        let phoneDevice = ChatDevice(name: "phone")
        let phone = makeMesh(phoneDevice)
        desk.discoveryPort = 49741
        desk.beaconIntervalMs = 200
        desk.addressSweepEnabled = false
        desk.bluetoothEnabled = false
        desk.loopbackIsLocalNetwork = true
        let deskStarted = await desk.start()
        let phoneStarted = await phone.start()
        XCTAssertTrue(deskStarted)
        XCTAssertTrue(phoneStarted)

        let crypto = await Task.detached { RoomCrypto.shared(meshRoomName) }.value
        let wire = try MeshUDPSocket.bind(address: .loopbackIPv4, port: 0, reuse: false)
        defer { wire.close() }
        let answers = BeaconBox()
        let listener = Task {
            for await datagram in wire.datagrams {
                guard case .object(let envelope)? = try? JSON.parse(datagram.data),
                    let plain = crypto.decrypt(envelope), let beacon = MeshBeacon.parse(plain)
                else { continue }
                await answers.add(beacon)
            }
        }
        defer { listener.cancel() }
        let phoneId = MeshBeacon.randomId()
        func beacon(reply: Bool) throws {
            let envelope = try crypto.encrypt(
                MeshBeacon(nodeId: phoneId, port: phone.listenPort, reply: reply, control: true).toJSON())
            _ = wire.send(Array(JSON.encode(.object(envelope)).utf8), to: .loopbackIPv4, port: 49741)
        }

        try beacon(reply: false)
        var got: [MeshBeacon] = []
        for _ in 0..<400 where got.isEmpty {
            got = await answers.all
            if got.isEmpty { try await Task.sleep(nanoseconds: 10_000_000) }
        }
        XCTAssertEqual(got.count, 1, "a beacon from an unlinked device is answered by unicast")
        XCTAssertEqual(got.first?.reply, true)
        XCTAssertEqual(got.first?.port, desk.listenPort)
        XCTAssertEqual(got.first?.nodeId, desk.beaconId)
        XCTAssertNotEqual(got.first?.nodeId, desk.sealKeyId, "the beacon never carries the long-lived key id")
        XCTAssertTrue(desk.controlHosts().isEmpty, "a beacon alone names no desktop")

        try await Task.sleep(nanoseconds: 150_000_000)
        try beacon(reply: true)
        let linked = await waitUntil { desk.peerCount == 1 && phone.peerCount == 1 }
        XCTAssertTrue(linked, "whichever id is higher, the pair ends up linked")
        let final = await answers.all
        XCTAssertEqual(final.count, 1, "a reply beacon is never answered")
        let control = await waitUntil { desk.controlHosts().map(\.nodeId) == [phoneDevice.identity.nodeId] }
        XCTAssertTrue(control, "the control node is the linked device's authenticated node id: \(desk.controlHosts())")

        await desk.stop()
        await phone.stop()
    }

    func testALateJoinerGetsNoEarlierMessagesButIsReachedWithNoDirectLink() async throws {
        let ada = makeMesh(ChatDevice(name: "ada"))
        let grace = makeMesh(ChatDevice(name: "grace"))
        let linus = makeMesh(ChatDevice(name: "linus"))
        for service in [ada, grace, linus] { _ = await service.start() }
        _ = await ada.connectToPeer(.loopbackIPv4, port: grace.listenPort)
        _ = await waitUntil { grace.peerCount == 1 }
        _ = await ada.sendMessage("anyone here?")
        _ = await waitUntil { !grace.messages.isEmpty }

        _ = await linus.connectToPeer(.loopbackIPv4, port: grace.listenPort)
        _ = await waitUntil { linus.peerCount == 1 && grace.peerCount == 2 }

        _ = await ada.sendMessage("second")
        let reached = await waitUntil { linus.messages.contains { $0.text == "second" } }
        XCTAssertTrue(reached, "a message crosses the mesh to a device with no direct link")
        XCTAssertEqual(linus.messages.map(\.text), ["second"], "nothing said before linus joined is sealed again to him")
        XCTAssertEqual(grace.messages.count, 2)

        for service in [ada, grace, linus] { await service.stop() }
    }

    func testTheSameRoomRunsOverBluetoothWithNoNetworkAtAll() async throws {
        let (phoneLink, laptopLink) = FakeBluetooth.pair("phone", "laptop")
        let phoneDevice = ChatDevice(name: "phone")
        let laptopDevice = ChatDevice(name: "laptop")
        let phone = LocalMeshService(
            settings: phoneDevice.settings, identity: phoneDevice.identity, secureStore: phoneDevice.secure,
            transport: phoneLink)
        let laptop = LocalMeshService(
            settings: laptopDevice.settings, identity: laptopDevice.identity, secureStore: laptopDevice.secure,
            transport: laptopLink)
        phone.accountProvider = { "phone" }
        laptop.accountProvider = { "laptop" }
        // No multicast and no TCP listener: Bluetooth carries the whole room.
        for service in [phone, laptop] {
            service.multicastEnabled = false
            service.networkEnabled = false
            let started = await service.start()
            XCTAssertTrue(started)
        }
        XCTAssertFalse(phone.networkActive)
        XCTAssertTrue(phone.bluetoothActive)

        phoneLink.connect()
        let linked = await waitUntil { phone.peerCount == 1 && laptop.peerCount == 1 }
        XCTAssertTrue(linked, "a Bluetooth link puts both devices in the room")

        _ = await phone.sendMessage("no wifi here")
        let crossed = await waitUntil { !laptop.messages.isEmpty }
        XCTAssertTrue(crossed, "the message crosses Bluetooth")
        XCTAssertEqual(laptop.messages.first?.text, "no wifi here")
        XCTAssertEqual(laptop.messages.first?.senderName.hasPrefix("Guest "), true)

        await phone.stop()
        await laptop.stop()
    }

    func testACrossingDialIsResolvedByNodeId() async throws {
        let (high, low) = orderedPair(makeMesh(ChatDevice(name: "ada")), makeMesh(ChatDevice(name: "grace")))
        _ = await high.start()
        _ = await low.start()
        _ = await high.connectToPeer(.loopbackIPv4, port: low.listenPort)
        let linked = await waitUntil { high.peerCount == 1 && low.peerCount == 1 }
        XCTAssertTrue(linked)

        _ = await low.connectToPeer(.loopbackIPv4, port: high.listenPort)
        let settled = await waitUntil {
            (high.diagnostics.lastDropReason.hasSuffix("duplicate link")
                || low.diagnostics.lastDropReason.hasSuffix("duplicate link"))
                && high.links.count == 1 && low.links.count == 1
        }
        XCTAssertTrue(settled, "\(high.diagnosticsDetail())\n\(low.diagnosticsDetail())")
        XCTAssertEqual(high.links.first?.outbound, true, "the link the higher id opened survives")
        XCTAssertEqual(low.links.first?.outbound, false)
        XCTAssertEqual(high.peerCount, 1)
        XCTAssertEqual(low.peerCount, 1)

        _ = await low.sendMessage("still one room")
        let crossed = await waitUntil { high.messages.contains { $0.text == "still one room" } }
        XCTAssertTrue(crossed)
        await high.stop()
        await low.stop()
    }

    func testAStaleLinkIsReplacedByANewOne() async throws {
        let (high, low) = orderedPair(makeMesh(ChatDevice(name: "ada")), makeMesh(ChatDevice(name: "grace")))
        for service in [high, low] {
            service.linkReplaceAfterMs = 0
            _ = await service.start()
        }
        _ = await high.connectToPeer(.loopbackIPv4, port: low.listenPort)
        let linked = await waitUntil { high.peerCount == 1 && low.peerCount == 1 }
        XCTAssertTrue(linked)

        _ = await low.connectToPeer(.loopbackIPv4, port: high.listenPort)
        let settled = await waitUntil {
            (high.diagnostics.lastDropReason.hasSuffix("replaced") || low.diagnostics.lastDropReason.hasSuffix("replaced"))
                && high.links.count == 1 && low.links.count == 1
        }
        XCTAssertTrue(settled, "\(high.diagnosticsDetail())\n\(low.diagnosticsDetail())")
        XCTAssertEqual(low.links.first?.outbound, true, "the new link survives even though the lower id opened it")
        XCTAssertEqual(high.links.first?.outbound, false)
        XCTAssertFalse(high.diagnostics.lastDropReason.hasSuffix("duplicate link"))
        XCTAssertFalse(low.diagnostics.lastDropReason.hasSuffix("duplicate link"))

        _ = await high.sendMessage("over the new link")
        let crossed = await waitUntil { low.messages.contains { $0.text == "over the new link" } }
        XCTAssertTrue(crossed)
        await high.stop()
        await low.stop()
    }

    func testTheNetworkLinkIsKeptOverBluetooth() async throws {
        let (phoneLink, laptopLink) = FakeBluetooth.pair("phone", "laptop")
        let phoneDevice = ChatDevice(name: "phone")
        let laptopDevice = ChatDevice(name: "laptop")
        let phone = LocalMeshService(
            settings: phoneDevice.settings, identity: phoneDevice.identity, secureStore: phoneDevice.secure,
            transport: phoneLink)
        let laptop = LocalMeshService(
            settings: laptopDevice.settings, identity: laptopDevice.identity, secureStore: laptopDevice.secure,
            transport: laptopLink)
        for service in [phone, laptop] {
            service.multicastEnabled = false
            service.addressSweepEnabled = false
            service.loopbackIsLocalNetwork = true
            let started = await service.start()
            XCTAssertTrue(started)
        }
        func onlyNetwork() -> Bool {
            phone.links.count == 1 && laptop.links.count == 1 && phone.links[0].bluetooth == false
                && laptop.links[0].bluetooth == false && phone.peerCount == 1 && laptop.peerCount == 1
        }

        phoneLink.connect()
        let overBluetooth = await waitUntil { phone.peerCount == 1 && laptop.peerCount == 1 }
        XCTAssertTrue(overBluetooth)
        _ = await phone.connectToPeer(.loopbackIPv4, port: laptop.listenPort)
        let replaced = await waitUntil { onlyNetwork() }
        XCTAssertTrue(replaced, "\(phone.diagnosticsDetail())\n\(laptop.diagnosticsDetail())")
        XCTAssertGreaterThanOrEqual(phoneLink.duplicateCloses + laptopLink.duplicateCloses, 1)

        let before = phoneLink.duplicateCloses + laptopLink.duplicateCloses
        phoneLink.connect()
        let refused = await waitUntil { phoneLink.duplicateCloses + laptopLink.duplicateCloses > before && onlyNetwork() }
        XCTAssertTrue(refused, "a Bluetooth link to a device already linked over the network is closed")

        _ = await laptop.sendMessage("over the network")
        let crossed = await waitUntil { phone.messages.contains { $0.text == "over the network" } }
        XCTAssertTrue(crossed)
        await phone.stop()
        await laptop.stop()
    }

    func testADroppedLinkIsRedialled() async throws {
        let ada = makeMesh(ChatDevice(name: "ada"))
        let grace = makeMesh(ChatDevice(name: "grace"))
        for service in [ada, grace] {
            service.linkStableMs = 0
            service.redialDelayMs = 100
            _ = await service.start()
        }
        _ = await ada.connectToPeer(.loopbackIPv4, port: grace.listenPort)
        let linked = await waitUntil { ada.peerCount == 1 && grace.peerCount == 1 }
        XCTAssertTrue(linked)
        XCTAssertTrue(
            grace.diagnosticsDetail().contains("127.0.0.1:\(ada.listenPort)"),
            "the hello says where its sender listens: \(grace.diagnosticsDetail())")

        grace.links.first?.socket?.close()
        let relinked = await waitUntil {
            ada.diagnostics.dials + grace.diagnostics.dials >= 2 && ada.links.count == 1 && grace.links.count == 1
                && ada.peerCount == 1 && grace.peerCount == 1
        }
        XCTAssertTrue(relinked, "\(ada.diagnosticsDetail())\n\(grace.diagnosticsDetail())")

        _ = await grace.sendMessage("back again")
        let crossed = await waitUntil { ada.messages.contains { $0.text == "back again" } }
        XCTAssertTrue(crossed)
        await ada.stop()
        await grace.stop()
    }

    func testResumeRebuildsTheSocketsOnTheSamePort() async throws {
        let deskDevice = ChatDevice(name: "desk")
        let desk = LocalMeshService(settings: deskDevice.settings, identity: deskDevice.identity, secureStore: deskDevice.secure)
        desk.discoveryPort = 49743
        desk.addressSweepEnabled = false
        desk.bluetoothEnabled = false
        desk.loopbackIsLocalNetwork = true
        let started = await desk.start()
        XCTAssertTrue(started)
        XCTAssertEqual(desk.listenPort, 49743, "the room listens on its own port when it is free")

        let phone = makeMesh(ChatDevice(name: "phone"))
        _ = await phone.start()
        _ = await phone.connectToPeer(.loopbackIPv4, port: desk.listenPort)
        let linked = await waitUntil { desk.peerCount == 1 && phone.peerCount == 1 }
        XCTAssertTrue(linked)

        await desk.resume()
        XCTAssertEqual(desk.listenPort, 49743)
        XCTAssertTrue(desk.diagnosticsLine().contains("udp 49743 bound"), desk.diagnosticsLine())
        XCTAssertEqual(desk.peerCount, 1, "links already up survive the rebuild")

        let tablet = makeMesh(ChatDevice(name: "tablet"))
        _ = await tablet.start()
        let dialled = await tablet.connectToPeer(.loopbackIPv4, port: 49743)
        XCTAssertTrue(dialled)
        let joined = await waitUntil { desk.peerCount == 2 && tablet.peerCount == 1 }
        XCTAssertTrue(joined, "the rebuilt listener takes new devices")

        let crypto = await Task.detached { RoomCrypto.shared(meshRoomName) }.value
        let wire = try MeshUDPSocket.bind(address: .loopbackIPv4, port: 0, reuse: false)
        defer { wire.close() }
        let answers = AnswerCounter()
        let listener = Task {
            for await _ in wire.datagrams { await answers.add() }
        }
        defer { listener.cancel() }
        let envelope = try crypto.encrypt(MeshBeacon(nodeId: "someone-else", port: 1).toJSON())
        _ = wire.send(Array(JSON.encode(.object(envelope)).utf8), to: .loopbackIPv4, port: 49743)
        var heard = false
        for _ in 0..<400 where !heard {
            heard = await answers.count > 0
            if !heard { try await Task.sleep(nanoseconds: 10_000_000) }
        }
        XCTAssertTrue(heard, "the rebuilt discovery socket hears and answers beacons")

        for service in [desk, phone, tablet] { await service.stop() }
    }

    func testStrangersNearbyLearnNoAccountAndNoWakeToken() async throws {
        let dir = temporaryDirectory("mesh-wire")
        defer { try? FileManager.default.removeItem(at: dir) }
        let historyFile = dir.appendingPathComponent("history.json")
        let adaDevice = ChatDevice(name: "ada")
        let graceDevice = ChatDevice(name: "grace")
        let ada = makeMesh(adaDevice)
        let grace = makeMesh(graceDevice, historyFile: historyFile)
        _ = await ada.start()
        _ = await grace.start()
        _ = await ada.connectToPeer(.loopbackIPv4, port: grace.listenPort)
        _ = await waitUntil { ada.peerCount == 1 && grace.peerCount == 1 }
        _ = await ada.sendMessage("hi")
        let crossed = await waitUntil { grace.messages.contains { $0.text == "hi" } }
        XCTAssertTrue(crossed)
        XCTAssertEqual(grace.members().first { !$0.isSelf }?.owner, "")
        await grace.stop()
        await ada.stop()

        let saved = try String(contentsOf: historyFile, encoding: .utf8)
        XCTAssertTrue(saved.contains("\"hi\""), saved)
        XCTAssertFalse(saved.contains("ownerUser"), "a #mesh frame names no account: \(saved)")
        XCTAssertFalse(saved.contains("\"sender\":\"ada\""), "nor carries the account as its name: \(saved)")
        XCTAssertTrue(saved.contains(guestChatName(adaDevice.identity.publicKeyB64url)), saved)
        XCTAssertFalse(saved.contains("\"wake\""), "a #mesh frame carries no wake token: \(saved)")
        let trust = try ChatKeyring.shared(graceDevice.secure).trust(meshRoomName)
        XCTAssertEqual(trust.record(ada.sealKeyId)?.acct, adaDevice.identity.nodeId, "the bundle's account is the node id")
    }

    func testTheBeaconIdIsRenewedWithTheNetwork() async throws {
        let mesh = makeMesh(ChatDevice(name: "ada"))
        _ = await mesh.start()
        let first = mesh.beaconId
        XCTAssertFalse(first.isEmpty)
        XCTAssertNotEqual(first, mesh.sealKeyId)
        await mesh.networkChanged()
        XCTAssertNotEqual(mesh.beaconId, first, "a new network hears a new id")
        let second = mesh.beaconId
        await mesh.stop()
        _ = await mesh.start()
        XCTAssertNotEqual(mesh.beaconId, second, "and so does every start")
        await mesh.stop()
    }

    func testAProjectSwitchRestartsTheMeshWithItsOwnHistoryAndKeys() async throws {
        let dir = temporaryDirectory("mesh-projects")
        defer { try? FileManager.default.removeItem(at: dir) }
        var project = "alpha"
        let device = ChatDevice(name: "ada")
        let mesh = LocalMeshService(
            settings: device.settings, identity: device.identity, secureStore: device.secure,
            historyFile: { dir.appendingPathComponent(meshHistoryFileName(project: project)) })
        mesh.multicastEnabled = false
        mesh.bluetoothEnabled = false
        mesh.addressSweepEnabled = false
        mesh.loopbackIsLocalNetwork = true
        mesh.projectScope = { project }
        _ = await mesh.start()
        XCTAssertEqual(mesh.meshSealScope, "\(meshRoomName)\nalpha")
        let said = await mesh.sendMessage("alpha only")
        XCTAssertFalse(said.isEmpty)

        await mesh.leaveProject()
        XCTAssertFalse(mesh.running)
        XCTAssertTrue(mesh.messages.isEmpty, "nothing of alpha's #mesh stays on screen")
        project = "beta"
        await mesh.enterProject()
        XCTAssertTrue(mesh.running, "the mesh that was running comes back up in the new project")
        XCTAssertEqual(mesh.meshSealScope, "\(meshRoomName)\nbeta")
        XCTAssertTrue(mesh.messages.isEmpty, "beta reads its own history file")
        _ = await mesh.sendMessage("beta only")

        await mesh.leaveProject()
        project = "alpha"
        await mesh.enterProject()
        XCTAssertEqual(mesh.messages.map(\.text), ["alpha only"])
        await mesh.stop()
        XCTAssertNotEqual(meshHistoryFileName(project: "alpha"), meshHistoryFileName(project: "beta"))
    }

    func testTheLinkIsAuthenticatedAndEncrypted() async throws {
        let adaDevice = ChatDevice(name: "ada")
        let graceDevice = ChatDevice(name: "grace")
        let ada = makeMesh(adaDevice)
        let grace = makeMesh(graceDevice)
        _ = await ada.start()
        _ = await grace.start()
        _ = await ada.connectToPeer(.loopbackIPv4, port: grace.listenPort)
        let linked = await waitUntil { ada.peerCount == 1 && grace.peerCount == 1 && ada.links.first?.keyKnown == true }
        XCTAssertTrue(linked)
        XCTAssertEqual(ada.links.first?.peerId, grace.sealKeyId, "the link's peer is the authenticated key id")
        XCTAssertEqual(grace.links.first?.peerId, ada.sealKeyId)
        XCTAssertEqual(ada.directLinks(), [grace.sealKeyId: .wifi])
        let named = await waitUntil { grace.members().count == 2 && grace.members()[1].id == adaDevice.identity.nodeId }
        XCTAssertTrue(named, "the hello inside the link names the node")
        await ada.stop()
        await grace.stop()
    }

    func testABlockedDeviceCannotLink() async throws {
        let adaDevice = ChatDevice(name: "ada")
        let graceDevice = ChatDevice(name: "grace")
        let adaRoom = try RoomSeal.loadOrCreate(
            meshRoomName, scope: meshRoomName, policy: .open, acct: "ada", node: adaDevice.identity.nodeId,
            secureStore: adaDevice.secure, nodeIdentity: adaDevice.identity)
        let graceRoom = try RoomSeal.loadOrCreate(
            meshRoomName, scope: meshRoomName, policy: .open, acct: "grace", node: graceDevice.identity.nodeId,
            secureStore: graceDevice.secure, nodeIdentity: graceDevice.identity)
        XCTAssertNotNil(graceRoom.acceptBundle(adaRoom.keyFrame()))
        XCTAssertTrue(graceRoom.block(adaRoom.keyId))
        let ada = makeMesh(adaDevice)
        let grace = makeMesh(graceDevice)
        _ = await ada.start()
        _ = await grace.start()
        _ = await ada.connectToPeer(.loopbackIPv4, port: grace.listenPort)
        let refused = await waitUntil { grace.diagnostics.lastDropReason.hasSuffix("blocked device") }
        XCTAssertTrue(refused, grace.diagnosticsDetail())
        XCTAssertEqual(grace.peerCount, 0)
        await ada.stop()
        await grace.stop()
    }

    func testAForgedLinkSignatureClosesTheLink() async throws {
        let grace = makeMesh(ChatDevice(name: "grace"))
        _ = await grace.start()
        let impostor = try XCTUnwrap(try ChatKeyring.load(InMemorySecureStore(), create: true))
        let handshake = try MeshLinkHandshake(keyring: impostor, initiator: true)
        let socket = try await MeshTCPConnection.connect(to: .loopbackIPv4, port: grace.listenPort, timeout: 5)
        defer { socket.close() }
        let reader = MeshByteBuffer()
        socket.write(meshFrameBytes(Array(JSON.encode(handshake.hello).utf8)))
        var peerHello: JSONObject?
        var closed = false
        for await event in socket.events {
            switch event {
            case .data(let data):
                reader.add(data)
                if peerHello == nil, let frame = try meshTakeFrame(reader) {
                    peerHello = JSON.parseObject(frame)
                    _ = try handshake.receiveHello(try XCTUnwrap(peerHello))
                    let forged: JSONObject = ["t": "fm-link-auth", "sig": .string(ChatV3.b64u([UInt8](repeating: 7, count: 64)))]
                    socket.write(meshFrameBytes(Array(JSON.encode(forged).utf8)))
                }
            case .closed:
                closed = true
            }
            if closed { break }
        }
        XCTAssertNotNil(peerHello)
        XCTAssertTrue(closed, "the responder hangs up on a bad signature")
        let dropped = await waitUntil { grace.diagnostics.lastDropReason.hasSuffix("link handshake") }
        XCTAssertTrue(dropped, grace.diagnosticsDetail())
        XCTAssertEqual(grace.peerCount, 0)
        await grace.stop()
    }

    func testWithNeitherTransportTheRoomDoesNotStart() async {
        let device = ChatDevice(name: "ada")
        let mesh = LocalMeshService(settings: device.settings, identity: device.identity, secureStore: device.secure)
        mesh.networkEnabled = false
        let started = await mesh.start()
        XCTAssertFalse(started, "no network and the iPhone's transport has no Bluetooth")
        XCTAssertFalse(mesh.running)
        XCTAssertTrue(mesh.diagnosticsLine().contains("no local network and no Bluetooth"))
        let sent = await mesh.sendMessage("hello")
        XCTAssertEqual(sent, "", "nothing is sent while the room is down")
    }
}

@MainActor
func orderedPair(_ a: LocalMeshService, _ b: LocalMeshService) -> (LocalMeshService, LocalMeshService) {
    let aId = (try? ChatKeyring.shared(a.secureStore))?.kid ?? ""
    let bId = (try? ChatKeyring.shared(b.secureStore))?.kid ?? ""
    return bId.utf16.lexicographicallyPrecedes(aId.utf16) ? (a, b) : (b, a)
}

actor AnswerCounter {
    private(set) var count = 0
    func add() { count += 1 }
}

actor BeaconBox {
    private(set) var all: [MeshBeacon] = []
    func add(_ beacon: MeshBeacon) { all.append(beacon) }
}
