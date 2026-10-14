package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ProductionEndpointTest {
    fun testAFreshInstallRelaysThroughProduction() {
        val settings = AccountSettings.create(MemoryPrefStore(), MemorySecretStore())

        assertEq(AccountSettings.PRODUCTION_SERVER_URL, settings.defaultServerUrl)
        assertEq(
            "wss://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws",
            settings.serverUrl,
        )
    }

    fun testHttpAndSignupOriginsFollowTheProductionRelayHost() {
        val prefs = MemoryPrefStore()
        val secrets = MemorySecretStore()
        val settings = AccountSettings.create(prefs, secrets)
        val identity = TestIdentity()
        val auth = AuthController.create(settings, prefs, secrets, { identity })

        val raw = ServerUrl.parse(settings.serverUrl).http("/api/repo/mainnode/forkmesh/raw", mapOf("path" to "README.md"))
        expect(raw.startsWith("https://app.forkmesh.com/"), raw)
        assertEq("https://app.forkmesh.com", ServerUrl.parse(settings.serverUrl).httpOrigin)
        assertEq("https://app.forkmesh.com/signup", auth.signupUrl)
        assertEq("https://app.forkmesh.com/forgot-password", auth.forgotPasswordUrl)
    }

    fun testASavedLocalWorkerDefaultIsClearedOnce() {
        val prefs = MemoryPrefStore(mapOf(AccountSettings.SERVER_URL_KEY to AccountSettings.LOCAL_WORKER_SERVER_URL))
        val settings = AccountSettings.create(prefs, MemorySecretStore())

        assertEq(AccountSettings.PRODUCTION_SERVER_URL, settings.serverUrl)
        assertEq(null, prefs.getString(AccountSettings.SERVER_URL_KEY))
        assertEq(true, prefs.getBool(AccountSettings.LOCAL_WORKER_URL_MIGRATION_KEY))
    }

    fun testTheEmulatorAndLegacyRoomLocalDefaultsAreClearedToo() {
        for (stored in listOf(
            AccountSettings.ANDROID_EMULATOR_LOCAL_WORKER_SERVER_URL,
            "ws://localhost:8787/api/repo/mainnode/forkmesh/rooms/general/ws",
            "ws://10.0.2.2:8787/api/repo/mainnode/forkmesh/rooms/general/ws",
        )) {
            expect(AccountSettings.isRetiredLocalWorkerServerUrl(stored), stored)
            val prefs = MemoryPrefStore(mapOf(AccountSettings.SERVER_URL_KEY to stored))
            val settings = AccountSettings.create(prefs, MemorySecretStore())
            assertEq(AccountSettings.PRODUCTION_SERVER_URL, settings.serverUrl, stored)
        }
    }

    fun testADeliberateRelayUrlSurvivesTheMigration() {
        val custom = "ws://192.168.1.20:8787/api/repo/mainnode/forkmesh/rooms/world-general/ws"
        val prefs = MemoryPrefStore(mapOf(AccountSettings.SERVER_URL_KEY to custom))
        val settings = AccountSettings.create(prefs, MemorySecretStore())

        assertEq(custom, settings.serverUrl)
        expect(!AccountSettings.isRetiredLocalWorkerServerUrl(custom))
    }

    fun testALocalUrlChosenAfterTheMigrationIsKept() {
        val prefs = MemoryPrefStore(
            mapOf(
                AccountSettings.LOCAL_WORKER_URL_MIGRATION_KEY to true,
                AccountSettings.SERVER_URL_KEY to AccountSettings.LOCAL_WORKER_SERVER_URL,
            ),
        )
        val settings = AccountSettings.create(prefs, MemorySecretStore())

        assertEq(AccountSettings.LOCAL_WORKER_SERVER_URL, settings.serverUrl)
    }
}
