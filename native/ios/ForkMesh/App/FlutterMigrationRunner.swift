import FMAccount
import FMApi
import FMBase
import Foundation

@MainActor
enum FlutterMigrationRunner {
    static func migration() -> FlutterMigration {
        FlutterMigration(
            flutterSecrets: KeychainBackend(service: FlutterMigration.flutterSecureService),
            flutterPrefs: UserDefaultsBackend(prefix: FlutterMigration.flutterPreferencePrefix),
            secrets: KeychainBackend(),
            prefs: UserDefaultsBackend())
    }

    static func eraseLeftoversOfADeletedApp() {
        let reset: FreshInstallReset = FreshInstallReset(
            prefs: UserDefaultsBackend(),
            flutterPrefs: UserDefaultsBackend(prefix: FlutterMigration.flutterPreferencePrefix),
            eraseKeychain: KeychainBackend.eraseAll)
        switch reset.run() {
        case .alreadyInstalled:
            break
        case .deferred(let reason):
            Log.warning("install", "Keychain left by a deleted install not erased yet: \(reason)")
        case let outcome:
            Log.info("install", "First launch of this install: \(outcome)")
        }
    }

    static func migrateBeforeLaunch() {
        let outcome: FlutterMigrationOutcome = migration().run()
        switch outcome {
        case .alreadyDone:
            break
        case .deferred(let reason):
            Log.warning("migration", "Flutter data not migrated yet: \(reason)")
            return
        default:
            Log.info("migration", "Flutter data migration: \(outcome)")
        }
        retireNextProject()
    }

    static func retireNextProject() {
        let retirement: NextProjectRetirement = NextProjectRetirement(
            secrets: KeychainBackend(), prefs: UserDefaultsBackend(), roots: ProjectRuntime.roots())
        let report: NextRetirementReport = retirement.run()
        if !report.alreadyDone {
            Log.info("migration", "ForkMesh Next project retired: \(report)")
        }
    }

    static func retireWhenVerified(_ services: AppServices) {
        let migration: FlutterMigration = migration()
        guard migration.isDone, !migration.isRetired else { return }
        let signedIn: Bool = services.auth.session != nil
        let api: ApiService = services.api
        Task { @MainActor in
            var accepted: Bool = false
            if signedIn {
                do {
                    _ = try await api.listSessions()
                    accepted = true
                } catch {
                    Log.info("migration", "Session not verified yet; the Flutter copies stay: \(error)")
                }
            }
            migration.retireFlutterData(sessionAccepted: accepted)
        }
    }
}
