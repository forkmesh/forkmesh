package com.forkmesh.nativeapp.migration

import com.forkmesh.nativeapp.account.projects.ProjectScope
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class VaultMigrationKeysTest {
    private val next = "app.next.forkmesh.com"

    private val flutterVault = linkedMapOf(
        "vault/alice/quickUnlock" to "{\"k\":\"legacy\"}",
        "vault/alice/autofill" to "1",
        "vault/alice/autoLockMinutes" to "15",
        "p/$next/vault/alice2/quickUnlock" to "{\"k\":\"next\"}",
        "p/$next/vault/alice2/autofill" to "0",
        "p/$next/vault/alice2/autoLockMinutes" to "1",
    )

    private fun migrate(): MemorySecretStore {
        val keystore = FakeKeystore()
        val writer = PluginWriter(keystore, FssKeyAlgorithm.RSA_ECB_OAEPwithSHA_256andMGF1Padding, FssStorageAlgorithm.AES_GCM_NoPadding)
        for ((key, value) in flutterVault) writer.write(key, value)
        val secrets = MemorySecretStore()
        val outcome = FlutterMigration(FakeFlutterSource(keystore, writer.files(), LinkedHashMap()), secrets, MemoryNativePreferences(), clock = { 1L }).run()
        expect(outcome is FlutterMigrationOutcome.Migrated, "$outcome")
        return secrets
    }

    fun testVaultDeviceSettingsKeepTheirKeysInTheLegacyProject() {
        val secrets = migrate()
        val view = ProjectScope.legacy.secrets(secrets)
        assertEq("{\"k\":\"legacy\"}", view.read("vault/alice/quickUnlock"))
        assertEq("1", view.read("vault/alice/autofill"))
        assertEq("15", view.read("vault/alice/autoLockMinutes"))
    }

    fun testVaultDeviceSettingsOfAnotherProjectStayInItsNamespace() {
        val secrets = migrate()
        val view = ProjectScope(next, ProjectScope.namespaceFor(next)).secrets(secrets)
        assertEq("{\"k\":\"next\"}", view.read("vault/alice2/quickUnlock"))
        assertEq("0", view.read("vault/alice2/autofill"))
        assertEq("1", view.read("vault/alice2/autoLockMinutes"))
        assertEq(null, view.read("vault/alice/quickUnlock"))
        assertEq(null, ProjectScope.legacy.secrets(secrets).read("vault/alice2/quickUnlock"))
    }
}
