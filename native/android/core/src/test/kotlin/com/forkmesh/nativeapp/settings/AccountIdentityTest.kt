package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class FakeAccountEndpoints : AccountEndpointCalling {
    override var serverOrigin: String = "https://app.forkmesh.com"
    data class Call(val method: String, val path: String, val body: Map<String, Any?>?)
    val calls = ArrayList<Call>()
    val answers = HashMap<String, Result<Map<String, Any?>>>()

    override fun accountCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
        calls.add(Call(method, path, body))
        return (answers["$method $path"] ?: Result.success(mapOf("ok" to true))).getOrThrow()
    }
}

private const val ED25519 = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl alice@laptop"

class SshKeyCheckTest {
    fun testAcceptsAnEd25519LineAndSplitsTheComment() {
        val check = SshKeys.check(ED25519) as SshKeyCheck.Ok
        assertEq("ssh-ed25519", check.keyType)
        expect(!check.line.contains("alice"))
        assertEq("alice@laptop", check.comment)
    }

    fun testRefusesPrivateKeys() {
        assertEq(SshKeyCheck.PrivateKey, SshKeys.check("-----BEGIN OPENSSH PRIVATE KEY-----\nabc\n-----END OPENSSH PRIVATE KEY-----"))  // forkmesh-secret-scan:ignore-line
        assertEq(SshKeyCheck.PrivateKey, SshKeys.check("PuTTY-User-Key-File-3: ssh-ed25519"))
    }

    fun testRefusesTwoKeysUnknownTypesAndMismatchedBlobs() {
        assertEq(SshKeyCheck.MultipleKeys, SshKeys.check(ED25519 + "\n" + ED25519))
        assertEq(SshKeyCheck.UnsupportedType("ssh-dss"), SshKeys.check("ssh-dss AAAAB3NzaC1kc3M="))
        assertEq(SshKeyCheck.Malformed, SshKeys.check("ssh-rsa AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl"))
        assertEq(SshKeyCheck.Malformed, SshKeys.check("ssh-ed25519 not-base64!"))
        assertEq(SshKeyCheck.Empty, SshKeys.check("  "))
    }

    fun testLabelPrefersTypedTextAndIsBounded() {
        assertEq("work", SshKeys.label("  work  ", "c"))
        assertEq("alice@laptop", SshKeys.label("", "alice@laptop"))
        assertEq(80, SshKeys.label("x".repeat(200), "").length)
    }

    fun testKeyIdsAndPageParsing() {
        expect(SshKeys.isKeyId("sk_abcdefghijklmnop"))
        expect(!SshKeys.isKeyId("sk_short"))
        expect(!SshKeys.isKeyId("sk_abcdefghijklmnop/../x"))
        val page = SshKeysPage.parse(
            mapOf(
                "keys" to listOf(
                    mapOf("id" to "sk_abcdefghijklmnop", "label" to "", "keyType" to "ssh-ed25519", "fingerprint" to "SHA256:x"),
                    mapOf("id" to "bad"),
                ),
                "maximumKeys" to 1L, "gatewayConfigured" to true, "gatewayHost" to "git.example.com", "gatewayPort" to 2222L,
            ),
        )
        assertEq(1, page.keys.size)
        assertEq("Ed25519 key", page.keys[0].title)
        expect(page.atLimit)
        assertEq("ssh://git@git.example.com:2222", page.gatewayLine)
    }

    fun testModelAddsWithTheCleanLineAndRemovesByOwnPath() {
        val fake = FakeAccountEndpoints()
        fake.answers["GET /api/accounts/ssh-keys"] = Result.success(mapOf("keys" to listOf(mapOf("id" to "sk_abcdefghijklmnop", "keyType" to "ssh-rsa"))))
        val model = SshKeysModel(fake)
        model.draftKey = ED25519
        expect(model.canAdd)
        expect(model.add())
        val post = fake.calls.first { it.method == "POST" }
        assertEq("alice@laptop", post.body?.get("label"))
        expect(!(post.body?.get("publicKey") as String).contains("alice"))
        assertEq(1, model.page?.keys?.size)
        assertEq("", model.draftKey)
        expect(model.remove("sk_abcdefghijklmnop"))
        assertEq("/api/accounts/ssh-keys/sk_abcdefghijklmnop", fake.calls.last().path)
        assertEq(0, model.page?.keys?.size)
        expect(!model.remove("../../etc"))
    }

    fun testModelMapsServerErrors() {
        val fake = FakeAccountEndpoints()
        fake.answers["POST /api/accounts/ssh-keys"] = Result.failure(AccountEndpointException("ssh_key_already_registered", 409))
        val model = SshKeysModel(fake)
        model.draftKey = ED25519
        expect(!model.add())
        expect(model.error.contains("already registered"))
        assertEq(ED25519, model.draftKey)
    }
}

class McpConnectorTest {
    private val tokenId = "a".repeat(32)
    private val token = "fmbot_${tokenId}_${"B".repeat(43)}"

    fun testTokenShapesAndUrls() {
        expect(McpConnector.isToken(token))
        expect(!McpConnector.isToken("fmbot_x_y"))
        assertEq("https://app.forkmesh.com/mcp", McpConnector.serverUrl("https://app.forkmesh.com/"))
        assertEq("/api/orgs/acmeco..x/bot-tokens", McpConnector.path("Acme Co/../x"))
        expect(McpConnector.mask(token).startsWith("fmbot_aaaaaa"))
        expect(!McpConnector.mask(token).contains("BBBBBBBBBB"))
        expect(McpConnector.configJson("https://h/mcp", "t").contains("\"Bearer t\""))
        assertEq(1, McpConnector.clampDays(0))
        assertEq(365, McpConnector.clampDays(999))
    }

    fun testRowStatus() {
        val row = McpTokenRow(id = tokenId, provider = "codex", label = "", scopes = McpConnector.taskScopes, expiresAt = 10L * 86_400_000)
        assertEq("Codex", row.title)
        expect(row.tasksOnly)
        assertEq("Expires in 10 days", row.status(0))
        assertEq("Expired", row.status(11L * 86_400_000))
        expect(!row.isActive(11L * 86_400_000))
    }

    fun testMintAsksForTaskScopesOnlyAndRotateRevokesTheOldToken() {
        val fake = FakeAccountEndpoints()
        val path = McpConnector.path("acme")
        fake.answers["POST $path"] = Result.success(mapOf("token" to token, "tokenId" to tokenId, "expiresAt" to 5L))
        val model = McpConnectorModel("acme", fake) { 0L }
        assertEq("https://app.forkmesh.com/mcp", model.serverUrl)
        assertEq(token, model.mint()?.token)
        val body = fake.calls.first { it.method == "POST" }.body
        assertEq(McpConnector.taskScopes, body?.get("scopes"))
        assertEq(90L, body?.get("expiresDays"))
        val old = McpTokenRow(id = "c".repeat(32), provider = "codex", label = "old", scopes = emptyList())
        expect(model.rotate(old) != null)
        assertEq(old.id, fake.calls.first { it.method == "DELETE" }.body?.get("tokenId"))
        assertEq(token, model.minted?.token)
    }

    fun testMintRejectsAMalformedAnswer() {
        val fake = FakeAccountEndpoints()
        fake.answers["POST ${McpConnector.path("acme")}"] = Result.success(mapOf("token" to "nope"))
        val model = McpConnectorModel("acme", fake)
        assertEq(null, model.mint())
        assertEq(null, model.minted)
        expect(model.error.isNotEmpty())
    }
}

class RecoveryKitTest {
    fun testPassphraseRules() {
        assertEq(RecoveryKitException.shortPassphrase.message, RecoveryKit.validatePassphrase("short", "short")?.message)
        assertEq(RecoveryKitException.mismatchedPassphrase.message, RecoveryKit.validatePassphrase("long enough pass", "other")?.message)
        assertEq(null, RecoveryKit.validatePassphrase("long enough pass", "long enough pass"))
        assertEq(0, RecoveryKit.strength("abc"))
        assertEq(3, RecoveryKit.strength("Correct Horse Battery 9"))
    }

    fun testKitRoundTripsAndMatchesThisDevice() {
        val identity = Identity.fromSeed(ByteArray(32) { 7 })
        val kit = RecoveryKit.export(identity, "a long passphrase", "a long passphrase", 1_000)
        expect(!kit.contains(B64.encodeUrlNoPadding(ByteArray(32) { 7 })))
        val check = RecoveryKit.check(kit, "a long passphrase", identity.publicKeyB64url)
        expect(check.matchesThisDevice)
        assertEq(1_000L, check.createdAt)
        expect(!RecoveryKit.check(kit, "a long passphrase", "someone-else").matchesThisDevice)
        assertFails<RecoveryKitException> { RecoveryKit.check(kit, "the wrong passphrase", "") }
        assertFails<RecoveryKitException> { RecoveryKit.check("{}", "a long passphrase", "") }
    }

    fun testFileNameAndBackupNag() {
        assertEq("forkmesh-alice-recovery-kit-1970-01-01.json", RecoveryKit.fileName("Alice!", 0))
        val store = IdentityBackupStore(MemoryPrefStore())
        expect(store.state.showsNag("k1", 0))
        store.snooze(0)
        expect(!store.state.showsNag("k1", 1_000))
        expect(store.state.showsNag("k1", IdentityBackupStore.SNOOZE_MILLIS))
        store.markBackedUp("k1", 5)
        expect(!store.state.needsBackup("k1"))
        expect(store.state.needsBackup("k2"))
        assertEq(5L, store.state.backedUpAt)
    }
}

class NodeLinkTest {
    private val url = "https://app.forkmesh.com/dashboard?link_node=alice-laptop&link_ts=1000000&link_sig=abc_-DEF"

    fun testParsesTheDesktopsGrantLink() {
        val grant = (NodeLink.parse(url) as NodeLinkParse.Grant).grant
        assertEq("alice-laptop", grant.node)
        assertEq("app.forkmesh.com", grant.host)
        assertEq(mapOf("nodeName" to "alice-laptop", "user" to "alice", "ts" to "1000000", "sig" to "abc_-DEF"), grant.body("Alice"))
        expect(grant.isFresh(1_000_000 + 60_000))
        expect(!grant.isFresh(1_000_000 + NodeLink.MAX_SKEW_MILLIS + 1))
    }

    fun testRejectsJunk() {
        assertEq(NodeLinkParse.InstallerCode("123456"), NodeLink.parse("123456"))
        expect(NodeLink.parse("https://x/dashboard?link_node=Bad%20Name&link_ts=1&link_sig=s") !is NodeLinkParse.Grant)
        expect(NodeLink.parse("https://x/dashboard?link_node=a&link_ts=soon&link_sig=s") !is NodeLinkParse.Grant)
        expect(NodeLink.parse("javascript:alert(1)?link_node=a&link_ts=1&link_sig=s") !is NodeLinkParse.Grant)
        expect(NodeLink.parse("https://x/") !is NodeLinkParse.Grant)
    }

    fun testHostAliases() {
        val grant = (NodeLink.parse(url) as NodeLinkParse.Grant).grant
        expect(NodeLink.hostMatches(grant, "https://forkmesh.com"))
        expect(NodeLink.hostMatches(grant, "https://app.next.forkmesh.com"))
        expect(!NodeLink.hostMatches(grant, "https://relay.example.com"))
    }

    fun testConfirmPostsTheGrantAsTheSignedInUser() {
        val fake = FakeAccountEndpoints()
        fake.answers["POST /api/accounts/link-grant"] = Result.success(mapOf("linked" to true, "nodeId" to "alice-laptop", "nodes" to listOf("alice-laptop")))
        val model = NodeLinkModel(fake, { "alice" }, { 1_000_000L })
        model.review(url)
        expect(model.pending != null)
        assertEq(NodeLinkOutcome.Linked("alice-laptop", listOf("alice-laptop")), model.confirm())
        assertEq("alice", fake.calls.first().body?.get("user"))
        assertEq(null, model.pending)
    }

    fun testStaleOrForeignLinksAreNotSent() {
        val fake = FakeAccountEndpoints()
        val stale = NodeLinkModel(fake, { "alice" }, { 10_000_000L })
        stale.review(url)
        assertEq(null, stale.pending)
        expect(stale.error.contains("expired"))
        fake.serverOrigin = "https://relay.example.com"
        val foreign = NodeLinkModel(fake, { "alice" }, { 1_000_000L })
        foreign.review(url)
        assertEq(null, foreign.pending)
        assertEq(null, foreign.confirm())
        expect(fake.calls.isEmpty())
    }
}

class AccountDataTest {
    fun testExportDropsSecretsAndNamesTheFile() {
        val file = AccountExport.file(
            mapOf(
                "ok" to true, "schema" to "forkmesh-account-export-v1",
                "account" to mapOf("name" to "alice", "pass_hash" to "x", "totp_secret" to "y"),
                "devices" to listOf(mapOf("id" to "d")), "sshKeys" to emptyList<Any?>(),
            ),
            "Alice",
            0,
        )
        assertEq("forkmesh-alice-export-1970-01-01.json", file.fileName)
        expect(!file.json.contains("pass_hash"))
        expect(!file.json.contains("totp_secret"))
        expect(!file.json.contains("\"ok\""))
        assertEq(1, file.devices)
        assertFails<AccountEndpointException> { AccountExport.file(mapOf("schema" to "other"), "a", 0) }
    }

    fun testExportOnAnOldServerSaysSo() {
        val fake = FakeAccountEndpoints()
        fake.answers["GET /api/accounts/export"] = Result.failure(AccountEndpointException("http_404", 404))
        val model = AccountDataModel(fake)
        assertEq(null, model.prepareExport("alice"))
        expect(model.error.contains("cannot export"))
    }

    fun testMembershipText() {
        assertEq("Core (free)", MembershipStatus.parse(mapOf("connected" to false, "configured" to true)).title)
        val member = MembershipStatus.parse(mapOf("connected" to true, "tier" to "pro", "patronStatus" to "active_patron"))
        assertEq("Pro member", member.title)
        assertEq("Active pledge on Patreon.", member.detail)
    }

    fun testSecurityReportValidatesThenPosts() {
        val fake = FakeAccountEndpoints()
        val model = AccountDataModel(fake)
        expect(!model.sendReport())
        expect(fake.calls.isEmpty())
        model.report = SecurityReportDraft(" XSS ", "steps", "website", "me@example.com")
        expect(model.sendReport())
        assertEq("/api/security/report", fake.calls.first().path)
        assertEq("XSS", fake.calls.first().body?.get("title"))
        assertEq(SecurityReportDraft(), model.report)
        expect(model.reportSent)
    }
}

class AccountDevicesTest {
    fun testRecentAccountsAreBoundedDedupedAndNewestFirst() {
        val store = RecentAccountsStore(MemoryPrefStore())
        for (index in 0 until 12) store.record("user$index", "", index.toLong())
        store.record("USER3", "u3@example.com", 100)
        store.record("preview-node", "", 200)
        assertEq(RecentAccountsStore.LIMIT, store.accounts.size)
        assertEq("USER3", store.accounts.first().name)
        assertEq("user11", store.others("user3").first().name)
        store.forget("user3")
        expect(store.accounts.none { it.id == "user3" })
    }

    fun testDesktopNicknames() {
        val names = DesktopNicknames(MemoryPrefStore())
        assertEq("Desktop abcdef", names.name("abcdefghijklmnopq", ""))
        assertEq("studio", names.name("k", "studio"))
        names.rename("k", "  Office\nPC  ")
        assertEq("OfficePC", names.name("k", "studio"))
        names.rename("k", " ")
        assertEq("studio", names.name("k", "studio"))
        names.rename("a", "A")
        names.rename("b", "B")
        names.prune(setOf("a"))
        assertEq(mapOf("a" to "A"), names.all)
    }

    fun testTrustListSummaryRejectsGarbage() {
        expect(TrustListSummary.summarize("{}").isFailure)
        expect(TrustListSummary.summarize("not json").isFailure)
    }
}

class SettingsCatalogTest {
    fun testSearchMatchesTitlesAndKeywords() {
        val all = SettingsSectionId.entries.toSet()
        assertEq(listOf(SettingsSectionId.SshKeys), SettingsCatalog.visible("ssh", all).map { it.id })
        expect(SettingsSectionId.General in SettingsCatalog.visible("DARK", all).map { it.id })
        expect(SettingsSectionId.Mcp in SettingsCatalog.visible("api token", all).map { it.id })
        assertEq(listOf(SettingsSectionId.Data, SettingsSectionId.Legal), SettingsCatalog.visible("", setOf(SettingsSectionId.Legal, SettingsSectionId.Data)).map { it.id })
        expect(SettingsCatalog.visible("zzz", all).isEmpty())
        assertEq(SettingsSectionId.entries.size, SettingsCatalog.entries.size)
        expect(SettingsSectionId.General in SettingsCatalog.visible("thème", all).map { it.id })
        for (word in listOf("sound", "Sounds", "mute", "silent", "audio", "volume")) {
            expect(SettingsSectionId.General in SettingsCatalog.visible(word, all).map { it.id })
            assertEq(listOf("Sounds"), SettingsCatalog.matchedSettings(SettingsSectionId.General, word))
        }
        assertEq(listOf("Haptics"), SettingsCatalog.matchedSettings(SettingsSectionId.General, "vibrate"))
        assertEq(emptyList<String>(), SettingsCatalog.matchedSettings(SettingsSectionId.General, ""))
        assertEq(emptyList<String>(), SettingsCatalog.matchedSettings(SettingsSectionId.Data, "sound"))
    }

    fun testSectionNamesFromRoutes() {
        assertEq(SettingsSectionId.SshKeys, SettingsCatalog.section("sshKeys"))
        assertEq(SettingsSectionId.SshKeys, SettingsCatalog.section("ssh-keys"))
        assertEq(SettingsSectionId.Identity, SettingsCatalog.section("wallet"))
        assertEq(SettingsSectionId.General, SettingsCatalog.section("sounds"))
        assertEq(SettingsSectionId.InstalledApps, SettingsCatalog.section("apps"))
        assertEq(SettingsSectionId.InstalledApps, SettingsCatalog.section("installedApps"))
        assertEq(null, SettingsCatalog.section("nope"))
        assertEq(null, SettingsCatalog.section(null))
    }

    fun testQuickSetupSteps() {
        val input = QuickSetupInput(signedIn = true, projectName = "ForkMesh")
        assertEq(3, QuickSetup.remaining(input))
        assertEq(QuickSetupStepId.Desktop, QuickSetup.next(input)?.id)
        assertEq(null, QuickSetup.next(input.copy(desktopsPaired = 2, identityBackedUp = true, pushEnabled = true)))
        assertEq(false, QuickSetup.steps(QuickSetupInput(signedIn = true, guest = true)).first().done)
    }
}

class DeviceSettingsTest {
    fun testDevicePreferences() {
        val prefs = MemoryPrefStore()
        expect(DeviceSettings.hapticsEnabled(prefs))
        DeviceSettings.setHapticsEnabled(prefs, false)
        expect(!DeviceSettings.hapticsEnabled(prefs))
        expect(DeviceSettings.homeHidesBarIcons(prefs))
        DeviceSettings.setHomeHidesBarIcons(prefs, false)
        expect(!DeviceSettings.homeHidesBarIcons(prefs))
        DeviceSettings.setDictationLocale(prefs, "en-GB;drop")
        assertEq("en-GBdrop", DeviceSettings.dictationLocale(prefs))
        DeviceSettings.setDictationLocale(prefs, "")
        assertEq("", DeviceSettings.dictationLocale(prefs))
        assertEq("main", DeviceSettings.localDefaultBranch(prefs))
        expect(!DeviceSettings.setLocalDefaultBranch(prefs, "bad..name"))
        expect(DeviceSettings.setLocalDefaultBranch(prefs, "trunk"))
        assertEq("trunk", DeviceSettings.localDefaultBranch(prefs))
        assertEq(null, DeviceSettings.localAuthorOverride(prefs))
        DeviceSettings.setLocalAuthorName(prefs, "Alice <x>")
        DeviceSettings.setLocalAuthorEmail(prefs, "alice@example.com")
        assertEq("Alice x", DeviceSettings.localAuthorOverride(prefs)?.first)
        expect(DeviceSettings.isPlausibleEmail("a@b.co"))
        expect(!DeviceSettings.isPlausibleEmail("a@b"))
    }

    fun testSoundsDefaultOnAndLegacyValueMigratesOnce() {
        val prefs = MemoryPrefStore()
        expect(DeviceSettings.soundsEnabled(prefs))
        prefs.putBool(DeviceSettings.LEGACY_SOUNDS_KEY, false)
        DeviceSettings.migrateLegacySounds(prefs)
        expect(!DeviceSettings.soundsEnabled(prefs))
        expect(!prefs.contains(DeviceSettings.LEGACY_SOUNDS_KEY))
        DeviceSettings.setSoundsEnabled(prefs, true)
        prefs.putBool(DeviceSettings.LEGACY_SOUNDS_KEY, false)
        DeviceSettings.migrateLegacySounds(prefs)
        expect(DeviceSettings.soundsEnabled(prefs))
        expect(!prefs.contains(DeviceSettings.LEGACY_SOUNDS_KEY))
    }
}

class AccountProfileTest {
    fun testParsesAUserWithNodes() {
        val profile = AccountProfile.parse(
            mapOf(
                "exists" to true, "name" to "alice", "kind" to "user", "status" to "active", "nodes" to listOf("alice-laptop", "Bad Node"),
                "solana" to "So11111111111111111111111111111111111111112", "pubkey" to "pk", "followers" to 3L,
            ),
            "Alice",
        )
        assertEq(listOf("alice-laptop"), profile.nodes)
        assertEq("1 node linked", profile.linkedNodesLine)
        expect(profile.canLinkNode("ALICE"))
        expect(!profile.canLinkNode("bob"))
        assertEq("solana:So11111111111111111111111111111111111111112", profile.walletQrText)
        assertEq("forkmesh:identity/alice/pk", profile.identityQrText)
        expect(SolanaAddress.isValid(profile.solana))
        expect(!SolanaAddress.isValid("0OIl"))
    }

    fun testPrivateAndNodeProfiles() {
        expect(AccountProfile.parse(mapOf("exists" to true, "name" to "p", "profilePrivate" to true), "p").isPrivate)
        val node = AccountProfile.parse(mapOf("exists" to true, "name" to "box", "kind" to "node", "owner" to "alice", "online" to true), "box")
        assertEq("Node · online", node.kindLabel)
        assertEq("Linked to @alice", node.linkedNodesLine)
        expect(!node.canLinkNode("box"))
        expect(!AccountProfile.parse(mapOf("exists" to false), "x").exists)
    }

    fun testModelRefusesBadNamesWithoutCalling() {
        val fake = FakeAccountEndpoints()
        val model = AccountProfileModel("../admin", fake)
        model.load()
        expect(fake.calls.isEmpty())
        expect(model.error.isNotEmpty())
        AccountProfileModel("@Alice", fake).load()
        assertEq("/api/accounts/alice", fake.calls.first().path)
    }
}

class DesktopStatusTest {
    fun testSummarizesStatusAndProviders() {
        val summary = DesktopStatusSummary.parse(
            mapOf("name" to "studio", "version" to "0.9.1", "running" to 2L, "transport" to "relay"),
            mapOf(
                "providers" to listOf(mapOf("id" to "claude", "label" to "Claude Code"), mapOf("id" to "codex", "label" to "Codex")),
                "defaultProvider" to "claude",
                "sessions" to listOf(mapOf("secret" to "x")),
            ),
        )
        assertEq("ForkMesh 0.9.1 · 2 agents running · through the relay", summary.line)
        assertEq("Claude Code, Codex. New agents use Claude Code.", summary.agentsLine)
        assertEq("No agent providers set up on this desktop.", DesktopStatusSummary.parse(emptyMap()).agentsLine)
    }
}
