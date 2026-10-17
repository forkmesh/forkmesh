import FMAccount
import FMBase
import CoreFoundation
import Foundation

/// The app's one project router: which project the preferences and the Keychain address. Resolved
/// on first use from the project list persisted in the preferences, so the stores built at launch
/// already point at the active project before any service reads them. A project switch flips it
/// (`ProjectRuntime` in AppServices+Projects.swift).
enum AppProjectScope {
    static let router: ProjectScopeRouter = ProjectScopeRouter(
        scope: ProjectStorage.activeScope(prefs: UserDefaultsBackend()))
}

/// The app's `KeyValueStore`: UserDefaults, with the per-project keys routed to the active
/// project's namespace (FMAccount's `ProjectRoutedKeyValueStore`).
final class UserDefaultsStore: KeyValueStore, @unchecked Sendable {
    private let routed: ProjectRoutedKeyValueStore

    init(defaults: UserDefaults = .standard, router: ProjectScopeRouter = AppProjectScope.router) {
        routed = ProjectRoutedKeyValueStore(base: UserDefaultsBackend(defaults: defaults), router: router)
    }

    /// The preferences underneath, addressed by full keys (`ProjectStorage.erase`).
    var base: KeyValueStore { routed.base }

    var router: ProjectScopeRouter { routed.router }

    func string(forKey key: String) -> String? { routed.string(forKey: key) }
    func bool(forKey key: String) -> Bool? { routed.bool(forKey: key) }
    func int(forKey key: String) -> Int? { routed.int(forKey: key) }
    func double(forKey key: String) -> Double? { routed.double(forKey: key) }
    func stringList(forKey key: String) -> [String]? { routed.stringList(forKey: key) }

    func set(_ value: String, forKey key: String) { routed.set(value, forKey: key) }
    func set(_ value: Bool, forKey key: String) { routed.set(value, forKey: key) }
    func set(_ value: Int, forKey key: String) { routed.set(value, forKey: key) }
    func set(_ value: Double, forKey key: String) { routed.set(value, forKey: key) }
    func set(_ value: [String], forKey key: String) { routed.set(value, forKey: key) }

    func remove(_ key: String) { routed.remove(key) }
    func contains(_ key: String) -> Bool { routed.contains(key) }
    var keys: Set<String> { routed.keys }
    func clear() { routed.clear() }
}

final class UserDefaultsBackend: KeyValueStore, @unchecked Sendable {
    static let appPrefix = "fm."
    private let prefix: String
    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard, prefix: String = UserDefaultsBackend.appPrefix) {
        self.defaults = defaults
        self.prefix = prefix
    }

    private func name(_ key: String) -> String { prefix + key }

    func string(forKey key: String) -> String? { defaults.object(forKey: name(key)) as? String }

    private func number(_ key: String) -> NSNumber? { defaults.object(forKey: name(key)) as? NSNumber }

    private static func isBool(_ value: NSNumber) -> Bool {
        return CFGetTypeID(value) == CFBooleanGetTypeID()
    }

    func bool(forKey key: String) -> Bool? {
        guard let value = number(key), Self.isBool(value) else { return nil }
        return value.boolValue
    }

    func int(forKey key: String) -> Int? {
        guard let value = number(key), !Self.isBool(value), !CFNumberIsFloatType(value as CFNumber) else { return nil }
        return value.intValue
    }

    func double(forKey key: String) -> Double? {
        guard let value = number(key), !Self.isBool(value) else { return nil }
        return value.doubleValue
    }

    func stringList(forKey key: String) -> [String]? { defaults.object(forKey: name(key)) as? [String] }

    func set(_ value: String, forKey key: String) { defaults.set(value, forKey: name(key)) }

    func set(_ value: Bool, forKey key: String) { defaults.set(value, forKey: name(key)) }

    func set(_ value: Int, forKey key: String) { defaults.set(value, forKey: name(key)) }

    func set(_ value: Double, forKey key: String) { defaults.set(value, forKey: name(key)) }

    func set(_ value: [String], forKey key: String) { defaults.set(value, forKey: name(key)) }

    func remove(_ key: String) { defaults.removeObject(forKey: name(key)) }

    func contains(_ key: String) -> Bool { defaults.object(forKey: name(key)) != nil }

    var keys: Set<String> {
        let all: [String] = Array(defaults.dictionaryRepresentation().keys)
        let own: [String] = all.filter { $0.hasPrefix(prefix) }
        return Set(own.map { String($0.dropFirst(prefix.count)) })
    }

    func clear() {
        for key in keys { remove(key) }
    }
}
