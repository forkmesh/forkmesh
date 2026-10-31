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
    private let eraseKeychain: () throws -> Void
    private let isKeychainLocked: () -> Bool

    public init(
        prefs: KeyValueStore, eraseKeychain: @escaping () throws -> Void, isKeychainLocked: @escaping () -> Bool = { false }
    ) {
        self.prefs = prefs
        self.eraseKeychain = eraseKeychain
        self.isKeychainLocked = isKeychainLocked
    }

    @discardableResult
    public func run() -> FreshInstallOutcome {
        if prefs.bool(forKey: FreshInstallReset.installedKey) == true { return .alreadyInstalled }
        if isKeychainLocked() { return .deferred(reason: "the phone has not been unlocked since it started") }
        let pending: Bool = prefs.bool(forKey: FreshInstallReset.pendingKey) == true
        if !pending && !prefs.keys.isEmpty {
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
