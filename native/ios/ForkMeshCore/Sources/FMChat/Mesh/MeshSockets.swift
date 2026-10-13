// The sockets under the local mesh room and the local control channel: TCP connections and
// listeners, UDP sockets and the network interface list.
//
// Decision: BSD sockets in the core, not Network.framework in the app target. The room's logic is
// written against socket behaviour (a unicast sweep sent from the discovery port so a stateful
// firewall passes the answer, Darwin's EHOSTUNREACH as the tell that Local Network access is off,
// "send buffer full" versus "refused", one datagram port shared by several sockets), which plain
// BSD sockets give directly. Keeping them here keeps that behaviour exact and lets Linux run the
// real loopback test cases. iOS allows BSD sockets to third-party apps; the Local Network prompt
// and the multicast entitlement apply to them.
//
// Every socket delivers what it reads through an AsyncStream (one consumer, in order), so callers
// on the main actor see bytes in the order the kernel handed them over. Writes are queued on a
// serial queue per connection and never block the caller.

#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#endif
import Dispatch
import Foundation

// MARK: - Addresses

/// An IPv4 or IPv6 address, without host names.
public struct MeshAddress: Hashable, Sendable, CustomStringConvertible {
    /// 4 bytes for IPv4, 16 for IPv6 (`rawAddress`).
    public let rawAddress: [UInt8]

    public init?(rawAddress: [UInt8]) {
        guard rawAddress.count == 4 || rawAddress.count == 16 else { return nil }
        self.rawAddress = rawAddress
    }

    /// A numeric address ("192.168.1.20", "fe80::1", "::ffff:10.0.0.1"); nil for anything else.
    public init?(_ text: String) {
        var v4 = in_addr()
        if inet_pton(AF_INET, text, &v4) == 1 {
            self.rawAddress = withUnsafeBytes(of: &v4) { Array($0) }
            return
        }
        var v6 = in6_addr()
        if inet_pton(AF_INET6, text, &v6) == 1 {
            self.rawAddress = withUnsafeBytes(of: &v6) { Array($0) }
            return
        }
        return nil
    }

    public static let loopbackIPv4 = MeshAddress(rawAddress: [127, 0, 0, 1])!
    public static let anyIPv4 = MeshAddress(rawAddress: [0, 0, 0, 0])!

    public var isIPv4: Bool { rawAddress.count == 4 }
    public var isIPv6: Bool { rawAddress.count == 16 }

    /// The numeric text form.
    public var address: String {
        var buffer = [CChar](repeating: 0, count: 64)
        if isIPv4 {
            var v4 = in_addr()
            withUnsafeMutableBytes(of: &v4) { $0.copyBytes(from: rawAddress) }
            inet_ntop(AF_INET, &v4, &buffer, socklen_t(buffer.count))
        } else {
            var v6 = in6_addr()
            withUnsafeMutableBytes(of: &v6) { $0.copyBytes(from: rawAddress) }
            inet_ntop(AF_INET6, &v6, &buffer, socklen_t(buffer.count))
        }
        return String(cString: buffer)
    }

    public var description: String { address }
}

/// True only for an address that cannot be routed off the local network: loopback, RFC 1918
/// private IPv4, RFC 3927 IPv4 link-local, IPv6 link-local and IPv6 unique-local. Carrier-grade
/// NAT space (100.64/10) is deliberately excluded: it belongs to an ISP, not to a LAN.
public func isLocalNetworkAddress(_ address: MeshAddress) -> Bool {
    let raw = address.rawAddress
    if address.isIPv4 {
        if raw[0] == 127 { return true }  // 127/8 loopback
        if raw[0] == 10 { return true }  // 10/8
        if raw[0] == 172 && raw[1] >= 16 && raw[1] <= 31 { return true }  // 172.16/12
        if raw[0] == 192 && raw[1] == 168 { return true }  // 192.168/16
        if raw[0] == 169 && raw[1] == 254 { return true }  // 169.254/16 link-local
        return false
    }
    // ::1
    if raw.prefix(15).allSatisfy({ $0 == 0 }) && raw[15] == 1 { return true }
    // An IPv4-mapped address is judged as the IPv4 address it carries.
    if raw.prefix(10).allSatisfy({ $0 == 0 }) && raw[10] == 0xff && raw[11] == 0xff {
        return isLocalNetworkAddress(MeshAddress(rawAddress: Array(raw[12..<16]))!)
    }
    if (raw[0] & 0xfe) == 0xfc { return true }  // fc00::/7 unique-local
    return raw[0] == 0xfe && (raw[1] & 0xc0) == 0x80  // fe80::/10 link-local
}

public func isSweptInterfaceName(_ name: String) -> Bool {
    ["en", "wlan", "eth", "bridge"].contains { name.hasPrefix($0) }
}

/// The hosts of the subnet around `address` that a unicast beacon sweep covers, minus `address`
/// itself. Empty for anything but a private IPv4 address, so a sweep never leaves the local
/// network. A prefix wider than /24, or an unknown one, is swept as the /24 around `address`;
/// an iPhone's Personal Hotspot hands out a /28.
public func sweepAddresses(_ address: MeshAddress, prefixLength: Int = 24) -> [MeshAddress] {
    guard let (network, hostBits) = sweptSubnet(address, prefixLength: prefixLength) else { return [] }
    let own = sweptValue(address.rawAddress)
    var hosts: [MeshAddress] = []
    for host in 1..<((UInt32(1) << hostBits) - 1) where network + host != own {
        hosts.append(sweptAddress(network + host))
    }
    return hosts
}

/// The broadcast address of the subnet `sweepAddresses` covers, or nil for anything it would
/// not sweep.
public func sweepBroadcastAddress(_ address: MeshAddress, prefixLength: Int = 24) -> MeshAddress? {
    guard let (network, hostBits) = sweptSubnet(address, prefixLength: prefixLength) else { return nil }
    return sweptAddress(network + (UInt32(1) << hostBits) - 1)
}

private func sweptSubnet(_ address: MeshAddress, prefixLength: Int) -> (UInt32, UInt32)? {
    let raw = address.rawAddress
    if !address.isIPv4 || prefixLength > 30 { return nil }
    if !isLocalNetworkAddress(address) || raw[0] == 127 { return nil }
    if raw[0] == 169 && raw[1] == 254 { return nil }
    let hostBits = UInt32(32 - max(24, prefixLength))
    return (sweptValue(raw) & (UInt32.max << hostBits), hostBits)
}

private func sweptValue(_ raw: [UInt8]) -> UInt32 {
    raw.prefix(4).reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
}

private func sweptAddress(_ value: UInt32) -> MeshAddress {
    MeshAddress(rawAddress: [UInt8(value >> 24), UInt8((value >> 16) & 0xff), UInt8((value >> 8) & 0xff), UInt8(value & 0xff)])!
}

// MARK: - Errors

/// A failed socket call.
public struct MeshSocketError: Error, Equatable, Sendable, CustomStringConvertible {
    /// errno, or 0 when the failure was not the system's (a timeout, a closed socket).
    public let code: Int32
    public let message: String

    public init(code: Int32, message: String) {
        self.code = code
        self.message = message
    }

    static func errno(_ value: Int32 = currentErrno()) -> MeshSocketError {
        MeshSocketError(code: value, message: String(cString: strerror(value)))
    }

    public static let timedOut = MeshSocketError(code: 0, message: "Connection timed out")
    public static let closed = MeshSocketError(code: 0, message: "Socket has been closed")

    /// "message (code)" for an OS error; the bare message otherwise.
    public var description: String { code == 0 ? message : "\(message) (\(code))" }

    /// The platform's "no route to host". iOS answers every local send with it while the app's
    /// Local Network access is off or its prompt is unanswered (65 on Darwin).
    public static var hostUnreachable: Int32 { EHOSTUNREACH }

    public var meansSocketGone: Bool {
        code == EBADF || code == ENOTSOCK || code == EPIPE || code == ENOTCONN
    }
}

@inline(__always)
func currentErrno() -> Int32 { errno }

// MARK: - Plumbing

enum SocketCalls {
    #if canImport(Darwin)
    static let stream = SOCK_STREAM
    static let datagram = SOCK_DGRAM
    static let sendFlags: Int32 = 0
    #else
    static let stream = Int32(SOCK_STREAM.rawValue)
    static let datagram = Int32(SOCK_DGRAM.rawValue)
    static let sendFlags = Int32(MSG_NOSIGNAL)
    #endif

    static func makeSocket(family: Int32, type: Int32) throws -> Int32 {
        let fd = socket(family, type, 0)
        if fd < 0 { throw MeshSocketError.errno() }
        #if canImport(Darwin)
        var on: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
        #endif
        return fd
    }

    static func setOption(_ fd: Int32, _ level: Int32, _ name: Int32, _ value: Int32) -> Bool {
        var v = value
        return setsockopt(fd, level, name, &v, socklen_t(MemoryLayout<Int32>.size)) == 0
    }

    static func setByteOption(_ fd: Int32, _ level: Int32, _ name: Int32, _ value: UInt8) -> Bool {
        var v = value
        return setsockopt(fd, level, name, &v, socklen_t(MemoryLayout<UInt8>.size)) == 0
    }

    static func setKeepAlive(_ fd: Int32, idle: Int32, interval: Int32, count: Int32) {
        _ = setOption(fd, SOL_SOCKET, SO_KEEPALIVE, 1)
        #if canImport(Darwin)
        _ = setOption(fd, Int32(IPPROTO_TCP), TCP_KEEPALIVE, idle)
        #else
        _ = setOption(fd, Int32(IPPROTO_TCP), TCP_KEEPIDLE, idle)
        #endif
        _ = setOption(fd, Int32(IPPROTO_TCP), TCP_KEEPINTVL, interval)
        _ = setOption(fd, Int32(IPPROTO_TCP), TCP_KEEPCNT, count)
    }

    static func setNonBlocking(_ fd: Int32, _ on: Bool) {
        let flags = fcntl(fd, F_GETFL)
        _ = fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK))
    }

    /// A sockaddr for `address:port`, handed to `body` with its length.
    static func withSockaddr<T>(
        _ address: MeshAddress, _ port: Int,
        _ body: (UnsafePointer<sockaddr>, socklen_t) -> T
    ) -> T {
        if address.isIPv4 {
            var sa = sockaddr_in()
            #if canImport(Darwin)
            sa.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            #endif
            sa.sin_family = sa_family_t(AF_INET)
            sa.sin_port = UInt16(truncatingIfNeeded: port).bigEndian
            withUnsafeMutableBytes(of: &sa.sin_addr) { $0.copyBytes(from: address.rawAddress) }
            return withUnsafePointer(to: &sa) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { body($0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
            }
        }
        var sa = sockaddr_in6()
        #if canImport(Darwin)
        sa.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size)
        #endif
        sa.sin6_family = sa_family_t(AF_INET6)
        sa.sin6_port = UInt16(truncatingIfNeeded: port).bigEndian
        withUnsafeMutableBytes(of: &sa.sin6_addr) { $0.copyBytes(from: address.rawAddress) }
        return withUnsafePointer(to: &sa) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { body($0, socklen_t(MemoryLayout<sockaddr_in6>.size)) }
        }
    }

    /// The address and port in a filled-in `sockaddr_storage`.
    static func endpoint(_ storage: inout sockaddr_storage) -> (MeshAddress, Int)? {
        switch Int32(storage.ss_family) {
        case AF_INET:
            return withUnsafePointer(to: &storage) {
                $0.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { pointer -> (MeshAddress, Int)? in
                    var addr = pointer.pointee.sin_addr
                    let raw = withUnsafeBytes(of: &addr) { Array($0) }
                    guard let address = MeshAddress(rawAddress: raw) else { return nil }
                    return (address, Int(UInt16(bigEndian: pointer.pointee.sin_port)))
                }
            }
        case AF_INET6:
            return withUnsafePointer(to: &storage) {
                $0.withMemoryRebound(to: sockaddr_in6.self, capacity: 1) { pointer -> (MeshAddress, Int)? in
                    var addr = pointer.pointee.sin6_addr
                    let raw = withUnsafeBytes(of: &addr) { Array($0) }
                    guard let address = MeshAddress(rawAddress: raw) else { return nil }
                    return (address, Int(UInt16(bigEndian: pointer.pointee.sin6_port)))
                }
            }
        default:
            return nil
        }
    }

    static func localPort(_ fd: Int32) -> Int {
        var storage = sockaddr_storage()
        var length = socklen_t(MemoryLayout<sockaddr_storage>.size)
        let ok = withUnsafeMutablePointer(to: &storage) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &length) }
        }
        if ok != 0 { return 0 }
        return endpoint(&storage)?.1 ?? 0
    }

    static func sysConnect(_ fd: Int32, _ address: UnsafePointer<sockaddr>, _ length: socklen_t) -> Int32 {
        #if canImport(Darwin)
        return Darwin.connect(fd, address, length)
        #else
        return Glibc.connect(fd, address, length)
        #endif
    }

    static func sysBind(_ fd: Int32, _ address: UnsafePointer<sockaddr>, _ length: socklen_t) -> Int32 {
        #if canImport(Darwin)
        return Darwin.bind(fd, address, length)
        #else
        return Glibc.bind(fd, address, length)
        #endif
    }

    static func closeDescriptor(_ fd: Int32) {
        #if canImport(Darwin)
        _ = Darwin.close(fd)
        #else
        _ = Glibc.close(fd)
        #endif
    }
}

// MARK: - TCP

/// What a TCP connection reports, in order.
public enum MeshSocketEvent: Sendable {
    case data(Data)
    /// The last event. nil error: the peer closed; otherwise the error that ended the stream.
    case closed(MeshSocketError?)
}

/// One TCP connection.
public final class MeshTCPConnection: @unchecked Sendable {
    public let remoteAddress: MeshAddress
    public let remotePort: Int
    /// Everything read, then one `.closed`. One consumer.
    public let events: AsyncStream<MeshSocketEvent>

    private let fd: Int32
    private let continuation: AsyncStream<MeshSocketEvent>.Continuation
    private let readQueue = DispatchQueue(label: "forkmesh.mesh.tcp.read")
    private let writeQueue = DispatchQueue(label: "forkmesh.mesh.tcp.write")
    private let lock = NSLock()
    private var closedFlag = false
    private var readSource: DispatchSourceRead?

    init(fd: Int32, remoteAddress: MeshAddress, remotePort: Int) {
        self.fd = fd
        self.remoteAddress = remoteAddress
        self.remotePort = remotePort
        var cont: AsyncStream<MeshSocketEvent>.Continuation!
        self.events = AsyncStream(bufferingPolicy: .unbounded) { cont = $0 }
        self.continuation = cont
        SocketCalls.setNonBlocking(fd, false)
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: readQueue)
        source.setEventHandler { [weak self] in self?.readAvailable() }
        let writeQueue = self.writeQueue
        source.setCancelHandler {
            // After any queued write has given up (the socket is shut down by then).
            writeQueue.async { SocketCalls.closeDescriptor(fd) }
        }
        readSource = source
        source.resume()
    }

    deinit {
        close()
    }

    public var isClosed: Bool {
        lock.lock()
        defer { lock.unlock() }
        return closedFlag
    }

    /// Dials `address:port`, failing after `timeout` seconds (`Socket.connect(timeout:)`).
    public static func connect(to address: MeshAddress, port: Int, timeout: TimeInterval) async throws -> MeshTCPConnection {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<MeshTCPConnection, Error>) in
            DispatchQueue.global(qos: .userInitiated).async {
                do {
                    let fd = try dial(address, port, timeout)
                    continuation.resume(returning: MeshTCPConnection(fd: fd, remoteAddress: address, remotePort: port))
                } catch {
                    continuation.resume(throwing: error)
                }
            }
        }
    }

    private static func dial(_ address: MeshAddress, _ port: Int, _ timeout: TimeInterval) throws -> Int32 {
        let fd = try SocketCalls.makeSocket(family: address.isIPv4 ? AF_INET : AF_INET6, type: SocketCalls.stream)
        SocketCalls.setNonBlocking(fd, true)
        let started = SocketCalls.withSockaddr(address, port) { sockaddr, length in
            SocketCalls.sysConnect(fd, sockaddr, length)
        }
        if started != 0 {
            let error = currentErrno()
            if error != EINPROGRESS {
                SocketCalls.closeDescriptor(fd)
                throw MeshSocketError.errno(error)
            }
            var poller = pollfd(fd: fd, events: Int16(POLLOUT), revents: 0)
            let ready = poll(&poller, 1, Int32(max(0, timeout) * 1000))
            if ready == 0 {
                SocketCalls.closeDescriptor(fd)
                throw MeshSocketError.timedOut
            }
            if ready < 0 {
                let pollError = currentErrno()
                SocketCalls.closeDescriptor(fd)
                throw MeshSocketError.errno(pollError)
            }
            var soError: Int32 = 0
            var length = socklen_t(MemoryLayout<Int32>.size)
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &length)
            if soError != 0 {
                SocketCalls.closeDescriptor(fd)
                throw MeshSocketError.errno(soError)
            }
        }
        return fd
    }

    /// `socket.setOption(SocketOption.tcpNoDelay, on)`.
    public func setNoDelay(_ on: Bool) {
        _ = SocketCalls.setOption(fd, Int32(IPPROTO_TCP), TCP_NODELAY, on ? 1 : 0)
    }

    public func setKeepAlive(idle: Int, interval: Int, count: Int) {
        SocketCalls.setKeepAlive(fd, idle: Int32(idle), interval: Int32(interval), count: Int32(count))
    }

    /// Queues `data` for sending and returns at once (`Socket.add`). A failed write closes the
    /// connection with the error.
    public func write(_ data: Data) {
        if data.isEmpty || isClosed { return }
        writeQueue.async { [weak self] in
            guard let self else { return }
            if self.isClosed { return }
            var offset = 0
            let failure: MeshSocketError? = data.withUnsafeBytes { buffer -> MeshSocketError? in
                guard let base = buffer.baseAddress else { return nil }
                while offset < buffer.count {
                    let sent = send(self.fd, base + offset, buffer.count - offset, SocketCalls.sendFlags)
                    if sent < 0 {
                        let error = currentErrno()
                        if error == EINTR { continue }
                        return MeshSocketError.errno(error)
                    }
                    offset += sent
                }
                return nil
            }
            if let failure { self.finish(failure) }
        }
    }

    /// Tears the connection down (`Socket.destroy`). Idempotent.
    public func close() {
        finish(nil, notify: true)
    }

    private func readAvailable() {
        var buffer = [UInt8](repeating: 0, count: 64 * 1024)
        let count = read(fd, &buffer, buffer.count)
        if count > 0 {
            continuation.yield(.data(Data(buffer[0..<count])))
            return
        }
        if count == 0 {
            finish(nil)
            return
        }
        let error = currentErrno()
        if error == EAGAIN || error == EWOULDBLOCK || error == EINTR { return }
        finish(MeshSocketError.errno(error))
    }

    private func finish(_ error: MeshSocketError?, notify: Bool = true) {
        lock.lock()
        if closedFlag {
            lock.unlock()
            return
        }
        closedFlag = true
        let source = readSource
        readSource = nil
        lock.unlock()
        shutdown(fd, Int32(SHUT_RDWR))
        if notify { continuation.yield(.closed(error)) }
        continuation.finish()
        source?.cancel()
    }
}

/// A listening TCP socket.
public final class MeshTCPListener: @unchecked Sendable {
    public let port: Int
    /// Every accepted connection. One consumer.
    public let connections: AsyncStream<MeshTCPConnection>

    private let fd: Int32
    private let continuation: AsyncStream<MeshTCPConnection>.Continuation
    private let queue = DispatchQueue(label: "forkmesh.mesh.tcp.accept")
    private let lock = NSLock()
    private let released = DispatchSemaphore(value: 0)
    private var source: DispatchSourceRead?

    /// `ServerSocket.bind(address, port)`; port 0 picks a free one.
    public static func bind(address: MeshAddress = .anyIPv4, port: Int = 0) throws -> MeshTCPListener {
        let fd = try SocketCalls.makeSocket(family: address.isIPv4 ? AF_INET : AF_INET6, type: SocketCalls.stream)
        _ = SocketCalls.setOption(fd, SOL_SOCKET, SO_REUSEADDR, 1)
        let bound = SocketCalls.withSockaddr(address, port) { sockaddr, length in
            SocketCalls.sysBind(fd, sockaddr, length)
        }
        if bound != 0 || listen(fd, 64) != 0 {
            let error = MeshSocketError.errno()
            SocketCalls.closeDescriptor(fd)
            throw error
        }
        return MeshTCPListener(fd: fd, port: SocketCalls.localPort(fd))
    }

    private init(fd: Int32, port: Int) {
        self.fd = fd
        self.port = port
        var cont: AsyncStream<MeshTCPConnection>.Continuation!
        self.connections = AsyncStream(bufferingPolicy: .unbounded) { cont = $0 }
        self.continuation = cont
        SocketCalls.setNonBlocking(fd, true)
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
        source.setEventHandler { [weak self] in self?.acceptAll() }
        let released = self.released
        source.setCancelHandler {
            SocketCalls.closeDescriptor(fd)
            released.signal()
        }
        self.source = source
        source.resume()
    }

    deinit {
        close()
    }

    private func acceptAll() {
        while true {
            var storage = sockaddr_storage()
            var length = socklen_t(MemoryLayout<sockaddr_storage>.size)
            let client = withUnsafeMutablePointer(to: &storage) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { accept(fd, $0, &length) }
            }
            if client < 0 {
                let error = currentErrno()
                if error == EAGAIN || error == EWOULDBLOCK { return }
                if error == EINTR || error == ECONNABORTED || error == EPROTO { continue }
                close()
                return
            }
            #if canImport(Darwin)
            _ = SocketCalls.setOption(client, SOL_SOCKET, SO_NOSIGPIPE, 1)
            #endif
            guard let (address, port) = SocketCalls.endpoint(&storage) else {
                SocketCalls.closeDescriptor(client)
                continue
            }
            continuation.yield(MeshTCPConnection(fd: client, remoteAddress: address, remotePort: port))
        }
    }

    /// Stops listening (`ServerSocket.close`). Idempotent.
    public func close() {
        _ = cancelSource()
    }

    public func closeAndWait(timeout: TimeInterval) {
        if cancelSource() { _ = released.wait(timeout: .now() + timeout) }
    }

    private func cancelSource() -> Bool {
        lock.lock()
        let current = source
        source = nil
        lock.unlock()
        guard let current else { return false }
        continuation.finish()
        current.cancel()
        return true
    }
}

// MARK: - UDP

/// One datagram received.
public struct MeshDatagram: Sendable {
    public let data: Data
    public let address: MeshAddress
    public let port: Int
}

/// The outcome of one datagram send.
public enum MeshSendResult: Equatable, Sendable {
    /// Handed to the system; the byte count.
    case sent(Int)
    /// The send buffer is full (nothing was sent); try again shortly.
    case bufferFull
    /// The system refused the datagram.
    case refused(MeshSocketError)
}

/// A bound UDP socket. A refused send does not close it.
public final class MeshUDPSocket: @unchecked Sendable {
    public let port: Int
    /// Every datagram received. One consumer.
    public let datagrams: AsyncStream<MeshDatagram>

    private let fd: Int32
    private let continuation: AsyncStream<MeshDatagram>.Continuation
    private let queue = DispatchQueue(label: "forkmesh.mesh.udp")
    private let lock = NSLock()
    private var source: DispatchSourceRead?

    /// `RawDatagramSocket.bind(address, port, reuseAddress:, reusePort:)`.
    public static func bind(address: MeshAddress = .anyIPv4, port: Int, reuse: Bool = true) throws -> MeshUDPSocket {
        let fd = try SocketCalls.makeSocket(family: address.isIPv4 ? AF_INET : AF_INET6, type: SocketCalls.datagram)
        if reuse {
            _ = SocketCalls.setOption(fd, SOL_SOCKET, SO_REUSEADDR, 1)
            _ = SocketCalls.setOption(fd, SOL_SOCKET, SO_REUSEPORT, 1)
        }
        let bound = SocketCalls.withSockaddr(address, port) { sockaddr, length in
            SocketCalls.sysBind(fd, sockaddr, length)
        }
        if bound != 0 {
            let error = MeshSocketError.errno()
            SocketCalls.closeDescriptor(fd)
            throw error
        }
        return MeshUDPSocket(fd: fd, port: SocketCalls.localPort(fd))
    }

    private init(fd: Int32, port: Int) {
        self.fd = fd
        self.port = port
        var cont: AsyncStream<MeshDatagram>.Continuation!
        self.datagrams = AsyncStream(bufferingPolicy: .bufferingNewest(512)) { cont = $0 }
        self.continuation = cont
        SocketCalls.setNonBlocking(fd, true)
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
        source.setEventHandler { [weak self] in self?.receiveAll() }
        source.setCancelHandler { SocketCalls.closeDescriptor(fd) }
        self.source = source
        source.resume()
    }

    deinit {
        close()
    }

    /// TTL for multicast (`multicastHops`). 1 keeps a datagram on the local segment.
    public func setMulticastHops(_ hops: UInt8) {
        _ = SocketCalls.setByteOption(fd, Int32(IPPROTO_IP), IP_MULTICAST_TTL, hops)
    }

    /// Whether this host hears its own multicast (`multicastLoopback`).
    public func setMulticastLoopback(_ on: Bool) {
        _ = SocketCalls.setByteOption(fd, Int32(IPPROTO_IP), IP_MULTICAST_LOOP, on ? 1 : 0)
    }

    /// `broadcastEnabled`. False when the system refused.
    @discardableResult
    public func setBroadcastEnabled(_ on: Bool) -> Bool {
        SocketCalls.setOption(fd, SOL_SOCKET, SO_BROADCAST, on ? 1 : 0)
    }

    /// `joinMulticast(group)` on the default interface (IPv4 groups only).
    public func joinMulticast(_ group: MeshAddress) throws {
        guard group.isIPv4 else { throw MeshSocketError(code: 0, message: "IPv6 groups are not supported") }
        var request = ip_mreq()
        withUnsafeMutableBytes(of: &request.imr_multiaddr) { $0.copyBytes(from: group.rawAddress) }
        request.imr_interface.s_addr = 0  // INADDR_ANY
        let ok = setsockopt(fd, Int32(IPPROTO_IP), IP_ADD_MEMBERSHIP, &request, socklen_t(MemoryLayout<ip_mreq>.size))
        if ok != 0 { throw MeshSocketError.errno() }
    }

    /// Sends one datagram without blocking.
    public func send(_ data: [UInt8], to address: MeshAddress, port: Int) -> MeshSendResult {
        let sent = SocketCalls.withSockaddr(address, port) { sockaddr, length -> Int in
            data.withUnsafeBytes { buffer in
                sendto(fd, buffer.baseAddress, buffer.count, SocketCalls.sendFlags | Int32(MSG_DONTWAIT), sockaddr, length)
            }
        }
        if sent >= 0 { return .sent(sent) }
        let error = currentErrno()
        if error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS { return .bufferFull }
        return .refused(MeshSocketError.errno(error))
    }

    private func receiveAll() {
        var buffer = [UInt8](repeating: 0, count: 65_536)
        while true {
            var storage = sockaddr_storage()
            var length = socklen_t(MemoryLayout<sockaddr_storage>.size)
            let count = withUnsafeMutablePointer(to: &storage) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    recvfrom(fd, &buffer, buffer.count, 0, $0, &length)
                }
            }
            if count < 0 {
                let error = currentErrno()
                if error == EAGAIN || error == EWOULDBLOCK { return }
                if error == EINTR || error == ECONNREFUSED || error == EHOSTUNREACH || error == ENETUNREACH
                    || error == EHOSTDOWN || error == ENETDOWN || error == EMSGSIZE
                {
                    continue
                }
                close()
                return
            }
            guard let (address, port) = SocketCalls.endpoint(&storage) else { continue }
            continuation.yield(MeshDatagram(data: Data(buffer[0..<count]), address: address, port: port))
        }
    }

    /// Idempotent.
    public func close() {
        lock.lock()
        let current = source
        source = nil
        lock.unlock()
        guard let current else { return }
        continuation.finish()
        current.cancel()
    }
}

// MARK: - Interfaces

/// One network interface, its addresses and, in the same order, each address's prefix length
/// (-1 when the system gave no netmask).
public struct MeshInterface: Equatable, Sendable {
    public let name: String
    public let addresses: [MeshAddress]
    public let prefixLengths: [Int]

    public init(name: String, addresses: [MeshAddress], prefixLengths: [Int] = []) {
        self.name = name
        self.addresses = addresses
        self.prefixLengths = addresses.indices.map { $0 < prefixLengths.count ? prefixLengths[$0] : -1 }
    }
}

public enum MeshInterfaces {
    /// `NetworkInterface.list(includeLoopback:, type: IPv4)`: interfaces that are up, in the
    /// system's order, with their IPv4 (or, with `ipv4Only` false, every) address.
    public static func list(includeLoopback: Bool = false, ipv4Only: Bool = true) throws -> [MeshInterface] {
        var head: UnsafeMutablePointer<ifaddrs>?
        if getifaddrs(&head) != 0 { throw MeshSocketError.errno() }
        defer { freeifaddrs(head) }
        var order: [String] = []
        var byName: [String: [MeshAddress]] = [:]
        var prefixesByName: [String: [Int]] = [:]
        var cursor = head
        while let entry = cursor {
            defer { cursor = entry.pointee.ifa_next }
            let flags = Int32(bitPattern: entry.pointee.ifa_flags)
            if (flags & Int32(IFF_UP)) == 0 { continue }
            if !includeLoopback && (flags & Int32(IFF_LOOPBACK)) != 0 { continue }
            guard let addr = entry.pointee.ifa_addr else { continue }
            let family = Int32(addr.pointee.sa_family)
            if family != AF_INET && (ipv4Only || family != AF_INET6) { continue }
            var storage = sockaddr_storage()
            let size = family == AF_INET ? MemoryLayout<sockaddr_in>.size : MemoryLayout<sockaddr_in6>.size
            withUnsafeMutableBytes(of: &storage) { target in
                target.copyMemory(from: UnsafeRawBufferPointer(start: UnsafeRawPointer(addr), count: size))
            }
            guard let (address, _) = SocketCalls.endpoint(&storage) else { continue }
            let name = String(cString: entry.pointee.ifa_name)
            if byName[name] == nil {
                order.append(name)
                byName[name] = []
            }
            byName[name]?.append(address)
            prefixesByName[name, default: []].append(prefixLength(entry.pointee.ifa_netmask, family: family))
        }
        return order.map {
            MeshInterface(name: $0, addresses: byName[$0] ?? [], prefixLengths: prefixesByName[$0] ?? [])
        }
    }

    private static func prefixLength(_ netmask: UnsafeMutablePointer<sockaddr>?, family: Int32) -> Int {
        guard let netmask else { return -1 }
        let offset = family == AF_INET
            ? MemoryLayout<sockaddr_in>.offset(of: \sockaddr_in.sin_addr)!
            : MemoryLayout<sockaddr_in6>.offset(of: \sockaddr_in6.sin6_addr)!
        var count = family == AF_INET ? 4 : 16
        #if canImport(Darwin)
        count = min(count, max(0, Int(netmask.pointee.sa_len) - offset))
        #endif
        let bytes = UnsafeRawPointer(netmask).advanced(by: offset).assumingMemoryBound(to: UInt8.self)
        var bits = 0
        for index in 0..<count {
            let byte = bytes[index]
            bits += byte.nonzeroBitCount
            if byte != 0xff { break }
        }
        return bits
    }
}
