import FMBase
import FMCrypto
import Foundation
import XCTest
@testable import FMAccount

@MainActor
final class FakeAccountEndpoints: AccountEndpointCalling {
    var serverOrigin: String = "https://app.forkmesh.com"
    var calls: [(method: String, path: String, body: JSONObject?)] = []
    var answers: [String: Result<JSONObject, AccountEndpointError>] = [:]

    func accountCall(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject {
        calls.append((method, path, body))
        switch answers["\(method) \(path)"] ?? .success(["ok": true]) {
        case .success(let map): return map
        case .failure(let error): throw error
        }
    }
}

final class SshKeyCheckTests: XCTestCase {
    static let ed25519 = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl alice@laptop"

    func testAcceptsAnEd25519LineAndSplitsTheComment() {
        guard case let .ok(type, line, comment) = SshKeys.check(Self.ed25519) else { return XCTFail("not ok") }
        XCTAssertEqual(type, "ssh-ed25519")
        XCTAssertFalse(line.contains("alice"))
        XCTAssertEqual(comment, "alice@laptop")
    }

    func testRefusesPrivateKeys() {
        XCTAssertEqual(SshKeys.check("-----BEGIN OPENSSH PRIVATE KEY-----\nabc\n-----END OPENSSH PRIVATE KEY-----"), .privateKey)  // forkmesh-secret-scan:ignore-line
        XCTAssertEqual(SshKeys.check("PuTTY-User-Key-File-3: ssh-ed25519"), .privateKey)
    }

    func testRefusesTwoKeysUnknownTypesAndMismatchedBlobs() {
        XCTAssertEqual(SshKeys.check(Self.ed25519 + "\n" + Self.ed25519), .multipleKeys)
        XCTAssertEqual(SshKeys.check("ssh-dss AAAAB3NzaC1kc3M="), .unsupportedType("ssh-dss"))
        XCTAssertEqual(SshKeys.check("ssh-rsa AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl"), .malformed)
        XCTAssertEqual(SshKeys.check("ssh-ed25519 not-base64!"), .malformed)
        XCTAssertEqual(SshKeys.check("  "), .empty)
    }

    func testLabelPrefersTypedTextAndIsBounded() {
        XCTAssertEqual(SshKeys.label("  work  ", comment: "c"), "work")
        XCTAssertEqual(SshKeys.label("", comment: "alice@laptop"), "alice@laptop")
        XCTAssertEqual(SshKeys.label(String(repeating: "x", count: 200), comment: "").count, 80)
    }

    func testKeyIdsAndPageParsing() {
        XCTAssertTrue(SshKeys.isKeyId("sk_abcdefghijklmnop"))
        XCTAssertFalse(SshKeys.isKeyId("sk_short"))
        XCTAssertFalse(SshKeys.isKeyId("sk_abcdefghijklmnop/../x"))
        let page = SshKeysPage.parse([
            "keys": [["id": "sk_abcdefghijklmnop", "label": "", "keyType": "ssh-ed25519", "fingerprint": "SHA256:x"],
                     ["id": "bad"]],
            "maximumKeys": 1, "gatewayConfigured": true, "gatewayHost": "git.example.com", "gatewayPort": 2222,
        ])
        XCTAssertEqual(page.keys.count, 1)
        XCTAssertEqual(page.keys[0].title, "Ed25519 key")
        XCTAssertTrue(page.atLimit)
        XCTAssertEqual(page.gatewayLine, "ssh://git@git.example.com:2222")
    }

    @MainActor
    func testModelAddsWithTheCleanLineAndRemovesByOwnPath() async {
        let fake = FakeAccountEndpoints()
        fake.answers["GET /api/accounts/ssh-keys"] = .success(["keys": [["id": "sk_abcdefghijklmnop", "keyType": "ssh-rsa"]]])
        let model = SshKeysModel(client: fake)
        model.draftKey = Self.ed25519
        XCTAssertTrue(model.canAdd)
        let added = await model.add()
        XCTAssertTrue(added)
        let post = fake.calls.first { $0.method == "POST" }
        XCTAssertEqual(post?.body?.str("label"), "alice@laptop")
        XCTAssertFalse(post?.body?.str("publicKey").contains("alice") ?? true)
        XCTAssertEqual(model.page?.keys.count, 1)
        XCTAssertEqual(model.draftKey, "")
        let removed = await model.remove("sk_abcdefghijklmnop")
        XCTAssertTrue(removed)
        XCTAssertEqual(fake.calls.last?.path, "/api/accounts/ssh-keys/sk_abcdefghijklmnop")
        XCTAssertEqual(model.page?.keys.count, 0)
        let refused = await model.remove("../../etc")
        XCTAssertFalse(refused)
    }

    @MainActor
    func testModelMapsServerErrors() async {
        let fake = FakeAccountEndpoints()
        fake.answers["POST /api/accounts/ssh-keys"] = .failure(AccountEndpointError("ssh_key_already_registered", status: 409))
        let model = SshKeysModel(client: fake)
        model.draftKey = Self.ed25519
        let added = await model.add()
        XCTAssertFalse(added)
        XCTAssertTrue(model.error.contains("already registered"))
        XCTAssertEqual(model.draftKey, Self.ed25519)
    }
}

final class McpConnectorTests: XCTestCase {
    static let tokenId = String(repeating: "a", count: 32)
    static let token = "fmbot_\(tokenId)_\(String(repeating: "B", count: 43))"

    func testTokenShapesAndURLs() {
        XCTAssertTrue(McpConnector.isToken(Self.token))
        XCTAssertFalse(McpConnector.isToken("fmbot_x_y"))
        XCTAssertEqual(McpConnector.serverURL(origin: "https://app.forkmesh.com/"), "https://app.forkmesh.com/mcp")
        XCTAssertEqual(McpConnector.path("Acme Co/../x"), "/api/orgs/acmeco..x/bot-tokens")
        XCTAssertTrue(McpConnector.mask(Self.token).hasPrefix("fmbot_aaaaaa"))
        XCTAssertFalse(McpConnector.mask(Self.token).contains("BBBBBBBBBB"))
        XCTAssertTrue(McpConnector.configJson(serverURL: "https://h/mcp", token: "t").contains("\"Bearer t\""))
        XCTAssertEqual(McpConnector.clampDays(0), 1)
        XCTAssertEqual(McpConnector.clampDays(999), 365)
    }

    func testRowStatus() {
        let row = McpTokenRow(id: Self.tokenId, provider: "codex", label: "", scopes: McpConnector.taskScopes,
                              expiresAt: 10 * 86_400_000)
        XCTAssertEqual(row.title, "Codex")
        XCTAssertTrue(row.tasksOnly)
        XCTAssertEqual(row.status(nowMillis: 0), "Expires in 10 days")
        XCTAssertEqual(row.status(nowMillis: 11 * 86_400_000), "Expired")
        XCTAssertFalse(row.isActive(nowMillis: 11 * 86_400_000))
    }

    @MainActor
    func testMintAsksForTaskScopesOnlyAndRotateRevokesTheOldToken() async {
        let fake = FakeAccountEndpoints()
        let path = McpConnector.path("acme")
        fake.answers["POST \(path)"] = .success(["token": .string(Self.token), "tokenId": .string(Self.tokenId), "expiresAt": 5])
        let model = McpConnectorModel(org: "acme", client: fake, clock: FixedClock(Date(timeIntervalSince1970: 0)))
        XCTAssertEqual(model.serverURL, "https://app.forkmesh.com/mcp")
        let minted = await model.mint()
        XCTAssertEqual(minted?.token, Self.token)
        let body = fake.calls.first { $0.method == "POST" }?.body
        XCTAssertEqual(body?.strings("scopes"), McpConnector.taskScopes)
        XCTAssertEqual(body?.int("expiresDays"), 90)
        let old = McpTokenRow(id: String(repeating: "c", count: 32), provider: "codex", label: "old", scopes: [])
        let rotated = await model.rotate(old)
        XCTAssertNotNil(rotated)
        let delete = fake.calls.first { $0.method == "DELETE" }
        XCTAssertEqual(delete?.body?.str("tokenId"), old.id)
        XCTAssertEqual(model.minted?.token, Self.token)
    }

    @MainActor
    func testMintRejectsAMalformedAnswer() async {
        let fake = FakeAccountEndpoints()
        fake.answers["POST \(McpConnector.path("acme"))"] = .success(["token": "nope"])
        let model = McpConnectorModel(org: "acme", client: fake)
        let minted = await model.mint()
        XCTAssertNil(minted)
        XCTAssertNil(model.minted)
        XCTAssertFalse(model.error.isEmpty)
    }
}

final class RecoveryKitTests: XCTestCase {
    func testPassphraseRules() {
        XCTAssertEqual(RecoveryKit.validatePassphrase("short", confirm: "short"), .shortPassphrase)
        XCTAssertEqual(RecoveryKit.validatePassphrase("long enough pass", confirm: "other"), .mismatchedPassphrase)
        XCTAssertNil(RecoveryKit.validatePassphrase("long enough pass", confirm: "long enough pass"))
        XCTAssertEqual(RecoveryKit.strength("abc"), 0)
        XCTAssertEqual(RecoveryKit.strength("Correct Horse Battery 9"), 3)
    }

    func testKitRoundTripsAndMatchesThisDevice() throws {
        let identity = try Identity.fromSeed(Array(repeating: 7, count: 32))
        let kit = try RecoveryKit.export(identity: identity, passphrase: "a long passphrase", confirm: "a long passphrase", createdAt: 1_000)
        XCTAssertFalse(kit.contains(B64.encodeUrlNoPadding(Array(repeating: UInt8(7), count: 32))))
        let check = try RecoveryKit.check(kit: kit, passphrase: "a long passphrase", currentPublicKey: identity.publicKeyB64url)
        XCTAssertTrue(check.matchesThisDevice)
        XCTAssertEqual(check.createdAt, 1_000)
        let other = try RecoveryKit.check(kit: kit, passphrase: "a long passphrase", currentPublicKey: "someone-else")
        XCTAssertFalse(other.matchesThisDevice)
        XCTAssertThrowsError(try RecoveryKit.check(kit: kit, passphrase: "the wrong passphrase", currentPublicKey: ""))
        XCTAssertThrowsError(try RecoveryKit.check(kit: "{}", passphrase: "a long passphrase", currentPublicKey: ""))
    }

    func testFileNameAndBackupNag() {
        XCTAssertEqual(RecoveryKit.fileName(account: "Alice!", createdAt: 0), "forkmesh-alice-recovery-kit-1970-01-01.json")
        let prefs = InMemoryKeyValueStore()
        let store = IdentityBackupStore(prefs: prefs)
        XCTAssertTrue(store.state.showsNag(currentKey: "k1", nowMillis: 0))
        store.snooze(nowMillis: 0)
        XCTAssertFalse(store.state.showsNag(currentKey: "k1", nowMillis: 1_000))
        XCTAssertTrue(store.state.showsNag(currentKey: "k1", nowMillis: IdentityBackupStore.snoozeMillis))
        store.markBackedUp(publicKey: "k1", at: 5)
        XCTAssertFalse(store.state.needsBackup(currentKey: "k1"))
        XCTAssertTrue(store.state.needsBackup(currentKey: "k2"))
        XCTAssertEqual(store.state.backedUpAt, 5)
    }
}

final class NodeLinkTests: XCTestCase {
    static let url = "https://app.forkmesh.com/dashboard?link_node=alice-laptop&link_ts=1000000&link_sig=abc_-DEF"

    func testParsesTheDesktopsGrantLink() {
        guard case let .grant(grant) = NodeLink.parse(Self.url) else { return XCTFail("no grant") }
        XCTAssertEqual(grant.node, "alice-laptop")
        XCTAssertEqual(grant.host, "app.forkmesh.com")
        XCTAssertEqual(grant.body(user: "Alice"), ["nodeName": "alice-laptop", "user": "alice", "ts": "1000000", "sig": "abc_-DEF"])
        XCTAssertTrue(grant.isFresh(nowMillis: 1_000_000 + 60_000))
        XCTAssertFalse(grant.isFresh(nowMillis: 1_000_000 + NodeLink.maxSkewMillis + 1))
    }

    func testRejectsJunk() {
        XCTAssertEqual(NodeLink.parse("123456"), .installerCode("123456"))
        if case .grant = NodeLink.parse("https://x/dashboard?link_node=Bad%20Name&link_ts=1&link_sig=s") { XCTFail("accepted") }
        if case .grant = NodeLink.parse("https://x/dashboard?link_node=a&link_ts=soon&link_sig=s") { XCTFail("accepted") }
        if case .grant = NodeLink.parse("javascript:alert(1)?link_node=a&link_ts=1&link_sig=s") { XCTFail("accepted") }
        if case .grant = NodeLink.parse("https://x/") { XCTFail("accepted") }
    }

    func testHostAliases() {
        guard case let .grant(grant) = NodeLink.parse(Self.url) else { return XCTFail("no grant") }
        XCTAssertTrue(NodeLink.hostMatches(grant, serverOrigin: "https://forkmesh.com"))
        XCTAssertTrue(NodeLink.hostMatches(grant, serverOrigin: "https://app.next.forkmesh.com"))
        XCTAssertFalse(NodeLink.hostMatches(grant, serverOrigin: "https://relay.example.com"))
    }

    @MainActor
    func testConfirmPostsTheGrantAsTheSignedInUser() async {
        let fake = FakeAccountEndpoints()
        fake.answers["POST /api/accounts/link-grant"] = .success(["linked": true, "nodeId": "alice-laptop", "nodes": ["alice-laptop"]])
        let model = NodeLinkModel(client: fake, user: { "alice" }, clock: FixedClock(Date(timeIntervalSince1970: 1_000)))
        model.review(Self.url)
        XCTAssertNotNil(model.pending)
        let outcome = await model.confirm()
        XCTAssertEqual(outcome, .linked(node: "alice-laptop", nodes: ["alice-laptop"]))
        XCTAssertEqual(fake.calls.first?.body?.str("user"), "alice")
        XCTAssertNil(model.pending)
    }

    @MainActor
    func testStaleOrForeignLinksAreNotSent() async {
        let fake = FakeAccountEndpoints()
        let stale = NodeLinkModel(client: fake, user: { "alice" }, clock: FixedClock(Date(timeIntervalSince1970: 10_000)))
        stale.review(Self.url)
        XCTAssertNil(stale.pending)
        XCTAssertTrue(stale.error.contains("expired"))
        fake.serverOrigin = "https://relay.example.com"
        let foreign = NodeLinkModel(client: fake, user: { "alice" }, clock: FixedClock(Date(timeIntervalSince1970: 1_000)))
        foreign.review(Self.url)
        XCTAssertNil(foreign.pending)
        let outcome = await foreign.confirm()
        XCTAssertNil(outcome)
        XCTAssertTrue(fake.calls.isEmpty)
    }
}

final class AccountDataTests: XCTestCase {
    func testExportDropsSecretsAndNamesTheFile() throws {
        let file = try AccountExport.file([
            "ok": true, "schema": "forkmesh-account-export-v1",
            "account": ["name": "alice", "pass_hash": "x", "totp_secret": "y"],
            "devices": [["id": "d"]], "sshKeys": [],
        ], account: "Alice", nowMillis: 0)
        XCTAssertEqual(file.fileName, "forkmesh-alice-export-1970-01-01.json")
        XCTAssertFalse(file.json.contains("pass_hash"))
        XCTAssertFalse(file.json.contains("totp_secret"))
        XCTAssertFalse(file.json.contains("\"ok\""))
        XCTAssertEqual(file.devices, 1)
        XCTAssertThrowsError(try AccountExport.file(["schema": "other"], account: "a", nowMillis: 0))
    }

    @MainActor
    func testExportOnAnOldServerSaysSo() async {
        let fake = FakeAccountEndpoints()
        fake.answers["GET /api/accounts/export"] = .failure(AccountEndpointError("http_404", status: 404))
        let model = AccountDataModel(client: fake)
        let file = await model.prepareExport(account: "alice")
        XCTAssertNil(file)
        XCTAssertTrue(model.error.contains("cannot export"))
    }

    @MainActor
    func testSecurityReportValidatesThenPosts() async {
        let fake = FakeAccountEndpoints()
        let model = AccountDataModel(client: fake)
        let empty = await model.sendReport()
        XCTAssertFalse(empty)
        XCTAssertTrue(fake.calls.isEmpty)
        model.report = SecurityReportDraft(title: " XSS ", body: "steps", component: "website", contact: "me@example.com")
        let sent = await model.sendReport()
        XCTAssertTrue(sent)
        XCTAssertEqual(fake.calls.first?.path, "/api/security/report")
        XCTAssertEqual(fake.calls.first?.body?.str("title"), "XSS")
        XCTAssertEqual(model.report, SecurityReportDraft())
        XCTAssertTrue(model.reportSent)
    }
}

final class AccountDevicesTests: XCTestCase {
    func testRecentAccountsAreBoundedDedupedAndNewestFirst() {
        let store = RecentAccountsStore(prefs: InMemoryKeyValueStore())
        for index in 0..<12 { store.record(name: "user\(index)", email: "", at: Int64(index)) }
        store.record(name: "USER3", email: "u3@example.com", at: 100)
        store.record(name: "preview-node", email: "", at: 200)
        XCTAssertEqual(store.accounts.count, RecentAccountsStore.limit)
        XCTAssertEqual(store.accounts.first?.name, "USER3")
        XCTAssertEqual(store.others(than: "user3").first?.name, "user11")
        store.forget("user3")
        XCTAssertFalse(store.accounts.contains { $0.id == "user3" })
    }

    func testDesktopNicknames() {
        let names = DesktopNicknames(prefs: InMemoryKeyValueStore())
        XCTAssertEqual(names.name(for: "abcdefghijklmnopq", fallback: ""), "Desktop abcdef")
        XCTAssertEqual(names.name(for: "k", fallback: "studio"), "studio")
        names.rename("k", to: "  Office\nPC  ")
        XCTAssertEqual(names.name(for: "k", fallback: "studio"), "OfficePC")
        names.rename("k", to: " ")
        XCTAssertEqual(names.name(for: "k", fallback: "studio"), "studio")
        names.rename("a", to: "A")
        names.rename("b", to: "B")
        names.prune(keeping: ["a"])
        XCTAssertEqual(names.all, ["a": "A"])
    }

    func testTrustListSummaryRejectsGarbage() {
        if case .success = TrustListSummary.summarize("{}") { XCTFail("accepted") }
        if case .success = TrustListSummary.summarize("not json") { XCTFail("accepted") }
    }
}

final class SettingsCatalogTests: XCTestCase {
    func testSearchMatchesTitlesAndKeywords() {
        let all = Set(SettingsSectionId.allCases)
        XCTAssertEqual(SettingsCatalog.visible("ssh", available: all).map(\.id), [.sshKeys])
        XCTAssertTrue(SettingsCatalog.visible("DARK", available: all).map(\.id).contains(.general))
        XCTAssertTrue(SettingsCatalog.visible("api token", available: all).map(\.id).contains(.mcp))
        XCTAssertEqual(SettingsCatalog.visible("", available: [.legal, .data]).map(\.id), [.data, .legal])
        XCTAssertTrue(SettingsCatalog.visible("zzz", available: all).isEmpty)
        XCTAssertEqual(SettingsCatalog.entries.count, SettingsSectionId.allCases.count)
        for word in ["sound", "Sounds", "mute", "silent", "audio", "volume", "genie", "Spider"] {
            XCTAssertTrue(SettingsCatalog.visible(word, available: all).map(\.id).contains(.general), word)
        }
    }

    func testSectionNamesFromRoutes() {
        XCTAssertEqual(SettingsCatalog.section(named: "sshKeys"), .sshKeys)
        XCTAssertEqual(SettingsCatalog.section(named: "ssh-keys"), .sshKeys)
        XCTAssertEqual(SettingsCatalog.section(named: "apps"), .apps)
        XCTAssertEqual(SettingsCatalog.section(named: "installed-apps"), .apps)
        XCTAssertEqual(SettingsCatalog.section(named: "sounds"), .general)
        XCTAssertEqual(SettingsCatalog.visible("slash commands", available: Set(SettingsSectionId.allCases)).map(\.id), [.apps])
        XCTAssertEqual(SettingsCatalog.section(named: "chat-privacy"), .chatPrivacy)
        XCTAssertEqual(SettingsCatalog.section(named: "face-id"), .chatPrivacy)
        for word in ["app switcher", "Face ID", "lock"] {
            XCTAssertTrue(
                SettingsCatalog.visible(word, available: Set(SettingsSectionId.allCases)).map(\.id).contains(.chatPrivacy), word)
        }
        XCTAssertNil(SettingsCatalog.section(named: "nope"))
        XCTAssertNil(SettingsCatalog.section(named: nil))
    }

    func testQuickSetupSteps() {
        var input = QuickSetupInput(signedIn: true, projectName: "ForkMesh")
        XCTAssertEqual(QuickSetup.remaining(input), 3)
        XCTAssertEqual(QuickSetup.next(input)?.id, .desktop)
        input.desktopsPaired = 2
        input.identityBackedUp = true
        input.pushEnabled = true
        XCTAssertNil(QuickSetup.next(input))
        XCTAssertEqual(QuickSetup.steps(QuickSetupInput(signedIn: true, guest: true)).first?.done, false)
    }
}

final class DevicePreferenceTests: XCTestCase {
    @MainActor
    func testDevicePreferences() async {
        let harness = Harness()
        let settings = harness.settings
        XCTAssertTrue(settings.hapticsEnabled)
        settings.setHapticsEnabled(false)
        XCTAssertFalse(SettingsService.hapticsEnabled(harness.prefs))
        settings.setDictationLocale("en-GB;drop")
        XCTAssertEqual(settings.dictationLocale, "en-GBdrop")
        settings.setDictationLocale("")
        XCTAssertEqual(settings.dictationLocale, "")
        XCTAssertTrue(settings.dictatesOnOpen)
        settings.setDictatesOnOpen(false)
        XCTAssertFalse(SettingsService.dictatesOnOpen(harness.prefs))
        settings.setDictatesOnOpen(true)
        XCTAssertTrue(SettingsService.dictatesOnOpen(harness.prefs))
        XCTAssertEqual(settings.localDefaultBranch, "main")
        XCTAssertFalse(settings.setLocalDefaultBranch("bad..name"))
        XCTAssertTrue(settings.setLocalDefaultBranch("trunk"))
        XCTAssertEqual(SettingsService.localDefaultBranch(harness.prefs), "trunk")
        XCTAssertNil(SettingsService.localAuthorOverride(harness.prefs))
        settings.setLocalAuthorName("Alice <x>")
        settings.setLocalAuthorEmail("alice@example.com")
        XCTAssertEqual(SettingsService.localAuthorOverride(harness.prefs)?.name, "Alice x")
        XCTAssertTrue(SettingsService.isPlausibleEmail("a@b.co"))
        XCTAssertFalse(SettingsService.isPlausibleEmail("a@b"))
    }

    @MainActor
    func testSoundsOnUnlessTheUserSwitchesThemOff() async {
        let harness = Harness()
        XCTAssertTrue(harness.settings.soundsEnabled)
        for key in SettingsService.retiredSoundsKeys {
            harness.prefs.set(false, forKey: key)
        }
        XCTAssertTrue(SettingsService.soundsEnabled(harness.prefs))
        SettingsService.retireStoredSounds(harness.prefs)
        for key in SettingsService.retiredSoundsKeys {
            XCTAssertFalse(harness.prefs.contains(key))
        }
        XCTAssertTrue(SettingsService.soundsEnabled(harness.prefs))
        harness.settings.setSoundsEnabled(false)
        XCTAssertFalse(harness.settings.soundsEnabled)
        SettingsService.retireStoredSounds(harness.prefs)
        XCTAssertFalse(SettingsService.soundsEnabled(harness.prefs))
        harness.settings.setSoundsEnabled(true)
        XCTAssertTrue(harness.settings.soundsEnabled)
        XCTAssertFalse(harness.prefs.contains(SettingsService.soundsOffKey))
    }
}

final class AccountProfileTests: XCTestCase {
    func testParsesAUserWithNodes() {
        let profile = AccountProfile.parse([
            "exists": true, "name": "alice", "kind": "user", "status": "active", "nodes": ["alice-laptop", "Bad Node"],
            "pubkey": "pk", "followers": 3,
        ], requested: "Alice")
        XCTAssertEqual(profile.nodes, ["alice-laptop"])
        XCTAssertEqual(profile.linkedNodesLine, "1 node linked")
        XCTAssertTrue(profile.canLinkNode(viewer: "ALICE"))
        XCTAssertFalse(profile.canLinkNode(viewer: "bob"))
        XCTAssertEqual(profile.identityQRText, "forkmesh:identity/alice/pk")
    }

    func testTipAddressLinksAndAvatar() {
        let profile = AccountProfile.parse([
            "exists": true, "name": "alice", "kind": "user", "avatarPng": "iVBOR",
            "solana": "So11111111111111111111111111111111111111112",
            "mastodon": "@alice@x.social", "mastodonUrl": "https://x.social/@alice",
            "profileLinks": [
                ["label": "Blog", "url": "https://alice.dev", "verified": true],
                ["label": "", "url": "https://two.dev", "domain": "two.dev"],
                ["label": "Bad", "url": "javascript:alert(1)"],
            ],
        ], requested: "alice")
        XCTAssertEqual(profile.tipAddress, "So11111111111111111111111111111111111111112")
        XCTAssertEqual(profile.walletQRText, "solana:So11111111111111111111111111111111111111112")
        XCTAssertEqual(profile.avatar, "iVBOR")
        XCTAssertEqual(profile.links, [
            ProfileLink(label: "Blog", url: "https://alice.dev", verified: true),
            ProfileLink(label: "two.dev", url: "https://two.dev", verified: false),
        ])
        XCTAssertEqual(profile.mastodonUrl, "https://x.social/@alice")
        let junk = AccountProfile.parse(["exists": true, "name": "bob", "solana": "0OIl", "mastodonUrl": "ftp://x"], requested: "bob")
        XCTAssertEqual(junk.tipAddress, "")
        XCTAssertEqual(junk.walletQRText, "")
        XCTAssertEqual(junk.mastodonUrl, "")
    }

    func testPeopleProfileKeepsOnlyAValidTipAddress() {
        XCTAssertEqual(
            PeopleProfile.parse(["solana": "So11111111111111111111111111111111111111112"], requested: "a").tipAddress,
            "So11111111111111111111111111111111111111112")
        XCTAssertEqual(PeopleProfile.parse(["solana": "not an address"], requested: "a").tipAddress, "")
    }

    func testPrivateAndNodeProfiles() {
        XCTAssertTrue(AccountProfile.parse(["exists": true, "name": "p", "profilePrivate": true], requested: "p").isPrivate)
        let node = AccountProfile.parse(["exists": true, "name": "box", "kind": "node", "owner": "alice", "online": true], requested: "box")
        XCTAssertEqual(node.kindLabel, "Node · online")
        XCTAssertEqual(node.linkedNodesLine, "Linked to @alice")
        XCTAssertFalse(node.canLinkNode(viewer: "box"))
        XCTAssertFalse(AccountProfile.parse(["exists": false], requested: "x").exists)
    }

    @MainActor
    func testModelRefusesBadNamesWithoutCalling() async {
        let fake = FakeAccountEndpoints()
        let model = AccountProfileModel(name: "../admin", client: fake)
        await model.load()
        XCTAssertTrue(fake.calls.isEmpty)
        XCTAssertFalse(model.error.isEmpty)
        let good = AccountProfileModel(name: "@Alice", client: fake)
        await good.load()
        XCTAssertEqual(fake.calls.first?.path, "/api/accounts/alice")
    }
}

final class DesktopStatusTests: XCTestCase {
    func testSummarizesStatusAndProviders() {
        let summary = DesktopStatusSummary.parse(
            status: ["name": "studio", "version": "0.9.1", "running": 2, "transport": "relay"],
            agents: ["providers": [["id": "claude", "label": "Claude Code"], ["id": "codex", "label": "Codex"]],
                     "defaultProvider": "claude", "sessions": [["secret": "x"]]])
        XCTAssertEqual(summary.line, "ForkMesh 0.9.1 · 2 agents running · through the relay")
        XCTAssertEqual(summary.agentsLine, "Claude Code, Codex. New agents use Claude Code.")
        XCTAssertEqual(DesktopStatusSummary.parse(status: [:]).agentsLine, "No agent providers set up on this desktop.")
    }
}
