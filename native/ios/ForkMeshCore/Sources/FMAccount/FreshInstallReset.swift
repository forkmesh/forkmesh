import FMBase
import Foundation

public enum FreshInstallOutcome: Equatable, Sendable {
    case alreadyInstalled
    case upgraded
    case erased
    case deferred(reason: String)
}

public final class FreshInstallReset {
    public static let installedKey = "install/installed"
    public static let pendingKey = "install/erasePending"

    private let prefs: KeyValueStore
    private let flutterPrefs: KeyValueStore
    private let eraseKeychain: () throws -> Void

    public init(prefs: KeyValueStore, flutterPrefs: KeyValueStore, eraseKeychain: @escaping () throws -> Void) {
        self.prefs = prefs
        self.flutterPrefs = flutterPrefs
        self.eraseKeychain = eraseKeychain
    }

    @discardableResult
    public func run() -> FreshInstallOutcome {
        if prefs.bool(forKey: FreshInstallReset.installedKey) == true { return .alreadyInstalled }
        let pending: Bool = prefs.bool(forKey: FreshInstallReset.pendingKey) == true
        if !pending && !(prefs.keys.isEmpty && flutterPrefs.keys.isEmpty) {
            prefs.set(true, forKey: FreshInstallReset.installedKey)
            return .upgraded
        }
        prefs.set(true, forKey: FreshInstallReset.pendingKey)
        do {
            try eraseKeychain()
        } catch {
            return .deferred(reason: "\(error)")
        }
        prefs.remove(FreshInstallReset.pendingKey)
        prefs.set(true, forKey: FreshInstallReset.installedKey)
        return .erased
    }
}
