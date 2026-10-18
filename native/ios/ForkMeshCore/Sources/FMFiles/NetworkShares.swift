import FMBase
import FMPackages
import Foundation

public struct SmbAddress: Equatable, Sendable {
    public var host: String
    public var port: UInt16
    public var share: String
    public var path: String
    public var user: String

    public init(host: String, port: UInt16 = 445, share: String = "", path: String = "", user: String = "") {
        self.host = host
        self.port = port
        self.share = share
        self.path = path
        self.user = user
    }

    public var display: String { port == 445 ? host : "\(host):\(port)" }

    public static func parse(_ input: String) -> (address: SmbAddress?, error: String) {
        var text = input.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.isEmpty { return (nil, "Enter the address of the computer or router that shares the files.") }
        if text.hasPrefix("\\\\") { text = "smb://" + text.dropFirst(2).replacingOccurrences(of: "\\", with: "/") }
        if !text.lowercased().hasPrefix("smb://") { text = "smb://" + text }
        guard let components = URLComponents(string: text), let rawHost = components.host, !rawHost.isEmpty else {
            return (nil, "\"\(input.trimmingCharacters(in: .whitespaces))\" is not an address. Use an IP address or a name such as router.lan.")
        }
        let host = rawHost.trimmingCharacters(in: CharacterSet(charactersIn: "[]"))
        if let port = components.port, !(1...65535).contains(port) { return (nil, "That port is not valid.") }
        let parts = components.path.split(separator: "/").map { $0.removingPercentEncoding ?? String($0) }
        var address = SmbAddress(host: host, port: UInt16(components.port ?? 445))
        address.share = parts.first ?? ""
        address.path = parts.dropFirst().joined(separator: "/")
        address.user = components.user?.removingPercentEncoding ?? ""
        if OpenWrtModel.isLiteralAddress(host), !OpenWrtModel.isLocalHost(host) { return (nil, SmbError.notLocal.message) }
        return (address, "")
    }

    public static func allLocal(_ addresses: [String]) -> Bool {
        !addresses.isEmpty && addresses.allSatisfy(OpenWrtModel.isLocalHost)
    }
}

public struct NetworkShare: Codable, Identifiable, Hashable, Sendable {
    public var id: String
    public var address: String
    public var share: String
    public var user: String
    public var guest: Bool
    public var rememberPassword: Bool

    public init(id: String = UUID().uuidString, address: String, share: String, user: String = "", guest: Bool = false, rememberPassword: Bool = false) {
        self.id = id
        self.address = address
        self.share = share
        self.user = user
        self.guest = guest
        self.rememberPassword = rememberPassword
    }

    public var parsed: SmbAddress? { SmbAddress.parse(address).address }
    public var host: String { parsed?.host ?? address }
    public var title: String { "\(share) on \(parsed?.display ?? address)" }

    public var url: String {
        let who = guest || user.isEmpty ? "" : (user.addingPercentEncoding(withAllowedCharacters: .urlUserAllowed) ?? user) + "@"
        return "smb://\(who)\(parsed?.display ?? address)/\(share)"
    }

    public func url(for path: String) -> String {
        let clean = FileBrowsing.clean(path)
        return clean.isEmpty ? url + "/" : url + "/" + clean
    }

    public var key: String { "\((parsed?.display ?? address).lowercased())/\(share.lowercased())/\(guest ? "" : user.lowercased())" }

    public static func problem(address: String, share: String, user: String, guest: Bool) -> String? {
        let parsed = SmbAddress.parse(address)
        if parsed.address == nil { return parsed.error }
        let name = share.trimmingCharacters(in: .whitespaces)
        if name.isEmpty { return "Enter the share name, or press Find shares." }
        if name.contains(where: { "/\\".contains($0) }) { return "Type only the share name, without slashes." }
        if !guest && user.trimmingCharacters(in: .whitespaces).isEmpty { return "Enter the user name, or switch on Sign in as guest." }
        return nil
    }
}

public final class NetworkShareStore: @unchecked Sendable {
    static let listKey = "files/network-shares"
    static let passwordPrefix = "files.network-share.password."
    private let prefs: KeyValueStore
    private let secure: SecureStore?
    private let lock = NSLock()

    public init(prefs: KeyValueStore, secure: SecureStore?) {
        self.prefs = prefs
        self.secure = secure
    }

    public func all() -> [NetworkShare] {
        lock.lock()
        defer { lock.unlock() }
        guard let text = prefs.string(forKey: Self.listKey), let data = text.data(using: .utf8),
              let shares = try? JSONDecoder().decode([NetworkShare].self, from: data) else { return [] }
        return shares
    }

    private func store(_ shares: [NetworkShare]) {
        guard let data = try? JSONEncoder().encode(shares), let text = String(data: data, encoding: .utf8) else { return }
        prefs.set(text, forKey: Self.listKey)
    }

    @discardableResult
    public func save(_ share: NetworkShare, password: String) -> NetworkShare {
        var shares = all()
        var saved = share
        if let existing = shares.first(where: { $0.id == share.id || $0.key == share.key }) {
            saved.id = existing.id
            shares.removeAll { $0.id == existing.id }
        }
        shares.append(saved)
        lock.lock()
        store(shares)
        lock.unlock()
        if saved.rememberPassword && !saved.guest && !password.isEmpty {
            secure?.write(Self.passwordPrefix + saved.id, password)
        } else {
            secure?.delete(Self.passwordPrefix + saved.id)
        }
        return saved
    }

    public func remove(_ id: String) {
        let shares = all().filter { $0.id != id }
        lock.lock()
        store(shares)
        lock.unlock()
        secure?.delete(Self.passwordPrefix + id)
    }

    public func password(for share: NetworkShare) -> String? {
        guard share.rememberPassword, !share.guest else { return nil }
        return secure?.read(Self.passwordPrefix + share.id)
    }
}

public struct RouterShareSuggestion: Equatable, Sendable, Identifiable {
    public var name: String
    public var host: String
    public var guests: Bool

    public init(name: String, host: String, guests: Bool) {
        self.name = name
        self.host = host
        self.guests = guests
    }

    public var id: String { host + "/" + name }
    public var title: String { "\(name) on router" }
    public var share: NetworkShare { NetworkShare(address: host, share: name, guest: guests) }
}
