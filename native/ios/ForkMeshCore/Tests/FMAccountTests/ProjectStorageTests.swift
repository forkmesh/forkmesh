// Per-project isolation for the native stores: the app's Keychain and preferences route every
// per-project key to the active project's namespace, and a switch moves them (and the node key)
// together.

import FMBase
import FMCrypto
import Foundation
import XCTest
@testable import FMAccount

final class ProjectStorageTests: XCTestCase {
    private let nextScope = ProjectScope(projectId: AppEnv.nextHost, namespace: ProjectScope.namespaceFor(AppEnv.nextHost))

    // MARK: Routing

    func testTheSecureStoreFollowsTheActiveProject() {
        let base = InMemorySecureStore()
        let router = ProjectScopeRouter()
        let store = ProjectRoutedSecureStore(base: base, router: router)
        store.write(Identity.seedKey, "legacy")

        XCTAssertTrue(router.activate(nextScope))
        XCTAssertFalse(router.activate(nextScope), "no change, no notification")
        XCTAssertNil(store.read(Identity.seedKey), "Next never sees production's node key")
        store.write(Identity.seedKey, "next")

        XCTAssertEqual(base.read("identity/ed25519Seed"), "legacy")
        XCTAssertEqual(base.read("p/app.next.forkmesh.com/identity/ed25519Seed"), "next")
        XCTAssertEqual(store.readAll(), ["identity/ed25519Seed": "next"])

        router.activate(.legacy)
        XCTAssertEqual(store.read(Identity.seedKey), "legacy")
        XCTAssertEqual(store.readAll(), ["identity/ed25519Seed": "legacy"], "another project's entries never show")
    }

    func testSessionsAndAgentKeysStayShared() {
        let base = InMemorySecureStore()
        let router = ProjectScopeRouter(scope: nextScope)
        let store = ProjectRoutedSecureStore(base: base, router: router)
        store.write("auth/session/\(AppEnv.nextHost)", "{\"nodeName\":\"bob\"}")
        store.write("agent/credentials-v1", "keys")
        XCTAssertEqual(base.read("auth/session/\(AppEnv.nextHost)"), "{\"nodeName\":\"bob\"}")
        XCTAssertEqual(base.read("agent/credentials-v1"), "keys")

        router.activate(.legacy)
        XCTAssertEqual(store.read("auth/session/\(AppEnv.nextHost)"), "{\"nodeName\":\"bob\"}",
                       "the switcher can say who you are on every project")
    }

    func testAnExplicitlyNamespacedKeyIsNotPrefixedTwice() {
        let base = InMemorySecureStore()
        let store = ProjectRoutedSecureStore(base: base, router: ProjectScopeRouter(scope: nextScope))
        store.write(nextScope.key("vault/deviceCopy"), "copy")
        XCTAssertEqual(base.read("p/app.next.forkmesh.com/vault/deviceCopy"), "copy")
        XCTAssertEqual(store.read("vault/deviceCopy"), "copy")

        let prefsBase = InMemoryKeyValueStore()
        let prefs = ProjectRoutedKeyValueStore(base: prefsBase, router: ProjectScopeRouter(scope: nextScope))
        prefs.set("bob", forKey: nextScope.key("profile/name"))
        XCTAssertEqual(prefsBase.string(forKey: "p/app.next.forkmesh.com/profile/name"), "bob")
    }

    func testOnlyTheFlutterScopedPreferencesArePerProject() {
        let base = InMemoryKeyValueStore()
        let router = ProjectScopeRouter()
        let prefs = ProjectRoutedKeyValueStore(base: base, router: router)
        prefs.set("alice", forKey: "profile/name")
        prefs.set("dark", forKey: "appearance/themeMode")

        router.activate(nextScope)
        XCTAssertNil(prefs.string(forKey: "profile/name"))
        XCTAssertEqual(prefs.string(forKey: "appearance/themeMode"), "dark", "appearance is the phone's, not a project's")
        prefs.set("bob", forKey: "profile/name")
        prefs.set(true, forKey: "node/serve")
        prefs.set(#"["desk"]"#, forKey: "localControl/pairedDesktops")

        XCTAssertEqual(base.string(forKey: "profile/name"), "alice")
        XCTAssertEqual(base.string(forKey: "p/app.next.forkmesh.com/profile/name"), "bob")
        XCTAssertEqual(base.bool(forKey: "p/app.next.forkmesh.com/node/serve"), true)
        XCTAssertNil(base.bool(forKey: "node/serve"))
        XCTAssertTrue(prefs.keys.contains("profile/name"))
        XCTAssertTrue(prefs.keys.contains("appearance/themeMode"))
        XCTAssertFalse(prefs.keys.contains { $0.hasPrefix("p/") })

        // The project list is shared: it is how every project is found.
        XCTAssertEqual(ProjectKeyPolicy.preferenceKey(RelayDirectory.listKey, in: nextScope), RelayDirectory.listKey)
        XCTAssertEqual(ProjectKeyPolicy.preferenceKey(RelayDirectory.activeKey, in: nextScope), RelayDirectory.activeKey)
    }

    func testAProjectViewNeverAdoptsLegacyPlaintextSecrets() {
        let base = InMemoryKeyValueStore([Identity.seedKey: "plaintext-legacy-seed"])
        let router = ProjectScopeRouter(scope: nextScope)
        let prefs = ProjectRoutedKeyValueStore(base: base, router: router)
        let secure = ProjectRoutedSecureStore(base: InMemorySecureStore(), router: router)
        secure.migrateFromPrefs(prefs, keys: [Identity.seedKey])
        XCTAssertNil(secure.read(Identity.seedKey))
        XCTAssertEqual(base.string(forKey: Identity.seedKey), "plaintext-legacy-seed", "left for production to adopt")
    }

    // MARK: Per-project services

    @MainActor
    func testSettingsSecretsAndNodeKeysArePerProject() async throws {
        let router = ProjectScopeRouter()
        let prefs = ProjectRoutedKeyValueStore(base: InMemoryKeyValueStore(), router: router)
        let secure = ProjectRoutedSecureStore(base: InMemorySecureStore(), router: router)
        let settings = SettingsService.create(prefs: prefs, secureStore: secure)
        let identity = try Identity.loadOrCreate(secureStore: secure, prefs: prefs)
        let prodKey = identity.publicKeyBytes
        settings.setDisplayName("alice")
        settings.setGithubToken("ghp_prod")

        router.activate(nextScope)
        settings.reloadProjectSecrets()
        try identity.reload()
        XCTAssertEqual(settings.displayName, "")
        XCTAssertEqual(settings.githubToken, "")
        XCTAssertNotEqual(identity.publicKeyBytes, prodKey, "every project has its own node key")
        let nextKey = identity.publicKeyBytes
        settings.setDisplayName("bob")

        router.activate(.legacy)
        settings.reloadProjectSecrets()
        try identity.reload()
        XCTAssertEqual(identity.publicKeyBytes, prodKey, "switching back finds the same key")
        XCTAssertEqual(settings.displayName, "alice")
        XCTAssertEqual(settings.githubToken, "ghp_prod")

        router.activate(nextScope)
        try identity.reload()
        XCTAssertEqual(identity.publicKeyBytes, nextKey)
        try identity.rotate()
        router.activate(.legacy)
        try identity.reload()
        XCTAssertEqual(identity.publicKeyBytes, prodKey, "a rotation stays in its own project")
    }

    @MainActor
    func testTheActiveScopeIsReadFromThePersistedList() async throws {
        XCTAssertEqual(ProjectStorage.activeScope(prefs: InMemoryKeyValueStore()), .legacy, "a fresh install")
        XCTAssertEqual(
            ProjectStorage.activeScope(prefs: InMemoryKeyValueStore([
                RelayDirectory.listKey: "not json {", RelayDirectory.activeKey: .string(AppEnv.nextHost),
            ])),
            .legacy)

        let prefs = InMemoryKeyValueStore()
        let directory = RelayDirectory.load(prefs: prefs)
        XCTAssertEqual(ProjectStorage.activeScope(prefs: prefs), .legacy.withId(AppEnv.prodHost))
        directory.add(try XCTUnwrap(RelayProfile.tryParse("app.acme.dev")))
        directory.setActive("app.acme.dev")
        XCTAssertEqual(
            ProjectStorage.activeScope(prefs: prefs),
            ProjectScope(projectId: "app.acme.dev", namespace: ProjectScope.namespaceFor("app.acme.dev")))
    }

    // MARK: Erasing

    @MainActor
    func testRemovingAProjectErasesOnlyItsOwnData() async throws {
        let secureBase = InMemorySecureStore()
        let prefsBase = InMemoryKeyValueStore()
        let router = ProjectScopeRouter()
        let secure = ProjectRoutedSecureStore(base: secureBase, router: router)
        let prefs = ProjectRoutedKeyValueStore(base: prefsBase, router: router)
        let acme = try XCTUnwrap(RelayProfile.tryParse("app.acme.dev"))

        secure.write(Identity.seedKey, "legacy-seed")
        prefs.set("alice", forKey: "profile/name")
        router.activate(acme.scope)
        secure.write(Identity.seedKey, "acme-seed")
        secure.write("auth/session/app.acme.dev", "session")
        prefs.set("carol", forKey: "profile/name")
        router.activate(.legacy)

        let temp = FileManager.default.temporaryDirectory.standardizedFileURL
        let root = temp.appendingPathComponent("fm-projects-\(UUID().uuidString)", isDirectory: true)
        addTeardownBlock {
            // Only ever the folder this test made, inside the temporary directory.
            if root.standardizedFileURL.path.hasPrefix(temp.path + "/"), root.lastPathComponent.hasPrefix("fm-projects-") {
                try? FileManager.default.removeItem(at: root)
            }
        }
        let mine = root.appendingPathComponent("projects/app.acme.dev/forkmesh", isDirectory: true)
        let legacy = root.appendingPathComponent("forkmesh/repos", isDirectory: true)
        try FileManager.default.createDirectory(at: mine, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: legacy, withIntermediateDirectories: true)

        ProjectStorage.erase(acme, secureStore: secure, prefs: prefs, roots: [root])

        XCTAssertNil(secureBase.read("p/app.acme.dev/identity/ed25519Seed"))
        XCTAssertEqual(secureBase.read(Identity.seedKey), "legacy-seed")
        XCTAssertEqual(secureBase.read("auth/session/app.acme.dev"), "session", "sessions are AuthService.forgetRelay's")
        XCTAssertNil(prefsBase.string(forKey: "p/app.acme.dev/profile/name"))
        XCTAssertEqual(prefsBase.string(forKey: "profile/name"), "alice")
        XCTAssertFalse(FileManager.default.fileExists(atPath: mine.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: legacy.path))

        // A project in the legacy namespace shares it with production: nothing of it is erased.
        let staging = RelayProfile.fromJSON(["id": "staging.forkmesh.com"])
        ProjectStorage.erase(staging, secureStore: secure, prefs: prefs, roots: [root])
        XCTAssertEqual(secureBase.read(Identity.seedKey), "legacy-seed")
        XCTAssertEqual(prefsBase.string(forKey: "profile/name"), "alice")
        XCTAssertTrue(FileManager.default.fileExists(atPath: legacy.path))
    }

    func testANamespaceDeleteRefusesAnythingButAProjectNamespace() {
        let base = InMemorySecureStore(["identity/ed25519Seed": "legacy", "auth/session/x": "s", "p/a/k": "v"])
        ProjectStorage.deleteSecureNamespace("", in: base)
        ProjectStorage.deleteSecureNamespace("p/", in: base)
        ProjectStorage.deleteSecureNamespace("auth/", in: base)
        XCTAssertEqual(base.readAll().count, 3)
        ProjectStorage.deleteSecureNamespace("p/a/", in: base)
        XCTAssertEqual(base.readAll().count, 2)
    }
}

private extension ProjectScope {
    /// The legacy namespace pinned to a project.
    func withId(_ id: String) -> ProjectScope { ProjectScope(projectId: id, namespace: namespace) }
}
