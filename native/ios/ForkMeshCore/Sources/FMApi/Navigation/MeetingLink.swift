import Foundation

public enum MeetingLink {
    public static let roomLength = 32
    public static let keyLength = 64
    public static let maxSaveNameLength = 120
    public static let maxSaveBytes = 2_000_000
    public static let defaultSaveName = "meeting-transcript.txt"

    public static func route(from url: URL) -> Route? {
        guard let components = URLComponents(url: url, resolvingAgainstBaseURL: false),
            components.scheme?.lowercased() == "https", let host = components.host, !host.isEmpty,
            isCallPath(components.percentEncodedPath)
        else { return nil }
        return route(query: components.percentEncodedQuery, fragment: components.percentEncodedFragment)
    }

    public static func route(query: String?, fragment: String?) -> Route? {
        guard let room = hex(query, length: roomLength), let key = hex(fragment, length: keyLength) else { return nil }
        return .meeting(room: room, key: key)
    }

    public static func isCallPath(_ path: String) -> Bool {
        let lowered = path.lowercased()
        return lowered == "/call" || lowered == "/call/"
    }

    public static func origin(of url: URL) -> URL? {
        guard url.scheme?.lowercased() == "https", let host = url.host, !host.isEmpty else { return nil }
        var components = URLComponents()
        components.scheme = "https"
        components.host = host.lowercased()
        components.port = url.port == 443 ? nil : url.port
        return components.url
    }

    public static func pageURL(origin: URL, room: String, key: String) -> URL {
        var components = URLComponents()
        let scheme = origin.scheme?.lowercased() ?? "https"
        components.scheme = scheme
        components.host = origin.host?.lowercased()
        components.port = origin.port == effectivePort(nil, scheme: scheme) ? nil : origin.port
        components.path = "/call"
        components.percentEncodedQuery = room.lowercased()
        components.percentEncodedFragment = key.lowercased()
        return components.url ?? origin
    }

    public static func isMeetingPage(_ url: URL, origin: URL) -> Bool {
        guard sameOrigin(scheme: url.scheme, host: url.host, port: url.port, origin: origin) else { return false }
        let path = URLComponents(url: url, resolvingAgainstBaseURL: false)?.percentEncodedPath ?? url.path
        return isCallPath(path)
    }

    public static func sameOrigin(scheme: String?, host: String?, port: Int?, origin: URL) -> Bool {
        guard let scheme = scheme?.lowercased(), scheme == origin.scheme?.lowercased(),
            let host = host?.lowercased(), !host.isEmpty, host == origin.host?.lowercased()
        else { return false }
        return effectivePort(port, scheme: scheme) == effectivePort(origin.port, scheme: scheme)
    }

    public static func saveName(_ raw: String?) -> String {
        let blocked = CharacterSet(charactersIn: "/\\:").union(.controlCharacters).union(.newlines)
        let cleaned = String(
            String.UnicodeScalarView(
                (raw ?? "").unicodeScalars.map { blocked.contains($0) ? "-" : $0 }))
            .trimmingCharacters(in: .whitespaces)
        var name = Substring(cleaned)
        while name.first == "." { name = name.dropFirst() }
        let capped = String(name.prefix(maxSaveNameLength)).trimmingCharacters(in: .whitespaces)
        return capped.isEmpty ? defaultSaveName : capped
    }

    public static func saveText(_ raw: String) -> String {
        guard raw.utf8.count > maxSaveBytes else { return raw }
        var out = ""
        var used = 0
        for character in raw {
            let size = character.utf8.count
            if used + size > maxSaveBytes { break }
            out.append(character)
            used += size
        }
        return out
    }

    private static func effectivePort(_ port: Int?, scheme: String) -> Int? {
        if let port { return port }
        switch scheme {
        case "https": return 443
        case "http": return 80
        default: return nil
        }
    }

    private static func hex(_ value: String?, length: Int) -> String? {
        guard let value, value.count == length, value.allSatisfy({ $0.isASCII && $0.isHexDigit }) else { return nil }
        return value.lowercased()
    }
}
