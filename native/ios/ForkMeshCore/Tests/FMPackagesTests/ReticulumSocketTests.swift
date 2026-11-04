import FMCrypto
import Foundation
import XCTest
@testable import FMPackages

#if canImport(Glibc)
import Glibc
private let streamType = Int32(SOCK_STREAM.rawValue)
#elseif canImport(Darwin)
import Darwin
private let streamType = SOCK_STREAM
#endif

private func posixWrite(_ fd: Int32, _ bytes: [UInt8]) -> Int {
    bytes.withUnsafeBytes { write(fd, $0.baseAddress, bytes.count) }
}

private func posixClose(_ fd: Int32) {
    shutdown(fd, Int32(SHUT_RDWR))
    close(fd)
}

private func loopback(port: UInt16) -> sockaddr_in {
    var address = sockaddr_in()
    address.sin_family = sa_family_t(AF_INET)
    address.sin_port = port.bigEndian
    address.sin_addr.s_addr = inet_addr("127.0.0.1")
    return address
}

private final class SocketPeer {
    private let lock = NSLock()
    private let listener: Int32
    private var client: Int32 = -1
    private var decoder = ReticulumFrameDecoder()
    private var received: [[UInt8]] = []
    private(set) var port: Int = 0

    init() throws {
        listener = socket(AF_INET, streamType, 0)
        guard listener >= 0 else { throw ReticulumError("socket failed") }
        var address = loopback(port: 0)
        let bound = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(listener, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        guard bound == 0, listen(listener, 4) == 0 else { throw ReticulumError("bind failed") }
        var length = socklen_t(MemoryLayout<sockaddr_in>.size)
        _ = withUnsafeMutablePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(listener, $0, &length) }
        }
        port = Int(UInt16(bigEndian: address.sin_port))
        Thread { [self] in serve() }.start()
    }

    private func serve() {
        while true {
            let accepted = accept(listener, nil, nil)
            guard accepted >= 0 else { return }
            lock.lock()
            client = accepted
            lock.unlock()
            var buffer = [UInt8](repeating: 0, count: 4096)
            while true {
                let count = read(accepted, &buffer, buffer.count)
                if count <= 0 { break }
                lock.lock()
                received += decoder.feed(Array(buffer[0..<count]))
                lock.unlock()
            }
        }
    }

    var connected: Bool {
        lock.lock()
        defer { lock.unlock() }
        return client >= 0
    }

    func send(_ raw: [UInt8]) {
        lock.lock()
        let target = client
        lock.unlock()
        let framed = ReticulumWire.hdlcFrame(raw)
        for start in stride(from: 0, to: framed.count, by: 11) {
            let piece = Array(framed[start..<min(framed.count, start + 11)])
            _ = posixWrite(target, piece)
        }
    }

    func find(_ type: ReticulumPacketType, _ context: UInt8) -> ReticulumPacket? {
        lock.lock()
        let frames = received
        lock.unlock()
        return frames.compactMap { try? ReticulumWire.parsePacket($0) }.first { $0.packetType == type && $0.context == context }
    }

    func dropClient() {
        lock.lock()
        let target = client
        client = -1
        lock.unlock()
        shutdown(target, Int32(SHUT_RDWR))
        close(target)
    }

    func stop() {
        dropClient()
        shutdown(listener, Int32(SHUT_RDWR))
        close(listener)
    }
}

private final class SocketLink: ReticulumLink {
    private let lock = NSLock()
    private let port: Int
    private let queue: DispatchQueue
    private var descriptor: Int32 = -1
    private var closed = false

    init(port: Int, queue: DispatchQueue) {
        self.port = port
        self.queue = queue
    }

    func open(_ handler: @escaping (ReticulumLinkEvent) -> Void) {
        let port = UInt16(self.port)
        Thread { [self] in
            let fd = socket(AF_INET, streamType, 0)
            var address = loopback(port: port)
            let result = withUnsafePointer(to: &address) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
            }
            lock.lock()
            descriptor = fd
            let gone = closed
            lock.unlock()
            if gone { return }
            guard result == 0 else {
                queue.async { handler(.failed("Connection refused")) }
                return
            }
            queue.async { handler(.connected) }
            var buffer = [UInt8](repeating: 0, count: 4096)
            while true {
                let count = read(fd, &buffer, buffer.count)
                if count <= 0 { break }
                let bytes = Array(buffer[0..<count])
                queue.async { handler(.received(bytes)) }
            }
            queue.async { handler(.closed) }
        }.start()
    }

    func write(_ bytes: [UInt8]) -> Int {
        lock.lock()
        let fd = descriptor
        lock.unlock()
        guard fd >= 0 else { return -1 }
        return posixWrite(fd, bytes)
    }

    func close() {
        lock.lock()
        closed = true
        let fd = descriptor
        lock.unlock()
        if fd >= 0 { posixClose(fd) }
    }
}

private final class SocketConnector: ReticulumConnector {
    let queue: DispatchQueue
    init(queue: DispatchQueue) { self.queue = queue }
    func tcp(host: String, port: Int) -> ReticulumLink? { SocketLink(port: port, queue: queue) }
}

final class ReticulumSocketTests: XCTestCase {
    private func waitFor(_ timeout: Double = 5, _ condition: () -> Bool) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while !condition() {
            if Date() > deadline { return false }
            usleep(10_000)
        }
        return true
    }

    func testLiveSessionOverTCP() throws {
        let peer = try SocketPeer()
        defer { peer.stop() }
        let queue = DispatchQueue(label: "reticulum.socket.test")
        let node = ReticulumNode(scheduler: ReticulumDispatchScheduler(queue: queue), connector: SocketConnector(queue: queue))
        queue.sync {
            node.setIdentity(.generate())
            node.setAnnounceName("Socket phone")
            node.setInterfaces([ReticulumInterfaceConfig(id: "peer", name: "Peer", host: "127.0.0.1", port: peer.port)])
            node.start()
        }
        XCTAssertTrue(waitFor { queue.sync { node.onlineCount == 1 } && peer.connected })
        peer.send(rnsHex(RNSVectors.announce))
        XCTAssertTrue(waitFor { queue.sync { node.destinations[rnsHex(RNSVectors.lxmf)] != nil } })
        XCTAssertEqual(queue.sync { node.destinations[rnsHex(RNSVectors.lxmf)]?.displayName }, "Field radio 3")
        XCTAssertTrue(queue.sync { node.announce() })
        XCTAssertTrue(waitFor { peer.find(.announce, ReticulumContext.none) != nil })
        let ours = try XCTUnwrap(peer.find(.announce, ReticulumContext.none))
        let parsed = try ReticulumWire.parseAnnounce(ours)
        XCTAssertNoThrow(try ReticulumWire.validateAnnounce(parsed))
        XCTAssertEqual(parsed.appData, Array("Socket phone".utf8))
        let destination = queue.sync { node.destinationHash }
        peer.send(ReticulumWire.buildPathRequest(destination: destination, tag: [UInt8](repeating: 0x33, count: 16)))
        XCTAssertTrue(waitFor { peer.find(.announce, ReticulumContext.pathResponse) != nil })
        XCTAssertTrue(waitFor(3) { queue.sync { node.history.count >= 2 } })
        peer.dropClient()
        XCTAssertTrue(waitFor { queue.sync { node.status("peer").state == .waiting } })
        XCTAssertEqual(queue.sync { node.status("peer").detail }, "127.0.0.1:\(peer.port) closed the connection")
        queue.sync { node.stop() }
        XCTAssertEqual(queue.sync { node.status("peer").state }, .disabled)
    }

    func testInteropWithPythonRNS() throws {
        let environment = ProcessInfo.processInfo.environment
        guard let endpoint = environment["RETICULUM_INTEROP_TCP"], let target = environment["RETICULUM_INTEROP_TARGET"],
              let out = environment["RETICULUM_INTEROP_OUT"], let port = endpoint.split(separator: ":").last.flatMap({ Int($0) }) else {
            throw XCTSkip("Set RETICULUM_INTEROP_TCP, RETICULUM_INTEROP_TARGET and RETICULUM_INTEROP_OUT to run against Python RNS")
        }
        let queue = DispatchQueue(label: "reticulum.interop")
        let node = ReticulumNode(scheduler: ReticulumDispatchScheduler(queue: queue), connector: SocketConnector(queue: queue))
        let remote = ReticulumWire.parseHash(target)
        queue.sync {
            node.setIdentity(.generate())
            node.setAnnounceName("ForkMesh interop")
            node.setInterfaces([ReticulumInterfaceConfig(id: "python", name: "Python RNS", host: "127.0.0.1", port: port)])
            node.start()
        }
        XCTAssertTrue(waitFor(15) { queue.sync { node.onlineCount == 1 } }, "INTEROP connect")
        XCTAssertTrue(waitFor(20) { queue.sync { node.destinations[remote] != nil } }, "INTEROP heard the Python announce")
        XCTAssertEqual(queue.sync { node.destinations[remote]?.displayName }, "Python peer", "INTEROP display name")
        queue.sync { _ = node.requestPath(remote) }
        XCTAssertTrue(waitFor(20) { queue.sync { node.lookups[remote]?.answered != nil } }, "INTEROP path answer")
        XCTAssertTrue(queue.sync { node.announce() }, "INTEROP announce")
        let own = queue.sync { ReticulumWire.hex(node.destinationHash) }
        try own.write(toFile: out, atomically: true, encoding: .utf8)
        _ = waitFor(25) { queue.sync { node.counters.pathRequestsAnswered > 0 } }
        XCTAssertGreaterThan(queue.sync { node.counters.pathRequestsAnswered }, 0, "INTEROP answered the Python path request")
        _ = waitFor(3) { false }
        let counters = queue.sync { node.counters }
        print("INTEROP destinations \(queue.sync { node.destinations.count }) rejected \(counters.rejectedAnnounces) invalid \(counters.invalid) answered \(counters.pathRequestsAnswered)")
        XCTAssertEqual(counters.rejectedAnnounces, 0, "INTEROP rejected")
        queue.sync { node.stop() }
    }

    func testLxmfInteropWithPython() throws {
        let environment = ProcessInfo.processInfo.environment
        guard let endpoint = environment["RETICULUM_LXMF_TCP"], let peerText = environment["RETICULUM_LXMF_PEER"],
              let port = endpoint.split(separator: ":").last.flatMap({ Int($0) }) else {
            throw XCTSkip("Set RETICULUM_LXMF_TCP, RETICULUM_LXMF_PEER and RETICULUM_LXMF_OUT to run against Python LXMF")
        }
        let out = environment["RETICULUM_LXMF_OUT"] ?? ""
        let peer = ReticulumWire.parseHash(peerText)
        let queue = DispatchQueue(label: "reticulum.lxmf.interop")
        let scheduler = ReticulumDispatchScheduler(queue: queue)
        let node = ReticulumNode(scheduler: scheduler, connector: SocketConnector(queue: queue))
        let messenger = ReticulumMessenger(node: node, scheduler: scheduler)
        queue.sync {
            node.setIdentity(.generate())
            messenger.setDisplayName("ForkMesh iPhone")
            node.onAnnounce = { messenger.destinationAnnounced($0) }
            node.packetHandler = { id, packet, raw, hops in messenger.handlePacket(interfaceId: id, packet: packet, raw: raw, hops: hops) }
            var config = ReticulumInterfaceConfig(id: "lxmf", name: "Python LXMF", host: "127.0.0.1", port: port)
            config.networkName = environment["RETICULUM_LXMF_NETNAME"] ?? ""
            config.passphrase = environment["RETICULUM_LXMF_PASSPHRASE"] ?? ""
            node.setInterfaces([config])
            node.start()
        }
        XCTAssertTrue(waitFor(10) { queue.sync { node.onlineCount == 1 } }, "LXMF: connected")
        XCTAssertTrue(waitFor(20) { queue.sync { node.destinations[peer] != nil } }, "LXMF: Python peer announce heard")
        print("LXMF: peer asks for \(queue.sync { messenger.peerStampCost(peer) }) bits")
        queue.sync { _ = node.announce() }
        try queue.sync { ReticulumWire.hex(messenger.address) }.write(toFile: out, atomically: true, encoding: .utf8)
        let longText = (0..<400).map { "line \($0) of a long message from ForkMesh\n" }.joined()
        let ids = queue.sync {
            [messenger.send(to: peer, content: "Hello from ForkMesh", title: "short"),
             messenger.send(to: peer, content: String(repeating: "m", count: 300), title: "medium"),
             messenger.send(to: peer, content: longText, title: "long")]
        }
        _ = waitFor(90) { queue.sync { ids.allSatisfy { messenger.message($0)?.state == .delivered } } }
        for id in ids {
            let message = queue.sync { messenger.message(id) }
            print("LXMF: \(message?.title ?? "-") \(message?.state.label ?? "-") by \(message?.method ?? "-") \(message?.note ?? "")")
            XCTAssertEqual(message?.state, .delivered, "LXMF: \(message?.title ?? "") delivered")
        }
        let titles = ["py-short", "py-medium", "py-long"]
        func received(_ title: String) -> ReticulumChatMessage? {
            queue.sync { messenger.messages(with: peer).first { !$0.outgoing && $0.title == title } }
        }
        _ = waitFor(90) { titles.allSatisfy { received($0) != nil } }
        for title in titles {
            let message = received(title)
            print("LXMF: received \(title) by \(message?.method ?? "-"), \(message?.content.count ?? 0) characters, signature \(message?.verified == true ? "verified" : "NOT verified")")
            XCTAssertTrue(message?.verified == true && !(message?.content.isEmpty ?? true), "LXMF: \(title)")
        }
        _ = waitFor(3) { false }
        queue.sync { node.stop() }
    }
}
