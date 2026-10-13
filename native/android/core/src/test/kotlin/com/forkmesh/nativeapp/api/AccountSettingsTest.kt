package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class AccountSettingsTest {
    private val room = "/api/repo/mainnode/forkmesh/rooms"

    fun testSecretsPersistAndTheLegacyPassphraseNormalises() {
        val prefs = MemoryPrefStore()
        val secrets = MemorySecretStore()
        val settings = AccountSettings.create(prefs, secrets)
        assertEq("", settings.passphrase)

        settings.setPassphrase("secret phrase")
        assertEq("secret phrase", secrets.read(AccountSettings.PASSPHRASE_KEY))
        assertEq("secret phrase", AccountSettings.create(prefs, secrets).passphrase)
        expect(!prefs.contains(AccountSettings.PASSPHRASE_KEY), "secrets never land in plain prefs")

        settings.setPassphrase(AccountSettings.LEGACY_PUBLIC_ROOM_PASSPHRASE)
        assertEq("", settings.passphrase)
        assertEq(null, secrets.read(AccountSettings.PASSPHRASE_KEY))

        val legacy = MemorySecretStore(mapOf(AccountSettings.PASSPHRASE_KEY to AccountSettings.LEGACY_PUBLIC_ROOM_PASSPHRASE))
        assertEq("", AccountSettings.create(MemoryPrefStore(), legacy).passphrase)

        settings.setGithubToken("ghp_x")
        settings.setGitlabToken("glpat_y")
        assertEq("ghp_x", AccountSettings.create(prefs, secrets).githubToken)
        assertEq("glpat_y", AccountSettings.create(prefs, secrets).gitlabToken)
        settings.setGithubToken("")
        assertEq(null, secrets.read(AccountSettings.GITHUB_TOKEN_KEY))

        prefs.putString(AccountSettings.GITLAB_TOKEN_KEY, "stray")
        settings.clearSecrets()
        assertEq("", settings.gitlabToken)
        assertEq(null, secrets.read(AccountSettings.GITLAB_TOKEN_KEY))
        expect(!prefs.contains(AccountSettings.GITLAB_TOKEN_KEY))
    }

    fun testSecretsMigrateOutOfPlainPrefsAndTheRoomSealIsDeleted() {
        val prefs = MemoryPrefStore(
            mapOf(
                AccountSettings.PASSPHRASE_KEY to "old phrase",
                AccountSettings.GITHUB_TOKEN_KEY to "plain-token",
            ),
        )
        val secrets = MemorySecretStore(
            mapOf(
                AccountSettings.GITHUB_TOKEN_KEY to "kept-token",
                AccountSettings.ROOM_SEAL_KEY to "manual seal",
            ),
        )
        val settings = AccountSettings.create(prefs, secrets)

        assertEq("old phrase", settings.passphrase)
        assertEq("old phrase", secrets.read(AccountSettings.PASSPHRASE_KEY))
        assertEq("kept-token", settings.githubToken, "an existing secret wins over the plain copy")
        expect(!prefs.contains(AccountSettings.PASSPHRASE_KEY))
        expect(!prefs.contains(AccountSettings.GITHUB_TOKEN_KEY))
        assertEq(null, secrets.read(AccountSettings.ROOM_SEAL_KEY))
    }

    fun testBugButtonPlacementClampsAndDefaults() {
        val prefs = MemoryPrefStore()
        val settings = AccountSettings.create(prefs, MemorySecretStore())
        assertEq(0.72, settings.bugButtonAnchor)
        expect(!settings.bugButtonOnLeft)

        settings.setBugButtonPlacement(anchor = 1.5, onLeft = true)
        assertEq(1.0, settings.bugButtonAnchor)
        expect(settings.bugButtonOnLeft)

        settings.setBugButtonPlacement(anchor = Double.NaN, onLeft = false)
        assertEq(0.72, settings.bugButtonAnchor)
        expect(!settings.bugButtonOnLeft)

        prefs.putDouble(AccountSettings.BUG_BUTTON_ANCHOR_KEY, -0.3)
        assertEq(0.0, settings.bugButtonAnchor)
        prefs.putDouble(AccountSettings.BUG_BUTTON_ANCHOR_KEY, Double.NaN)
        assertEq(0.72, settings.bugButtonAnchor)
    }

    fun testServeAsNodeDefaultsOffAndPersists() {
        val prefs = MemoryPrefStore()
        val settings = AccountSettings.create(prefs, MemorySecretStore())
        expect(!settings.serveAsNode)
        settings.setServeAsNode(true)
        expect(AccountSettings.create(prefs, MemorySecretStore()).serveAsNode)
    }

    fun testTelemetryStripDefaultsOnAndPersists() {
        val prefs = MemoryPrefStore()
        val settings = AccountSettings.create(prefs, MemorySecretStore())
        var notified = 0
        settings.addListener { notified++ }
        expect(settings.showTelemetryStrip)
        settings.setShowTelemetryStrip(false)
        assertEq(1, notified)
        expect(!AccountSettings.create(prefs, MemorySecretStore()).showTelemetryStrip)
        settings.setShowTelemetryStrip(true)
        expect(AccountSettings.create(prefs, MemorySecretStore()).showTelemetryStrip)
    }

    fun testProfileAndRoomValues() {
        val prefs = MemoryPrefStore()
        val settings = AccountSettings.create(prefs, MemorySecretStore())
        assertEq("world-general", settings.room)
        assertEq("", settings.displayName)
        settings.setRoom("general")
        settings.setDisplayName("alice")
        settings.setSolanaAddress("So1ana")
        assertEq("general", prefs.getString("server/room"))
        assertEq("alice", prefs.getString("profile/name"))
        assertEq("So1ana", prefs.getString("profile/solana"))
        assertEq("https://world.forkmesh.com/", settings.worldUri)
        assertEq("https://world.forkmesh.com/", AccountSettings.worldUriForServerUrl("wss://secret-token@relay/x"))
    }

    fun testEnvironmentAdoptionDropsOnlyAMismatchedUrlAfterARecordedChange() {
        val loopback = "ws://localhost:8787/custom"
        val devRecorded = MemoryPrefStore(
            mapOf(AccountSettings.SERVER_URL_KEY to loopback, AccountSettings.SERVER_URL_ENV_KEY to "dev"),
        )
        val prod = AccountSettings.create(devRecorded, MemorySecretStore())
        assertEq(null, devRecorded.getString(AccountSettings.SERVER_URL_KEY))
        assertEq("prod", devRecorded.getString(AccountSettings.SERVER_URL_ENV_KEY))
        assertEq(AccountSettings.PRODUCTION_SERVER_URL, prod.serverUrl)

        val firstRecord = MemoryPrefStore(mapOf(AccountSettings.SERVER_URL_KEY to loopback))
        val kept = AccountSettings.create(firstRecord, MemorySecretStore())
        assertEq(loopback, kept.serverUrl)
        assertEq("prod", firstRecord.getString(AccountSettings.SERVER_URL_ENV_KEY))

        val prodRecorded = MemoryPrefStore(
            mapOf(AccountSettings.SERVER_URL_KEY to "wss://staging.example/ws", AccountSettings.SERVER_URL_ENV_KEY to "prod"),
        )
        val dev = AccountSettings.create(prodRecorded, MemorySecretStore(), AppEnv(rawEnvironment = "dev"))
        assertEq(null, prodRecorded.getString(AccountSettings.SERVER_URL_KEY))
        assertEq(AccountSettings.ANDROID_EMULATOR_LOCAL_WORKER_SERVER_URL, dev.serverUrl)
        assertEq("dev", prodRecorded.getString(AccountSettings.SERVER_URL_ENV_KEY))

        val sameEnv = MemoryPrefStore(
            mapOf(AccountSettings.SERVER_URL_KEY to loopback, AccountSettings.SERVER_URL_ENV_KEY to "prod"),
        )
        assertEq(loopback, AccountSettings.create(sameEnv, MemorySecretStore()).serverUrl)
    }

    fun testServerUrlOverrideAndReset() {
        val prefs = MemoryPrefStore()
        val settings = AccountSettings.create(prefs, MemorySecretStore())
        expect(!settings.hasServerUrlOverride)

        settings.setServerUrl(" wss://app.forkmesh.com$room/general/ws ")
        expect(!settings.hasServerUrlOverride, "the legacy room on the canonical host is the default")
        assertEq(AccountSettings.PRODUCTION_SERVER_URL, settings.serverUrl)

        settings.setServerUrl("ws://192.168.1.2:8787/ws")
        expect(settings.hasServerUrlOverride)
        assertEq("ws://192.168.1.2:8787/ws", settings.serverUrl)

        settings.setServerUrl("   ")
        expect(!settings.hasServerUrlOverride)
        expect(!prefs.contains(AccountSettings.SERVER_URL_KEY))
    }

    fun testCanonicalServerUrl() {
        val current = "$room/world-general/ws"
        assertEq("wss://app.forkmesh.com$current", AccountSettings.canonicalServerUrl("ws://forkmesh.com$room/general/ws"))
        assertEq("wss://app.forkmesh.com$current", AccountSettings.canonicalServerUrl("ws://FORKMESH.com$room/general/ws"))
        assertEq("wss://app.forkmesh.com$current", AccountSettings.canonicalServerUrl("wss://api.forkmesh.com$current"))
        assertEq(
            "wss://api.forkmesh.com:8443$current",
            AccountSettings.canonicalServerUrl("wss://api.forkmesh.com:8443$room/general/ws"),
        )
        assertEq("ws://localhost:8787$current", AccountSettings.canonicalServerUrl("ws://localhost:8787$room/general/ws"))
        assertEq("ws://localhost:8787$current?x=1", AccountSettings.canonicalServerUrl("ws://localhost:8787$room/general/ws?x=1"))
        assertEq("ws://localhost:8787/other/ws", AccountSettings.canonicalServerUrl("ws://localhost:8787/other/ws"))
        assertEq("https://forkmesh.com$room/general/ws", AccountSettings.canonicalServerUrl("https://forkmesh.com$room/general/ws"))
        assertEq("not a url", AccountSettings.canonicalServerUrl("not a url"))
    }

    fun testReleaseModeIgnoresStoredOverrides() {
        val prefs = MemoryPrefStore(mapOf(AccountSettings.SERVER_URL_KEY to "ws://192.168.1.2:8787/ws"))
        val settings = AccountSettings.create(prefs, MemorySecretStore(), releaseMode = true)
        assertEq(AccountSettings.PRODUCTION_SERVER_URL, settings.serverUrl)
    }

    fun testListenersFireOnEveryChange() {
        val settings = AccountSettings.create(MemoryPrefStore(), MemorySecretStore())
        var calls = 0
        val listener: () -> Unit = { calls++ }
        settings.addListener(listener)
        settings.setDisplayName("alice")
        settings.setServerUrl("ws://192.168.1.2:8787/ws")
        settings.resetServerUrl()
        settings.setRoom("general")
        settings.setPassphrase("p")
        settings.setBugButtonPlacement(0.5, true)
        settings.setServeAsNode(true)
        settings.setSolanaAddress("s")
        settings.setGithubToken("g")
        settings.setGitlabToken("l")
        settings.clearSecrets()
        assertEq(11, calls)
        settings.removeListener(listener)
        settings.setDisplayName("bob")
        assertEq(11, calls)
    }

    fun testAppEnvDefaultsAndOverrides() {
        assertEq(AppEnv.PROD_SERVER_URL, AppEnv.PROD.defaultServerUrl)
        assertEq("prod", AppEnv.PROD.environmentName)
        assertEq(AppEnv.ANDROID_EMULATOR_SERVER_URL, AppEnv(rawEnvironment = "Development").defaultServerUrl)
        expect(!AppEnv(rawEnvironment = "prdo").isDev, "a typo resolves to prod")
        assertEq("ws://localhost:9000${AppEnv.RELAY_PATH}", AppEnv(apiHostOverride = "localhost:9000").defaultServerUrl)
        assertEq("wss://staging.forkmesh.com${AppEnv.RELAY_PATH}", AppEnv(apiHostOverride = "staging.forkmesh.com").defaultServerUrl)
        assertEq("wss://x/ws", AppEnv(rawEnvironment = "dev", apiHostOverride = "h", serverUrlOverride = "wss://x/ws").defaultServerUrl)
        assertEq("prod · app.forkmesh.com", AppEnv.PROD.describe(AppEnv.PROD_SERVER_URL))
        assertEq("dev", AppEnv(rawEnvironment = "local").describe(""))
        expect(AppEnv.isLoopbackUrl("ws://10.0.2.2:8787/x"))
        expect(AppEnv.isLoopbackUrl("ws://[::1]:8787/x"))
        expect(!AppEnv.isLoopbackUrl("wss://app.forkmesh.com/x"))
        expect(!AppEnv.isLoopbackUrl("localhost"))
    }
}
