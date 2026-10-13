// The project store, plus the ProjectScope value itself.

import FMBase
import Foundation
import XCTest
@testable import FMAccount

final class ProjectScopeTests: XCTestCase {
    // MARK: ProjectScope

    func testTheLegacyScopeIsThePreProjectsLayout() {
        let scope = ProjectScope.legacy
        XCTAssertTrue(scope.isLegacy)
        XCTAssertEqual(scope.key("profile/name"), "profile/name")
        let base = URL(fileURLWithPath: "/tmp/fm-base", isDirectory: true)
        XCTAssertEqual(scope.directory(base), base)
        let store = InMemorySecureStore()
        XCTAssertTrue(scope.secure(store) === store, "the legacy view is the store itself")
        let prefs = InMemoryKeyValueStore()
        XCTAssertTrue(scope.prefs(prefs) === prefs)
    }

    func testAProjectScopeIsPrefixedWithItsId() {
        let scope = ProjectScope(projectId: AppEnv.nextHost, namespace: ProjectScope.namespaceFor(AppEnv.nextHost))
        XCTAssertFalse(scope.isLegacy)
        XCTAssertEqual(scope.namespace, "p/app.next.forkmesh.com/")
        XCTAssertEqual(scope.key("identity/ed25519Seed"), "p/app.next.forkmesh.com/identity/ed25519Seed")
        let base = URL(fileURLWithPath: "/tmp/fm-base", isDirectory: true)
        XCTAssertEqual(scope.directory(base).path, "/tmp/fm-base/projects/app.next.forkmesh.com")
    }

    func testSlugsAreFilesystemSafe() {
        XCTAssertEqual(ProjectScope.slugFor("Relay.Test:8787"), "relay.test_8787")
        XCTAssertEqual(ProjectScope.slugFor("[::1]:80"), "___1__80")
        XCTAssertEqual(ProjectScope.slugFor("a_b-c.d"), "a_b-c.d")
        XCTAssertEqual(ProjectScope.slugFor("h\u{00E9}.test"), "h_.test")
    }

    func testScopesAreEqualByIdAndNamespace() {
        let a = ProjectScope(projectId: "relay.test", namespace: "p/relay.test/")
        XCTAssertEqual(a, ProjectScope(projectId: "relay.test", namespace: "p/relay.test/"))
        XCTAssertNotEqual(a, ProjectScope(projectId: "relay.test", namespace: ""))
        XCTAssertEqual(ProjectScope.legacy.description, "ProjectScope(-, legacy)")
    }

    // MARK: Views

    func testASecureStoreViewOnlySeesItsOwnNamespace() {
        let base = InMemorySecureStore()
        let next = ProjectScope(projectId: AppEnv.nextHost, namespace: ProjectScope.namespaceFor(AppEnv.nextHost))
            .secure(base)
        base.write("identity/ed25519Seed", "legacy")
        next.write("identity/ed25519Seed", "next")

        XCTAssertEqual(base.read("identity/ed25519Seed"), "legacy")
        XCTAssertEqual(next.read("identity/ed25519Seed"), "next")
        XCTAssertEqual(next.readAll(), ["identity/ed25519Seed": "next"])
        XCTAssertEqual(base.read("p/app.next.forkmesh.com/identity/ed25519Seed"), "next", "byte-compatible key")

        (next as? ScopedSecureStore)?.deleteNamespace("")
        XCTAssertNil(next.read("identity/ed25519Seed"))
        XCTAssertEqual(base.read("identity/ed25519Seed"), "legacy")
    }

    func testAPreferenceViewOnlySeesItsOwnNamespace() {
        let base = InMemoryKeyValueStore(["profile/name": "alice"])
        let next = ProjectScope(projectId: "relay.test", namespace: "p/relay.test/").prefs(base)
        XCTAssertNil(next.string(forKey: "profile/name"))
        next.set("bob", forKey: "profile/name")
        next.set(true, forKey: "node/serve")
        XCTAssertEqual(base.string(forKey: "p/relay.test/profile/name"), "bob")
        XCTAssertEqual(base.string(forKey: "profile/name"), "alice")
        XCTAssertEqual(next.keys, ["profile/name", "node/serve"])
        next.clear()
        XCTAssertTrue(next.keys.isEmpty)
        XCTAssertEqual(base.string(forKey: "profile/name"), "alice")
    }

    // MARK: Profiles

    func testANewProjectGetsItsOwnNamespaceAndAnOldRowKeepsTheLegacyOne() throws {
        let added = try XCTUnwrap(RelayProfile.tryParse("app.acme.dev", label: "Acme"))
        XCTAssertEqual(added.namespace, "p/app.acme.dev/")
        XCTAssertEqual(added.scope.projectId, "app.acme.dev")
        XCTAssertEqual(added.worldURL?.host, "world.acme.dev", "derived")
        XCTAssertEqual(added.homepageURL?.host, "acme.dev", "the app host without its app. label")

        let restored = RelayProfile.fromJSON(try XCTUnwrap(try JSON.parse(JSON.encode(.object(added.toJSON()))).objectValue))
        XCTAssertEqual(restored.namespace, added.namespace)

        // A row saved before projects existed has no namespace field.
        let old = RelayProfile.fromJSON(["id": "staging.forkmesh.com", "label": "Staging"])
        XCTAssertTrue(old.scope.isLegacy)
    }

    func testWorldAndHomepageDerivations() throws {
        XCTAssertEqual(RelayProfile.deriveWorldHost("app.acme.dev"), "world.acme.dev")
        XCTAssertEqual(RelayProfile.deriveWorldHost("acme.dev"), "world.acme.dev")
        XCTAssertEqual(RelayProfile.deriveWorldHost("localhost"), "")
        XCTAssertEqual(RelayProfile.deriveWorldHost("192.168.1.20"), "")
        XCTAssertEqual(RelayProfile.deriveWorldHost("intranet"), "")

        let typed = try XCTUnwrap(RelayProfile.tryParse("relay.test", worldUrl: "https://play.relay.test/some/path"))
        XCTAssertEqual(typed.worldUrl, "https://play.relay.test/")
        XCTAssertEqual(typed.worldURL?.host, "play.relay.test")
        XCTAssertEqual(RelayProfile.normalizeWorldUrl("not a host"), "")

        let local = try XCTUnwrap(RelayProfile.tryParse("localhost:8787"))
        XCTAssertEqual(local.homepageURL?.absoluteString, "http://localhost:8787/")
    }

    func testLegalLinksFollowAProjectsHomepage() throws {
        let homepage = try XCTUnwrap(URL(string: "https://next.forkmesh.com/"))
        XCTAssertEqual(LegalLinks.termsFor(homepage).absoluteString, "https://next.forkmesh.com/terms")
        XCTAssertEqual(LegalLinks.privacyFor(homepage).absoluteString, "https://next.forkmesh.com/privacy")
        let custom = try XCTUnwrap(URL(string: "http://localhost:8787/"))
        XCTAssertEqual(LegalLinks.termsFor(custom).absoluteString, "http://localhost:8787/terms")
    }

    // MARK: The project store

    @MainActor
    func testForkMeshShipsBuiltInInTheLegacyNamespace() async throws {
        let directory = RelayDirectory.load(prefs: InMemoryKeyValueStore())
        let prod = try XCTUnwrap(directory.byId(AppEnv.prodHost))
        XCTAssertTrue(prod.builtIn)
        XCTAssertTrue(prod.scope.isLegacy)
        XCTAssertNil(directory.byId(AppEnv.nextHost))
        XCTAssertEqual(prod.worldURL?.absoluteString, "https://world.forkmesh.com/", "derived from the app host")
        XCTAssertEqual(prod.homepageURL?.absoluteString, "https://forkmesh.com/")
    }

    @MainActor
    func testCustomProjectsAreAddedEditedAndRemoved() async throws {
        let directory = RelayDirectory.load(prefs: InMemoryKeyValueStore())
        let added = directory.add(try XCTUnwrap(RelayProfile.tryParse("app.acme.dev", label: "Acme")))
        XCTAssertEqual(added.namespace, "p/app.acme.dev/")

        directory.update("app.acme.dev", label: "Acme Corp", worldUrl: "https://play.acme.dev")
        let edited = try XCTUnwrap(directory.byId("app.acme.dev"))
        XCTAssertEqual(edited.label, "Acme Corp")
        XCTAssertEqual(edited.worldURL?.host, "play.acme.dev")
        XCTAssertEqual(edited.namespace, added.namespace, "identity is stable")

        XCTAssertTrue(directory.remove("app.acme.dev"))
        XCTAssertNil(directory.byId("app.acme.dev"))
    }

    @MainActor
    func testBuiltInProjectsCannotBeRemovedOrRepointed() async throws {
        let directory = RelayDirectory.load(prefs: InMemoryKeyValueStore())
        directory.add(try XCTUnwrap(RelayProfile.tryParse("relay.test")))
        directory.setActive("relay.test")
        XCTAssertFalse(directory.remove(AppEnv.prodHost))

        directory.update(AppEnv.prodHost, label: "Main", worldUrl: "https://evil.example")
        let prod = try XCTUnwrap(directory.byId(AppEnv.prodHost))
        XCTAssertEqual(prod.label, "Main", "a name is the user's to choose")
        XCTAssertEqual(prod.worldURL?.host, "world.forkmesh.com")
        XCTAssertEqual(directory.relays.count, 2)
    }

    @MainActor
    func testAPreProjectsListBecomesProjectsWithNothingMoved() async throws {
        let list: JSONValue = [
            ["id": .string(AppEnv.prodHost), "label": .string(AppEnv.prodHost), "serverUrl": .string(AppEnv.prodServerUrl),
             "builtIn": true],
            ["id": "staging.forkmesh.com", "label": "Staging",
             "serverUrl": .string(AppEnv.relayUrlForHost("staging.forkmesh.com"))],
        ]
        let prefs = InMemoryKeyValueStore([
            RelayDirectory.listKey: .string(JSON.encode(list)),
            RelayDirectory.activeKey: "staging.forkmesh.com",
            RelayDirectory.migratedKey: true,
        ])
        let directory = RelayDirectory.load(prefs: prefs)
        let prod = try XCTUnwrap(directory.byId(AppEnv.prodHost))
        XCTAssertEqual(prod.label, "ForkMesh")
        XCTAssertTrue(prod.scope.isLegacy)
        XCTAssertTrue(
            try XCTUnwrap(directory.byId("staging.forkmesh.com")).scope.isLegacy,
            "it shared the one store before projects and keeps it")
        XCTAssertNil(directory.byId(AppEnv.nextHost))
        XCTAssertEqual(directory.activeId, "staging.forkmesh.com")
        let scope = ProjectStorage.activeScope(prefs: prefs)
        XCTAssertTrue(scope.isLegacy)
        XCTAssertEqual(scope.namespace, "")
        XCTAssertEqual(scope.projectId, "staging.forkmesh.com")
    }

    @MainActor
    func testSettingsExposeTheActiveProjectsScope() async throws {
        let h = Harness()
        XCTAssertTrue(h.settings.scope.isLegacy)
        XCTAssertEqual(h.settings.projectId, AppEnv.prodHost)
        h.settings.relays.add(try XCTUnwrap(RelayProfile.tryParse("app.acme.dev")))
        h.settings.relays.setActive("app.acme.dev")
        XCTAssertEqual(h.settings.scope.namespace, "p/app.acme.dev/")
        XCTAssertEqual(h.settings.worldUri?.host, "world.acme.dev")
        XCTAssertEqual(h.settings.homepageUri?.host, "acme.dev")
    }
}
