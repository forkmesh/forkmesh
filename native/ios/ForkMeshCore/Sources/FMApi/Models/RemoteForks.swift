import FMAccount
import FMBase
import Foundation

public struct RemoteFork: Equatable, Hashable, Sendable, Identifiable {
    public var url: String
    public var origin: String
    public var host: String
    public var owner: String
    public var name: String
    public var label: String

    public var id: String { url }

    public init(url: String, origin: String, host: String, owner: String, name: String, label: String = "") {
        self.url = url
        self.origin = origin
        self.host = host
        self.owner = owner
        self.name = name
        self.label = label
    }

    public init?(json j: JSONObject) {
        let origin = j.str("origin").lowercased()
        let owner = j.str("owner")
        let name = j.str("name")
        guard RemoteFork.validOrigin(origin), RemoteFork.validName(owner), RemoteFork.validName(name) else { return nil }
        let host = j.str("host").isEmpty ? String(origin.dropFirst("https://".count)) : j.str("host")
        self.init(
            url: j.str("url").isEmpty ? "\(origin)/\(owner)/\(name)" : j.str("url"), origin: origin, host: host,
            owner: owner, name: name, label: j.str("label"))
    }

    public var displayName: String { "\(owner)/\(name) on \(host)" }

    public var pullsURL: URL? {
        URL(string: "\(origin)/api/repo/\(DartURI.encodeComponent(owner))/\(DartURI.encodeComponent(name))/pulls")
    }

    public static func validOrigin(_ origin: String) -> Bool {
        origin.range(of: #"^https://[a-z0-9.-]+(?::\d{1,5})?$"#, options: .regularExpression) != nil
    }

    static func validName(_ value: String) -> Bool {
        value.range(of: #"^[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}$"#, options: .regularExpression) != nil
    }

    public static func list(_ data: JSONValue?) -> [RemoteFork] {
        (data?.objectValue?["forks"]?.arrayValue ?? []).compactMap(\.objectValue).compactMap(RemoteFork.init(json:))
    }

    public static func signedHead(host: String, owner: String, name: String, branch: String) -> String {
        "\(host)/\(owner)/\(name):\(branch)"
    }
}
