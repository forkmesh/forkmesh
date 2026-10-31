import FMAccount
import FMBase
import Foundation

@MainActor
enum StartupMaintenance {
    static func eraseLeftoversOfADeletedApp() {
        let reset: FreshInstallReset = FreshInstallReset(
            prefs: UserDefaultsBackend(), eraseKeychain: KeychainBackend.eraseAll,
            isKeychainLocked: KeychainBackend.isLockedUntilFirstUnlock)
        switch reset.run() {
        case .alreadyInstalled:
            break
        case .deferred(let reason):
            Log.warning("install", "Keychain left by a deleted install not erased yet: \(reason)")
        case let outcome:
            Log.info("install", "First launch of this install: \(outcome)")
        }
    }

    static func retireNextProject() {
        let retirement: NextProjectRetirement = NextProjectRetirement(
            secrets: KeychainBackend(), prefs: UserDefaultsBackend(), roots: ProjectRuntime.roots())
        let report: NextRetirementReport = retirement.run()
        if !report.alreadyDone {
            Log.info("migration", "ForkMesh Next project retired: \(report)")
        }
    }
}
