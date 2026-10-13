import FMBase
import FMCrypto
import Foundation
import XCTest

@testable import FMAccount

final class NextProjectRetirementTests: XCTestCase {
    let nextNamespace = ProjectScope.namespaceFor(AppEnv.nextHost)
    var root: URL!

    override func setUpWithError() throws {
        root = FileManager.default.temporaryDirectory.appendingPathComponent("next-retirement-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: root)
    }

    struct Phone {
        var secrets = MemoryKeychain()
        let prefs = InMemoryKeyValueStore()
    }

    func nextRow(label: String = "ForkMesh Next", lastUsedAt: Int64 = 0) -> RelayProfile {
        RelayProfile(
            id: AppEnv.nextHost, label: label, serverUrl: AppEnv.relayUrlForHost(AppEnv.nextHost), addedAt: 1,
            lastUsedAt: lastUsedAt, builtIn: true, worldUrl: "https://world.next.forkmesh.com/",
            homepageUrl: "https://next.forkmesh.com/", namespace: nextNamespace)
    }

    func prodRow() -> RelayProfile {
        RelayProfile(
            id: AppEnv.prodHost, label: "ForkMesh", serverUrl: AppEnv.prodServerUrl, addedAt: 1, builtIn: true)
    }

    func phone(active: String, rows: [RelayProfile]) -> Phone {
        var phone = Phone()
        phone.secrets = MemoryKeychain([
            Identity.seedKey: "legacy-auto-seed",
            OwnerCrypto.seedKey: "legacy-auto-owner",
            "\(nextNamespace)\(Identity.seedKey)": "next-seed",
            "\(nextNamespace)\(OwnerCrypto.seedKey)": "next-owner",
        ])
        phone.prefs.set(JSON.encode(.array(rows.map { .object($0.toJSON()) })), forKey: RelayDirectory.listKey)
        phone.prefs.set(active, forKey: RelayDirectory.activeKey)
        phone.prefs.set(true, forKey: RelayDirectory.migratedKey)
        return phone
    }

    func useNext(_ phone: Phone) throws {
        try phone.secrets.write(AuthService.sessionKeyFor(AppEnv.nextHost), "{\"nodeName\":\"alice\"}")
        try phone.secrets.write("\(nextNamespace)vault/deviceCopy", "vault-copy")
        phone.prefs.set("alice", forKey: "\(nextNamespace)profile/name")
        phone.prefs.set(true, forKey: AuthService.revokedBannerKeyFor(AppEnv.nextHost))
        phone.prefs.set("[{\"id\":\"c1\"}]", forKey: "nodeSync/carried/\(AppEnv.nextHost)")
        let repos = projectDir(AppEnv.nextHost).appendingPathComponent("forkmesh/repos/demo", isDirectory: true)
        try FileManager.default.createDirectory(at: repos, withIntermediateDirectories: true)
        try Data("x".utf8).write(to: repos.appendingPathComponent("README"))
    }

    func projectDir(_ id: String) -> URL {
        root.appendingPathComponent("projects", isDirectory: true).appendingPathComponent(id, isDirectory: true)
    }

    func retire(_ phone: Phone) -> NextRetirementReport {
        NextProjectRetirement(secrets: phone.secrets, prefs: phone.prefs, roots: [root]).run()
    }

    @MainActor
    func testAFreshInstallHasNothingToRetireAndGetsOnlyForkMesh() async {
        let phone = Phone()
        XCTAssertEqual(retire(phone), NextRetirementReport())
        XCTAssertEqual(phone.prefs.bool(forKey: NextProjectRetirement.doneKey), true)
        XCTAssertNil(phone.prefs.string(forKey: RelayDirectory.listKey))
        let directory = RelayDirectory.load(prefs: phone.prefs)
        XCTAssertEqual(directory.relays.map(\.id), [AppEnv.prodHost])
        XCTAssertTrue(directory.active.scope.isLegacy)
        XCTAssertEqual(retire(phone), NextRetirementReport(alreadyDone: true))
    }

    @MainActor
    func testAnUnusedNextRowIsDroppedAndItsAutoKeysErased() async throws {
        let phone = phone(active: AppEnv.nextHost, rows: [nextRow(lastUsedAt: 9), prodRow()])
        try FileManager.default.createDirectory(at: projectDir(AppEnv.nextHost), withIntermediateDirectories: true)
        phone.prefs.set(true, forKey: AuthService.revokedBannerKeyFor(AppEnv.nextHost))

        XCTAssertEqual(retire(phone), NextRetirementReport(dropped: [AppEnv.nextHost]))
        XCTAssertEqual(phone.prefs.string(forKey: RelayDirectory.activeKey), AppEnv.prodHost)
        XCTAssertNil(try phone.secrets.read("\(nextNamespace)\(Identity.seedKey)"))
        XCTAssertNil(try phone.secrets.read("\(nextNamespace)\(OwnerCrypto.seedKey)"))
        XCTAssertFalse(phone.prefs.contains(AuthService.revokedBannerKeyFor(AppEnv.nextHost)))
        XCTAssertFalse(FileManager.default.fileExists(atPath: projectDir(AppEnv.nextHost).path))
        XCTAssertEqual(try phone.secrets.read(Identity.seedKey), "legacy-auto-seed", "ForkMesh's own keys stay")

        let directory = RelayDirectory.load(prefs: phone.prefs)
        XCTAssertEqual(directory.relays.map(\.id), [AppEnv.prodHost])
        XCTAssertEqual(directory.activeId, AppEnv.prodHost)
        XCTAssertEqual(ProjectStorage.activeScope(prefs: phone.prefs), ProjectScope(projectId: AppEnv.prodHost, namespace: ""))
    }

    @MainActor
    func testAUsedNextFoldsIntoAnUnusedForkMeshWithoutSigningOut() async throws {
        let phone = phone(active: AppEnv.nextHost, rows: [nextRow(lastUsedAt: 7), prodRow()])
        try useNext(phone)
        phone.prefs.set(true, forKey: AuthService.guestModeKeyFor(AppEnv.nextHost))

        XCTAssertEqual(retire(phone), NextRetirementReport(adopted: AppEnv.nextHost))
        XCTAssertEqual(phone.prefs.string(forKey: RelayDirectory.activeKey), AppEnv.prodHost)
        XCTAssertEqual(try phone.secrets.read(AuthService.sessionKeyFor(AppEnv.prodHost)), "{\"nodeName\":\"alice\"}")
        XCTAssertNil(try phone.secrets.read(AuthService.sessionKeyFor(AppEnv.nextHost)))
        XCTAssertEqual(phone.prefs.bool(forKey: AuthService.guestModeKeyFor(AppEnv.prodHost)), true)
        XCTAssertEqual(phone.prefs.bool(forKey: AuthService.revokedBannerKeyFor(AppEnv.prodHost)), true)
        XCTAssertFalse(phone.prefs.contains(AuthService.revokedBannerKeyFor(AppEnv.nextHost)))
        XCTAssertEqual(phone.prefs.string(forKey: "nodeSync/carried/\(AppEnv.prodHost)"), "[{\"id\":\"c1\"}]")
        XCTAssertFalse(phone.prefs.contains("nodeSync/carried/\(AppEnv.nextHost)"))
        XCTAssertFalse(FileManager.default.fileExists(atPath: projectDir(AppEnv.nextHost).path))
        XCTAssertTrue(FileManager.default.fileExists(
            atPath: projectDir(AppEnv.prodHost).appendingPathComponent("forkmesh/repos/demo/README").path))
        XCTAssertEqual(try phone.secrets.read(Identity.seedKey), "legacy-auto-seed", "nothing is merged into the legacy keys")

        let directory = RelayDirectory.load(prefs: phone.prefs)
        XCTAssertEqual(directory.relays.map(\.id), [AppEnv.prodHost])
        let prod = directory.active
        XCTAssertEqual(prod.id, AppEnv.prodHost)
        XCTAssertTrue(prod.builtIn)
        XCTAssertEqual(prod.namespace, nextNamespace)
        XCTAssertEqual(prod.lastUsedAt, 7)
        XCTAssertEqual(prod.scope.directory(root).path, projectDir(AppEnv.prodHost).path)

        let scope = ProjectStorage.activeScope(prefs: phone.prefs)
        XCTAssertEqual(scope, ProjectScope(projectId: AppEnv.prodHost, namespace: nextNamespace))
        let router = ProjectScopeRouter(scope: scope)
        let secure = ProjectRoutedSecureStore(base: FallbackSecureStore(backend: phone.secrets), router: router)
        let prefs = ProjectRoutedKeyValueStore(base: phone.prefs, router: router)
        XCTAssertEqual(secure.read(Identity.seedKey), "next-seed", "the same node key")
        XCTAssertEqual(secure.read("vault/deviceCopy"), "vault-copy")
        XCTAssertEqual(prefs.string(forKey: "profile/name"), "alice")
        XCTAssertEqual(secure.read(AuthService.sessionKeyFor(AppEnv.prodHost)), "{\"nodeName\":\"alice\"}")
    }

    @MainActor
    func testANextActiveOnlyAsAGuestIsStillFolded() async throws {
        let phone = phone(active: AppEnv.nextHost, rows: [nextRow(), prodRow()])
        phone.prefs.set(true, forKey: AuthService.guestModeKeyFor(AppEnv.nextHost))
        XCTAssertEqual(retire(phone), NextRetirementReport(adopted: AppEnv.nextHost))
        XCTAssertEqual(phone.prefs.bool(forKey: AuthService.guestModeKeyFor(AppEnv.prodHost)), true)
    }

    @MainActor
    func testAUsedNextBesideASignedInForkMeshIsKeptAsItsOwnProject() async throws {
        let phone = phone(active: AppEnv.nextHost, rows: [prodRow(), nextRow()])
        try useNext(phone)
        try phone.secrets.write(AuthService.sessionKeyFor(AppEnv.prodHost), "{\"nodeName\":\"bob\"}")

        XCTAssertEqual(retire(phone), NextRetirementReport(kept: [AppEnv.nextHost]))
        XCTAssertEqual(phone.prefs.string(forKey: RelayDirectory.activeKey), AppEnv.nextHost, "the user stays where they were")
        XCTAssertEqual(try phone.secrets.read(AuthService.sessionKeyFor(AppEnv.prodHost)), "{\"nodeName\":\"bob\"}")
        XCTAssertEqual(try phone.secrets.read(AuthService.sessionKeyFor(AppEnv.nextHost)), "{\"nodeName\":\"alice\"}")
        XCTAssertTrue(FileManager.default.fileExists(atPath: projectDir(AppEnv.nextHost).path))
        XCTAssertFalse(FileManager.default.fileExists(atPath: projectDir(AppEnv.prodHost).path))

        let directory = RelayDirectory.load(prefs: phone.prefs)
        XCTAssertEqual(directory.relays.map(\.id), [AppEnv.prodHost, AppEnv.nextHost])
        XCTAssertEqual(directory.byId(AppEnv.prodHost)?.namespace, "")
        let kept = directory.active
        XCTAssertEqual(kept.id, AppEnv.nextHost)
        XCTAssertEqual(kept.label, RelayDirectory.formerNextLabel)
        XCTAssertEqual(kept.serverUrl, AppEnv.prodServerUrl)
        XCTAssertEqual(kept.namespace, nextNamespace)
        XCTAssertFalse(kept.builtIn)
        XCTAssertEqual(ProjectStorage.activeScope(prefs: phone.prefs).namespace, nextNamespace)
        directory.setActive(AppEnv.prodHost)
        XCTAssertTrue(directory.remove(AppEnv.nextHost), "an ordinary project the user may remove later")
    }

    @MainActor
    func testARenamedKeptRowKeepsItsName() async throws {
        let phone = phone(active: AppEnv.prodHost, rows: [prodRow(), nextRow(label: "Work")])
        try useNext(phone)
        phone.prefs.set("bob", forKey: "profile/name")
        XCTAssertEqual(retire(phone), NextRetirementReport(kept: [AppEnv.nextHost]))
        XCTAssertEqual(RelayDirectory.load(prefs: phone.prefs).byId(AppEnv.nextHost)?.label, "Work")
        XCTAssertEqual(phone.prefs.string(forKey: RelayDirectory.activeKey), AppEnv.prodHost)
    }

    @MainActor
    func testLegacyForkMeshDataWithoutASessionStillKeepsNextApart() async throws {
        let phone = phone(active: AppEnv.nextHost, rows: [prodRow(), nextRow()])
        try useNext(phone)
        try phone.secrets.write("vault/deviceCopy", "prod-vault")
        XCTAssertEqual(retire(phone), NextRetirementReport(kept: [AppEnv.nextHost]))
        XCTAssertEqual(try phone.secrets.read("vault/deviceCopy"), "prod-vault")
        XCTAssertEqual(try phone.secrets.read("\(nextNamespace)vault/deviceCopy"), "vault-copy")
    }

    @MainActor
    func testForkMeshRepositoriesOnDiskBlockTheFold() async throws {
        let phone = phone(active: AppEnv.nextHost, rows: [prodRow(), nextRow()])
        try useNext(phone)
        let legacyRepo = root.appendingPathComponent("forkmesh/repos/mine", isDirectory: true)
        try FileManager.default.createDirectory(at: legacyRepo, withIntermediateDirectories: true)
        XCTAssertEqual(retire(phone), NextRetirementReport(kept: [AppEnv.nextHost]))
        XCTAssertTrue(FileManager.default.fileExists(atPath: projectDir(AppEnv.nextHost).path))
    }

    @MainActor
    func testAnOccupiedForkMeshFolderBlocksTheFold() async throws {
        let phone = phone(active: AppEnv.nextHost, rows: [prodRow(), nextRow()])
        try useNext(phone)
        try FileManager.default.createDirectory(at: projectDir(AppEnv.prodHost), withIntermediateDirectories: true)
        XCTAssertEqual(retire(phone), NextRetirementReport(kept: [AppEnv.nextHost]))
        XCTAssertEqual(try phone.secrets.read(AuthService.sessionKeyFor(AppEnv.nextHost)), "{\"nodeName\":\"alice\"}")
        XCTAssertNil(try phone.secrets.read(AuthService.sessionKeyFor(AppEnv.prodHost)))
    }

    @MainActor
    func testANextRowInTheLegacyNamespaceFoldsIntoForkMeshWhenForkMeshHasNoSession() async throws {
        let legacyNext = nextRow().copyWith(namespace: "")
        let phone = phone(active: AppEnv.nextHost, rows: [legacyNext])
        try phone.secrets.write(AuthService.sessionKeyFor(AppEnv.nextHost), "{\"nodeName\":\"alice\"}")
        try phone.secrets.write("vault/deviceCopy", "shared-vault")
        XCTAssertEqual(retire(phone), NextRetirementReport(adopted: AppEnv.nextHost))
        let directory = RelayDirectory.load(prefs: phone.prefs)
        XCTAssertEqual(directory.relays.map(\.id), [AppEnv.prodHost])
        XCTAssertTrue(directory.active.scope.isLegacy)
        XCTAssertEqual(try phone.secrets.read(AuthService.sessionKeyFor(AppEnv.prodHost)), "{\"nodeName\":\"alice\"}")
        XCTAssertEqual(try phone.secrets.read("vault/deviceCopy"), "shared-vault")
    }

    @MainActor
    func testANextRowInTheLegacyNamespaceBesideASignedInForkMeshIsKept() async throws {
        let legacyNext = nextRow().copyWith(namespace: "")
        let phone = phone(active: AppEnv.nextHost, rows: [prodRow(), legacyNext])
        try phone.secrets.write(AuthService.sessionKeyFor(AppEnv.prodHost), "{\"nodeName\":\"bob\"}")
        XCTAssertEqual(retire(phone), NextRetirementReport(kept: [AppEnv.nextHost]))
        let directory = RelayDirectory.load(prefs: phone.prefs)
        XCTAssertEqual(directory.activeId, AppEnv.nextHost)
        XCTAssertEqual(directory.active.serverUrl, AppEnv.prodServerUrl)
        XCTAssertTrue(directory.active.scope.isLegacy)
    }

    @MainActor
    func testALockedKeychainDefersTheRetirementWithoutTouchingAnything() async throws {
        let phone = phone(active: AppEnv.nextHost, rows: [prodRow(), nextRow()])
        try useNext(phone)
        let list = phone.prefs.string(forKey: RelayDirectory.listKey)
        phone.secrets.failing = true
        XCTAssertEqual(retire(phone), NextRetirementReport(deferred: true))
        XCTAssertNil(phone.prefs.bool(forKey: NextProjectRetirement.doneKey))
        XCTAssertEqual(phone.prefs.string(forKey: RelayDirectory.listKey), list)
        XCTAssertEqual(phone.prefs.string(forKey: RelayDirectory.activeKey), AppEnv.nextHost)
        XCTAssertTrue(FileManager.default.fileExists(atPath: projectDir(AppEnv.nextHost).path))

        phone.secrets.failing = false
        XCTAssertEqual(retire(phone), NextRetirementReport(adopted: AppEnv.nextHost))
        XCTAssertEqual(try phone.secrets.read(AuthService.sessionKeyFor(AppEnv.prodHost)), "{\"nodeName\":\"alice\"}")
    }
}
