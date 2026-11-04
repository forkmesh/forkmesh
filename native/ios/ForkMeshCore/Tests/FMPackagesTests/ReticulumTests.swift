import FMBase
import FMCrypto
import Foundation
import XCTest
@testable import FMPackages

func rnsHex(_ text: String) -> [UInt8] { CryptoBytes.fromHex(text) ?? [] }

enum RNSVectors {
    static let privateKey = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"  // forkmesh-secret-scan:ignore-line
    static let publicKey = "8f40c5adb68f25624ae5b214ea767a6ec94d829d3d7b5e1ad1ba6f3e2138285f29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd7"
    static let announce = "0100fae321c442e3c9bdcd7a3e79d850e03c008f40c5adb68f25624ae5b214ea767a6ec94d829d3d7b5e1ad1ba6f3e2138285f29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd76ec60bc318e2c0f0d9080102030405006955b900b5fd2a0dce06f324f4ba40cef2dd50b2fde097047ac7a137b4feb5fe8277af2ddeb6608cb297548ddc310df93bd21fa3fc8524730d84d220e455e337fc0cbd0492c40d4669656c6420726164696f203308"
    static let discovery = "008b00b14261636b626f6e65496e7465726661636501c3ccfec410aca31af0441d81dbec71e82da0b4b5f5ccfda3524e53ccfca5312e352e37ccffa8546573742068756203cb404a2f5c28f5c28f04cb40138f5c28f5c28f05cb402400000000000002af6875622e6578616d706c652e6f726706cd1092d83b79d7a76c6bf37b3a640a739f67f92b7c3cf9efac45e303030a5cfaf7b0cc"
    static let lxmf = "fae321c442e3c9bdcd7a3e79d850e03c"
    static let emitted: Int64 = 1_767_225_600
}

final class ManualScheduler: ReticulumScheduler {
    final class Entry: ReticulumTimer {
        let at: Double
        let serial: Int
        let action: () -> Void
        var cancelled = false
        init(at: Double, serial: Int, action: @escaping () -> Void) {
            self.at = at
            self.serial = serial
            self.action = action
        }
        func cancel() { cancelled = true }
    }

    var now: Double = Double(RNSVectors.emitted) + 3600
    private var entries: [Entry] = []
    private var serial = 0

    func after(_ seconds: Double, _ action: @escaping () -> Void) -> ReticulumTimer {
        serial += 1
        let entry = Entry(at: now + max(0, seconds), serial: serial, action: action)
        entries.append(entry)
        return entry
    }

    func advance(_ seconds: Double) {
        let end = now + seconds
        while let next = entries.filter({ !$0.cancelled && $0.at <= end }).min(by: { ($0.at, $0.serial) < ($1.at, $1.serial) }) {
            entries.removeAll { $0 === next }
            now = max(now, next.at)
            next.action()
        }
        now = end
        entries.removeAll { $0.cancelled }
    }
}

final class FakeLink: ReticulumLink {
    let host: String
    let port: Int
    let scheduler: ManualScheduler
    var autoConnect: Bool
    var handler: ((ReticulumLinkEvent) -> Void)?
    var bytesWritten = 0
    var closed = false
    var decoder = ReticulumFrameDecoder()
    var frames: [[UInt8]] = []

    init(host: String, port: Int, scheduler: ManualScheduler, autoConnect: Bool) {
        self.host = host
        self.port = port
        self.scheduler = scheduler
        self.autoConnect = autoConnect
    }

    func open(_ handler: @escaping (ReticulumLinkEvent) -> Void) {
        self.handler = handler
        if autoConnect { _ = scheduler.after(0.02) { [weak self] in self?.handler?(.connected) } }
    }

    func write(_ bytes: [UInt8]) -> Int {
        guard !closed else { return -1 }
        bytesWritten += bytes.count
        frames += decoder.feed(bytes)
        return bytes.count
    }

    func close() {
        closed = true
        handler = nil
    }

    func send(_ raw: [UInt8], chunk: Int = 7) {
        let framed = ReticulumWire.hdlcFrame(raw)
        var index = 0
        while index < framed.count {
            let end = min(framed.count, index + chunk)
            handler?(.received(Array(framed[index..<end])))
            index = end
        }
    }

    func find(_ type: ReticulumPacketType, _ context: UInt8) -> ReticulumPacket? {
        frames.compactMap { try? ReticulumWire.parsePacket($0) }.first { $0.packetType == type && $0.context == context }
    }
}

final class FakeConnector: ReticulumConnector {
    let scheduler: ManualScheduler
    var autoConnect = true
    var links: [FakeLink] = []

    init(scheduler: ManualScheduler) { self.scheduler = scheduler }

    func tcp(host: String, port: Int) -> ReticulumLink? {
        let link = FakeLink(host: host, port: port, scheduler: scheduler, autoConnect: autoConnect)
        links.append(link)
        return link
    }

    func unavailableReason(_ kind: ReticulumInterfaceKind) -> String? {
        kind == .tcp ? nil : "Not on iPhone"
    }

    var last: FakeLink? { links.last }
}

final class ReticulumProtocolTests: XCTestCase {
    func testFraming() throws {
        let payload: [UInt8] = [0x01, 0x7e, 0x7d, 0x02, 0xc0, 0xdb] + [UInt8](repeating: 0x78, count: 30)
        let framed = ReticulumWire.hdlcFrame(payload)
        XCTAssertFalse(framed[1..<(framed.count - 1)].contains(0x7e))
        var decoder = ReticulumFrameDecoder()
        var frames: [[UInt8]] = []
        for byte in framed + ReticulumWire.hdlcKeepalive() + framed { frames += decoder.feed([byte]) }
        XCTAssertEqual(frames, [payload, payload])

        var small = ReticulumFrameDecoder(framing: .hdlc, maximumFrame: 40)
        let dropped = small.feed(ReticulumWire.hdlcFrame([UInt8](repeating: 0x61, count: 100)) + ReticulumWire.hdlcFrame([UInt8](repeating: 0x62, count: 25)))
        XCTAssertEqual(dropped, [[UInt8](repeating: 0x62, count: 25)])
        XCTAssertEqual(small.invalidFrames, 1)

        var kiss = ReticulumFrameDecoder(framing: .kiss)
        let kissFrames = kiss.feed(ReticulumWire.kissFrame(payload) + ReticulumWire.kissFrame(payload))
        XCTAssertEqual(kissFrames, [payload, payload])

        var big = ReticulumFrameDecoder()
        XCTAssertTrue(big.feed(ReticulumWire.hdlcFrame([UInt8](repeating: 1, count: Reticulum.streamHardwareMtu + 1))).isEmpty)
        XCTAssertEqual(big.invalidFrames, 1)
        XCTAssertEqual(big.feed(ReticulumWire.hdlcFrame([UInt8](repeating: 1, count: 10))), [])
        XCTAssertEqual(big.invalidFrames, 2)
    }

    func testPythonVectors() throws {
        XCTAssertEqual(ReticulumWire.hex(ReticulumWire.nameHash("rnstransport.path.request")), "7926bbe7dd7f9aba88b0")
        XCTAssertEqual(ReticulumWire.hex(ReticulumWire.pathRequestDestination), "6b9f66014d9853faab220fba47d02761")
        let identity = try XCTUnwrap(ReticulumIdentity(privateKey: rnsHex(RNSVectors.privateKey)))
        XCTAssertEqual(identity.publicKey, rnsHex(RNSVectors.publicKey))
        XCTAssertEqual(ReticulumWire.hex(identity.hash), "aca31af0441d81dbec71e82da0b4b5f5")
        XCTAssertEqual(ReticulumWire.hex(ReticulumWire.destinationHash(name: "lxmf.delivery", identityHash: identity.hash)), RNSVectors.lxmf)
        XCTAssertEqual(ReticulumWire.hex(ReticulumWire.hkdf(length: 80, material: Array("abc".utf8), salt: Array("salt".utf8), context: Array("ctx".utf8))),
                       "ffae909e84651ee55e1826c11f1a9e722af61742efc3d00b94d3067fce9e548268c612d3465a2ad08bac2eec009bb36d4fc86cb070ff64e60b45ef0fe2e76eab0cb34a87e3cabea1b0089d14dbc204de")

        let raw = rnsHex(RNSVectors.announce)
        XCTAssertEqual(ReticulumWire.hex(ReticulumWire.packetHash(raw)), "498d563bd52778f53e58a33d05de475df0aa2042044fa6b087e5ce2691b9e2cb")
        let packet = try ReticulumWire.parsePacket(raw)
        XCTAssertEqual(packet.packetType, .announce)
        XCTAssertEqual(packet.hops, 0)
        XCTAssertEqual(ReticulumWire.packPacket(packet), raw)
        let announce = try ReticulumWire.parseAnnounce(packet)
        XCTAssertNoThrow(try ReticulumWire.validateAnnounce(announce))
        XCTAssertEqual(announce.emittedAt, RNSVectors.emitted)
        XCTAssertEqual(ReticulumWire.displayName(kind: "peer", appData: announce.appData), "Field radio 3")
        XCTAssertEqual(ReticulumWire.knownAspect(nameHash: announce.nameHash)?.name, "lxmf.delivery")

        var tampered = raw
        tampered[tampered.count - 3] ^= 0x01
        let bad = try ReticulumWire.parseAnnounce(try ReticulumWire.parsePacket(tampered))
        XCTAssertThrowsError(try ReticulumWire.validateAnnounce(bad))

        let tag = (16..<32).map { UInt8($0) }
        XCTAssertEqual(ReticulumWire.buildPathRequest(destination: rnsHex(RNSVectors.lxmf), tag: tag),
                       rnsHex("08006b9f66014d9853faab220fba47d0276100fae321c442e3c9bdcd7a3e79d850e03c101112131415161718191a1b1c1d1e1f"))

        let built = ReticulumWire.buildAnnounce(identity: identity, name: "lxmf.delivery", appData: Array("hello".utf8), pathResponse: false,
                                                unixTime: RNSVectors.emitted, randomPrefix: [1, 2, 3, 4, 5])
        let builtPacket = try ReticulumWire.parsePacket(built)
        XCTAssertNoThrow(try ReticulumWire.validateAnnounce(try ReticulumWire.parseAnnounce(builtPacket)))
        XCTAssertEqual(builtPacket.destinationHash, rnsHex(RNSVectors.lxmf))
        XCTAssertEqual(Array(built.prefix(103)), Array(raw.prefix(103)))

        let fresh = ReticulumIdentity.generate()
        XCTAssertTrue(fresh.hasPrivateKey)
        XCTAssertEqual(ReticulumIdentity(privateKey: fresh.privateKey)?.publicKey, fresh.publicKey)

        let discovered = try ReticulumWire.parseDiscovery(rnsHex(RNSVectors.discovery))
        XCTAssertEqual(discovered.name, "Test hub")
        XCTAssertEqual(discovered.type, "BackboneInterface")
        XCTAssertEqual(discovered.reachableOn, "hub.example.org")
        XCTAssertEqual(discovered.port, 4242)
        XCTAssertTrue(discovered.transport)
        XCTAssertTrue(discovered.hasLocation)
        XCTAssertEqual(discovered.stampValue, 11)
        XCTAssertFalse(discovered.stampValid)
        XCTAssertTrue(discovered.connectable)
    }

    func testRefusedPackets() {
        let raw = rnsHex(RNSVectors.announce)
        XCTAssertThrowsError(try ReticulumWire.parsePacket([UInt8](repeating: 0, count: 10)))
        var hops = raw
        hops[1] = 200
        XCTAssertThrowsError(try ReticulumWire.parsePacket(hops))
        hops[1] = 127
        XCTAssertNoThrow(try ReticulumWire.parsePacket(hops))
        hops[1] = 128
        XCTAssertThrowsError(try ReticulumWire.parsePacket(hops))
        var ifac = raw
        ifac[0] |= 0x80
        XCTAssertThrowsError(try ReticulumWire.parsePacket(ifac))
        var header2 = raw
        header2[0] |= 0x40
        XCTAssertThrowsError(try ReticulumWire.parsePacket(Array(header2.prefix(35))))
        var short = try! ReticulumWire.parsePacket(raw)
        short.data = Array(short.data.prefix(100))
        XCTAssertThrowsError(try ReticulumWire.parseAnnounce(short))
    }

    func testMsgPackBounds() {
        XCTAssertNil(ReticulumWire.decodeMsgPack([0xdd, 0xff, 0xff, 0xff, 0xff]))
        XCTAssertNil(ReticulumWire.decodeMsgPack([0xdf, 0xff, 0xff, 0xff, 0xff]))
        XCTAssertNil(ReticulumWire.decodeMsgPack([0xc6, 0x7f, 0xff, 0xff, 0xff, 0x00]))
        XCTAssertNil(ReticulumWire.decodeMsgPack([0xdb, 0x00, 0x00, 0x10, 0x00, 0x41]))
        XCTAssertNil(ReticulumWire.decodeMsgPack([]))
        XCTAssertNil(ReticulumWire.decodeMsgPack([0xc1]))
        XCTAssertNil(ReticulumWire.decodeMsgPack([0xcd, 0x01]))
        let nineDeep = [UInt8](repeating: 0x91, count: 9) + [0x01]
        XCTAssertNil(ReticulumWire.decodeMsgPack(nineDeep))
        let eightDeep = [UInt8](repeating: 0x91, count: 8) + [0x01]
        XCTAssertNotNil(ReticulumWire.decodeMsgPack(eightDeep))
        let many = [0xdc, 0x10, 0x00] + [UInt8](repeating: 0x01, count: 4096)
        XCTAssertNil(ReticulumWire.decodeMsgPack(many))
        let enough = [0xdc, 0x0f, 0xff] + [UInt8](repeating: 0x01, count: 4095)
        XCTAssertEqual(ReticulumWire.decodeMsgPack(enough)?.list.count, 4095)
        let map = ReticulumWire.decodeMsgPack([0x82, 0x01, 0xa1, 0x61, 0xc4, 0x01, 0x6b, 0xcb, 0x40, 0x09, 0x21, 0xfb, 0x54, 0x44, 0x2d, 0x18])
        XCTAssertEqual(map?.dictionary["1"], .string("a"))
        XCTAssertEqual(map?.dictionary["k"]?.doubleValue ?? 0, Double.pi, accuracy: 1e-12)
        XCTAssertEqual(ReticulumWire.decodeMsgPack([0xd0, 0xff]), .int(-1))
        XCTAssertEqual(ReticulumWire.decodeMsgPack([0xcf, 0, 0, 0, 1, 0, 0, 0, 0]), .uint(1 << 32))
    }

    func testDisplayNames() {
        let name = Array("Peer 7".utf8)
        XCTAssertEqual(ReticulumWire.displayName(kind: "peer", appData: [0x92, 0xc4, UInt8(name.count)] + name + [0x08]), "Peer 7")
        XCTAssertEqual(ReticulumWire.displayName(kind: "node", appData: Array("  Hill\tnode\u{0}  ".utf8)), "Hill node")
        XCTAssertEqual(ReticulumWire.displayName(kind: "node", appData: [0xff, 0xfe, 0x41]), "")
        XCTAssertEqual(ReticulumWire.displayName(kind: "discovery", appData: name), "")
        let metadata: [UInt8] = [0x97, 0xc2, 0x01, 0x01, 0x01, 0x01, 0x01, 0x81, 0x01, 0xa4] + Array("Prop".utf8)
        XCTAssertEqual(ReticulumWire.displayName(kind: "propagation", appData: metadata), "Prop")
        XCTAssertEqual(ReticulumWire.cleanText(String(repeating: "a", count: 200)).count, 120)
        XCTAssertEqual(ReticulumWire.parseHash("<FAE321C442E3C9BDCD7A3E79D850E03C>"), rnsHex(RNSVectors.lxmf))
        XCTAssertEqual(ReticulumWire.parseHash("fa:e3"), [])
        XCTAssertEqual(ReticulumWire.short(rnsHex(RNSVectors.lxmf)), "<fae321…e03c>")
    }

    func testConfigImport() throws {
        let text = """
        [reticulum]
          share_instance = Yes
          instance_name = lab

        [interfaces]
          [[Default Interface]]
            type = AutoInterface
            enabled = yes
          [[RNS Testnet Dublin]]
            type = TCPClientInterface
            enabled = yes
            target_host = dublin.connect.reticulum.network
            target_port = 4965
          [[Private]]
            type = TCPClientInterface
            enabled = yes
            target_host = 10.0.0.2
            target_port = 4242
            network_name = secret
          [[LAN UDP]]
            type = UDPInterface
            interface_enabled = no
            port = 4243 # both
        """
        let result = ReticulumConfigImport.parse(text)
        XCTAssertEqual(result.interfaces.count, 4)
        XCTAssertEqual(result.interfaces[0].kind, .shared)
        XCTAssertEqual(result.interfaces[0].instanceName, "lab")
        XCTAssertEqual(result.interfaces[1].host, "dublin.connect.reticulum.network")
        XCTAssertEqual(result.interfaces[1].port, 4965)
        XCTAssertTrue(result.interfaces[1].enabled)
        XCTAssertEqual(result.interfaces[2].networkName, "secret")
        XCTAssertTrue(result.interfaces[2].usesAccessCode)
        XCTAssertEqual(result.interfaces[3].kind, .udp)
        XCTAssertEqual(result.interfaces[3].listenPort, 4243)
        XCTAssertFalse(result.interfaces[3].enabled)
        XCTAssertEqual(result.skipped.count, 1)
        let sized = ReticulumConfigImport.parse("[interfaces]\n [[Mesh]]\n  type = TCPClientInterface\n  target_host = 10.0.0.9\n  target_port = 4242\n  passphrase = \"pw\"\n  ifac_size = 64\n")
        XCTAssertEqual(sized.interfaces.last?.passphrase, "pw")
        XCTAssertEqual(sized.interfaces.last?.accessCodeSize, 8)
        let protected = try XCTUnwrap(ReticulumInterfaceConfig(json: result.interfaces[2].json))
        XCTAssertEqual(protected.networkName, "secret")
        XCTAssertEqual(protected.accessCodeSize, 16)

        var bad = ReticulumInterfaceConfig(id: "x", name: "x", host: "bad host!")
        XCTAssertNotNil(bad.validationError)
        bad.host = "rmap.world"
        XCTAssertNil(bad.validationError)
        bad.port = 70000
        XCTAssertNotNil(bad.validationError)
        for host in ["10.0.0.2", "::1", "fe80::1%en0", "2001:db8::8:800:200c:417a", "::ffff:10.0.0.1", "rmap.world."] {
            XCTAssertTrue(ReticulumAddress.validHost(host), host)
        }
        for host in ["", "-a.example", "a..b", "1:2:3:4:5:6:7:8:9", "a_b.example"] { XCTAssertFalse(ReticulumAddress.validHost(host), host) }

        let round = try XCTUnwrap(ReticulumInterfaceConfig(json: result.interfaces[1].json))
        XCTAssertTrue(round.sameTarget(result.interfaces[1]))
        XCTAssertEqual(round, result.interfaces[1])
        let list = ReticulumInterfaceConfig.list(fromJSON: ReticulumInterfaceConfig.json(result.interfaces))
        XCTAssertEqual(list, result.interfaces)
        let desktop = #"[{"enabled":true,"host":"rmap.world","id":"public-rmap-1","kind":"tcp","kiss":false,"name":"RMAP Backbone 1","port":4242},{"kind":"shared","name":"Reticulum on this computer","instanceName":"default"},{"kind":"bogus","name":"x"}]"#
        let parsed = ReticulumInterfaceConfig.list(fromJSON: desktop)
        XCTAssertEqual(parsed.count, 2)
        XCTAssertEqual(parsed[0].endpoint, "rmap.world:4242")
        XCTAssertEqual(parsed[1].port, 37428)
        XCTAssertFalse(parsed[1].id.isEmpty)
    }

    func testMapLayout() throws {
        var records: [ReticulumDestination] = []
        for index in 0..<40 {
            var record = ReticulumDestination()
            record.hash = ReticulumWire.truncatedHash([UInt8(index)])
            record.hops = index % 10
            record.interfaceId = index % 3 == 0 ? "a" : index % 3 == 1 ? "b" : "gone"
            records.append(record)
        }
        let spokes = [ReticulumMapSpoke(id: "a", name: "A", online: true), ReticulumMapSpoke(id: "b", name: "B", online: false)]
        let layout = ReticulumMapLayout.compute(width: 360, height: 360, spokes: spokes, destinations: records)
        XCTAssertEqual(layout.positions.count, 40)
        XCTAssertEqual(layout.sectors.map(\.id), ["a", "b", ""])
        XCTAssertEqual(layout.maxRing, 8)
        XCTAssertEqual(layout.sectors.reduce(0) { $0 + $1.span }, 2 * Double.pi, accuracy: 1e-9)
        for point in layout.positions.values {
            XCTAssertTrue(point.x > 0 && point.x < 360 && point.y > 0 && point.y < 360)
        }
        let first = try XCTUnwrap(layout.positions[records[0].hash])
        XCTAssertEqual(layout.hash(at: first), records[0].hash)
        XCTAssertEqual(layout.ringLabel(8), "8+ hops")
        XCTAssertEqual(ReticulumMapLayout.compute(width: 300, height: 300, spokes: [], destinations: []).positions.count, 0)
        XCTAssertEqual(ReticulumFormat.bytes(999), "999 B")
        XCTAssertEqual(ReticulumFormat.bytes(1536), "1.5 KB")
        XCTAssertEqual(ReticulumFormat.bytes(25_000_000), "25 MB")
        XCTAssertEqual(ReticulumFormat.duration(3725), "1 h 2 min")
        XCTAssertEqual(ReticulumFormat.niceCeiling(563.2), 1000)
        XCTAssertEqual(ReticulumFormat.chartTop([]), 1000)
        XCTAssertEqual(ReticulumMapGroup(kind: "discovery"), .transport)
        XCTAssertEqual(ReticulumMapGroup(kind: "forkmesh"), .peer)
        XCTAssertEqual(ReticulumMapGroup(kind: "service"), .node)
    }
}

final class ReticulumNodeTests: XCTestCase {
    private func makeNode(autoConnect: Bool = true) -> (ReticulumNode, FakeConnector, ManualScheduler) {
        let scheduler = ManualScheduler()
        let connector = FakeConnector(scheduler: scheduler)
        connector.autoConnect = autoConnect
        let node = ReticulumNode(scheduler: scheduler, connector: connector)
        return (node, connector, scheduler)
    }

    private func fakeInterface(_ id: String = "fake") -> ReticulumInterfaceConfig {
        ReticulumInterfaceConfig(id: id, name: "Fake node", host: "127.0.0.1", port: 4242)
    }

    func testLiveSession() throws {
        let (node, connector, scheduler) = makeNode()
        node.setIdentity(.generate())
        node.setAnnounceName("Test iPhone")
        var asked: [String] = []
        node.authorizer = { _, url in
            asked.append(url)
            return true
        }
        var changes = 0
        node.onChange = { changes += 1 }
        var pulsed: [[UInt8]] = []
        node.onAnnounce = { pulsed.append($0) }
        node.setInterfaces([fakeInterface()])
        node.start()
        XCTAssertEqual(node.status("fake").state, .connecting)
        scheduler.advance(0.2)
        XCTAssertEqual(node.onlineCount, 1)
        XCTAssertEqual(asked, ["tcp://127.0.0.1:4242"])
        XCTAssertEqual(node.status("fake").detail, "Connected in 20 ms")
        XCTAssertGreaterThan(changes, 0)
        let link = try XCTUnwrap(connector.last)

        link.send(rnsHex(RNSVectors.announce))
        let lxmf = rnsHex(RNSVectors.lxmf)
        let record = try XCTUnwrap(node.destinations[lxmf])
        XCTAssertEqual(record.displayName, "Field radio 3")
        XCTAssertEqual(record.hops, 1)
        XCTAssertEqual(record.kind, "peer")
        XCTAssertEqual(record.interfaceId, "fake")
        XCTAssertEqual(pulsed, [lxmf])
        link.send(rnsHex(RNSVectors.announce))
        XCTAssertEqual(node.counters.duplicates, 1)
        XCTAssertGreaterThan(node.status("fake").rxBytes, 0)
        XCTAssertEqual(node.status("fake").announces, 2)
        XCTAssertEqual(node.events.first?.kind, "announce")

        XCTAssertTrue(node.announce())
        let ours = try XCTUnwrap(link.find(.announce, ReticulumContext.none))
        let parsed = try ReticulumWire.parseAnnounce(ours)
        XCTAssertNoThrow(try ReticulumWire.validateAnnounce(parsed))
        XCTAssertEqual(parsed.destinationHash, node.destinationHash)
        XCTAssertEqual(parsed.appData, Array("Test iPhone".utf8))
        XCTAssertEqual(parsed.nameHash, ReticulumWire.nameHash("forkmesh.node"))
        XCTAssertEqual(node.status("fake").txPackets, 1)
        XCTAssertEqual(node.events.first?.text, "Announced forkmesh.node as \(ReticulumWire.pretty(node.destinationHash))")

        let remote = ReticulumIdentity.generate()
        let remoteHash = ReticulumWire.destinationHash(name: "nomadnetwork.node", identityHash: remote.hash)
        XCTAssertFalse(node.requestPath(remoteHash).isEmpty)
        let request = try XCTUnwrap(link.find(.data, ReticulumContext.none))
        let asks = try XCTUnwrap(ReticulumWire.parsePathRequest(request))
        XCTAssertEqual(asks.destination, remoteHash)
        XCTAssertEqual(asks.tag.count, 16)
        scheduler.advance(0.25)
        var response = ReticulumWire.buildAnnounce(identity: remote, name: "nomadnetwork.node", appData: Array("Hill node".utf8),
                                                   pathResponse: true, unixTime: Int64(scheduler.now))
        response[1] = 2
        link.send(response)
        let lookup = try XCTUnwrap(node.lookups[remoteHash])
        XCTAssertEqual(lookup.answerMs, 250)
        XCTAssertEqual(lookup.hops, 3)
        XCTAssertEqual(node.destinations[remoteHash]?.displayName, "Hill node")
        XCTAssertEqual(node.events.first?.kind, "path-found")

        link.frames.removeAll()
        link.send(ReticulumWire.buildPathRequest(destination: node.destinationHash, tag: [UInt8](repeating: 0x71, count: 16)))
        XCTAssertNotNil(link.find(.announce, ReticulumContext.pathResponse))
        XCTAssertEqual(node.counters.pathRequestsAnswered, 1)
        link.frames.removeAll()
        link.send(ReticulumWire.buildPathRequest(destination: node.destinationHash, tag: [UInt8](repeating: 0x72, count: 16)))
        XCTAssertNil(link.find(.announce, ReticulumContext.pathResponse))
        XCTAssertEqual(node.counters.pathRequestsSeen, 2)

        scheduler.advance(3)
        XCTAssertGreaterThan(node.history.count, 0)
        XCTAssertGreaterThan(node.history.reduce(0) { $0 + $1.tx }, 0)
        XCTAssertEqual(node.status("fake").history.count, node.history.count)

        link.handler?(.closed)
        XCTAssertEqual(node.status("fake").state, .waiting)
        XCTAssertEqual(node.status("fake").detail, "127.0.0.1:4242 closed the connection")
        XCTAssertTrue(link.closed)
        XCTAssertEqual(node.events.first?.text, "Fake node went offline: 127.0.0.1:4242 closed the connection")
        scheduler.advance(5.1)
        XCTAssertEqual(connector.links.count, 2)
        XCTAssertEqual(node.onlineCount, 1)
        XCTAssertEqual(node.status("fake").sessions, 2)
        node.stop()
        XCTAssertEqual(node.status("fake").state, .disabled)
        XCTAssertTrue(connector.links.allSatisfy(\.closed))
    }

    func testRejectsBadAnnouncesAndKeepsBestRoute() throws {
        let (node, connector, scheduler) = makeNode()
        node.setIdentity(.generate())
        node.setInterfaces([fakeInterface()])
        node.start()
        scheduler.advance(0.1)
        let link = try XCTUnwrap(connector.last)
        var tampered = rnsHex(RNSVectors.announce)
        tampered[tampered.count - 3] ^= 0x01
        link.send(tampered)
        XCTAssertEqual(node.counters.rejectedAnnounces, 1)
        XCTAssertEqual(node.status("fake").rejected, 1)
        XCTAssertTrue(node.destinations.isEmpty)
        XCTAssertEqual(node.events.first?.kind, "rejected")

        var far = rnsHex(RNSVectors.announce)
        far[1] = 5
        link.send(far)
        XCTAssertEqual(node.destinations[rnsHex(RNSVectors.lxmf)]?.hops, 6)
        var near = far
        near[1] = 1
        link.send(near)
        XCTAssertEqual(node.counters.duplicates, 1)
        XCTAssertEqual(node.destinations[rnsHex(RNSVectors.lxmf)]?.hops, 2)

        link.send([UInt8](repeating: 1, count: 10))
        XCTAssertEqual(node.counters.invalid, 0)
        XCTAssertEqual(node.status("fake").invalidFrames, 1)
        var ifac = far
        ifac[0] |= 0x80
        link.send(ifac)
        XCTAssertEqual(node.counters.invalid, 0)
        XCTAssertEqual(node.status("fake").invalidFrames, 2)

        let own = ReticulumWire.buildAnnounce(identity: node.identity, name: "forkmesh.node", appData: [], pathResponse: false, unixTime: Int64(scheduler.now))
        link.send(own)
        XCTAssertNil(node.destinations[node.destinationHash])
    }

    func testFirewallDenied() {
        let (node, connector, scheduler) = makeNode()
        node.authorizer = { _, _ in false }
        node.setInterfaces([fakeInterface("denied")])
        node.start()
        scheduler.advance(30)
        XCTAssertEqual(node.status("denied").state, .blocked)
        XCTAssertTrue(connector.links.isEmpty)
    }

    func testUnsupportedKindsFail() {
        let (node, connector, scheduler) = makeNode()
        var udp = fakeInterface("udp")
        udp.kind = .udp
        udp.listenAddress = "0.0.0.0"
        udp.forwardAddress = "255.255.255.255"
        node.setInterfaces([udp, ReticulumInterfaceConfig.sharedInstance])
        node.start()
        scheduler.advance(30)
        XCTAssertEqual(node.status("udp").state, .failed)
        XCTAssertEqual(node.status("shared-instance").state, .failed)
        XCTAssertTrue(connector.links.isEmpty)
    }

    func testReconnectBackoffAndConnectTimeout() throws {
        let (node, connector, scheduler) = makeNode(autoConnect: false)
        node.setInterfaces([fakeInterface()])
        node.start()
        var delays: [Double] = []
        for _ in 0..<8 {
            let link = try XCTUnwrap(connector.last)
            link.handler?(.failed("Connection refused"))
            let status = node.status("fake")
            XCTAssertEqual(status.state, .waiting)
            XCTAssertEqual(status.detail, "Connection refused")
            delays.append(((status.nextAttempt ?? 0) - scheduler.now).rounded())
            scheduler.advance((status.nextAttempt ?? 0) - scheduler.now + 0.01)
        }
        XCTAssertEqual(delays, [5, 10, 20, 40, 80, 120, 120, 120])

        node.reconnectInterface("fake")
        XCTAssertEqual(node.status("fake").state, .connecting)
        scheduler.advance(9.9)
        XCTAssertEqual(node.status("fake").state, .connecting)
        scheduler.advance(0.2)
        XCTAssertEqual(node.status("fake").state, .waiting)
        XCTAssertEqual(node.status("fake").detail, "No answer from 127.0.0.1:4242 after 10 s")
        XCTAssertTrue(connector.links.last?.closed ?? false)

        node.setInterfaceEnabled("fake", false)
        XCTAssertEqual(node.status("fake").state, .disabled)
        let count = connector.links.count
        scheduler.advance(300)
        XCTAssertEqual(connector.links.count, count)
    }

    func testLimits() throws {
        let (node, _, scheduler) = makeNode()
        node.setInterfaces([fakeInterface()])
        node.start()
        scheduler.advance(0.1)
        for index in 0..<(ReticulumNode.maxEvents + 20) {
            node.requestPath(ReticulumWire.truncatedHash([UInt8(index & 0xff), UInt8(index >> 8)]))
        }
        XCTAssertEqual(node.events.count, ReticulumNode.maxEvents)
        scheduler.advance(25)
        XCTAssertTrue(node.lookups.values.allSatisfy(\.timedOut))
        XCTAssertEqual(node.events.first?.kind, "path-timeout")
        scheduler.advance(Double(ReticulumNode.historySeconds + 30))
        XCTAssertEqual(node.history.count, ReticulumNode.historySeconds)
        XCTAssertEqual(node.status("fake").history.count, ReticulumNode.historySeconds)
        XCTAssertEqual(node.requestPath([1, 2, 3]), [])
    }
}

final class ReticulumHubTests: XCTestCase {
    @MainActor
    private func makeHub(prefs: InMemoryKeyValueStore = InMemoryKeyValueStore(), secure: InMemorySecureStore = InMemorySecureStore(),
                         scheduler: ManualScheduler = ManualScheduler()) -> (ReticulumHub, FakeConnector, ManualScheduler) {
        let connector = FakeConnector(scheduler: scheduler)
        let hub = ReticulumHub(prefs: prefs, secure: secure, connector: connector, scheduler: scheduler, accountName: "alice")
        return (hub, connector, scheduler)
    }

    @MainActor
    func testFirstOpenAnnouncesNothingAndStoresTheKeyInTheSecureStore() async throws {
        let prefs = InMemoryKeyValueStore()
        let secure = InMemorySecureStore()
        let (hub, connector, scheduler) = makeHub(prefs: prefs, secure: secure)
        let key = try XCTUnwrap(secure.read(ReticulumHub.identityKey))
        XCTAssertEqual(key.count, Reticulum.privateKeyLength * 2)
        XCTAssertFalse(prefs.keys.contains { $0.contains("identity") })
        XCTAssertTrue(hub.node.interfaces.isEmpty)
        XCTAssertTrue(hub.showsOnboarding)
        XCTAssertEqual(hub.announceName, "alice")
        hub.open()
        XCTAssertTrue(hub.node.running)
        XCTAssertEqual(hub.tiles[0].value, "No interfaces")

        hub.joinPublicNetwork()
        let link = try XCTUnwrap(connector.last)
        XCTAssertEqual(link.host, "rmap.world")
        XCTAssertEqual(link.port, 4242)
        scheduler.advance(120)
        XCTAssertEqual(hub.node.onlineCount, 1)
        XCTAssertTrue(link.frames.isEmpty)
        XCTAssertEqual(hub.node.counters.announcesSent, 0)
        XCTAssertFalse(hub.showsOnboarding)
        XCTAssertEqual(hub.tiles[0].value, "Online")
        XCTAssertTrue(hub.summary.hasPrefix("1 of 1 interfaces online · you are <"))
        let stored = ReticulumInterfaceConfig.list(fromJSON: prefs.string(forKey: ReticulumHub.interfacesKey) ?? "")
        XCTAssertEqual(stored.map(\.endpoint), ["rmap.world:4242"])

        hub.addPublicEntryPoint(ReticulumInterfaceConfig.publicEntryPoints[0])
        XCTAssertEqual(hub.node.interfaces.count, 1)
        XCTAssertEqual(hub.notice?.text, "rmap.world:4242 is already in your interfaces as RMAP Backbone 1.")

        let destination = hub.destinationHashText
        let (again, _, _) = makeHub(prefs: prefs, secure: secure)
        XCTAssertEqual(again.destinationHashText, destination)
        XCTAssertEqual(again.node.interfaces.map(\.endpoint), ["rmap.world:4242"])
    }

    @MainActor
    func testAnnounceIntervalAndSettings() async throws {
        let prefs = InMemoryKeyValueStore()
        let (hub, connector, scheduler) = makeHub(prefs: prefs)
        hub.setAnnounceMinutes(15)
        hub.setAnswerPathRequests(false)
        hub.announceName = "  Field phone  "
        hub.commitAnnounceName()
        XCTAssertEqual(prefs.int(forKey: ReticulumHub.announceMinutesKey), 15)
        XCTAssertEqual(prefs.bool(forKey: ReticulumHub.answerPathsKey), false)
        XCTAssertEqual(prefs.string(forKey: ReticulumHub.announceNameKey), "Field phone")
        hub.open()
        XCTAssertNil(hub.saveInterface(ReticulumInterfaceConfig(id: "a", name: "Lab", host: "10.0.0.2", port: 4242), creating: true))
        var udp = ReticulumInterfaceConfig(id: "u", name: "LAN")
        udp.kind = .udp
        XCTAssertEqual(hub.saveInterface(udp, creating: true), "Only TCP interfaces can run on this iPhone.")
        XCTAssertEqual(hub.saveInterface(ReticulumInterfaceConfig(id: "b", name: "", host: "x"), creating: true),
                       "Give the interface a name of up to 80 characters.")
        scheduler.advance(1)
        let link = try XCTUnwrap(connector.last)
        XCTAssertNil(link.find(.announce, ReticulumContext.none))
        scheduler.advance(3)
        let first = try XCTUnwrap(link.find(.announce, ReticulumContext.none))
        XCTAssertEqual(try ReticulumWire.parseAnnounce(first).appData, Array("Field phone".utf8))
        link.frames.removeAll()
        scheduler.advance(15 * 60)
        XCTAssertNotNil(link.find(.announce, ReticulumContext.none))

        link.frames.removeAll()
        link.send(ReticulumWire.buildPathRequest(destination: hub.node.destinationHash, tag: [UInt8](repeating: 1, count: 16)))
        XCTAssertNil(link.find(.announce, ReticulumContext.pathResponse))

        hub.toggleNetwork()
        XCTAssertFalse(hub.node.running)
        XCTAssertEqual(prefs.bool(forKey: ReticulumHub.runningKey), false)
        let (later, laterConnector, _) = makeHub(prefs: prefs)
        later.open()
        XCTAssertFalse(later.node.running)
        XCTAssertTrue(laterConnector.links.isEmpty)
        XCTAssertEqual(later.announceMinutes, 15)
        XCTAssertFalse(later.answerPathRequests)
    }

    @MainActor
    func testImportOnPhoneSkipsDaemonAndUdp() async throws {
        let (hub, connector, scheduler) = makeHub()
        hub.open()
        let text = "[reticulum]\n share_instance = yes\n[interfaces]\n [[Home]]\n  type = TCPClientInterface\n  enabled = yes\n  target_host = 192.168.1.20\n  target_port = 4242\n [[LAN]]\n  type = UDPInterface\n  enabled = yes\n  port = 4242\n"
        let message = hub.importConfig(text, from: "config")
        XCTAssertEqual(hub.node.interfaces.map(\.endpoint), ["192.168.1.20:4242"])
        XCTAssertTrue(message.hasPrefix("Imported 1 interface from config. Skipped"), message)
        XCTAssertTrue(message.contains("Reticulum on this computer (no Reticulum daemon on this iPhone)"), message)
        XCTAssertTrue(message.contains("LAN (UDP is not available on this iPhone)"), message)
        scheduler.advance(1)
        XCTAssertEqual(connector.links.count, 1)
        hub.importConfig(text, from: "config")
        XCTAssertEqual(hub.node.interfaces.count, 1)
    }

    @MainActor
    func testViewsOverHeardDestinations() async throws {
        let (hub, connector, scheduler) = makeHub()
        hub.open()
        _ = hub.saveInterface(ReticulumInterfaceConfig(id: "a", name: "Lab", host: "10.0.0.2", port: 4242), creating: true)
        scheduler.advance(0.1)
        let link = try XCTUnwrap(connector.last)
        link.send(rnsHex(RNSVectors.announce))
        let hub2 = hub
        for (index, aspect) in ["nomadnetwork.node", "rnstransport.discovery.interface", "example.unknown"].enumerated() {
            let node = ReticulumIdentity.generate()
            let data: [UInt8] = aspect == "rnstransport.discovery.interface" ? rnsHex(RNSVectors.discovery) : Array("Node \(index)".utf8)
            var raw = ReticulumWire.buildAnnounce(identity: node, name: aspect, appData: data, pathResponse: false, unixTime: Int64(scheduler.now))
            raw[1] = UInt8(index + 1)
            link.send(raw)
        }
        scheduler.advance(2)
        XCTAssertEqual(hub2.destinationList.count, 4)
        XCTAssertEqual(hub2.tiles[1].value, "4")
        XCTAssertEqual(hub2.tiles[1].detail, "4 new in 10 min · 1 one hop away")
        XCTAssertEqual(hub2.filteredDestinations.count, 4)
        hub2.kindFilter = .peer
        XCTAssertEqual(hub2.filteredDestinations.map(\.displayName), ["Field radio 3"])
        hub2.kindFilter = nil
        hub2.destinationFilter = "Test hub"
        XCTAssertEqual(hub2.filteredDestinations.first?.kind, "discovery")
        hub2.destinationFilter = ""
        hub2.sort = .hops
        XCTAssertEqual(hub2.filteredDestinations.map(\.hops), [1, 2, 3, 4])
        XCTAssertEqual(hub2.discoveredRows.count, 1)
        XCTAssertEqual(hub2.discoveredRows.first?.address, "hub.example.org:4242")
        XCTAssertEqual(hub2.discoveredRows.first?.usable, false)
        XCTAssertNil(hub2.discoveredDraft(hub2.discoveredRows[0].hash))

        hub2.select(rnsHex(RNSVectors.lxmf), showMap: true)
        XCTAssertEqual(hub2.detailTitle, "Field radio 3")
        XCTAssertEqual(hub2.detailRows.first?.value, "LXMF messaging (lxmf.delivery)")
        XCTAssertTrue(hub2.detailRows.contains(ReticulumRow("Distance", "1 hop")))
        hub2.lookupText = "nope"
        hub2.lookup()
        XCTAssertTrue(hub2.lookupResult.hasPrefix("A destination hash is 32"))
        hub2.lookupText = RNSVectors.lxmf
        hub2.lookup()
        XCTAssertEqual(hub2.lookupResult, "Asking the network…")
        XCTAssertEqual(hub2.tiles[5].detail, "1 waiting for an answer")
        XCTAssertEqual(hub2.tiles[4].label, "Messages")
        hub2.forget(rnsHex(RNSVectors.lxmf))
        XCTAssertTrue(hub2.selected.isEmpty)
        XCTAssertEqual(hub2.destinationList.count, 3)
        XCTAssertEqual(hub2.interfaceRows.first?.heard, 3)
        XCTAssertEqual(hub2.counterRows.first, ReticulumRow("Announces", "4"))
        hub2.newIdentity()
        XCTAssertTrue(hub2.notice?.text.hasPrefix("New identity <") ?? false)
        hub2.clearDestinations()
        XCTAssertTrue(hub2.destinationList.isEmpty)
    }

    @MainActor
    func testPassphraseInKeychainAndMessagesOnDisk() async throws {
        let prefs = InMemoryKeyValueStore()
        let secure = InMemorySecureStore()
        let disk = RSSReaderMemoryDisk()
        let scheduler = ManualScheduler()
        let connector = FakeConnector(scheduler: scheduler)
        let hub = ReticulumHub(prefs: prefs, secure: secure, connector: connector, scheduler: scheduler, accountName: "alice", disk: disk)
        hub.open()
        var config = ReticulumInterfaceConfig(id: "mesh", name: "Mesh", host: "10.0.0.2", port: 4242)
        config.networkName = "testnet"
        config.passphrase = "hunter2"
        config.accessCodeSize = 8
        XCTAssertNil(hub.saveInterface(config, creating: true))
        let stored = prefs.string(forKey: ReticulumHub.interfacesKey) ?? ""
        XCTAssertFalse(stored.contains("hunter2"))
        XCTAssertTrue(stored.contains("testnet"))
        XCTAssertEqual(secure.read(ReticulumHub.passphrasePrefix + "mesh"), "hunter2")
        scheduler.advance(0.1)
        let link = try XCTUnwrap(connector.last)
        XCTAssertTrue(hub.node.announce())
        let code = ReticulumAccessCode(networkName: "testnet", passphrase: "hunter2", size: 8)
        XCTAssertNotNil(link.frames.first.flatMap { code.remove($0) })

        let peer = ReticulumIdentity.generate()
        var announce = ReticulumWire.buildAnnounce(identity: peer, name: "lxmf.delivery", appData: ReticulumLxmf.announceData(displayName: "Bob"),
                                                   pathResponse: false, unixTime: Int64(scheduler.now))
        announce = code.apply(announce)
        link.send(announce)
        let bob = ReticulumWire.destinationHash(name: "lxmf.delivery", identityHash: peer.hash)
        XCTAssertEqual(hub.messagePeers.first?.displayName, "Bob")
        hub.openConversation(bob)
        XCTAssertEqual(hub.tab, .messages)
        XCTAssertEqual(hub.chatTitle, "Bob")
        hub.composerText = "hello bob"
        XCTAssertTrue(hub.composerHint.hasSuffix("fits in a single packet"))
        let id = hub.sendMessage()
        XCTAssertFalse(id.isEmpty)
        XCTAssertEqual(hub.composerText, "")
        scheduler.advance(3)
        XCTAssertEqual(hub.chat.first?.state, .sent)
        XCTAssertEqual(ReticulumHub.bubbleMeta(hub.chat[0]).components(separatedBy: " · ").dropFirst(), ["✓ Sent", "single packet"])
        XCTAssertEqual(hub.conversations.first?.name, "Bob")
        XCTAssertEqual(hub.tiles[4].detail, "1 conversation · 0 links open")
        hub.saveNow()
        XCTAssertNotNil(disk.read(ReticulumHub.messagesFile))

        hub.removeInterface("mesh")
        XCTAssertNil(secure.read(ReticulumHub.passphrasePrefix + "mesh"))
        _ = hub.saveInterface(config, creating: true)
        let again = ReticulumHub(prefs: prefs, secure: secure, connector: FakeConnector(scheduler: scheduler), scheduler: scheduler, disk: disk)
        XCTAssertEqual(again.node.interfaces.first?.passphrase, "hunter2")
        XCTAssertEqual(again.node.interfaces.first?.accessCodeSize, 8)
        XCTAssertEqual(again.messenger.messages(with: bob).first?.state, .sent)
        XCTAssertEqual(again.messenger.messages(with: bob).first?.content, "hello bob")
    }
}
