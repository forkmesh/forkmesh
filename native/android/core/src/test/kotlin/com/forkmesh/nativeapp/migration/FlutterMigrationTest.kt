package com.forkmesh.nativeapp.migration

import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.security.SecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class FlutterMigrationTest {
    private val session = "{\"nodeName\":\"alice\",\"sessionToken\":\"tok\"}"

    private class Env(
        val source: FakeFlutterSource,
        val secrets: SecretStore,
        val prefs: MemoryNativePreferences = MemoryNativePreferences(),
    ) {
        val migration = FlutterMigration(source, secrets, prefs, clock = { 42L })
    }

    private fun flutterInstall(
        secrets: Map<String, String> = mapOf(
            "identity/ed25519Seed" to "seed",
            "ownerCrypto/hybridV1" to "owner",
            "auth/session/app.forkmesh.com" to "{\"nodeName\":\"alice\",\"sessionToken\":\"tok\"}",
            "p/app.next.forkmesh.com/identity/ed25519Seed" to "next-seed",
            "auth/session/app.next.forkmesh.com" to "{\"nodeName\":\"alice2\",\"sessionToken\":\"tok2\"}",
            "vault/deviceKey" to "vault",
            "agent/credentials-v1" to "creds",
        ),
        prefs: Map<String, Any?> = linkedMapOf(
            "flutter.relays/active" to "app.next.forkmesh.com",
            "flutter.relays/list" to "[{\"id\":\"app.forkmesh.com\"}]",
            "flutter.profile/name" to "Alice",
            "flutter.p/app.next.forkmesh.com/profile/name" to "Alice Next",
            "flutter.ads/hiddenUntilMs" to 1_800_000_000_000L,
            "flutter.bugReport/anchor" to FlutterPreferences.DOUBLE_PREFIX + "0.5",
        ),
        secretStore: SecretStore = MemorySecretStore(),
    ): Env {
        val keystore = FakeKeystore()
        val writer = PluginWriter(keystore, FssKeyAlgorithm.RSA_ECB_OAEPwithSHA_256andMGF1Padding, FssStorageAlgorithm.AES_GCM_NoPadding)
        for ((key, value) in secrets) writer.write(key, value)
        return Env(FakeFlutterSource(keystore, writer.files(), LinkedHashMap(prefs)), secretStore)
    }

    fun testCopiesEverySecretAndPreferenceUnderTheSameKeys() {
        val env = flutterInstall()
        val outcome = env.migration.run()
        assertEq(FlutterMigrationOutcome.Migrated(secrets = 7, preferences = 6, undecryptable = 0), outcome)
        assertEq("seed", env.secrets.read("identity/ed25519Seed"))
        assertEq("owner", env.secrets.read("ownerCrypto/hybridV1"))
        assertEq(session, env.secrets.read("auth/session/app.forkmesh.com"))
        assertEq("next-seed", env.secrets.read("p/app.next.forkmesh.com/identity/ed25519Seed"))
        assertEq("vault", env.secrets.read("vault/deviceKey"))
        assertEq("creds", env.secrets.read("agent/credentials-v1"))
        assertEq(PreferenceValue.Text("app.next.forkmesh.com"), env.prefs.read("relays/active"))
        assertEq(PreferenceValue.Text("Alice Next"), env.prefs.read("p/app.next.forkmesh.com/profile/name"))
        assertEq(PreferenceValue.Text("1800000000000"), env.prefs.read("ads/hiddenUntilMs"))
        assertEq(PreferenceValue.Real(0.5), env.prefs.read("bugReport/anchor"))
        expect(env.migration.isDone)
        assertEq(PreferenceValue.Bool(true), env.prefs.read(FlutterMigration.UPGRADED_KEY))
        assertEq(listOf("app.forkmesh.com", "app.next.forkmesh.com"), env.migration.migratedSessionProjects)
        expect(env.source.deletedEntries.isEmpty() && env.source.deletedPrefs.isEmpty())
        expect(env.source.files.data.isNotEmpty())
    }

    fun testRunningAgainIsANoOp() {
        val env = flutterInstall()
        env.migration.run()
        val writes = env.prefs.writes
        env.source.keystore.unavailable = true
        assertEq(FlutterMigrationOutcome.AlreadyDone, env.migration.run())
        assertEq(writes, env.prefs.writes)
    }

    fun testUpgradeWithoutAnActiveProjectLandsOnProduction() {
        val env = flutterInstall(prefs = mapOf("flutter.profile/name" to "Old"))
        env.migration.run()
        assertEq(PreferenceValue.Text("app.forkmesh.com"), env.prefs.read("relays/active"))
    }

    fun testFlutterValueWinsOverAnEarlierNativeWrite() {
        val env = flutterInstall()
        env.secrets.write("identity/ed25519Seed", "native-made-while-locked")
        env.prefs.write("profile/name", PreferenceValue.Text("native"))
        env.migration.run()
        assertEq("seed", env.secrets.read("identity/ed25519Seed"))
        assertEq(PreferenceValue.Text("Alice"), env.prefs.read("profile/name"))
    }

    fun testLockedKeystoreDefersAndALaterLaunchFinishes() {
        val env = flutterInstall()
        env.source.keystore.unavailable = true
        val first = env.migration.run()
        expect(first is FlutterMigrationOutcome.Deferred)
        expect(!env.migration.isDone)
        assertEq(null, env.secrets.read("identity/ed25519Seed"))
        assertEq(null, env.prefs.read("profile/name"))
        env.source.keystore.unavailable = false
        expect(env.migration.run() is FlutterMigrationOutcome.Migrated)
        assertEq("seed", env.secrets.read("identity/ed25519Seed"))
        assertEq(PreferenceValue.Int64(2), env.prefs.read(FlutterMigration.ATTEMPTS_KEY))
    }

    private class FlakySecrets(var failAfter: Int) : SecretStore {
        val inner = MemorySecretStore()
        var writes = 0

        override fun read(key: String): String? = inner.read(key)

        override fun write(key: String, value: String) {
            writes++
            if (writes > failAfter) throw IllegalStateException("keystore write failed")
            inner.write(key, value)
        }

        override fun delete(key: String) = inner.delete(key)
    }

    fun testAFailedWriteMidwayResumesOnTheNextLaunch() {
        val flaky = FlakySecrets(failAfter = 3)
        val env = flutterInstall(secretStore = flaky)
        expect(env.migration.run() is FlutterMigrationOutcome.Deferred)
        expect(!env.migration.isDone)
        flaky.failAfter = Int.MAX_VALUE
        expect(env.migration.run() is FlutterMigrationOutcome.Migrated)
        assertEq("creds", env.secrets.read("agent/credentials-v1"))
    }

    private class DroppingSecrets : SecretStore {
        override fun read(key: String): String? = null

        override fun write(key: String, value: String) {}

        override fun delete(key: String) {}
    }

    fun testReadBackMismatchNeverCountsAsDone() {
        val env = flutterInstall(secretStore = DroppingSecrets())
        val outcome = env.migration.run()
        expect(outcome is FlutterMigrationOutcome.Deferred)
        expect(!env.migration.isDone)
        expect(!env.migration.retireFlutterData(sessionAccepted = true))
        expect(env.source.deletedEntries.isEmpty())
    }

    fun testFlutterDataStaysUntilTheSessionIsVerified() {
        val env = flutterInstall()
        expect(!env.migration.retireFlutterData(sessionAccepted = true))
        env.migration.run()
        expect(!env.migration.canRetire(sessionAccepted = false))
        expect(!env.migration.retireFlutterData(sessionAccepted = false))
        expect(env.source.deletedEntries.isEmpty())
        assertEq(7, FlutterSecureStorageReader.readAll(env.source.files, env.source.keystore, PACKAGE).values.size)

        expect(env.migration.retireFlutterData(sessionAccepted = true))
        expect(env.migration.isRetired)
        assertEq(0, FlutterSecureStorageReader.readAll(env.source.files, env.source.keystore, PACKAGE).values.size)
        expect(env.source.prefs.keys.none { it.startsWith(FlutterPreferences.PREFIX) })
        assertEq("seed", env.secrets.read("identity/ed25519Seed"))
        expect(!env.migration.retireFlutterData(sessionAccepted = true))
    }

    fun testWithoutAMigratedSessionTheFlutterDataStays() {
        val env = flutterInstall(secrets = mapOf("identity/ed25519Seed" to "seed"))
        env.migration.run()
        expect(!env.migration.migratedAnySession)
        expect(!env.migration.retireFlutterData(sessionAccepted = false))
        expect(!env.migration.retireFlutterData(sessionAccepted = true))
        expect(env.source.deletedEntries.isEmpty())
        expect(env.source.prefs.keys.any { it.startsWith(FlutterPreferences.PREFIX) })
        expect(!env.migration.isRetired)
    }

    fun testGuestPreferencesOnlyInstallKeepsTheFlutterPreferences() {
        val env = flutterInstall(secrets = emptyMap())
        env.migration.run()
        expect(env.migration.isDone)
        expect(!env.migration.retireFlutterData(sessionAccepted = false))
        expect(env.source.prefs.keys.any { it.startsWith(FlutterPreferences.PREFIX) })
    }

    fun testUndecryptableSecretsBlockRetiring() {
        val env = flutterInstall()
        val data = LinkedHashMap(env.source.files.data)
        data[FlutterSecureStorageReader.prefixed("vault/other")] = Base64s.encodeDefault(ByteArray(64))
        env.source.files = FlutterSecureStorageFiles(data, env.source.files.keyStorage, env.source.files.config)
        val outcome = env.migration.run()
        assertEq(FlutterMigrationOutcome.Migrated(secrets = 7, preferences = 6, undecryptable = 1), outcome)
        assertEq(listOf("vault/other"), env.migration.undecryptableSecretKeys)
        expect(!env.migration.retireFlutterData(sessionAccepted = true))
        expect(env.source.deletedEntries.isEmpty())
    }

    fun testFreshInstallHasNothingToMigrate() {
        val env = Env(FakeFlutterSource(FakeKeystore(), FlutterSecureStorageFiles(emptyMap(), emptyMap())), MemorySecretStore())
        assertEq(FlutterMigrationOutcome.NothingToMigrate, env.migration.run())
        expect(env.migration.isDone)
        assertEq(null, env.prefs.read(FlutterMigration.UPGRADED_KEY))
        assertEq(null, env.prefs.read("relays/active"))
        expect(env.migration.retireFlutterData(sessionAccepted = false))
    }

    fun testCustomCipherEntriesBesideLeftoverEspKeysetsAreNotUndecryptable() {
        val fixture = com.forkmesh.nativeapp.json.Json.parseObject(com.forkmesh.nativeapp.testing.resourceText("migration/esp_fixture.json"))
        @Suppress("UNCHECKED_CAST")
        val espPrefs = fixture["prefs"] as Map<String, Any?>
        val env = flutterInstall(secrets = mapOf("identity/ed25519Seed" to "seed", "ownerCrypto/hybridV1" to "owner"))
        env.source.keystore.aes[TinkEsp.MASTER_KEY_ALIAS] = com.forkmesh.nativeapp.testing.bytesOfHex(fixture["masterKeyHex"] as String)
        val data = LinkedHashMap(env.source.files.data)
        data[TinkEsp.KEY_KEYSET_PREF] = espPrefs[TinkEsp.KEY_KEYSET_PREF]
        data[TinkEsp.VALUE_KEYSET_PREF] = espPrefs[TinkEsp.VALUE_KEYSET_PREF]
        env.source.files = FlutterSecureStorageFiles(data, env.source.files.keyStorage, env.source.files.config)
        val outcome = env.migration.run()
        assertEq(FlutterMigrationOutcome.Migrated(secrets = 2, preferences = 6, undecryptable = 0), outcome)
        assertEq(emptyList<String>(), env.migration.undecryptableSecretKeys)
        assertEq("seed", env.secrets.read("identity/ed25519Seed"))
    }

    fun testEspOnlyInstallMigratesAndRetiresItsEntries() {
        val fixture = com.forkmesh.nativeapp.json.Json.parseObject(com.forkmesh.nativeapp.testing.resourceText("migration/esp_fixture.json"))
        @Suppress("UNCHECKED_CAST")
        val prefs = LinkedHashMap(fixture["prefs"] as Map<String, Any?>)
        val keystore = FakeKeystore().apply {
            aes[TinkEsp.MASTER_KEY_ALIAS] = com.forkmesh.nativeapp.testing.bytesOfHex(fixture["masterKeyHex"] as String)
        }
        val env = Env(FakeFlutterSource(keystore, FlutterSecureStorageFiles(prefs, emptyMap())), MemorySecretStore())
        assertEq(FlutterMigrationOutcome.Migrated(secrets = 3, preferences = 0, undecryptable = 0), env.migration.run())
        assertEq("c2VlZC1vbmUtdHdvLXRocmVl", env.secrets.read("identity/ed25519Seed"))
        assertEq(PreferenceValue.Text("app.forkmesh.com"), env.prefs.read("relays/active"))
        expect(env.migration.retireFlutterData(sessionAccepted = true))
        assertEq(3, env.source.deletedEntries.size)
        expect(env.source.files.data.containsKey(TinkEsp.KEY_KEYSET_PREF))
    }
}
