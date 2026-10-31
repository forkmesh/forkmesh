import FMBase
import FMCrypto
import Foundation
import Observation

@MainActor
public protocol AccountSyncSource: AnyObject {
    var key: String { get }
    var kind: String { get }
    var label: String { get }
    func snapshot() -> JSONValue?
    func restore(_ value: JSONValue) -> Int
}

public enum AccountSyncKeys {
    public static let enabled = "accountSync/enabled"
    public static let savedAt = "accountSync/savedAt"
    public static let prefix = "accountSync/"
    public static let maxValueChars = 32 * 1024

    public static let homePrefixes = ["shell/", "insights/stats/layout", "insights/stats/desktop-tiles"]

    public static let skippedPrefixes = [
        "p/", "auth/", "identity/", "import/", "ownerCrypto/", "server/", "relays/", "device/iosUdid",
        "dataFolder/", "install/", "accountSync/", "chat/e2ee/", "chat/history", "chat/viewOnce/",
        "chat/welcomeAnnounced/", "chat/workspace/", "chat/local-mesh/", "chat/people-pictures", "agent/",
        "agents/readBaselineMs", "agents/seenFinishedAt", "agents/next-task", "home/next-task/", "inbox/",
        "insights/stats/history", "insights/stats/offered", "packages/account/", "packages/project-installs",
        "packages/versions", "packages/legacy-project", "nodeSync/", "node/", "localControl/",
        "shell/bar-recent", "shell/bar-uses", "code/current-repo", "codeInsight/lastTarget", "diffViewed/",
        "account/recentAccounts", "vault/", "push/", "fresh/", "tour/",
    ]

    public static func skipped(_ key: String) -> Bool {
        skippedPrefixes.contains { key.hasPrefix($0) } || DataFolder.isDeviceKey(key)
    }

    public static func isHome(_ key: String) -> Bool {
        homePrefixes.contains { key.hasPrefix($0) }
    }

    public static func hash(account: String, source: String) -> String {
        prefix + "hash/" + account.lowercased() + "/" + source
    }
}

@MainActor
public final class PreferenceSyncSource: AccountSyncSource {
    public let key: String
    public let kind: String
    public let label: String
    private let store: KeyValueStore
    private let includes: (String) -> Bool

    public init(key: String, kind: String, label: String, store: KeyValueStore, includes: @escaping (String) -> Bool) {
        self.key = key
        self.kind = kind
        self.label = label
        self.store = store
        self.includes = includes
    }

    public static func home(_ store: KeyValueStore) -> PreferenceSyncSource {
        PreferenceSyncSource(key: "ios.home", kind: "layout", label: "Home screen and bar layout", store: store) {
            AccountSyncKeys.isHome($0) && !AccountSyncKeys.skipped($0)
        }
    }

    public static func settings(_ store: KeyValueStore) -> PreferenceSyncSource {
        PreferenceSyncSource(key: "ios.settings", kind: "settings", label: "App settings", store: store) {
            !AccountSyncKeys.isHome($0) && !AccountSyncKeys.skipped($0)
        }
    }

    public func snapshot() -> JSONValue? {
        let exported = DataFolder.exportSettings(store)
        var values: JSONObject = [:]
        for (name, value) in exported.obj("values") where includes(name) {
            if case .string(let text) = value, text.count > AccountSyncKeys.maxValueChars { continue }
            values[name] = value
        }
        guard !values.isEmpty else { return nil }
        var document = exported
        document["values"] = .object(values)
        return .object(document)
    }

    public func restore(_ value: JSONValue) -> Int {
        guard var document = value.objectValue else { return 0 }
        var values: JSONObject = [:]
        for (name, item) in document.obj("values") where includes(name) {
            values[name] = item
        }
        document["values"] = .object(values)
        return DataFolder.applySettings(document, to: store)
    }
}

@MainActor
@Observable
public final class AccountSyncService {
    public private(set) var busy = false
    public private(set) var error = ""
    public private(set) var lastSavedAt: Int64 = 0
    public private(set) var lastSaved = 0
    public private(set) var lastRestored = 0

    @ObservationIgnored private let client: AccountEndpointCalling
    @ObservationIgnored private let prefs: KeyValueStore
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored private let account: () -> String
    @ObservationIgnored private let device: String
    @ObservationIgnored public private(set) var sources: [AccountSyncSource]

    public init(client: AccountEndpointCalling, prefs: KeyValueStore, sources: [AccountSyncSource] = [], device: String = "",
                clock: Clock = SystemClock(), account: @escaping () -> String) {
        self.client = client
        self.prefs = prefs
        self.sources = sources
        self.device = device
        self.clock = clock
        self.account = account
        lastSavedAt = Int64(prefs.double(forKey: AccountSyncKeys.savedAt) ?? 0)
        enabled = prefs.bool(forKey: AccountSyncKeys.enabled) ?? false
    }

    public var enabled: Bool {
        didSet {
            guard enabled != oldValue else { return }
            prefs.set(enabled, forKey: AccountSyncKeys.enabled)
            if !enabled { forgetHashes() }
        }
    }

    public func register(_ source: AccountSyncSource) {
        sources.removeAll { $0.key == source.key }
        sources.append(source)
    }

    public static func digest(_ value: JSONValue) -> String {
        Sha256.hex(Array(JSON.encode(value).utf8))
    }

    @discardableResult
    public func saveIfChanged() async -> Int {
        guard enabled, !busy, !account().isEmpty else { return 0 }
        return await save(force: false)
    }

    @discardableResult
    public func saveNow() async -> Int {
        guard !busy, !account().isEmpty else { return 0 }
        return await save(force: true)
    }

    private func save(force: Bool) async -> Int {
        busy = true
        defer { busy = false }
        let owner = account()
        var saved = 0
        var failure = ""
        for source in sources {
            guard let value = source.snapshot() else { continue }
            let digest = AccountSyncService.digest(value)
            let hashKey = AccountSyncKeys.hash(account: owner, source: source.key)
            if !force, prefs.string(forKey: hashKey) == digest { continue }
            var body: JSONObject = ["kind": .string(source.kind), "label": .string(source.label), "value": value]
            if !device.isEmpty { body["device"] = .string(device) }
            do {
                _ = try await client.accountCall("PUT", AccountDataPaths.storeItem(source.key), body)
                prefs.set(digest, forKey: hashKey)
                saved += 1
            } catch let problem as AccountEndpointError where problem.status == 404 || problem.status == 405 {
                failure = "This server cannot save your data yet."
                break
            } catch {
                failure = AccountErrorText.text(error, fallback: "Could not save your data to ForkMesh.")
            }
        }
        error = failure
        lastSaved = saved
        if saved > 0 || (force && failure.isEmpty) {
            lastSavedAt = clock.nowMillis
            prefs.set(Double(lastSavedAt), forKey: AccountSyncKeys.savedAt)
        }
        return saved
    }

    @discardableResult
    public func restore() async -> Int {
        guard !busy, !account().isEmpty else { return 0 }
        busy = true
        defer { busy = false }
        let owner = account()
        var applied = 0
        var found = 0
        var failure = ""
        for source in sources {
            do {
                let map = try await client.accountCall("GET", AccountDataPaths.storeItem(source.key), nil)
                guard let value = map.obj("item")["value"] else { continue }
                found += 1
                applied += source.restore(value)
                if let fresh = source.snapshot() {
                    prefs.set(AccountSyncService.digest(fresh), forKey: AccountSyncKeys.hash(account: owner, source: source.key))
                }
            } catch let problem as AccountEndpointError where problem.status == 404 {
                if problem.code != "not_found" {
                    failure = "This server cannot restore saved data yet."
                    break
                }
            } catch {
                failure = AccountErrorText.text(error, fallback: "Could not restore your saved data.")
            }
        }
        if failure.isEmpty, found == 0 { failure = "Nothing is saved on ForkMesh yet." }
        error = failure
        lastRestored = applied
        return applied
    }

    public func forgetHashes() {
        for key in prefs.keys where key.hasPrefix(AccountSyncKeys.prefix + "hash/") {
            prefs.remove(key)
        }
    }
}
