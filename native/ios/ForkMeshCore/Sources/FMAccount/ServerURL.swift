// Port of native/android/.../api/ServerUrl.kt: the URL derivation every service shares, with its
// percent-encoding.

import Foundation

/// The configured relay URL, parsed leniently, and the HTTP and WebSocket endpoints the services
/// derive from it: `http` for a `ws` relay and `https` otherwise, the same host, the explicit port
/// kept, then the encoded path and query.
public struct ServerURL: Equatable, Sendable {
    /// Lower-cased scheme of the configured URL ("wss", "ws", ...).
    public let scheme: String
    /// Lower-cased host, without IPv6 brackets.
    public let host: String
    /// The explicit port, or nil when absent or the scheme's default.
    public let port: Int?

    public init(scheme: String, host: String, port: Int?) {
        self.scheme = scheme
        self.host = host
        self.port = port
    }

    /// Never fails: a string without an authority gives an empty host.
    public static func parse(_ url: String) -> ServerURL {
        let text = url
        var rest = Substring(text)
        var scheme = ""
        // ^([a-zA-Z][a-zA-Z0-9+.-]*):
        if let colon = rest.firstIndex(of: ":") {
            let candidate = rest[rest.startIndex..<colon]
            if let first = candidate.first, first.isASCII, first.isLetter,
               candidate.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "+" || $0 == "." || $0 == "-") }) {
                scheme = candidate.lowercased()
                rest = rest[rest.index(after: colon)...]
            }
        }
        var authority = Substring("")
        if rest.hasPrefix("//") {
            let after = rest.dropFirst(2)
            let end = after.firstIndex(where: { $0 == "/" || $0 == "?" || $0 == "#" }) ?? after.endIndex
            authority = after[after.startIndex..<end]
        }
        let hostPort = authority.lastIndex(of: "@").map { authority[authority.index(after: $0)...] } ?? authority
        var host: String
        var portText = ""
        if hostPort.hasPrefix("[") {
            if let close = hostPort.firstIndex(of: "]") {
                host = String(hostPort[hostPort.index(after: hostPort.startIndex)..<close])
                let after = hostPort.index(after: close)
                if after < hostPort.endIndex, hostPort[after] == ":" {
                    portText = String(hostPort[hostPort.index(after: after)...])
                }
            } else {
                host = String(hostPort.dropFirst())
            }
        } else if let colon = hostPort.lastIndex(of: ":") {
            host = String(hostPort[hostPort.startIndex..<colon])
            portText = String(hostPort[hostPort.index(after: colon)...])
        } else {
            host = String(hostPort)
        }
        host = DartURI.decodePercent(host).lowercased()
        let port = Int(portText).flatMap { $0 == defaultPort(scheme) ? nil : $0 }
        return ServerURL(scheme: scheme, host: host, port: port)
    }

    /// `https://host[:port]<path>?<query>`, or `http` when the relay URL is `ws`.
    public func http(_ path: String, query: [(String, String)] = []) -> String {
        build(scheme == "ws" ? "http" : "https", path, query)
    }

    /// `http(path)` as a URL. Only nil for a host no URL can carry.
    public func httpURL(_ path: String, query: [(String, String)] = []) -> URL? {
        URL(string: http(path, query: query))
    }

    /// `wss://...`, or `ws` when the relay URL is `ws` or `http` (`accountEventsUri`).
    public func webSocket(_ path: String, query: [(String, String)] = []) -> String {
        let secure = scheme != "ws" && scheme != "http"
        return build(secure ? "wss" : "ws", path, query)
    }

    /// `scheme://host[:port]` of the HTTP origin.
    public var httpOrigin: String {
        let target = scheme == "ws" ? "http" : "https"
        return "\(target)://\(authority(target))"
    }

    private func authority(_ targetScheme: String) -> String {
        let hostPart = host.contains(":") ? "[\(host)]" : host
        guard let port, port != ServerURL.defaultPort(targetScheme) else { return hostPart }
        return "\(hostPart):\(port)"
    }

    private func build(_ targetScheme: String, _ path: String, _ query: [(String, String)]) -> String {
        var out = "\(targetScheme)://\(authority(targetScheme))"
        let cleanPath = path.isEmpty || path.hasPrefix("/") ? path : "/" + path
        out += DartURI.encodePath(cleanPath)
        if !query.isEmpty { out += "?" + DartURI.queryString(query) }
        return out
    }

    /// Default ports: only http and https have one (`ws://h:80` keeps its port).
    static func defaultPort(_ scheme: String) -> Int {
        switch scheme {
        case "http": return 80
        case "https": return 443
        default: return 0
        }
    }

    public static func == (lhs: ServerURL, rhs: ServerURL) -> Bool {
        lhs.scheme == rhs.scheme && lhs.host == rhs.host && lhs.port == rhs.port
    }
}

/// Percent-encoding for components, query components and paths.
public enum DartURI {
    private static let hex = Array("0123456789ABCDEF".utf8)
    private static let componentExtra = Set("-_.!~*'()".utf8)
    private static let queryExtra = Set("-._~".utf8)
    private static let pathExtra = Set("-._~!$&'()*+,;=:@/".utf8)

    /// `Uri.encodeComponent`.
    public static func encodeComponent(_ value: String) -> String {
        encode(value, componentExtra, spaceToPlus: false, keepEscapes: false)
    }

    /// `Uri.encodeQueryComponent`: spaces become '+'.
    public static func encodeQueryComponent(_ value: String) -> String {
        encode(value, queryExtra, spaceToPlus: true, keepEscapes: false)
    }

    /// A `Uri(path: ...)` path: path characters are kept, valid `%XX` escapes too.
    public static func encodePath(_ path: String) -> String {
        encode(path, pathExtra, spaceToPlus: false, keepEscapes: true)
    }

    /// `Uri(queryParameters: ...)`, in the order given.
    public static func queryString(_ params: [(String, String)]) -> String {
        params.map { encodeQueryComponent($0.0) + "=" + encodeQueryComponent($0.1) }.joined(separator: "&")
    }

    /// `Uri.decodeComponent`, leaving malformed escapes as they are.
    public static func decodePercent(_ text: String) -> String {
        guard text.contains("%") else { return text }
        let bytes = Array(text.utf8)
        var out = [UInt8]()
        var i = 0
        while i < bytes.count {
            if bytes[i] == 0x25, i + 2 < bytes.count, let hi = hexValue(bytes[i + 1]), let lo = hexValue(bytes[i + 2]) {
                out.append(hi << 4 | lo)
                i += 3
            } else {
                out.append(bytes[i])
                i += 1
            }
        }
        return String(decoding: out, as: UTF8.self)
    }

    private static func hexValue(_ c: UInt8) -> UInt8? {
        switch c {
        case 0x30...0x39: return c - 0x30
        case 0x41...0x46: return c - 0x41 + 10
        case 0x61...0x66: return c - 0x61 + 10
        default: return nil
        }
    }

    private static func isUnreserved(_ c: UInt8) -> Bool {
        (c >= 0x61 && c <= 0x7a) || (c >= 0x41 && c <= 0x5a) || (c >= 0x30 && c <= 0x39)
    }

    private static func encode(_ value: String, _ extra: Set<UInt8>, spaceToPlus: Bool, keepEscapes: Bool) -> String {
        let bytes = Array(value.utf8)
        var out = [UInt8]()
        out.reserveCapacity(bytes.count + 8)
        var i = 0
        while i < bytes.count {
            let c = bytes[i]
            if isUnreserved(c) || extra.contains(c) {
                out.append(c)
            } else if c == 0x20 && spaceToPlus {
                out.append(0x2b)
            } else if keepEscapes, c == 0x25, i + 2 < bytes.count, hexValue(bytes[i + 1]) != nil, hexValue(bytes[i + 2]) != nil {
                out.append(0x25)
                out.append(upper(bytes[i + 1]))
                out.append(upper(bytes[i + 2]))
                i += 2
            } else {
                out.append(0x25)
                out.append(hex[Int(c >> 4)])
                out.append(hex[Int(c & 0xf)])
            }
            i += 1
        }
        return String(decoding: out, as: UTF8.self)
    }

    private static func upper(_ c: UInt8) -> UInt8 { c >= 0x61 && c <= 0x7a ? c - 0x20 : c }
}
