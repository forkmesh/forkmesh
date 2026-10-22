#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#endif
import FMNet
import Foundation

public typealias ChatHostResolver = @Sendable (String) async -> [[UInt8]]

public enum ChatLinkAddresses {
    public static func isPublic(_ raw: [UInt8]) -> Bool {
        switch raw.count {
        case 4:
            return ChatLinkPreviewParsing.isPublicIPv4(raw.map(Int.init))
        case 16:
            return isPublicIPv6(raw)
        default:
            return false
        }
    }

    static func isPublicIPv6(_ b: [UInt8]) -> Bool {
        let head = (UInt16(b[0]) << 8) | UInt16(b[1])
        let second = (UInt16(b[2]) << 8) | UInt16(b[3])
        let embedded = Array(b[12..<16])
        if b[0..<10].allSatisfy({ $0 == 0 }) {
            if b[10] == 0xff && b[11] == 0xff { return ChatLinkPreviewParsing.isPublicIPv4(embedded.map(Int.init)) }
            return false
        }
        if head == 0x0064 && second == 0xff9b {
            if b[4..<12].allSatisfy({ $0 == 0 }) { return ChatLinkPreviewParsing.isPublicIPv4(embedded.map(Int.init)) }
            return false
        }
        if head == 0x2002 { return ChatLinkPreviewParsing.isPublicIPv4(Array(b[2..<6]).map(Int.init)) }
        if head == 0x2001 && (second == 0x0000 || second == 0x0db8 || (second & 0xfff0) == 0x0010 || (second & 0xfff0) == 0x0020) {
            return false
        }
        if head & 0xfe00 == 0xfc00 || head & 0xffc0 == 0xfe80 || head & 0xffc0 == 0xfec0 || head & 0xff00 == 0xff00 {
            return false
        }
        return head & 0xe000 == 0x2000
    }

    public static func allPublic(_ addresses: [[UInt8]]) -> Bool {
        !addresses.isEmpty && addresses.allSatisfy(isPublic)
    }

    public static let system: ChatHostResolver = { host in
        await Task.detached(priority: .utility) { resolve(host) }.value
    }

    static func resolve(_ host: String) -> [[UInt8]] {
        var name = host
        if name.hasPrefix("[") && name.hasSuffix("]") { name = String(name.dropFirst().dropLast()) }
        if name.isEmpty { return [] }
        var hints = addrinfo()
        hints.ai_family = AF_UNSPEC
        hints.ai_socktype = SocketCalls.stream
        var list: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(name, nil, &hints, &list) == 0, let first = list else { return [] }
        defer { freeaddrinfo(first) }
        var out: [[UInt8]] = []
        var cursor: UnsafeMutablePointer<addrinfo>? = first
        while let entry = cursor {
            if let address = entry.pointee.ai_addr {
                switch entry.pointee.ai_family {
                case AF_INET:
                    address.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { pointer in
                        var raw = pointer.pointee.sin_addr
                        out.append(withUnsafeBytes(of: &raw) { Array($0) })
                    }
                case AF_INET6:
                    address.withMemoryRebound(to: sockaddr_in6.self, capacity: 1) { pointer in
                        var raw = pointer.pointee.sin6_addr
                        out.append(withUnsafeBytes(of: &raw) { Array($0) })
                    }
                default:
                    out.append([])
                }
            }
            cursor = entry.pointee.ai_next
        }
        return out
    }
}

public final class ChatLinkGuardedHTTP: HTTPClient, @unchecked Sendable {
    public let base: HTTPClient
    public let resolver: ChatHostResolver

    public init(base: HTTPClient, resolver: @escaping ChatHostResolver = ChatLinkAddresses.system) {
        self.base = base
        self.resolver = resolver
    }

    func check(_ request: HTTPRequest) async throws {
        guard let host = request.url.host, ChatLinkPreviewParsing.previewable(request.url.absoluteString) != nil,
            ChatLinkAddresses.allPublic(await resolver(host))
        else { throw HTTPError.transport("refused a link preview to a private address") }
    }

    public func send(_ request: HTTPRequest) async throws -> HTTPResponse {
        try await check(request)
        var guarded = request
        guarded.followRedirects = false
        return try await base.send(guarded)
    }

    public func stream(_ request: HTTPRequest) async throws -> HTTPStreamedResponse {
        try await check(request)
        var guarded = request
        guarded.followRedirects = false
        return try await base.stream(guarded)
    }
}
