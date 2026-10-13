// `url` is normalized as the Kotlin port's RemoteUrl.kt does it: lowercase
// scheme and host, the default port dropped, dot segments removed from the
// path. `value` is that URL's text, what is written into config.

import Foundation

public struct GitRemoteUrl: Hashable, Sendable, CustomStringConvertible {
    public let url: URL

    private init(_ url: URL) { self.url = url }

    public static func parse(_ value: String) throws -> GitRemoteUrl {
        let failure = GitException(.protocolUnsupported, "remote URL must be a credential-free HTTPS URL")
        guard let components = URLComponents(string: value),
              let scheme = components.scheme, scheme.lowercased() == "https",
              let host = components.host, !host.isEmpty,
              components.user == nil, components.password == nil,
              (components.fragment ?? "").isEmpty,
              (components.query ?? "").isEmpty
        else {
            throw failure
        }
        var text = "https://"
        let lowerHost = host.lowercased()
        text += lowerHost.contains(":") && !lowerHost.hasPrefix("[") ? "[\(lowerHost)]" : lowerHost
        if let port = components.port, port != 443 { text += ":\(port)" }
        text += removeDotSegments(components.percentEncodedPath)
        guard let url = URL(string: text) else { throw failure }
        return GitRemoteUrl(url)
    }

    /// The normalized URL text.
    public var value: String { url.absoluteString }

    /// `scheme://host[:port]`, never a path, query, or credential.
    public var redactedOrigin: String {
        let host = url.host ?? ""
        let shownHost = host.contains(":") && !host.hasPrefix("[") ? "[\(host)]" : host
        if let port = url.port { return "https://\(shownHost):\(port)" }
        return "https://\(shownHost)"
    }

    public var description: String { value }

    /// RFC 3986 section 5.2.4, applied to a hierarchical path.
    private static func removeDotSegments(_ path: String) -> String {
        if path.isEmpty { return "" }
        var output = [String]()
        let segments = path.engSplit(0x2f)
        for (index, segment) in segments.enumerated() {
            let last = index == segments.count - 1
            if segment == "." {
                if last { output.append("") }
            } else if segment == ".." {
                if output.count > 1 { output.removeLast() }
                if last { output.append("") }
            } else {
                output.append(segment)
            }
        }
        var joined = output.joined(separator: "/")
        if path.engHasPrefix("/") && !joined.engHasPrefix("/") { joined = "/" + joined }
        return joined
    }
}
