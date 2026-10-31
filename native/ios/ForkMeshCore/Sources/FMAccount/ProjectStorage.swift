import FMBase
import Foundation

// MARK: - Which keys belong to a project

public enum ProjectKeyPolicy {
    /// Secure-store key prefixes kept in the shared store.
    public static let sharedSecurePrefixes: [String] = ["auth/", "agent/credentials"]

    /// Preference keys each project keeps for itself.
    public static let projectPreferenceKeys: Set<String> = [
        "profile/name",
        "server/room",
        "profile/solana",
        "node/serve",
        "nodeSync/applied",
        "node/catalogNodeId",
        "agent/branch-records-v1",
        "localControl/pairedDesktops",
        "inbox/local-pings.v1",
    ]

    /// Secrets an older build left in plaintext preferences. They predate projects, so they belong
    /// to the legacy namespace; a project view adopting them would move the ForkMesh project's
    /// secrets into another project, so a non-legacy project neither adopts nor deletes them.
    public static let legacyPlaintextSecrets: Set<String> = [
        "identity/ed25519Seed",
        "server/passphrase",
        "server/roomSeal",
        "import/githubToken",
        "import/gitlabToken",
    ]

    /// True for a key that already names a project namespace (`p/<id>/...`).
    public static func isNamespaced(_ key: String) -> Bool { key.hasPrefix(ProjectScope.namespaceRoot) }

    public static func isSharedSecureKey(_ key: String) -> Bool {
        sharedSecurePrefixes.contains { key.hasPrefix($0) }
    }

    /// Where `key` lives in the secure store while `scope` is active.
    public static func secureKey(_ key: String, in scope: ProjectScope) -> String {
        if isNamespaced(key) || isSharedSecureKey(key) { return key }
        return scope.key(key)
    }

    /// Where `key` lives in the preferences while `scope` is active.
    public static func preferenceKey(_ key: String, in scope: ProjectScope) -> String {
        if isNamespaced(key) || !projectPreferenceKeys.contains(key) { return key }
        return scope.key(key)
    }
}

// MARK: - The active project

/// Which project the app's stores address right now. One per app, shared by its secure store and
/// its preference store so the two can never point at different projects. A project switch flips
/// it (`activate`) between taking the old project's sockets down and bringing the new one's up.
public final class ProjectScopeRouter: @unchecked Sendable {
    private let lock = NSLock()
    private var current: ProjectScope
    /// Told after every change of scope.
    public let listeners = Listeners()

    public init(scope: ProjectScope = .legacy) {
        current = scope
    }

    public var scope: ProjectScope {
        lock.lock()
        defer { lock.unlock() }
        return current
    }

    /// Address `scope` from now on. Returns whether anything changed.
    @discardableResult
    public func activate(_ scope: ProjectScope) -> Bool {
        lock.lock()
        let changed = current != scope
        current = scope
        lock.unlock()
        if changed { listeners.notify() }
        return changed
    }
}

/// The app's `SecureStore`: the shared store, with every per-project key sent to the active
/// project's namespace (`ProjectKeyPolicy.secureKey`).
public final class ProjectRoutedSecureStore: SecureStore, @unchecked Sendable {
    /// The store underneath, addressed by full keys. Only `ProjectStorage` should write it directly.
    public let base: SecureStore
    public let router: ProjectScopeRouter

    public init(base: SecureStore, router: ProjectScopeRouter) {
        self.base = base
        self.router = router
    }

    public var scope: ProjectScope { router.scope }

    public func read(_ key: String) -> String? { base.read(ProjectKeyPolicy.secureKey(key, in: scope)) }

    public func write(_ key: String, _ value: String) { base.write(ProjectKeyPolicy.secureKey(key, in: scope), value) }

    public func delete(_ key: String) { base.delete(ProjectKeyPolicy.secureKey(key, in: scope)) }

    /// What this project can see, keyed so `read` finds each entry again: the shared entries, and
    /// its own entries without their prefix. Another project's entries are never included.
    public func readAll() -> [String: String] {
        let scope = self.scope
        var visible: [String: String] = [:]
        for (key, value) in base.readAll() {
            if ProjectKeyPolicy.isSharedSecureKey(key) {
                visible[key] = value
            } else if scope.isLegacy {
                if !ProjectKeyPolicy.isNamespaced(key) { visible[key] = value }
            } else if key.hasPrefix(scope.namespace) {
                visible[String(key.dropFirst(scope.namespace.count))] = value
            }
        }
        return visible
    }
}

/// The app's `KeyValueStore`: the shared preferences, with the per-project keys
/// (`ProjectKeyPolicy.projectPreferenceKeys`) sent to the active project's namespace.
public final class ProjectRoutedKeyValueStore: KeyValueStore, @unchecked Sendable {
    public let base: KeyValueStore
    public let router: ProjectScopeRouter

    public init(base: KeyValueStore, router: ProjectScopeRouter) {
        self.base = base
        self.router = router
    }

    public var scope: ProjectScope { router.scope }

    private func name(_ key: String) -> String { ProjectKeyPolicy.preferenceKey(key, in: scope) }

    /// A legacy plaintext secret, which a non-legacy project must neither adopt nor delete.
    private func hidden(_ key: String) -> Bool {
        !scope.isLegacy && ProjectKeyPolicy.legacyPlaintextSecrets.contains(key)
    }

    public func string(forKey key: String) -> String? { hidden(key) ? nil : base.string(forKey: name(key)) }
    public func bool(forKey key: String) -> Bool? { base.bool(forKey: name(key)) }
    public func int(forKey key: String) -> Int? { base.int(forKey: name(key)) }
    public func double(forKey key: String) -> Double? { base.double(forKey: name(key)) }
    public func stringList(forKey key: String) -> [String]? { base.stringList(forKey: name(key)) }

    public func set(_ value: String, forKey key: String) { base.set(value, forKey: name(key)) }
    public func set(_ value: Bool, forKey key: String) { base.set(value, forKey: name(key)) }
    public func set(_ value: Int, forKey key: String) { base.set(value, forKey: name(key)) }
    public func set(_ value: Double, forKey key: String) { base.set(value, forKey: name(key)) }
    public func set(_ value: [String], forKey key: String) { base.set(value, forKey: name(key)) }

    public func remove(_ key: String) {
        if hidden(key) { return }
        base.remove(name(key))
    }

    public func contains(_ key: String) -> Bool { hidden(key) ? false : base.contains(name(key)) }

    /// The shared keys, plus this project's own per-project keys under their plain names.
    public var keys: Set<String> {
        let scope = self.scope
        var visible = Set<String>()
        for key in base.keys {
            if ProjectKeyPolicy.isNamespaced(key) {
                if !scope.isLegacy, key.hasPrefix(scope.namespace) {
                    let plain = String(key.dropFirst(scope.namespace.count))
                    if ProjectKeyPolicy.projectPreferenceKeys.contains(plain) { visible.insert(plain) }
                }
                continue
            }
            if !scope.isLegacy && ProjectKeyPolicy.projectPreferenceKeys.contains(key) { continue }
            if hidden(key) { continue }
            visible.insert(key)
        }
        return visible
    }

    /// Clears what this store can see; other projects' keys stay.
    public func clear() {
        for key in keys { remove(key) }
    }
}

// MARK: - Switching and erasing

/// What the relay switch controller calls to move storage and the per-project services from one
/// project to another. The app registers one (`Services.put(runtime, as: ProjectActivator.self)`).
@MainActor
public protocol ProjectActivator: AnyObject {
    /// Point storage and the per-project services at `project`. Called after the directory flipped
    /// and before the session is adopted, while the old project's sockets are down. Must not fail
    /// half way: storage that points at one project while the directory names another is exactly
    /// the leak projects exist to prevent, so a step that cannot complete is logged and skipped.
    func activate(_ project: RelayProfile)

    /// Erase what `project` stored on this phone. Called after it left the list.
    func erase(_ project: RelayProfile)
}

/// Erases what a removed project stored on this phone.
///
/// Only ever a project's *own* namespace: a project in the legacy namespace shares it with
/// production (it predates projects), so its data is not its alone to delete and is left in place.
public enum ProjectStorage {
    /// The scope of the active project as persisted by `RelayDirectory`, read straight from the
    /// preferences so the app's stores can address it before any service exists. Anything
    /// unreadable is the legacy namespace, which is what a fresh or pre-projects install uses.
    public static func activeScope(prefs: KeyValueStore) -> ProjectScope {
        let activeId = prefs.string(forKey: RelayDirectory.activeKey) ?? ""
        let raw = prefs.string(forKey: RelayDirectory.listKey) ?? ""
        if activeId.isEmpty || raw.isEmpty { return .legacy }
        guard let decoded = try? JSON.parse(raw), let items = decoded.arrayValue else { return .legacy }
        for item in items {
            guard let object = item.objectValue, object.str("id") == activeId else { continue }
            return RelayProfile.fromJSON(object).scope
        }
        return .legacy
    }

    /// Erase `project`'s secure entries, per-project preferences and folders under `roots`.
    /// Routed stores are unwrapped to their base, so this always addresses full keys.
    public static func erase(
        _ project: RelayProfile,
        secureStore: SecureStore,
        prefs: KeyValueStore,
        roots: [URL] = [],
        fileManager: FileManager = .default
    ) {
        let scope = project.scope
        if scope.isLegacy { return }
        let secureBase = (secureStore as? ProjectRoutedSecureStore)?.base ?? secureStore
        deleteSecureNamespace(scope.namespace, in: secureBase)
        let prefsBase = (prefs as? ProjectRoutedKeyValueStore)?.base ?? prefs
        for key in prefsBase.keys where key.hasPrefix(scope.namespace) {
            prefsBase.remove(key)
        }
        for root in roots {
            eraseDirectory(scope, under: root, fileManager: fileManager)
        }
    }

    /// Delete every secure entry whose key starts with `prefix`. Refuses anything that is not a
    /// project namespace: an empty prefix would be everything, and the shared entries are not one
    /// project's to delete.
    public static func deleteSecureNamespace(_ prefix: String, in store: SecureStore) {
        guard prefix.count > ProjectScope.namespaceRoot.count, ProjectKeyPolicy.isNamespaced(prefix) else { return }
        for key in store.readAll().keys where key.hasPrefix(prefix) {
            store.delete(key)
        }
    }

    /// Remove `<root>/projects/<slug>`. Belt and braces: never recurse into anything but a project
    /// folder directly under `root/projects`.
    static func eraseDirectory(_ scope: ProjectScope, under root: URL, fileManager: FileManager) {
        if scope.isLegacy || scope.slug.isEmpty || scope.slug == "." || scope.slug == ".." { return }
        let base = root.standardizedFileURL
        let dir = scope.directory(base).standardizedFileURL
        let parent = base.appendingPathComponent("projects", isDirectory: true).standardizedFileURL
        guard dir.deletingLastPathComponent().standardizedFileURL.path == parent.path,
            dir.path != base.path
        else { return }
        guard fileManager.fileExists(atPath: dir.path) else { return }
        do {
            try fileManager.removeItem(at: dir)
        } catch {
            Log.warning("projects", "Project files erase failed: \(error)")
        }
    }
}
