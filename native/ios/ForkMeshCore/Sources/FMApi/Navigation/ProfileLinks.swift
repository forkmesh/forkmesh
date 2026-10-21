import Foundation

public enum ProfileLinks {
    public static let prefix = "forkmesh://profile/@"
    static let reserved: Set<String> = ["forkbot", "here", "channel", "everyone"]
    private static let accountPattern = try! NSRegularExpression(pattern: "^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")

    public static func account(_ raw: String) -> String? {
        var name = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if name.hasPrefix("@") { name.removeFirst() }
        name = name.lowercased()
        guard !reserved.contains(name),
            accountPattern.firstMatch(in: name, range: NSRange(name.startIndex..., in: name)) != nil
        else { return nil }
        return name
    }

    public static func link(_ raw: String) -> String? {
        account(raw).map { prefix + $0 }
    }

    public static func url(_ raw: String) -> URL? {
        link(raw).flatMap { URL(string: $0) }
    }
}
