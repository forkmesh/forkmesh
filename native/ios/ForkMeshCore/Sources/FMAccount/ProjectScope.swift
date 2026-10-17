// Project namespaces, plus the scoped `SecureStore` view (`SecureStore` is a protocol in FMBase,
// so the view is a wrapper type here).

import FMBase
import Foundation

public struct ProjectScope: Hashable, Sendable, CustomStringConvertible {
    /// The project this scope belongs to: its origin (`host[:port]`). Empty only for `legacy` as
    /// used by harnesses that build services without a project at all.
    public let projectId: String

    /// Key prefix: empty for the legacy namespace, else `p/<projectId>/`.
    public let namespace: String

    public init(projectId: String, namespace: String) {
        self.projectId = projectId
        self.namespace = namespace
    }

    /// The pre-projects storage layout, unpinned.
    public static let legacy = ProjectScope(projectId: "", namespace: "")

    /// The prefix every non-legacy namespace starts with.
    public static let namespaceRoot = "p/"

    /// Namespace given to a project created after the upgrade.
    public static func namespaceFor(_ projectId: String) -> String { "\(namespaceRoot)\(projectId)/" }

    public var isLegacy: Bool { namespace.isEmpty }

    /// A preference (or secure-store) key in this project's namespace.
    public func key(_ name: String) -> String { namespace + name }

    /// This project's view of `base`: every key it reads or writes lives under `namespace`.
    public func secure(_ base: SecureStore) -> SecureStore {
        isLegacy ? base : ScopedSecureStore(base: base, prefix: namespace)
    }

    /// This project's view of a preference store: every key lives under `namespace`.
    public func prefs(_ base: KeyValueStore) -> KeyValueStore {
        isLegacy ? base : ScopedKeyValueStore(base: base, prefix: namespace)
    }

    /// A filesystem-safe form of the project id (`host:port` and IPv6 literals carry characters
    /// some filesystems refuse): lower-cased, anything outside `[a-z0-9._-]` becomes `_`.
    public var slug: String { ProjectScope.slugFor(projectId) }

    public static func slugFor(_ projectId: String) -> String {
        let allowed: (Character) -> Bool = { c in
            guard c.isASCII else { return false }
            return c.isLetter || c.isNumber || c == "." || c == "_" || c == "-"
        }
        return String(projectId.lowercased().map { allowed($0) ? $0 : "_" })
    }

    /// `base` for the legacy namespace, `<base>/projects/<slug>` otherwise.
    public func directory(_ base: URL) -> URL {
        if isLegacy { return base }
        return base.appendingPathComponent("projects", isDirectory: true)
            .appendingPathComponent(slug, isDirectory: true)
    }

    public var description: String {
        "ProjectScope(\(projectId.isEmpty ? "-" : projectId), \(isLegacy ? "legacy" : namespace))"
    }
}

/// `SecureStore.scoped(prefix)`: a view of `base` whose keys all live under `prefix`, so one
/// project's identity, vault and secrets can never be read (or overwritten) through another
/// project's services. Views share the base, so a Keychain failure that moved it to memory moves
/// them all.
public final class ScopedSecureStore: SecureStore, @unchecked Sendable {
    public let base: SecureStore
    public let prefix: String

    public init(base: SecureStore, prefix: String) {
        self.base = base
        self.prefix = prefix
    }

    public func read(_ key: String) -> String? { base.read(prefix + key) }

    public func write(_ key: String, _ value: String) { base.write(prefix + key, value) }

    public func delete(_ key: String) { base.delete(prefix + key) }

    /// Only this view's entries, with the prefix taken off.
    public func readAll() -> [String: String] {
        var own: [String: String] = [:]
        for (key, value) in base.readAll() where key.hasPrefix(prefix) {
            own[String(key.dropFirst(prefix.count))] = value
        }
        return own
    }

    /// Delete every entry under `sub` (relative to this view). Refuses the empty prefix of an
    /// empty view, which would be everything.
    public func deleteNamespace(_ sub: String) {
        ProjectStorage.deleteSecureNamespace(prefix + sub, in: base)
    }
}

/// The preference half of a project view: every key lives under `prefix`.
public final class ScopedKeyValueStore: KeyValueStore, @unchecked Sendable {
    public let base: KeyValueStore
    public let prefix: String

    public init(base: KeyValueStore, prefix: String) {
        self.base = base
        self.prefix = prefix
    }

    private func name(_ key: String) -> String { prefix + key }

    public func string(forKey key: String) -> String? { base.string(forKey: name(key)) }
    public func bool(forKey key: String) -> Bool? { base.bool(forKey: name(key)) }
    public func int(forKey key: String) -> Int? { base.int(forKey: name(key)) }
    public func double(forKey key: String) -> Double? { base.double(forKey: name(key)) }
    public func stringList(forKey key: String) -> [String]? { base.stringList(forKey: name(key)) }

    public func set(_ value: String, forKey key: String) { base.set(value, forKey: name(key)) }
    public func set(_ value: Bool, forKey key: String) { base.set(value, forKey: name(key)) }
    public func set(_ value: Int, forKey key: String) { base.set(value, forKey: name(key)) }
    public func set(_ value: Double, forKey key: String) { base.set(value, forKey: name(key)) }
    public func set(_ value: [String], forKey key: String) { base.set(value, forKey: name(key)) }

    public func remove(_ key: String) { base.remove(name(key)) }
    public func contains(_ key: String) -> Bool { base.contains(name(key)) }

    public var keys: Set<String> {
        Set(base.keys.filter { $0.hasPrefix(prefix) }.map { String($0.dropFirst(prefix.count)) })
    }

    /// Clears this view only.
    public func clear() {
        for key in base.keys where key.hasPrefix(prefix) { base.remove(key) }
    }
}
