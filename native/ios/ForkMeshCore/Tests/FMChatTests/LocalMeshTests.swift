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

    func testABeaconRoundTripsEveryFieldTheRosterNeeds() {
        let beacon = MeshBeacon(
            nodeId: "node-ada-1", port: 4242, name: "ada", nodeName: "workshop", platform: "android",
            version: "flutter-1.0", timestampMs: 1_750_000_000_000)
        let parsed = MeshBeacon.parse(beacon.toJSON())
        XCTAssertEqual(parsed?.nodeId, "node-ada-1")
        XCTAssertEqual(parsed?.port, 4242)
        XCTAssertEqual(parsed?.name, "ada")
        XCTAssertEqual(parsed?.nodeName, "workshop")
        XCTAssertEqual(parsed?.platform, "android")
        XCTAssertEqual(parsed?.timestampMs, 1_750_000_000_000)
    }

    func testTheBeaconWireOrderMatchesTheDesktop() {
        let json = JSON.encode(.object(MeshBeacon(nodeId: "a", port: 1, name: "n", reply: true, control: true).toJSON()))
        XCTAssertEqual(json, #"{"t":"forkmesh-lan-mesh","v":1,"id":"a","port":1,"ts":0,"name":"n","reply":true,"control":true}"#)
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
        double["v"] = .double(1)
        XCTAssertNotNil(MeshBeacon.parse(double))
    }

    func testLongFieldsAreBoundedAsTheDesktopBoundsThem() {
        var beacon = MeshBeacon(nodeId: "a", port: 1).toJSON()
        beacon["name"] = .string(String(repeating: "x", count: 50))
        beacon["platform"] = .string(String(repeating: "p", count: 40))
        let parsed = MeshBeacon.parse(beacon)
        XCTAssertEqual(parsed?.name.count, 32)
        XCTAssertEqual(parsed?.platform.count, 16)
    }

    func testTheBeaconOnTheWireIsCiphertextNotAnAdvertisement() async throws {
        let beacon = MeshBeacon(nodeId: "node-ada-1", port: 4242, name: "ada", nodeName: "workshop")
        let crypto = await Task.detached { RoomCrypto.shared(meshRoomName) }.value
        let envelope = try crypto.encrypt(beacon.toJSON())
        let datagram = JSON.encode(.object(envelope))
        XCTAssertFalse(datagram.contains("node-ada-1"))
        XCTAssertFalse(datagram.contains("workshop"))
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
        XCTAssertTrue(grace.diagnosticsDetail().contains("peer ada"), grace.diagnosticsDetail())

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

        // carol links only to grace, and still gets ada's message.
        _ = await carol.connectToPeer(.loopbackIPv4, port: grace.listenPort)
        let relayed = await waitUntil { carol.messages.contains { $0.id == sent && $0.text == "anyone out there?" } }
        XCTAssertTrue(relayed, "a joiner gets the chat from whichever device it links to")

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
        let envelope = try crypto.encrypt(MeshBeacon(nodeId: "someone-else", port: 1, name: "phone").toJSON())
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
        let phone = makeMesh(ChatDevice(name: "phone"))
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
        let phoneId = phone.members()[0].id
        func beacon(reply: Bool) throws {
            let envelope = try crypto.encrypt(
                MeshBeacon(nodeId: phoneId, port: phone.listenPort, name: "phone", reply: reply).toJSON())
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

        try await Task.sleep(nanoseconds: 150_000_000)
        try beacon(reply: true)
        let linked = await waitUntil { desk.peerCount == 1 && phone.peerCount == 1 }
        XCTAssertTrue(linked, "whichever id is higher, the pair ends up linked")
        let final = await answers.all
        XCTAssertEqual(final.count, 1, "a reply beacon is never answered")

        await desk.stop()
        await phone.stop()
    }

    func testALateJoinerIsBackfilledAndReachedWithNoDirectLink() async throws {
        let ada = makeMesh(ChatDevice(name: "ada"))
        let grace = makeMesh(ChatDevice(name: "grace"))
        let linus = makeMesh(ChatDevice(name: "linus"))
        for service in [ada, grace, linus] { _ = await service.start() }
        _ = await ada.connectToPeer(.loopbackIPv4, port: grace.listenPort)
        _ = await waitUntil { grace.peerCount == 1 }
        _ = await ada.sendMessage("anyone here?")
        _ = await waitUntil { !grace.messages.isEmpty }

        _ = await linus.connectToPeer(.loopbackIPv4, port: grace.listenPort)
        let backfilled = await waitUntil { !linus.messages.isEmpty }
        XCTAssertTrue(backfilled, "the devices already here backfill a late joiner")
        XCTAssertEqual(linus.messages.first?.text, "anyone here?")

        // ada and linus never linked to each other: the room still reaches both.
        _ = await ada.sendMessage("second")
        let reached = await waitUntil { linus.messages.count == 2 }
        XCTAssertTrue(reached, "a message crosses the mesh to a device with no direct link")
        XCTAssertEqual(linus.messages.last?.text, "second")
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
        XCTAssertEqual(laptop.messages.first?.senderName, "phone")

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
        let envelope = try crypto.encrypt(MeshBeacon(nodeId: "someone-else", port: 1, name: "watch").toJSON())
        _ = wire.send(Array(JSON.encode(.object(envelope)).utf8), to: .loopbackIPv4, port: 49743)
        var heard = false
        for _ in 0..<400 where !heard {
            heard = await answers.count > 0
            if !heard { try await Task.sleep(nanoseconds: 10_000_000) }
        }
        XCTAssertTrue(heard, "the rebuilt discovery socket hears and answers beacons")

        for service in [desk, phone, tablet] { await service.stop() }
    }

    func testADeviceThatLeftIsWokenByTheNextMessage() async throws {
        let expires = Int64(Date().timeIntervalSince1970 * 1000) + 3_600_000
        let phoneToken = "mw1.\(String(repeating: "a", count: 64)).\(expires).sig"
        let deskToken = "mw1.\(String(repeating: "b", count: 64)).\(expires).sig"
        let phone = makeMesh(ChatDevice(name: "phone"))
        phone.wakeTokenProvider = { phoneToken }
        let desk = makeMesh(ChatDevice(name: "desk"))
        desk.wakeTokenProvider = { deskToken }
        var woken: [[String]] = []
        desk.wakeSender = { woken.append($0) }
        _ = await phone.start()
        _ = await desk.start()
        _ = await phone.connectToPeer(.loopbackIPv4, port: desk.listenPort)
        let learned = await waitUntil { desk.wakeTargets() == [phoneToken] }
        XCTAssertTrue(learned, "the phone's hello hands the desk its wake token")
        XCTAssertEqual(phone.wakeTargets(), [deskToken])

        await phone.stop()
        let left = await waitUntil { desk.peerCount == 0 }
        XCTAssertTrue(left)
        _ = await desk.sendMessage("are you there?")
        let sent = await waitUntil { woken == [[phoneToken]] }
        XCTAssertTrue(sent, "a device that left is still woken, and the sender never wakes itself")
        await desk.stop()
    }

    func testWakeTokensAreReadOnlyWhileUsable() {
        XCTAssertEqual(meshWakeTokenExpiry("mw1.abc.42.sig"), 42)
        XCTAssertNil(meshWakeTokenExpiry("mw2.abc.42.sig"))
        XCTAssertNil(meshWakeTokenExpiry("mw1.abc.42"))
        XCTAssertNil(meshWakeTokenExpiry("mw1.abc.soon.sig"))
        XCTAssertNil(meshWakeTokenExpiry("mw1.\(String(repeating: "a", count: 200)).42.sig"))
        XCTAssertTrue(meshWakeTokenUsable("mw1.abc.42.sig", now: 41))
        XCTAssertFalse(meshWakeTokenUsable("mw1.abc.42.sig", now: 42))
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
    let aId = a.members()[0].id
    let bId = b.members()[0].id
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
