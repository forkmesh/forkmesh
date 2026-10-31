// The SettingsService cases: appearance, secure store migration, production endpoint, world
// launcher contract and UDID, plus native/android/.../api/AccountSettingsTest.kt.

import FMBase
import FMCrypto
import XCTest
@testable import FMAccount

final class SettingsServiceTests: XCTestCase {
    // MARK: Production endpoint

    @MainActor
    func testAFreshInstallRelaysThroughProduction() async {
        let h = Harness()
        XCTAssertEqual(h.settings.defaultServerUrl, AppEnv.prodServerUrl)
        XCTAssertEqual(h.settings.serverUrl, "wss://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws")
        XCTAssertEqual(h.settings.server.http("/signup"), "https://app.forkmesh.com/signup")
        XCTAssertFalse(h.settings.hasServerUrlOverride)
    }

    @MainActor
    func testASavedLocalWorkerDefaultIsClearedOnce() async {
        let h = Harness(prefs: [SettingsService.serverUrlKey: .string(AppEnv.devServerUrl)])
        XCTAssertEqual(h.settings.serverUrl, AppEnv.prodServerUrl)
        XCTAssertNil(h.prefs.string(forKey: SettingsService.serverUrlKey))
        XCTAssertEqual(h.prefs.bool(forKey: SettingsService.localWorkerUrlMigrationKey), true)
    }

    @MainActor
    func testTheEmulatorAndLegacyRoomLocalDefaultsAreClearedToo() async {
        for stored in [
            SettingsService.androidEmulatorLocalWorkerServerUrl,
            "ws://localhost:8787/api/repo/mainnode/forkmesh/rooms/general/ws",
            "ws://10.0.2.2:8787/api/repo/mainnode/forkmesh/rooms/general/ws",
        ] {
            XCTAssertTrue(SettingsService.isRetiredLocalWorkerServerUrl(stored), stored)
            let h = Harness(prefs: [SettingsService.serverUrlKey: .string(stored)])
            XCTAssertEqual(h.settings.serverUrl, AppEnv.prodServerUrl, stored)
        }
    }

    @MainActor
    func testADeliberateRelayUrlSurvivesTheMigration() async {
        let custom = "ws://192.168.1.20:8787/api/repo/mainnode/forkmesh/rooms/world-general/ws"
        let h = Harness(prefs: [SettingsService.serverUrlKey: .string(custom)])
        XCTAssertEqual(h.settings.serverUrl, custom)
        XCTAssertFalse(SettingsService.isRetiredLocalWorkerServerUrl(custom))
        XCTAssertTrue(h.settings.hasServerUrlOverride)
        XCTAssertEqual(h.settings.server.http("/api/version"), "http://192.168.1.20:8787/api/version")
    }

    @MainActor
    func testALocalUrlChosenAfterTheMigrationIsKept() async {
        let h = Harness(prefs: [
            SettingsService.localWorkerUrlMigrationKey: true,
            SettingsService.serverUrlKey: .string(AppEnv.devServerUrl),
        ])
        XCTAssertEqual(h.settings.serverUrl, AppEnv.devServerUrl)
    }

    @MainActor
    func testAnEnvironmentChangeDropsARelayOfTheOtherEnvironment() async {
        // A dev build left a loopback URL and recorded its environment; a prod build drops it.
        let h = Harness(prefs: [
            SettingsService.localWorkerUrlMigrationKey: true,
            "server/urlEnv": "dev",
            SettingsService.serverUrlKey: "ws://localhost:9999/ws",
        ])
        XCTAssertEqual(h.settings.serverUrl, AppEnv.prodServerUrl)
        XCTAssertEqual(h.prefs.string(forKey: "server/urlEnv"), "prod")

        // The first launch that records an environment keeps what it finds.
        let first = Harness(prefs: [
            SettingsService.localWorkerUrlMigrationKey: true,
            SettingsService.serverUrlKey: "ws://localhost:9999/ws",
        ])
        XCTAssertEqual(first.settings.serverUrl, "ws://localhost:9999/ws")
    }

    func testCanonicalAppAndRetiredRoomDefaultsMigrate() {
        for alias in [
            "wss://forkmesh.com/api/repo/mainnode/forkmesh/rooms/general/ws",
            "wss://api.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws",
            "ws://forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws",
            "ws://forkmesh.com/api/repo/mainnode/forkmesh/rooms/general/ws",
            "ws://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws",
        ] {
            XCTAssertEqual(SettingsService.canonicalServerUrl(alias), AppEnv.prodServerUrl, alias)
        }
        XCTAssertEqual(
            SettingsService.canonicalServerUrl("ws://localhost:8787/api/repo/mainnode/forkmesh/rooms/general/ws"),
            AppEnv.devServerUrl)
        XCTAssertEqual(SettingsService.canonicalServerUrl("ws://relay.test/ws"), "ws://relay.test/ws")
        XCTAssertEqual(SettingsService.canonicalServerUrl("https://forkmesh.com/x"), "https://forkmesh.com/x")
        XCTAssertEqual(
            SettingsService.canonicalServerUrl("wss://forkmesh.com:8443/api/repo/mainnode/forkmesh/rooms/general/ws"),
            "wss://forkmesh.com:8443/api/repo/mainnode/forkmesh/rooms/world-general/ws")
    }

    @MainActor
    func testWorldUrlAlwaysPointsAtThePublicWorldSite() async {
        let h = Harness()
        for relay in [
            "wss://relay.example.test:9443/api/repo/private/name/rooms/main/ws?ticket=never-copy",
            "ws://127.0.0.1:8787/api/repo/mainnode/forkmesh/rooms/general/ws",
            "wss://user:secret@relay.example.test/api/world/ws",
            "file:///tmp/private",
        ] {
            XCTAssertEqual(h.settings.worldUriForServerUrl(relay)?.absoluteString, "https://world.forkmesh.com/")
        }
        XCTAssertEqual(h.settings.worldUri?.absoluteString, "https://world.forkmesh.com/")
    }

    @MainActor
    func testSetServerUrlAddsAndActivatesAndEmptyResets() async {
        let h = Harness()
        h.settings.setServerUrl("  ws://relay.test/ws ")
        XCTAssertEqual(h.settings.serverUrl, "ws://relay.test/ws")
        XCTAssertTrue(h.settings.hasServerUrlOverride)
        h.settings.setServerUrl("")
        XCTAssertEqual(h.settings.serverUrl, AppEnv.prodServerUrl)
        XCTAssertNotNil(h.settings.relays.byId("relay.test"), "a reset is a switch, not a forget")
    }

    // MARK: Secrets

    @MainActor
    func testMigratesIdentitySeedAndImportTokensOffPreferences() async throws {
        let seedB64 = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=" // 32 zero bytes
        let h = Harness(prefs: [
            Identity.seedKey: .string(seedB64),
            SettingsService.githubTokenKey: "ghp_legacy",
            SettingsService.gitlabTokenKey: "glpat_legacy",
            SettingsService.passphraseKey: "room-secret",
            "profile/name": "alice",
        ])
        let identity = try h.identity()

        XCTAssertFalse(identity.publicKeyB64url.isEmpty)
        XCTAssertEqual(h.settings.githubToken, "ghp_legacy")
        XCTAssertEqual(h.settings.gitlabToken, "glpat_legacy")
        XCTAssertEqual(h.settings.passphrase, "room-secret")
        XCTAssertEqual(h.settings.displayName, "alice")
        XCTAssertNil(h.prefs.string(forKey: Identity.seedKey))
        XCTAssertNil(h.prefs.string(forKey: SettingsService.githubTokenKey))
        XCTAssertNil(h.prefs.string(forKey: SettingsService.gitlabTokenKey))
        XCTAssertNil(h.prefs.string(forKey: SettingsService.passphraseKey))
        XCTAssertEqual(h.secure.read(SettingsService.githubTokenKey), "ghp_legacy")
        XCTAssertEqual(h.secure.read(Identity.seedKey), seedB64)
    }

    @MainActor
    func testClearSecretsRemovesSecureImportTokensAndPassphrase() async {
        let h = Harness(secure: [SettingsService.githubTokenKey: "ghp_x", SettingsService.passphraseKey: "secret"])
        XCTAssertEqual(h.settings.githubToken, "ghp_x")
        h.settings.clearSecrets()
        XCTAssertEqual(h.settings.githubToken, "")
        XCTAssertEqual(h.settings.passphrase, "")
        XCTAssertNil(h.secure.read(SettingsService.githubTokenKey))
    }

    @MainActor
    func testTheRetiredRoomPhraseIsDroppedAndTheLegacyPublicPassphraseIsBlank() async {
        let h = Harness(secure: [
            SettingsService.roomSealKey: "old manual phrase",
            SettingsService.passphraseKey: SettingsService.legacyPublicRoomPassphrase,
        ])
        XCTAssertNil(h.secure.read(SettingsService.roomSealKey))
        XCTAssertEqual(h.settings.passphrase, "")
        h.settings.setPassphrase(SettingsService.legacyPublicRoomPassphrase)
        XCTAssertNil(h.secure.read(SettingsService.passphraseKey))
        h.settings.setPassphrase("mine")
        XCTAssertEqual(h.secure.read(SettingsService.passphraseKey), "mine")
        h.settings.setGithubToken("t")
        h.settings.setGithubToken("")
        XCTAssertNil(h.secure.read(SettingsService.githubTokenKey))
    }

    // MARK: Appearance

    @MainActor
    func testAppearanceDefaultsToTheSystemThemeAndTheBlueAccent() async {
        let h = Harness()
        XCTAssertEqual(h.settings.themeMode, .system)
        XCTAssertEqual(h.settings.accent, .blue)
    }

    @MainActor
    func testLayoutModeDefaultsToControlTowerAndPersistsByName() async {
        let h = Harness()
        XCTAssertEqual(h.settings.layoutMode, .controlTower)
        h.settings.setLayoutMode(.focus)
        XCTAssertEqual(h.prefs.string(forKey: SettingsService.layoutModeKey), "focus")
        XCTAssertEqual(h.settings.layoutMode, .focus)
        h.prefs.set("cockpit", forKey: SettingsService.layoutModeKey)
        XCTAssertEqual(h.settings.layoutMode, .controlTower)
    }

    @MainActor
    func testAppearancePersistsChoicesByNameAndIgnoresUnknownValues() async {
        let h = Harness()
        h.settings.setThemeMode(.dark)
        h.settings.setAccent(.teal)
        XCTAssertEqual(h.prefs.string(forKey: SettingsService.themeModeKey), "dark")
        XCTAssertEqual(h.prefs.string(forKey: SettingsService.accentKey), "teal")
        XCTAssertEqual(h.settings.themeMode, .dark)
        XCTAssertEqual(h.settings.accent, .teal)

        h.prefs.set("sepia", forKey: SettingsService.themeModeKey)
        h.prefs.set("magenta", forKey: SettingsService.accentKey)
        XCTAssertEqual(h.settings.themeMode, .system)
        XCTAssertEqual(h.settings.accent, .blue)
        XCTAssertEqual(
            AccentChoice.allCases.map(\.label),
            ["Blue", "Cyan", "Teal", "Green", "Lime", "Amber", "Orange", "Red", "Pink", "Violet", "Purple", "Graphite"])
        h.settings.setAccent(.graphite)
        XCTAssertEqual(h.prefs.string(forKey: SettingsService.accentKey), "graphite")
        XCTAssertEqual(AccentChoice.fromName("orange"), .orange)
    }

    @MainActor
    func testBackgroundImageIsAFileNameAndDimIsClamped() async {
        let h = Harness()
        XCTAssertEqual(h.settings.backgroundImage, "")
        XCTAssertEqual(h.settings.backgroundDim, SettingsService.defaultBackgroundDim)

        h.settings.setBackgroundImage("wallpaper-1.jpg")
        XCTAssertEqual(h.prefs.string(forKey: SettingsService.backgroundImageKey), "wallpaper-1.jpg")
        XCTAssertEqual(h.settings.backgroundImage, "wallpaper-1.jpg")
        h.prefs.set("../escape.jpg", forKey: SettingsService.backgroundImageKey)
        XCTAssertEqual(h.settings.backgroundImage, "")
        h.settings.setBackgroundImage("")
        XCTAssertEqual(h.settings.backgroundImage, "")

        h.settings.setBackgroundDim(0.25)
        XCTAssertEqual(h.settings.backgroundDim, 0.25)
        h.settings.setBackgroundDim(3)
        XCTAssertEqual(h.settings.backgroundDim, SettingsService.maxBackgroundDim)
        h.settings.setBackgroundDim(-1)
        XCTAssertEqual(h.settings.backgroundDim, 0)
        h.prefs.set(Double.nan, forKey: SettingsService.backgroundDimKey)
        XCTAssertEqual(h.settings.backgroundDim, SettingsService.defaultBackgroundDim)
    }

    // MARK: Other preferences

    @MainActor
    func testBugButtonPlacementIsClampedAndRemembered() async {
        let h = Harness(prefs: [SettingsService.bugButtonAnchorKey: 0.1, SettingsService.bugButtonLeftKey: true])
        XCTAssertEqual(h.settings.bugButtonAnchor, 0.1)
        XCTAssertTrue(h.settings.bugButtonOnLeft)
        h.settings.setBugButtonPlacement(anchor: 3, onLeft: false)
        XCTAssertEqual(h.settings.bugButtonAnchor, 1)
        XCTAssertFalse(h.settings.bugButtonOnLeft)
        h.settings.setBugButtonPlacement(anchor: .nan, onLeft: false)
        XCTAssertEqual(h.settings.bugButtonAnchor, 0.72)
        XCTAssertEqual(Harness().settings.bugButtonAnchor, 0.72)
    }

    @MainActor
    func testTelemetryStripDefaultsOnAndIsRemembered() async {
        let prefs = InMemoryKeyValueStore()
        let settings = SettingsService.create(prefs: prefs, secureStore: InMemorySecureStore())
        XCTAssertTrue(settings.showTelemetryStrip)
        settings.setShowTelemetryStrip(false)
        XCTAssertEqual(prefs.bool(forKey: SettingsService.showTelemetryStripKey), false)
        XCTAssertFalse(SettingsService.create(prefs: prefs, secureStore: InMemorySecureStore()).showTelemetryStrip)
        settings.setShowTelemetryStrip(true)
        XCTAssertTrue(settings.showTelemetryStrip)
    }

    @MainActor
    func testAgentDotsDefaultOffAndIsRemembered() async {
        let prefs = InMemoryKeyValueStore()
        let settings = SettingsService.create(prefs: prefs, secureStore: InMemorySecureStore())
        XCTAssertFalse(settings.showAgentDots)
        settings.setShowAgentDots(true)
        XCTAssertEqual(prefs.bool(forKey: SettingsService.showAgentDotsKey), true)
        XCTAssertTrue(SettingsService.create(prefs: prefs, secureStore: InMemorySecureStore()).showAgentDots)
        settings.setShowAgentDots(false)
        XCTAssertFalse(settings.showAgentDots)
    }

    @MainActor
    func testANewProjectIsMarkedNewOnceAndSignInStartsOn() async {
        let prefs = InMemoryKeyValueStore()
        let settings = SettingsService.create(prefs: prefs, secureStore: InMemorySecureStore())
        XCTAssertFalse(settings.legacyProject)
        XCTAssertEqual(prefs.string(forKey: ProjectLegacyMarker.key), ProjectLegacyMarker.fresh)
        XCTAssertTrue(settings.signInOn)
        prefs.set("{}", forKey: "shell/bar-layout")
        XCTAssertFalse(SettingsService.create(prefs: prefs, secureStore: InMemorySecureStore()).legacyProject)
        settings.setSignInOn(false)
        XCTAssertEqual(prefs.bool(forKey: SettingsService.signInOnKey), false)
        XCTAssertFalse(settings.signInOn)
    }

    @MainActor
    func testANewProjectChoosesItsStarterPackOnce() async {
        let prefs = InMemoryKeyValueStore()
        let settings = SettingsService.create(prefs: prefs, secureStore: InMemorySecureStore())
        XCTAssertNil(settings.starterPack)
        XCTAssertTrue(settings.needsStarterPack)
        settings.setStarterPack("minimal")
        XCTAssertEqual(prefs.string(forKey: settings.scope.key(SettingsService.starterPackKey)), "minimal")
        XCTAssertEqual(settings.starterPack, "minimal")
        XCTAssertFalse(settings.needsStarterPack)
        let reopened = SettingsService.create(prefs: prefs, secureStore: InMemorySecureStore())
        XCTAssertEqual(reopened.starterPack, "minimal")
        XCTAssertFalse(reopened.needsStarterPack)
        XCTAssertTrue(reopened.signInOn)
    }

    @MainActor
    func testALegacyProjectIsNeverAskedForAStarterPack() async {
        let prefs = InMemoryKeyValueStore()
        prefs.set("{}", forKey: "shell/bar-uses")
        let settings = SettingsService.create(prefs: prefs, secureStore: InMemorySecureStore())
        XCTAssertTrue(settings.legacyProject)
        XCTAssertNil(settings.starterPack)
        XCTAssertFalse(settings.needsStarterPack)
    }

    @MainActor
    func testEarlierUseMarksTheProjectLegacyAndSignInStartsOn() async {
        let prefs = InMemoryKeyValueStore()
        prefs.set("{\"packages\":[]}", forKey: "packages/account/state")
        let settings = SettingsService.create(prefs: prefs, secureStore: InMemorySecureStore())
        XCTAssertTrue(settings.legacyProject)
        XCTAssertTrue(settings.signInOn)
        prefs.remove("packages/account/state")
        XCTAssertTrue(SettingsService.create(prefs: prefs, secureStore: InMemorySecureStore()).legacyProject)
    }

    func testASessionOrAProjectKeyIsEarlierUse() {
        let scope = ProjectScope(projectId: "app.forkmesh.com", namespace: "p/app.forkmesh.com/")
        let secure = InMemorySecureStore()
        let prefs = InMemoryKeyValueStore()
        prefs.set("{}", forKey: "packages/account/catalog")
        XCTAssertFalse(ProjectLegacyMarker.evidence(prefs: prefs, secure: secure, scope: scope))
        secure.write(AuthService.sessionKeyFor("app.forkmesh.com"), "{}")
        XCTAssertTrue(ProjectLegacyMarker.evidence(prefs: prefs, secure: secure, scope: scope))
        let other = InMemoryKeyValueStore()
        other.set("me", forKey: "profile/name")
        XCTAssertTrue(ProjectLegacyMarker.evidence(prefs: other, secure: InMemorySecureStore(), scope: scope))
        XCTAssertFalse(ProjectLegacyMarker.settle(prefs: prefs, secure: InMemorySecureStore(), scope: scope))
        XCTAssertEqual(prefs.string(forKey: "p/app.forkmesh.com/packages/legacy-project"), "new")
        XCTAssertFalse(ProjectLegacyMarker.settle(prefs: prefs, secure: secure, scope: scope))
    }

    @MainActor
    func testAProjectAddedOnThisPhoneStaysNewAfterSignIn() async throws {
        let prefs = InMemoryKeyValueStore()
        let secure = InMemorySecureStore()
        let directory = RelayDirectory.load(prefs: prefs)
        let named = try XCTUnwrap(directory.addNamed("Tea"))
        let added = directory.add(try XCTUnwrap(RelayProfile.tryParse("app.acme.dev")))
        for project in [named, added] {
            XCTAssertEqual(prefs.string(forKey: project.scope.key(ProjectLegacyMarker.key)), ProjectLegacyMarker.fresh)
            secure.write(AuthService.sessionKeyFor(project.id), "{}")
            XCTAssertTrue(ProjectLegacyMarker.evidence(prefs: prefs, secure: secure, scope: project.scope))
            XCTAssertFalse(ProjectLegacyMarker.settle(prefs: prefs, secure: secure, scope: project.scope))
        }
        directory.add(try XCTUnwrap(RelayProfile.tryParse("app.acme.dev")))
        prefs.set(ProjectLegacyMarker.legacy, forKey: added.scope.key(ProjectLegacyMarker.key))
        directory.add(try XCTUnwrap(RelayProfile.tryParse("app.acme.dev")))
        XCTAssertEqual(ProjectLegacyMarker.stored(prefs, scope: added.scope), true)
    }

    @MainActor
    func testSponsoredMessagesAndServeAsNode() async {
        let prefs = InMemoryKeyValueStore()
        let clock = FixedClock(epochMillis: 1_000_000)
        let settings = SettingsService.create(prefs: prefs, secureStore: InMemorySecureStore(), clock: clock)
        XCTAssertTrue(settings.showSponsoredMessages)
        XCTAssertFalse(settings.sponsoredMessagesHidden)
        settings.hideSponsoredMessages()
        XCTAssertTrue(settings.sponsoredMessagesHidden)
        XCTAssertEqual(prefs.int(forKey: SettingsService.sponsoredHiddenUntilKey), 1_000_000 + 7 * 86_400_000)
        clock.advance(by: 7 * 86_400 + 1)
        XCTAssertFalse(settings.sponsoredMessagesHidden)
        settings.setShowSponsoredMessages(false)
        XCTAssertFalse(settings.showSponsoredMessages)

        XCTAssertFalse(settings.serveAsNode)
        settings.setServeAsNode(true)
        XCTAssertEqual(prefs.bool(forKey: SettingsService.serveAsNodeKey), true)
        XCTAssertTrue(settings.serveAsNode)
        XCTAssertEqual(settings.room, "world-general")
    }

    @MainActor
    func testListenersHearSettingChangesAndRelaySwitches() async throws {
        let h = Harness()
        let counter = Counter()
        let before = h.settings.revision
        let token = h.settings.addListener { counter.increment() }
        h.settings.setDisplayName("alice")
        h.settings.relays.add(try XCTUnwrap(RelayProfile.tryParse("relay.test")))
        h.settings.relays.setActive("relay.test")
        XCTAssertEqual(counter.value, 3, "relay changes look like settings changes")
        XCTAssertEqual(h.settings.revision, before + 3)
        XCTAssertEqual(h.settings.serverUrl, AppEnv.relayUrlForHost("relay.test"))
        h.settings.removeListener(token)
        h.settings.setRoom("x")
        XCTAssertEqual(counter.value, 3)
        XCTAssertTrue(h.settings.supports(.issues))
        h.settings.relays.markFeatureMissing("relay.test", .issues)
        XCTAssertFalse(h.settings.supports(.issues))
    }

    // MARK: iOS UDID

    static let nonce = "abcdefghijklmnop0123456789_-XYZ"

    func link(_ nonce: String) -> IosUdidLink {
        IosUdidLink(udid: "00008030-001A2D3C3E3A802E", nonce: nonce, product: "iPhone15,2", version: "22A3354")
    }

    @MainActor
    func testUdidSavesOnlyTheAnswerToTheReadItStartedOnce() async {
        let settings = Harness().settings
        XCTAssertFalse(settings.acceptIosUdidLink(link(Self.nonce)))
        XCTAssertEqual(settings.iosUdid, "")

        let nonce = settings.beginIosUdidRead()
        XCTAssertFalse(settings.acceptIosUdidLink(link(Self.nonce)))
        XCTAssertTrue(settings.acceptIosUdidLink(link(nonce)))
        XCTAssertEqual(settings.iosUdid, "00008030-001A2D3C3E3A802E")
        XCTAssertEqual(settings.iosUdidProduct, "iPhone15,2")
        XCTAssertEqual(settings.iosUdidVersion, "22A3354")
        // The nonce is spent: replaying the same link is ignored.
        XCTAssertFalse(settings.acceptIosUdidLink(link(nonce)))
    }

    @MainActor
    func testUdidANewReadSupersedesAnUnfinishedOne() async {
        let settings = Harness().settings
        let first = settings.beginIosUdidRead()
        let second = settings.beginIosUdidRead()
        XCTAssertFalse(settings.acceptIosUdidLink(link(first)))
        XCTAssertTrue(settings.acceptIosUdidLink(link(second)))
    }
}

/// IosUdidLink and the legal links.
final class IosUdidLinkTests: XCTestCase {
    let nonce = "abcdefghijklmnop0123456789_-XYZ"

    func link(_ query: String) -> URL { URL(string: "forkmesh://ios-udid?\(query)")! }

    func testReadsThePageHandBackAndUpperCasesTheUdid() throws {
        let link = try XCTUnwrap(IosUdidLink.tryParse(link(
            "udid=00008030-001a2d3c3e3a802e&product=iPhone15%2C2&version=22A3354&n=\(nonce)")))
        XCTAssertEqual(link.udid, "00008030-001A2D3C3E3A802E")
        XCTAssertEqual(link.product, "iPhone15,2")
        XCTAssertEqual(link.version, "22A3354")
        XCTAssertEqual(link.nonce, nonce)
    }

    func testAcceptsLegacy40HexUdids() {
        XCTAssertEqual(IosUdidLink.tryParse(link("udid=\(String(repeating: "a", count: 40))&n=\(nonce)"))?.udid,
                       String(repeating: "A", count: 40))
    }

    func testRejectsOtherLinksBadUdidsAndMissingNonces() {
        let a40 = String(repeating: "a", count: 40)
        for url in [
            URL(string: "https://app.forkmesh.com/api/ios-udid?udid=\(a40)&n=\(nonce)")!,
            URL(string: "forkmesh://other?udid=\(a40)&n=\(nonce)")!,
            link("udid=1234&n=\(nonce)"),
            link("udid=%3Cscript%3E&n=\(nonce)"),
            link("udid=\(a40)"),
            link("udid=\(a40)&n=short"),
        ] {
            XCTAssertNil(IosUdidLink.tryParse(url), url.absoluteString)
        }
    }

    func testDropsAMalformedModelOrBuildButKeepsTheUdid() throws {
        let link = try XCTUnwrap(IosUdidLink.tryParse(link(
            "udid=\(String(repeating: "b", count: 40))&product=iPhone%2015&version=\(String(repeating: "9", count: 40))&n=\(nonce)")))
        XCTAssertEqual(link.product, "")
        XCTAssertEqual(link.version, "")
    }

    func testNoncesSatisfyTheServerPatternAndDifferPerRead() {
        let a = IosUdidLink.newNonce()
        let b = IosUdidLink.newNonce()
        XCTAssertTrue(IosUdidLink.isNonce(a))
        XCTAssertTrue(IosUdidLink.isNonce(b))
        XCTAssertNotEqual(a, b)
        XCTAssertEqual(IosUdidLink.newNonce([UInt8](repeating: 0xff, count: 24)), String(repeating: "_", count: 32))
    }

    func testPageUriFollowsTheRelayOrigin() {
        XCTAssertEqual(
            IosUdidLink.pageUri("wss://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws", nonce),
            "https://app.forkmesh.com/api/ios-udid?n=\(nonce)")
        XCTAssertEqual(IosUdidLink.pageUri("ws://localhost:8787/x", nonce), "http://localhost:8787/api/ios-udid?n=\(nonce)")
        XCTAssertEqual(IosUdidLink.pageUri("", nonce), "https://app.forkmesh.com/api/ios-udid?n=\(nonce)")
    }

    func testLegalLinks() {
        XCTAssertEqual(LegalLinks.terms.absoluteString, "https://forkmesh.com/terms")
        XCTAssertEqual(LegalLinks.privacy.absoluteString, "https://forkmesh.com/privacy")
        XCTAssertEqual(LegalLinks.appleHealth.absoluteString, "https://forkmesh.com/privacy#apple-health")
        XCTAssertEqual(LegalLinks.design.absoluteString, "https://forkmesh.com/design")
        XCTAssertEqual(LegalLinks.desktop.absoluteString, "https://forkmesh.com/desktop")
        XCTAssertEqual(
            LegalLinks.designFor(URL(string: "https://next.forkmesh.com/start?x=1#top")!).absoluteString,
            "https://next.forkmesh.com/design")
    }
}
