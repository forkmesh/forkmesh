import FMBase
import FMCrypto
import Foundation

public enum MastodonServer {
    public static func normalize(_ input: String) -> String? {
        var text = input.trimmingCharacters(in: .whitespacesAndNewlines)
        while text.hasPrefix("@") { text.removeFirst() }
        if !text.contains("://"), let at = text.lastIndex(of: "@") {
            text = String(text[text.index(after: at)...])
        }
        guard !text.isEmpty, !text.contains(where: { $0.isWhitespace }) else { return nil }
        if !text.contains("://") { text = "https://" + text }
        guard let components = URLComponents(string: text),
              components.scheme?.lowercased() == "https",
              let host = components.host?.lowercased(), host.contains("."), !host.hasPrefix("."), !host.hasSuffix("."),
              components.user == nil, components.password == nil,
              components.path.isEmpty || components.path == "/",
              components.query == nil, components.fragment == nil
        else { return nil }
        if let port = components.port { return "https://\(host):\(port)" }
        return "https://" + host
    }

    public static func webLink(_ value: String) -> String {
        let text = value.trimmingCharacters(in: .whitespacesAndNewlines)
        let lowered = text.lowercased()
        let start: Int
        if lowered.hasPrefix("https://") {
            start = "https://".count
        } else if lowered.hasPrefix("http://") {
            start = "http://".count
        } else {
            return ""
        }
        guard let first = text.dropFirst(start).first, first != "/",
              !text.unicodeScalars.contains(where: { CharacterSet.whitespacesAndNewlines.contains($0) || CharacterSet.controlCharacters.contains($0) })
        else { return "" }
        return text
    }

    public static func host(_ server: String) -> String {
        URLComponents(string: server)?.host ?? server
    }
}

public enum MastodonForm {
    private static let allowed = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~")

    public static func escape(_ value: String) -> String {
        value.addingPercentEncoding(withAllowedCharacters: allowed) ?? value
    }

    public static func encode(_ fields: [(String, String)]) -> String {
        fields.map { escape($0.0) + "=" + escape($0.1) }.joined(separator: "&")
    }
}

public struct MastodonPKCE: Equatable, Sendable {
    public let verifier: String
    public let challenge: String
    public let state: String

    public init(verifier: String, state: String) {
        self.verifier = verifier
        self.state = state
        challenge = MastodonPKCE.challenge(for: verifier)
    }

    public static func make() -> MastodonPKCE {
        MastodonPKCE(verifier: B64.encodeUrlNoPadding(CryptoBytes.random(32)), state: Hex.encode(CryptoBytes.random(16)))
    }

    public static func challenge(for verifier: String) -> String {
        B64.encodeUrlNoPadding(Sha256.hash(Array(verifier.utf8)))
    }
}

public enum MastodonOAuth {
    public static let redirect = "forkmesh://mastodon/callback"
    public static let callbackScheme = "forkmesh"
    public static let outOfBand = "urn:ietf:wg:oauth:2.0:oob"
    public static let scopes = "read write follow"
    public static let clientName = "ForkMesh"
    public static let website = "https://forkmesh.com"

    public static func authorizeURL(server: String, clientId: String, redirect: String, state: String, challenge: String) -> URL? {
        let query = MastodonForm.encode([
            ("response_type", "code"), ("client_id", clientId), ("redirect_uri", redirect), ("scope", scopes),
            ("state", state), ("code_challenge", challenge), ("code_challenge_method", "S256"),
        ])
        return URL(string: server + "/oauth/authorize?" + query)
    }

    public static func isCallback(_ url: URL) -> Bool {
        url.scheme?.lowercased() == callbackScheme && url.host?.lowercased() == "mastodon" && url.path == "/callback"
    }

    public static func code(from input: String, state expected: String) -> String? {
        let text = input.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.contains("?"), let components = URLComponents(string: text), let items = components.queryItems,
           let code = items.first(where: { $0.name == "code" })?.value {
            if items.contains(where: { $0.name == "error" }) { return nil }
            if let state = items.first(where: { $0.name == "state" })?.value, state != expected { return nil }
            return validCode(code) ? code : nil
        }
        return validCode(text) ? text : nil
    }

    private static func validCode(_ value: String) -> Bool {
        (4...256).contains(value.count) && value.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "_" || $0 == "-") }
    }

    public static func callbackError(_ input: String) -> String? {
        guard let items = URLComponents(string: input)?.queryItems,
              let error = items.first(where: { $0.name == "error" })?.value else { return nil }
        return items.first(where: { $0.name == "error_description" })?.value ?? error
    }
}

public enum MastodonText {
    public static let urlWeight = 23

    public static func length(_ text: String) -> Int {
        var out = text
        out = replace(out, pattern: "https?://\\S+", with: String(repeating: "x", count: urlWeight))
        out = replace(out, pattern: "@([A-Za-z0-9_]+)@[A-Za-z0-9.-]+", with: "@$1")
        return out.unicodeScalars.count
    }

    public static func remaining(limit: Int, text: String, spoiler: String, warning: Bool) -> Int {
        limit - length(text) - (warning ? length(spoiler) : 0)
    }

    private static func replace(_ text: String, pattern: String, with template: String) -> String {
        guard let regex = try? NSRegularExpression(pattern: pattern) else { return text }
        let range = NSRange(text.startIndex..., in: text)
        return regex.stringByReplacingMatches(in: text, range: range, withTemplate: template)
    }
}

public enum MastodonSegment: Equatable, Sendable {
    case text(String)
    case link(label: String, url: String)
    case mention(label: String, account: String)
    case hashtag(label: String, name: String)

    public var plain: String {
        switch self {
        case .text(let value): return value
        case .link(let label, _), .mention(let label, _), .hashtag(let label, _): return label
        }
    }
}

public enum MastodonHTML {
    private struct Anchor {
        var href: String
        var classes: String
        var label = ""
    }

    public static func text(_ html: String) -> String {
        segments(html).map(\.plain).joined()
    }

    public static func segments(_ html: String, mentions: [MastodonMention] = [], tags: [MastodonTag] = []) -> [MastodonSegment] {
        var out: [MastodonSegment] = []
        var buffer = ""
        var anchor: Anchor?
        var spans: [String] = []
        var hidden = 0
        var started = false

        func emit(_ value: String) {
            guard hidden == 0, !value.isEmpty else { return }
            started = true
            if anchor != nil { anchor?.label += value } else { buffer += value }
        }
        func flush() {
            if !buffer.isEmpty {
                if case .text(let previous)? = out.last {
                    out[out.count - 1] = .text(previous + buffer)
                } else {
                    out.append(.text(buffer))
                }
            }
            buffer = ""
        }

        var index = html.startIndex
        while index < html.endIndex {
            let char = html[index]
            if char == "<", let close = html[index...].firstIndex(of: ">") {
                let tag = String(html[html.index(after: index)..<close])
                index = html.index(after: close)
                let closing = tag.hasPrefix("/")
                let body = closing ? String(tag.dropFirst()) : tag
                let name = String(body.prefix { $0.isLetter || $0.isNumber }).lowercased()
                switch (name, closing) {
                case ("br", false):
                    emit("\n")
                case ("p", false):
                    if started { emit("\n\n") }
                case ("a", false):
                    flush()
                    anchor = Anchor(href: decode(attribute("href", in: body)), classes: attribute("class", in: body))
                case ("a", true):
                    if let current = anchor {
                        anchor = nil
                        let segment = classify(current, mentions: mentions, tags: tags)
                        if case .text(let value) = segment { buffer += value } else { out.append(segment) }
                    }
                case ("span", false):
                    let classes = attribute("class", in: body).split(separator: " ").map(String.init)
                    let kind = classes.contains("invisible") ? "invisible" : classes.contains("ellipsis") ? "ellipsis" : ""
                    spans.append(kind)
                    if kind == "invisible" { hidden += 1 }
                case ("span", true):
                    let kind = spans.popLast() ?? ""
                    if kind == "invisible" { hidden = max(0, hidden - 1) }
                    if kind == "ellipsis" { emit("…") }
                default:
                    break
                }
                continue
            }
            let next = html[index...].firstIndex(of: "<") ?? html.endIndex
            let end = next == index ? html.index(after: index) : next
            emit(decode(String(html[index..<end])))
            index = end
        }
        if let current = anchor {
            let segment = classify(current, mentions: mentions, tags: tags)
            if case .text(let value) = segment { buffer += value } else { out.append(segment) }
        }
        flush()
        return trimmed(out)
    }

    private static func trimmed(_ segments: [MastodonSegment]) -> [MastodonSegment] {
        var out = segments
        if case .text(let first)? = out.first {
            let value = String(first.drop { $0.isWhitespace })
            if value.isEmpty { out.removeFirst() } else { out[0] = .text(value) }
        }
        if case .text(let last)? = out.last {
            var value = last
            while let tail = value.last, tail.isWhitespace { value.removeLast() }
            if value.isEmpty { out.removeLast() } else { out[out.count - 1] = .text(value) }
        }
        return out
    }

    private static func classify(_ anchor: Anchor, mentions: [MastodonMention], tags: [MastodonTag]) -> MastodonSegment {
        let label = anchor.label
        if let mention = mentions.first(where: { $0.url == anchor.href }) {
            return .mention(label: label, account: mention.id)
        }
        let classes = anchor.classes.split(separator: " ").map(String.init)
        if let name = tagName(anchor.href, label: label, hashtag: classes.contains("hashtag")) {
            return .hashtag(label: label, name: name)
        }
        if let tag = tags.first(where: { $0.url == anchor.href }) {
            return .hashtag(label: label, name: tag.name.lowercased())
        }
        let lower = anchor.href.lowercased()
        if lower.hasPrefix("https://") || lower.hasPrefix("http://") {
            return .link(label: label, url: anchor.href)
        }
        return .text(label)
    }

    private static func tagName(_ href: String, label: String, hashtag: Bool) -> String? {
        if let components = URLComponents(string: href) {
            let parts = components.path.split(separator: "/").map(String.init)
            if parts.count == 2, parts[0] == "tags", !parts[1].isEmpty {
                return (parts[1].removingPercentEncoding ?? parts[1]).lowercased()
            }
        }
        guard hashtag else { return nil }
        let name = label.hasPrefix("#") ? String(label.dropFirst()) : label
        return name.isEmpty ? nil : name.lowercased()
    }

    static func attribute(_ name: String, in tag: String) -> String {
        guard let regex = try? NSRegularExpression(
            pattern: "(?:^|\\s)" + name + "\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))", options: [.caseInsensitive]),
            let match = regex.firstMatch(in: tag, range: NSRange(tag.startIndex..., in: tag))
        else { return "" }
        for group in 1...3 {
            if let range = Range(match.range(at: group), in: tag) { return String(tag[range]) }
        }
        return ""
    }

    public static func decode(_ text: String) -> String {
        guard text.contains("&") else { return text }
        var out = ""
        var index = text.startIndex
        while index < text.endIndex {
            if text[index] == "&", let semi = text[index...].prefix(12).firstIndex(of: ";") {
                let entity = String(text[text.index(after: index)..<semi])
                if let value = entityValue(entity) {
                    out += value
                    index = text.index(after: semi)
                    continue
                }
            }
            out.append(text[index])
            index = text.index(after: index)
        }
        return out
    }

    private static func entityValue(_ entity: String) -> String? {
        switch entity {
        case "amp": return "&"
        case "lt": return "<"
        case "gt": return ">"
        case "quot": return "\""
        case "apos": return "'"
        case "nbsp": return "\u{00A0}"
        default: break
        }
        guard entity.hasPrefix("#") else { return nil }
        let digits = entity.dropFirst()
        let code: UInt32?
        if digits.hasPrefix("x") || digits.hasPrefix("X") {
            code = UInt32(digits.dropFirst(), radix: 16)
        } else {
            code = UInt32(digits, radix: 10)
        }
        guard let code, let scalar = Unicode.Scalar(code) else { return nil }
        return String(Character(scalar))
    }
}

public enum MastodonPaging {
    public static func next(_ link: String?, server: String) -> String? {
        guard let link else { return nil }
        for part in link.split(separator: ",") {
            let pieces = part.split(separator: ";").map { $0.trimmingCharacters(in: .whitespaces) }
            guard let first = pieces.first, first.hasPrefix("<"), first.hasSuffix(">") else { continue }
            let isNext = pieces.dropFirst().contains { $0.replacingOccurrences(of: " ", with: "").lowercased() == "rel=\"next\"" || $0.lowercased() == "rel=next" }
            guard isNext else { continue }
            let url = String(first.dropFirst().dropLast())
            return url.hasPrefix(server + "/") ? url : nil
        }
        return nil
    }
}

public enum MastodonDate {
    public static func millis(_ text: String) -> Int64 {
        guard !text.isEmpty else { return 0 }
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        if let date = formatter.date(from: text) { return Int64(date.timeIntervalSince1970 * 1000) }
        formatter.formatOptions = [.withInternetDateTime]
        if let date = formatter.date(from: text) { return Int64(date.timeIntervalSince1970 * 1000) }
        return 0
    }
}
