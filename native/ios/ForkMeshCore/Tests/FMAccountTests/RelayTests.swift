// The relay directory and the RelayFeature / RelayFeatureSet cases (the RelayProbe cases belong
// with the probe).

import FMBase
import XCTest
@testable import FMAccount

final class RelayTests: XCTestCase {
    @MainActor
    func load(_ seed: JSONObject = [:]) -> (RelayDirectory, InMemoryKeyValueStore) {
        let prefs = InMemoryKeyValueStore(seed)
        return (RelayDirectory.load(prefs: prefs, canonicalizeUrl: SettingsService.canonicalServerUrl), prefs)
    }

    // MARK: Origin normalization

    func testAcceptsBareHostsOriginsPortsAndFullRelayUrls() {
        XCTAssertEqual(RelayProfile.normalizeOrigin("app.forkmesh.com"), "app.forkmesh.com")
        XCTAssertEqual(RelayProfile.normalizeOrigin("  HTTPS://App.ForkMesh.com/  "), "app.forkmesh.com")
        XCTAssertEqual(RelayProfile.normalizeOrigin("wss://staging.forkmesh.com/api/x/ws"), "staging.forkmesh.com")
        XCTAssertEqual(RelayProfile.normalizeOrigin("192.168.1.20:8787"), "192.168.1.20:8787")
    }

    func testDropsADefaultPortSoOneRelayCannotBeAddedTwice() {
        XCTAssertEqual(RelayProfile.normalizeOrigin("https://relay.test:443"), RelayProfile.normalizeOrigin("relay.test"))
        XCTAssertEqual(RelayProfile.normalizeOrigin("http://relay.test:80"), "relay.test")
        XCTAssertEqual(RelayProfile.normalizeOrigin("relay.test:8443"), "relay.test:8443")
        XCTAssertEqual(RelayProfile.normalizeOrigin("wss://relay.test:443/ws"), "relay.test")
    }

    func testRefusesAnythingThatIsNotHostShaped() {
        for input in ["", "   ", "not a host", "http://", "///", "a..b", "relay.test:abc", "-bad.test", "[::1]:8787"] {
            XCTAssertNil(RelayProfile.normalizeOrigin(input), input)
        }
    }

    // MARK: Directory

    @MainActor
    func testAFreshInstallHoldsOnlyForkMesh() async {
        let (directory, prefs) = load()
        XCTAssertEqual(directory.relays.map(\.id), [AppEnv.prodHost])
        XCTAssertTrue(directory.active.builtIn)
        XCTAssertEqual(directory.active.label, "ForkMesh")
        XCTAssertEqual(directory.active.namespace, "")
        XCTAssertEqual(directory.activeServerUrl, AppEnv.prodServerUrl)
        XCTAssertNil(directory.byId(AppEnv.nextHost))
        XCTAssertFalse(directory.hasMultiple)
        XCTAssertEqual(RelayDirectory.load(prefs: prefs).relays.map(\.id), [AppEnv.prodHost])
    }

    @MainActor
    func testANextBuildConfigurationStillStartsOnForkMesh() async {
        let prefs = InMemoryKeyValueStore()
        let directory = RelayDirectory.load(prefs: prefs, env: AppEnv(apiHostOverride: AppEnv.nextHost))
        XCTAssertEqual(directory.relays.map(\.id), [AppEnv.prodHost])
        XCTAssertEqual(directory.activeServerUrl, AppEnv.prodServerUrl)
    }

    func testTheNextHostsAreAliasesOfForkMesh() throws {
        for input in [
            "app.next.forkmesh.com", "next.forkmesh.com", "https://App.Next.ForkMesh.com/", "wss://app.next.forkmesh.com:443/ws",
            "forkmesh.com", "www.forkmesh.com", "api.forkmesh.com",
        ] {
            XCTAssertEqual(RelayProfile.normalizeOrigin(input), AppEnv.prodHost, input)
            let parsed = try XCTUnwrap(RelayProfile.tryParse(input), input)
            XCTAssertEqual(parsed.id, AppEnv.prodHost, input)
            XCTAssertEqual(parsed.serverUrl, AppEnv.prodServerUrl, input)
        }
        XCTAssertEqual(RelayProfile.normalizeOrigin("app.next.forkmesh.com:8443"), "app.next.forkmesh.com:8443")
        XCTAssertEqual(RelayProfile.normalizeOrigin("staging.next.forkmesh.com"), "staging.next.forkmesh.com")
        XCTAssertEqual(RelayProfile.normalizeWorldUrl("world.next.forkmesh.com"), "https://world.next.forkmesh.com/")
        XCTAssertEqual(
            SettingsService.canonicalServerUrl("wss://app.next.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws"),
            AppEnv.prodServerUrl)
    }

    @MainActor
    func testAddingANextHostResolvesToTheForkMeshProject() async throws {
        let (directory, _) = load()
        let added = directory.add(try XCTUnwrap(RelayProfile.tryParse("https://app.next.forkmesh.com")))
        XCTAssertEqual(added.id, AppEnv.prodHost)
        XCTAssertEqual(added.namespace, "")
        directory.add(try XCTUnwrap(RelayProfile.tryParse("next.forkmesh.com")))
        XCTAssertEqual(directory.relays.map(\.id), [AppEnv.prodHost])
        let used = directory.useUrl("wss://app.next.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws")
        XCTAssertEqual(used?.id, AppEnv.prodHost)
        XCTAssertEqual(directory.relays.map(\.id), [AppEnv.prodHost])
        XCTAssertEqual(directory.activeServerUrl, AppEnv.prodServerUrl)
    }

    @MainActor
    func testALeftoverNextRowIsRetargetedAtForkMeshAndKeepsItsNamespace() async throws {
        let namespace = ProjectScope.namespaceFor(AppEnv.nextHost)
        let next = RelayProfile(
            id: AppEnv.nextHost, label: "ForkMesh Next", serverUrl: AppEnv.relayUrlForHost(AppEnv.nextHost),
            lastUsedAt: 5, builtIn: true, worldUrl: "https://world.next.forkmesh.com/",
            homepageUrl: "https://next.forkmesh.com/", namespace: namespace)
        let (directory, _) = load([
            RelayDirectory.listKey: .string(JSON.encode(.array([.object(next.toJSON())]))),
            RelayDirectory.activeKey: .string(AppEnv.nextHost),
        ])
        XCTAssertEqual(directory.relays.map(\.id), [AppEnv.prodHost, AppEnv.nextHost])
        XCTAssertEqual(directory.activeId, AppEnv.nextHost)
        let kept = try XCTUnwrap(directory.byId(AppEnv.nextHost))
        XCTAssertEqual(kept.label, RelayDirectory.formerNextLabel)
        XCTAssertEqual(kept.serverUrl, AppEnv.prodServerUrl)
        XCTAssertEqual(kept.httpOrigin, "https://app.forkmesh.com")
        XCTAssertEqual(kept.worldURL?.host, "world.forkmesh.com")
        XCTAssertEqual(kept.homepageURL?.host, "forkmesh.com")
        XCTAssertEqual(kept.displayOrigin, AppEnv.prodHost)
        XCTAssertEqual(kept.namespace, namespace)
        XCTAssertFalse(kept.builtIn)
        XCTAssertEqual(directory.byId(AppEnv.prodHost)?.namespace, "")
    }

    @MainActor
    func testAddedRelaysSurviveARelaunchAndAddingDoesNotSwitch() async throws {
        let prefs = InMemoryKeyValueStore()
        let first = RelayDirectory.load(prefs: prefs)
        first.add(try XCTUnwrap(RelayProfile.tryParse("staging.forkmesh.com")))
        XCTAssertEqual(first.activeId, AppEnv.prodHost, "adding must not switch")

        let second = RelayDirectory.load(prefs: prefs)
        XCTAssertEqual(second.relays.map(\.id), [AppEnv.prodHost, "staging.forkmesh.com"])
        XCTAssertEqual(second.activeId, AppEnv.prodHost)
    }

    @MainActor
    func testTheSameHostCannotBeAddedTwice() async throws {
        let (directory, _) = load()
        directory.add(try XCTUnwrap(RelayProfile.tryParse("relay.test")))
        directory.add(try XCTUnwrap(RelayProfile.tryParse("https://relay.test:443")))
        XCTAssertEqual(directory.relays.filter { $0.id == "relay.test" }.count, 1)
        XCTAssertEqual(directory.relays.count, 2)
    }

    @MainActor
    func testTheBuiltInRelayCannotBeRemoved() async {
        let (directory, _) = load()
        XCTAssertFalse(directory.remove(directory.active.id))
        XCTAssertEqual(directory.relays.count, 1)
    }

    @MainActor
    func testTheActiveRelayCannotBeRemovedOutFromUnderTheApp() async throws {
        let (directory, _) = load()
        directory.add(try XCTUnwrap(RelayProfile.tryParse("relay.test")))
        directory.setActive("relay.test")
        XCTAssertFalse(directory.remove("relay.test"))
        XCTAssertEqual(directory.activeId, "relay.test")

        directory.setActive(directory.fallbackFor("relay.test").id)
        XCTAssertTrue(directory.remove("relay.test"))
        XCTAssertEqual(directory.relays.count, 1)
    }

    @MainActor
    func testFallbackForPrefersTheMostRecentlyUsedOtherRelay() async throws {
        let prefs = InMemoryKeyValueStore()
        let clock = FixedClock(epochMillis: 1_000)
        let directory = RelayDirectory.load(prefs: prefs, clock: clock)
        directory.add(try XCTUnwrap(RelayProfile.tryParse("one.test")))
        directory.add(try XCTUnwrap(RelayProfile.tryParse("two.test")))
        directory.setActive("one.test")
        clock.advance(by: 1)
        directory.setActive("two.test")
        XCTAssertEqual(directory.fallbackFor("two.test").id, "one.test")
    }

    @MainActor
    func testSetActiveStampsLastUsedAtAndIsPersisted() async throws {
        let prefs = InMemoryKeyValueStore()
        let directory = RelayDirectory.load(prefs: prefs)
        directory.add(try XCTUnwrap(RelayProfile.tryParse("relay.test")))
        directory.setActive("relay.test")
        XCTAssertGreaterThan(directory.byId("relay.test")?.lastUsedAt ?? 0, 0)
        XCTAssertEqual(RelayDirectory.load(prefs: prefs).activeId, "relay.test")
    }

    @MainActor
    func testAnUnknownStoredActiveIdFallsBackInsteadOfCrashing() async {
        let (directory, _) = load([RelayDirectory.activeKey: "relay-that-was-removed.test"])
        XCTAssertEqual(directory.activeId, AppEnv.prodHost)
    }

    @MainActor
    func testACorruptListIsRebuiltAroundTheBuiltInRelay() async {
        let (directory, _) = load([RelayDirectory.listKey: "not json {"])
        XCTAssertEqual(directory.relays.count, 1)
        XCTAssertTrue(directory.active.builtIn)
    }

    @MainActor
    func testRenameFallsBackToTheHostWhenCleared() async throws {
        let (directory, _) = load()
        directory.add(try XCTUnwrap(RelayProfile.tryParse("relay.test")))
        directory.rename("relay.test", "  Staging  ")
        XCTAssertEqual(directory.byId("relay.test")?.label, "Staging")
        directory.rename("relay.test", "   ")
        XCTAssertEqual(directory.byId("relay.test")?.label, "relay.test")
    }

    @MainActor
    func testListenersHearChanges() async throws {
        let (directory, _) = load()
        let counter = Counter()
        let token = directory.addListener { counter.increment() }
        directory.add(try XCTUnwrap(RelayProfile.tryParse("relay.test")))
        directory.setActive("relay.test")
        XCTAssertEqual(counter.value, 2)
        directory.removeListener(token)
        directory.rename("relay.test", "Lab")
        XCTAssertEqual(counter.value, 2)
    }

    @MainActor
    func testADevBuildReplacesAnUntouchedProdBuiltInAndKeepsAUsedOne() async throws {
        let prefs = InMemoryKeyValueStore()
        _ = RelayDirectory.load(prefs: prefs)
        let dev = AppEnv(rawEnvironment: "dev")
        let switched = RelayDirectory.load(prefs: prefs, env: dev)
        XCTAssertEqual(switched.relays.map(\.id), ["localhost:8787", AppEnv.prodHost])
        XCTAssertTrue(switched.relays.allSatisfy(\.builtIn))
        XCTAssertEqual(switched.byId("localhost:8787")?.namespace, ProjectScope.namespaceFor("localhost:8787"))

        // A built-in row the user signed into survives an environment change.
        let used = InMemoryKeyValueStore()
        let prod = RelayDirectory.load(prefs: used)
        prod.add(try XCTUnwrap(RelayProfile.tryParse("relay.test")))
        prod.setActive("relay.test")
        prod.setActive(AppEnv.prodHost)
        let devAgain = RelayDirectory.load(prefs: used, env: dev)
        XCTAssertEqual(devAgain.relays.map(\.id), ["localhost:8787", AppEnv.prodHost, "relay.test"])
    }

    // MARK: Migration from the single-relay preference

    @MainActor
    func testADeliberateOverrideBecomesARelayAndStaysActive() async {
        let custom = "wss://staging.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws"
        let (directory, _) = load(["server/url": .string(custom)])
        XCTAssertNotNil(directory.byId("staging.forkmesh.com"))
        XCTAssertEqual(directory.activeId, "staging.forkmesh.com")
        XCTAssertEqual(directory.activeServerUrl, custom)
    }

    @MainActor
    func testAnAliasOfTheProductionRelayDoesNotBecomeASecondRow() async {
        let (directory, _) = load(["server/url": "ws://forkmesh.com/api/repo/mainnode/forkmesh/rooms/general/ws"])
        XCTAssertEqual(directory.relays.count, 1)
        XCTAssertEqual(directory.activeId, AppEnv.prodHost)
        XCTAssertEqual(directory.activeServerUrl, AppEnv.prodServerUrl)
    }

    @MainActor
    func testTheLegacyPreferenceIsReadExactlyOnce() async {
        let custom = "wss://staging.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws"
        let prefs = InMemoryKeyValueStore(["server/url": .string(custom)])
        let first = RelayDirectory.load(prefs: prefs, canonicalizeUrl: SettingsService.canonicalServerUrl)
        first.setActive(AppEnv.prodHost)
        XCTAssertTrue(first.remove("staging.forkmesh.com"))

        let second = RelayDirectory.load(prefs: prefs, canonicalizeUrl: SettingsService.canonicalServerUrl)
        XCTAssertNil(second.byId("staging.forkmesh.com"))
    }

    // MARK: useUrl

    @MainActor
    func testUseUrlKeepsAUrlVerbatimAndMakesItActive() async {
        let (directory, _) = load()
        directory.useUrl("ws://relay.test/ws")
        XCTAssertEqual(directory.activeId, "relay.test")
        XCTAssertEqual(directory.activeServerUrl, "ws://relay.test/ws")
    }

    @MainActor
    func testUseUrlReusesTheExistingRowForAHostAlreadyKnown() async throws {
        let (directory, _) = load()
        directory.add(try XCTUnwrap(RelayProfile.tryParse("relay.test", label: "Lab")))
        directory.useUrl("ws://relay.test/ws")
        XCTAssertEqual(directory.relays.count, 2)
        XCTAssertEqual(directory.byId("relay.test")?.label, "Lab")
        XCTAssertEqual(directory.byId("relay.test")?.serverUrl, "ws://relay.test/ws")
    }

    // MARK: Profiles

    func testProfilesRoundTripThroughJSON() throws {
        let relay = try XCTUnwrap(RelayProfile.tryParse("relay.test:8787", label: "Lab"))
            .copyWith(lastUsedAt: 42, verifiedAt: 7, verifiedRev: "abc123")
        let restored = RelayProfile.fromJSON(try XCTUnwrap(try JSON.parse(JSON.encode(.object(relay.toJSON()))).objectValue))
        XCTAssertEqual(restored.id, "relay.test:8787")
        XCTAssertEqual(restored.label, "Lab")
        XCTAssertEqual(restored.serverUrl, relay.serverUrl)
        XCTAssertEqual(restored.lastUsedAt, 42)
        XCTAssertEqual(restored.verifiedRev, "abc123")
        XCTAssertEqual(restored.httpOrigin, "https://relay.test:8787")
        XCTAssertEqual(
            relay.toJSON().keys,
            [
                "id", "label", "serverUrl", "addedAt", "lastUsedAt", "verifiedAt", "verifiedRev", "builtIn", "features",
                "worldUrl", "homepageUrl", "namespace",
            ])
    }

    @MainActor
    func testAProjectNeedsOnlyANameAndKeepsItsOwnNamespace() async throws {
        let (directory, prefs) = load()
        let project = try XCTUnwrap(directory.addNamed("  Side Quest ", logo: "side.png"))
        XCTAssertFalse(project.hasInstance)
        XCTAssertEqual(project.label, "Side Quest")
        XCTAssertTrue(project.id.hasPrefix("local-side_quest-"))
        XCTAssertEqual(project.namespace, ProjectScope.namespaceFor(project.id))
        XCTAssertEqual(project.serverUrl, "")
        XCTAssertEqual(project.httpOrigin, "")
        XCTAssertEqual(project.subtitle, RelayProfile.noInstanceLabel)
        XCTAssertEqual(project.displayOrigin, RelayProfile.noInstanceLabel)
        XCTAssertEqual(project.logo, "side.png")
        XCTAssertNil(directory.addNamed("   "))
        let other = try XCTUnwrap(directory.addNamed("Side Quest"))
        XCTAssertNotEqual(other.id, project.id)
        let restored = try XCTUnwrap(RelayDirectory.load(prefs: prefs).byId(project.id))
        XCTAssertFalse(restored.hasInstance)
        XCTAssertEqual(restored.logo, "side.png")
        XCTAssertEqual(restored.serverUrl, "")
        directory.setActive(project.id)
        XCTAssertEqual(directory.activeServerUrl, "")
    }

    func testANoInstanceProjectSavedWithAServerUrlIsNotLinkedToIt() throws {
        let relay = RelayProfile.fromJSON([
            "id": .string("local-tea-1"), "label": .string("Tea"), "serverUrl": .string(AppEnv.prodServerUrl),
            "hasInstance": .bool(false),
        ])
        XCTAssertFalse(relay.hasInstance)
        XCTAssertEqual(relay.serverUrl, "")
        XCTAssertEqual(relay.httpOrigin, "")
    }

    @MainActor
    func testAttachingAnInstanceLinksTheSameProject() async throws {
        let (directory, prefs) = load()
        let project = try XCTUnwrap(directory.addNamed("Tea"))
        let instance = try XCTUnwrap(RelayProfile.tryParse("relay.example.com")).copyWith(verifiedRev: "abc")
        XCTAssertNil(directory.attach(project.id, to: try XCTUnwrap(RelayProfile.named("Other"))))
        XCTAssertNil(directory.attach(AppEnv.prodHost, to: instance))
        let attached = try XCTUnwrap(directory.attach(project.id, to: instance))
        XCTAssertEqual(attached.id, project.id)
        XCTAssertEqual(attached.namespace, project.namespace)
        XCTAssertTrue(attached.hasInstance)
        XCTAssertEqual(attached.serverUrl, instance.serverUrl)
        XCTAssertEqual(attached.httpOrigin, "https://relay.example.com")
        XCTAssertEqual(attached.verifiedRev, "abc")
        XCTAssertNil(directory.attach(project.id, to: instance))
        let restored = try XCTUnwrap(RelayDirectory.load(prefs: prefs).byId(project.id))
        XCTAssertTrue(restored.hasInstance)
        XCTAssertEqual(restored.serverUrl, instance.serverUrl)
    }

    @MainActor
    func testALogoCanBeSetAndClearedOnAnyProject() async throws {
        let (directory, _) = load()
        directory.setLogo(AppEnv.prodHost, "fm.png")
        XCTAssertEqual(directory.byId(AppEnv.prodHost)?.logo, "fm.png")
        XCTAssertEqual(directory.byId(AppEnv.prodHost)?.toJSON()["logo"], .string("fm.png"))
        directory.setLogo(AppEnv.prodHost, "")
        XCTAssertEqual(directory.byId(AppEnv.prodHost)?.logo, "")
        XCTAssertNil(directory.byId(AppEnv.prodHost)?.toJSON()["logo"])
    }

    func testAnInstanceProjectFromBeforeLogosReadsAsAttached() throws {
        let relay = RelayProfile.fromJSON(["id": .string("relay.test"), "label": .string("Lab")])
        XCTAssertTrue(relay.hasInstance)
        XCTAssertEqual(relay.logo, "")
        XCTAssertNil(relay.toJSON()["hasInstance"])
    }

    func testALoopbackRelayStaysOnPlainHttpARemoteOneOnHttps() throws {
        let local = try XCTUnwrap(RelayProfile.tryParse("localhost:8787"))
        XCTAssertEqual(local.httpOrigin, "http://localhost:8787")
        XCTAssertTrue(local.isLoopback)
        XCTAssertTrue(local.subtitle.contains("on this network"))

        let remote = try XCTUnwrap(RelayProfile.tryParse("staging.forkmesh.com"))
        XCTAssertEqual(remote.httpOrigin, "https://staging.forkmesh.com")
        XCTAssertFalse(remote.isLoopback)
        XCTAssertEqual(remote.subtitle, "staging.forkmesh.com")
    }

    // MARK: Features

    func testEveryCoreFeatureIsOneARelayIsRefusedWithout() {
        XCTAssertFalse(RelayFeature.coreFeatures.isEmpty)
        for feature in RelayFeature.coreFeatures {
            let without = RelayFeatureSet.declared(RelayFeature.coreFeatures.subtracting([feature]))
            XCTAssertFalse(without.hasCore, feature.rawValue)
            XCTAssertEqual(without.missingCore, [feature])
        }
    }

    func testARelayThatSaysNothingIsAssumedToServeEverything() {
        let features = RelayFeatureSet.fromVersionBody(["ok": true, "rev": "abc1234"])
        XCTAssertFalse(features.isDeclared)
        for feature in RelayFeature.allCases { XCTAssertTrue(features.has(feature), feature.rawValue) }
    }

    func testARelayThatDeclaresIsTakenAtItsWordAndUnknownNamesAreIgnored() {
        let features = RelayFeatureSet.fromVersionBody(["features": ["accounts", "repositories", "repoRead", "issues", "teleportation"]])
        XCTAssertTrue(features.isDeclared)
        XCTAssertTrue(features.hasCore)
        XCTAssertTrue(features.has(.issues))
        XCTAssertFalse(features.has(.notifications))
        XCTAssertFalse(features.has(.push))
        XCTAssertTrue(RelayFeatureSet.fromVersionBody(["features": []]).isDeclared)
    }

    func testARelayWideRouteProvesItsFeatureAbsent() {
        XCTAssertEqual(RelayFeature.learnableFromPath("/api/notifications"), .notifications)
        XCTAssertEqual(RelayFeature.learnableFromPath("/api/accounts/push-test"), .push)
        XCTAssertEqual(RelayFeature.learnableFromPath("/api/sync"), .nodeSync)
        XCTAssertEqual(RelayFeature.learnableFromPath("/api/orgs/acme/members"), .orgs)
        XCTAssertEqual(RelayFeature.learnableFromPath("/api/tasks/42"), .plugins)
        XCTAssertEqual(RelayFeature.learnableFromPath("/api/notifications?limit=5"), .notifications)
    }

    func testARepoScoped404ProvesNothing() {
        for path in ["/api/repo/someone/thing/issues", "/api/repo/someone/thing/pulls",
                     "/api/repo/someone/thing/agents/list", "/api/repo/someone/thing/bounty"] {
            XCTAssertNil(RelayFeature.learnableFromPath(path), path)
        }
        XCTAssertNil(RelayFeature.learnableFromPath("/api/version"))
        XCTAssertNil(RelayFeature.learnableFromPath("/dashboard/settings"))
        XCTAssertNil(RelayFeature.learnableFromPath("/api/notificationsx"))
    }

    @MainActor
    func testALearnedGapSticksAndSurvivesARelaunch() async throws {
        let prefs = InMemoryKeyValueStore()
        let directory = RelayDirectory.load(prefs: prefs)
        directory.add(try XCTUnwrap(RelayProfile.tryParse("relay.test")))
        XCTAssertTrue(directory.byId("relay.test")?.features.has(.push) ?? false)
        directory.markFeatureMissing("relay.test", .push)
        XCTAssertFalse(directory.byId("relay.test")?.features.has(.push) ?? true)

        let reloaded = RelayDirectory.load(prefs: prefs)
        XCTAssertFalse(reloaded.byId("relay.test")?.features.has(.push) ?? true, "rediscovering a gap means failing every request again")
        XCTAssertTrue(reloaded.byId("relay.test")?.features.has(.notifications) ?? false, "one 404 says nothing about the others")
    }

    func testARelayThatDeclaredItsFeaturesIgnoresA404() {
        let declared = RelayFeatureSet.declared([.accounts, .repositories, .repoRead, .notifications])
        let after = declared.markMissing(.notifications)
        XCTAssertTrue(after.has(.notifications))
        XCTAssertEqual(after, declared)
    }

    func testAFeatureSetRoundTripsThroughJSONInBothStates() throws {
        let declared = RelayFeatureSet.declared([.accounts, .issues])
        let restored = RelayFeatureSet.fromJSON(try JSON.parse(JSON.encode(declared.jsonValue)))
        XCTAssertTrue(restored.isDeclared)
        XCTAssertTrue(restored.has(.issues))
        XCTAssertFalse(restored.has(.push))

        let learned = RelayFeatureSet.undeclared().markMissing(.push).markMissing(.orgs)
        XCTAssertEqual(learned.toJSON(), ["!push", "!orgs"], "declaration order")
        let restoredLearned = RelayFeatureSet.fromJSON(try JSON.parse(JSON.encode(learned.jsonValue)))
        XCTAssertFalse(restoredLearned.isDeclared)
        XCTAssertFalse(restoredLearned.has(.push))
        XCTAssertFalse(restoredLearned.has(.orgs))
        XCTAssertTrue(restoredLearned.has(.notifications))
        XCTAssertEqual(restoredLearned, learned)
        XCTAssertEqual(RelayFeatureSet.fromJSON(nil), .undeclared())
    }

    /// The wire names are a contract with app/src/app_runtime.py's RELAY_FEATURES.
    func testTheRelayAndTheAppAgreeOnEveryFeatureName() throws {
        let path = repoRoot().appendingPathComponent("app/src/app_runtime.py")
        guard let worker = try? String(contentsOf: path, encoding: .utf8),
              let start = worker.range(of: "RELAY_FEATURES = (") else {
            throw XCTSkip("the Worker does not declare RELAY_FEATURES yet")
        }
        let rest = worker[start.upperBound...]
        let block = rest[rest.startIndex..<(rest.firstIndex(of: ")") ?? rest.endIndex)]
        var declared = Set<String>()
        for part in block.split(separator: "\"").enumerated() where part.offset % 2 == 1 {
            let name = String(part.element)
            if !name.isEmpty, name.allSatisfy({ $0.isASCII && $0.isLetter }) { declared.insert(name) }
        }
        XCTAssertEqual(declared, Set(RelayFeature.allCases.map(\.wireName)))
    }
}
