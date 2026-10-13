import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest
@testable import FMAccount

final class MemoryKeychain: ThrowingSecureBackend, @unchecked Sendable {
    struct Locked: Error, CustomStringConvertible {
        var description: String { "errSecInteractionNotAllowed" }
    }

    private let lock = NSLock()
    private var values: [String: String]
    var failing = false

    init(_ seed: [String: String] = [:]) { values = seed }

    var snapshot: [String: String] {
        lock.lock()
        defer { lock.unlock() }
        return values
    }

    func read(_ key: String) throws -> String? {
        if failing { throw Locked() }
        return snapshot[key]
    }

    func write(_ key: String, _ value: String) throws {
        if failing { throw Locked() }
        lock.lock()
        values[key] = value
        lock.unlock()
    }

    func delete(_ key: String) throws {
        if failing { throw Locked() }
        lock.lock()
        values.removeValue(forKey: key)
        lock.unlock()
    }

    func readAll() throws -> [String: String] {
        if failing { throw Locked() }
        return snapshot
    }
}

final class FlutterMigrationTests: XCTestCase {
    let nativeEnv = AppEnv(apiHostOverride: AppEnv.nextHost)
    let nextNamespace = ProjectScope.namespaceFor(AppEnv.nextHost)

    static let seed = Data(repeating: 7, count: 32).base64EncodedString()
    static let nextSeed = Data(repeating: 9, count: 32).base64EncodedString()

    static func session(_ name: String, _ token: String) -> String {
        JSON.encode(.object([
            "nodeName": .string(name),
            "email": .string("\(name)@example.com"),
            "status": "active",
            "sessionToken": .string(token),
            "sessionKind": "mobile_node",
            "deviceKind": "mobile_node",
            "deviceId": .string("dev-\(name)"),
            "capabilities": ["browse", "publish_repo", "owner_sign"],
        ]))
    }

    func flutterSecrets() -> [String: String] {
        [
            "identity/ed25519Seed": Self.seed,
            "ownerCrypto/hybridV1": "{\"v\":1,\"x25519Pub\":\"AA==\"}",
            "auth/session/app.forkmesh.com": Self.session("alice", "token-prod"),
            "auth/session/app.next.forkmesh.com": Self.session("alice", "token-next"),
            "\(nextNamespace)identity/ed25519Seed": Self.nextSeed,
            "server/passphrase": "hunter2",
            "vault/alice/autofill": "1",
            "agent/credentials-v1": "{\"kind\":\"anthropic\",\"apiKey\":\"k\",\"model\":\"m\"}",
        ]
    }

    func flutterRelayList() -> String {
        JSON.encode(.array([
            .object([
                "id": .string(AppEnv.prodHost), "label": "ForkMesh", "serverUrl": .string(AppEnv.prodServerUrl),
                "addedAt": 1, "lastUsedAt": 0, "verifiedAt": 0, "verifiedRev": "", "builtIn": true,
                "features": [], "worldUrl": "", "homepageUrl": "", "namespace": "",
            ]),
            .object([
                "id": .string(AppEnv.nextHost), "label": "ForkMesh Next",
                "serverUrl": .string(AppEnv.relayUrlForHost(AppEnv.nextHost)),
                "addedAt": 1, "lastUsedAt": 0, "verifiedAt": 0, "verifiedRev": "", "builtIn": true,
                "features": [], "worldUrl": "https://world.next.forkmesh.com/", "homepageUrl": "https://next.forkmesh.com/",
                "namespace": .string(nextNamespace),
            ]),
        ]))
    }

    func flutterPrefs(active: String? = nil) -> InMemoryKeyValueStore {
        var seed: JSONObject = [
            "relays/list": .string(flutterRelayList()),
            "relays/migratedFromServerUrl": true,
            "appearance/themeMode": "dark",
            "appearance/accent": "teal",
            "profile/name": "alice",
            "\(nextNamespace)profile/name": "alice-next",
            "node/serve": true,
            "ads/hiddenUntilMs": .int(1_800_000_000_000),
            "bugReport/anchor": .double(0.25),
            "agentWatch/hidden": ["a", "b"],
            "device/iosUdid": "00008130-000A685436E2001C",
        ]
        if let active { seed["relays/active"] = .string(active) }
        return InMemoryKeyValueStore(seed)
    }

    struct Phone {
        let flutterSecrets: MemoryKeychain
        let flutterPrefs: InMemoryKeyValueStore
        let secrets: MemoryKeychain
        let prefs: InMemoryKeyValueStore

        func migration(clock: Clock = FixedClock(epochMillis: 1_000)) -> FlutterMigration {
            FlutterMigration(
                flutterSecrets: flutterSecrets, flutterPrefs: flutterPrefs, secrets: secrets, prefs: prefs, clock: clock)
        }
    }

    func upgradedPhone(active: String? = nil) -> Phone {
        Phone(
            flutterSecrets: MemoryKeychain(flutterSecrets()), flutterPrefs: flutterPrefs(active: active),
            secrets: MemoryKeychain(), prefs: InMemoryKeyValueStore())
    }

    func testAFreshInstallHasNothingToMigrate() {
        let phone = Phone(
            flutterSecrets: MemoryKeychain(), flutterPrefs: InMemoryKeyValueStore(), secrets: MemoryKeychain(),
            prefs: InMemoryKeyValueStore())
        let migration = phone.migration()
        XCTAssertEqual(migration.run(), .nothingToMigrate)
        XCTAssertTrue(migration.isDone)
        XCTAssertNil(phone.prefs.string(forKey: RelayDirectory.activeKey), "a fresh install keeps the build's default project")
        XCTAssertEqual(migration.run(), .alreadyDone)
    }

    func testEverySecretAndPreferenceKeepsItsKeyAndType() throws {
        let phone = upgradedPhone()
        let migration = phone.migration()
        XCTAssertEqual(migration.run(), .migrated(secrets: 8, preferences: 11))

        XCTAssertEqual(phone.secrets.snapshot, flutterSecrets(), "keys are byte-for-byte the native ones")
        XCTAssertEqual(phone.prefs.string(forKey: "appearance/themeMode"), "dark")
        XCTAssertEqual(phone.prefs.bool(forKey: "node/serve"), true)
        XCTAssertNil(phone.prefs.int(forKey: "node/serve"), "a Bool stays a Bool")
        XCTAssertEqual(phone.prefs.int(forKey: "ads/hiddenUntilMs"), 1_800_000_000_000)
        XCTAssertEqual(phone.prefs.double(forKey: "bugReport/anchor"), 0.25)
        XCTAssertNil(phone.prefs.int(forKey: "bugReport/anchor"), "a Double stays a Double")
        XCTAssertEqual(phone.prefs.stringList(forKey: "agentWatch/hidden"), ["a", "b"])
        XCTAssertEqual(phone.prefs.string(forKey: "\(nextNamespace)profile/name"), "alice-next")

        XCTAssertEqual(migration.migratedSessionProjects, [AppEnv.prodHost, AppEnv.nextHost].sorted())
        XCTAssertEqual(phone.prefs.int(forKey: FlutterMigration.doneAtKey), 1_000)
        XCTAssertEqual(phone.flutterSecrets.snapshot, flutterSecrets())
        XCTAssertEqual(phone.flutterPrefs.keys.count, 11)
    }

    @MainActor
    func testTheUpgradedAppIsSignedInOnForkMeshWithTheSameNodeKey() throws {
        let phone = upgradedPhone()
        XCTAssertEqual(phone.migration().run(), .migrated(secrets: 8, preferences: 11))
        XCTAssertEqual(phone.prefs.string(forKey: RelayDirectory.activeKey), AppEnv.prodHost)

        let router = ProjectScopeRouter(scope: ProjectStorage.activeScope(prefs: phone.prefs))
        XCTAssertTrue(router.scope.isLegacy, "production owns the legacy namespace the Flutter data is in")
        let prefs = ProjectRoutedKeyValueStore(base: phone.prefs, router: router)
        let secure = ProjectRoutedSecureStore(base: FallbackSecureStore(backend: phone.secrets), router: router)
        let settings = SettingsService.create(prefs: prefs, secureStore: secure, env: nativeEnv)
        XCTAssertEqual(settings.relays.activeId, AppEnv.prodHost)
        XCTAssertEqual(settings.relays.active.label, RelayDirectory.productionLabel)
        XCTAssertTrue(settings.relays.contains(AppEnv.nextHost))
        XCTAssertEqual(settings.relays.byId(AppEnv.nextHost)?.namespace, nextNamespace)
        XCTAssertEqual(settings.themeMode, .dark)

        let identity = try Identity.loadOrCreate(secureStore: secure, prefs: prefs)
        XCTAssertEqual(phone.secrets.snapshot[Identity.seedKey], Self.seed, "no new node key was minted")
        let auth = AuthService.create(
            settings: settings, identity: identity, http: FakeHTTPClient { _ in HTTPResponse(status: 200, json: [:]) })
        XCTAssertEqual(auth.session?.nodeName, "alice")
        XCTAssertEqual(auth.session?.sessionToken, "token-prod")
        XCTAssertEqual(auth.sessionForRelay(AppEnv.nextHost)?.sessionToken, "token-next")

        router.activate(ProjectScope(projectId: AppEnv.nextHost, namespace: nextNamespace))
        XCTAssertEqual(secure.read(Identity.seedKey), Self.nextSeed)
        XCTAssertEqual(prefs.string(forKey: "profile/name"), "alice-next")
    }

    func testTheFlutterActiveProjectIsKept() {
        let phone = upgradedPhone(active: AppEnv.nextHost)
        XCTAssertEqual(phone.migration().run(), .migrated(secrets: 8, preferences: 12))
        XCTAssertEqual(phone.prefs.string(forKey: RelayDirectory.activeKey), AppEnv.nextHost)
        XCTAssertEqual(ProjectStorage.activeScope(prefs: phone.prefs).namespace, nextNamespace)
    }

    @MainActor
    func testAFlutterPhoneSignedInOnBothKeepsNextAsItsOwnProject() throws {
        let phone = upgradedPhone(active: AppEnv.nextHost)
        XCTAssertEqual(phone.migration().run(), .migrated(secrets: 8, preferences: 12))
        let report = NextProjectRetirement(secrets: phone.secrets, prefs: phone.prefs).run()
        XCTAssertEqual(report, NextRetirementReport(kept: [AppEnv.nextHost]))
        XCTAssertEqual(phone.secrets.snapshot[AuthService.sessionKeyFor(AppEnv.nextHost)], Self.session("alice", "token-next"))
        XCTAssertEqual(phone.secrets.snapshot[AuthService.sessionKeyFor(AppEnv.prodHost)], Self.session("alice", "token-prod"))
        let directory = RelayDirectory.load(prefs: phone.prefs, env: nativeEnv)
        XCTAssertEqual(directory.activeId, AppEnv.nextHost)
        XCTAssertEqual(directory.active.serverUrl, AppEnv.prodServerUrl)
        XCTAssertEqual(directory.active.namespace, nextNamespace)
        XCTAssertEqual(directory.relays.map(\.id), [AppEnv.prodHost, AppEnv.nextHost])
    }

    func testALockedKeychainDefersWithoutWritingAnything() {
        let phone = upgradedPhone()
        phone.flutterSecrets.failing = true
        let migration = phone.migration()
        guard case .deferred = migration.run() else { return XCTFail("expected deferred") }
        XCTAssertFalse(migration.isDone)
        XCTAssertTrue(phone.secrets.snapshot.isEmpty)
        XCTAssertNil(phone.prefs.string(forKey: "appearance/themeMode"))

        phone.flutterSecrets.failing = false
        XCTAssertEqual(migration.run(), .migrated(secrets: 8, preferences: 11))
        XCTAssertEqual(phone.prefs.int(forKey: FlutterMigration.attemptsKey), 2)
    }

    func testANativeKeychainFailureDefers() {
        let phone = upgradedPhone()
        phone.secrets.failing = true
        let migration = phone.migration()
        guard case .deferred = migration.run() else { return XCTFail("expected deferred") }
        XCTAssertFalse(migration.isDone)
        phone.secrets.failing = false
        XCTAssertEqual(migration.run(), .migrated(secrets: 8, preferences: 11))
    }

    func testFlutterValuesWinOverOnesWrittenBeforeTheCopyFinished() throws {
        let phone = upgradedPhone()
        try phone.secrets.write(Identity.seedKey, "minted-on-a-locked-launch")
        phone.prefs.set(AppEnv.nextHost, forKey: RelayDirectory.activeKey)
        phone.prefs.set("light", forKey: "appearance/themeMode")
        phone.prefs.set(true, forKey: "native/onlyKey")

        XCTAssertEqual(phone.migration().run(), .migrated(secrets: 8, preferences: 11))
        XCTAssertEqual(phone.secrets.snapshot[Identity.seedKey], Self.seed)
        XCTAssertEqual(phone.prefs.string(forKey: RelayDirectory.activeKey), AppEnv.prodHost)
        XCTAssertEqual(phone.prefs.string(forKey: "appearance/themeMode"), "dark")
        XCTAssertEqual(phone.prefs.bool(forKey: "native/onlyKey"), true, "native-only keys are left alone")
    }

    func testOnceDoneNativeChangesAreNeverOverwritten() {
        let phone = upgradedPhone()
        let migration = phone.migration()
        XCTAssertEqual(migration.run(), .migrated(secrets: 8, preferences: 11))
        phone.prefs.set("light", forKey: "appearance/themeMode")
        XCTAssertEqual(migration.run(), .alreadyDone)
        XCTAssertEqual(phone.prefs.string(forKey: "appearance/themeMode"), "light")
    }

    func testTheFlutterDataStaysUntilTheSessionIsVerified() {
        let phone = upgradedPhone()
        let migration = phone.migration()
        XCTAssertFalse(migration.retireFlutterData(sessionAccepted: true), "nothing to retire before the copy")
        XCTAssertEqual(migration.run(), .migrated(secrets: 8, preferences: 11))

        XCTAssertFalse(migration.canRetire(sessionAccepted: false))
        XCTAssertFalse(migration.retireFlutterData(sessionAccepted: false))
        XCTAssertEqual(phone.flutterSecrets.snapshot, flutterSecrets())

        XCTAssertTrue(migration.retireFlutterData(sessionAccepted: true))
        XCTAssertTrue(migration.isRetired)
        XCTAssertTrue(phone.flutterSecrets.snapshot.isEmpty)
        XCTAssertTrue(phone.flutterPrefs.keys.isEmpty)
        XCTAssertEqual(phone.secrets.snapshot, flutterSecrets(), "the native copies stay")
        XCTAssertEqual(phone.prefs.string(forKey: "appearance/themeMode"), "dark")
        XCTAssertFalse(migration.retireFlutterData(sessionAccepted: true), "once")
    }

    func testRetiringTouchesOnlyWhatWasMigrated() throws {
        let phone = upgradedPhone()
        let migration = phone.migration()
        XCTAssertEqual(migration.run(), .migrated(secrets: 8, preferences: 11))
        try phone.flutterSecrets.write("written/after", "x")
        XCTAssertTrue(migration.retireFlutterData(sessionAccepted: true))
        XCTAssertEqual(phone.flutterSecrets.snapshot, ["written/after": "x"])
    }

    func testAFailedDeleteRetriesLater() {
        let phone = upgradedPhone()
        let migration = phone.migration()
        XCTAssertEqual(migration.run(), .migrated(secrets: 8, preferences: 11))
        phone.flutterSecrets.failing = true
        XCTAssertFalse(migration.retireFlutterData(sessionAccepted: true))
        XCTAssertFalse(migration.isRetired)
        phone.flutterSecrets.failing = false
        XCTAssertTrue(migration.retireFlutterData(sessionAccepted: true))
    }

    func testWithoutAFlutterSessionThereIsNothingToVerify() {
        var secrets = flutterSecrets()
        secrets = secrets.filter { !$0.key.hasPrefix("auth/session/") }
        let phone = Phone(
            flutterSecrets: MemoryKeychain(secrets), flutterPrefs: flutterPrefs(), secrets: MemoryKeychain(),
            prefs: InMemoryKeyValueStore())
        let migration = phone.migration()
        XCTAssertEqual(migration.run(), .migrated(secrets: 6, preferences: 11))
        XCTAssertTrue(migration.migratedSessionProjects.isEmpty)
        XCTAssertFalse(migration.migratedAnySession)
        XCTAssertTrue(migration.canRetire(sessionAccepted: false))
    }

    func testTheLegacyUnscopedSessionAlsoNeedsVerifying() {
        var secrets = flutterSecrets().filter { !$0.key.hasPrefix("auth/session/") }
        secrets["auth/session"] = Self.session("alice", "token-legacy")
        let phone = Phone(
            flutterSecrets: MemoryKeychain(secrets), flutterPrefs: flutterPrefs(), secrets: MemoryKeychain(),
            prefs: InMemoryKeyValueStore())
        let migration = phone.migration()
        XCTAssertEqual(migration.run(), .migrated(secrets: 7, preferences: 11))
        XCTAssertTrue(migration.migratedAnySession)
        XCTAssertFalse(migration.canRetire(sessionAccepted: false))
    }
}
