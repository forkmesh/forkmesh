package com.forkmesh.nativeapp.migration

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.assertHex
import com.forkmesh.nativeapp.testing.bytesOfHex
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.resourceText

class FlutterSecureStorageReaderTest {
    private val sample = linkedMapOf(
        "identity/ed25519Seed" to "c2VlZC1ieXRlcy1oZXJl",
        "ownerCrypto/hybridV1" to "{\"x25519\":\"abc\",\"mlkem\":\"def\"}",
        "auth/session/app.forkmesh.com" to "{\"nodeName\":\"alice\",\"sessionToken\":\"tökén-🚀\"}",
        "p/app.next.forkmesh.com/identity/ed25519Seed" to "bmV4dC1zZWVk",
        "p/app.next.forkmesh.com/vault/deviceKey" to "v".repeat(300),
        "agent/credentials-v1" to "",
    )

    private fun roundTrip(writer: PluginWriter) {
        for ((key, value) in sample) writer.write(key, value)
        val result = FlutterSecureStorageReader.readAll(writer.files(), writer.keystore, PACKAGE)
        assertEq(sample, result.values)
        assertEq(emptyList<String>(), result.undecryptable)
    }

    fun testPrefixIsTheBase64OfThePluginSentence() {
        val decoded = String(java.util.Base64.getDecoder().decode(FlutterSecureStorageReader.KEY_PREFIX.padEnd((FlutterSecureStorageReader.KEY_PREFIX.length + 3) / 4 * 4, '=')), Charsets.UTF_8)
        assertEq("This is the prefix for a secure storage\n", decoded)
        assertEq("${FlutterSecureStorageReader.KEY_PREFIX}_auth/session", FlutterSecureStorageReader.prefixed("auth/session"))
        assertEq("auth/session", FlutterSecureStorageReader.plainKey(FlutterSecureStorageReader.prefixed("auth/session")))
        assertEq(null, FlutterSecureStorageReader.plainKey("FlutterSecureSAlgorithmKey"))
        assertEq("$PACKAGE.FlutterSecureStoragePluginKeyOAEP", FlutterSecureStorageReader.rsaAlias(PACKAGE, FssKeyAlgorithm.RSA_ECB_OAEPwithSHA_256andMGF1Padding))
        assertEq("$PACKAGE.FlutterSecureStoragePluginKey", FlutterSecureStorageReader.rsaAlias(PACKAGE, FssKeyAlgorithm.RSA_ECB_PKCS1Padding))
    }

    fun testVersion10DefaultsOaepAndGcm() {
        roundTrip(PluginWriter(FakeKeystore(), FssKeyAlgorithm.RSA_ECB_OAEPwithSHA_256andMGF1Padding, FssStorageAlgorithm.AES_GCM_NoPadding))
    }

    fun testMarkersInTheGlobalConfigFile() {
        roundTrip(
            PluginWriter(
                FakeKeystore(),
                FssKeyAlgorithm.RSA_ECB_OAEPwithSHA_256andMGF1Padding,
                FssStorageAlgorithm.AES_GCM_NoPadding,
                markers = PluginWriter.MarkerPlace.LEGACY_CONFIG,
            ),
        )
    }

    fun testVersion9MarkerlessPkcs1AndCbc() {
        val writer = PluginWriter(FakeKeystore(), FssKeyAlgorithm.RSA_ECB_PKCS1Padding, FssStorageAlgorithm.AES_CBC_PKCS7Padding, markers = PluginWriter.MarkerPlace.NONE)
        assertEq(FssKeyAlgorithm.RSA_ECB_PKCS1Padding to FssStorageAlgorithm.AES_CBC_PKCS7Padding, FlutterSecureStorageReader.savedAlgorithms(writer.files()))
        roundTrip(writer)
    }

    fun testVersion9GcmUnderTheTypoKeyWithMarkersInTheDataFile() {
        roundTrip(
            PluginWriter(
                FakeKeystore(),
                FssKeyAlgorithm.RSA_ECB_OAEPwithSHA_256andMGF1Padding,
                FssStorageAlgorithm.AES_GCM_NoPadding,
                markers = PluginWriter.MarkerPlace.DATA,
                gcmKeyName = FlutterSecureStorageReader.WRAPPED_KEY_GCM_V9,
            ),
        )
    }

    fun testInterruptedPluginMigrationReadsBothGenerations() {
        val keystore = FakeKeystore()
        val old = PluginWriter(keystore, FssKeyAlgorithm.RSA_ECB_PKCS1Padding, FssStorageAlgorithm.AES_CBC_PKCS7Padding, markers = PluginWriter.MarkerPlace.NONE)
        old.write("identity/ed25519Seed", "old-generation")
        val new = PluginWriter(
            keystore,
            FssKeyAlgorithm.RSA_ECB_OAEPwithSHA_256andMGF1Padding,
            FssStorageAlgorithm.AES_GCM_NoPadding,
            data = old.data,
            keyStorage = old.keyStorage,
        )
        new.write("auth/session/app.forkmesh.com", "new-generation")
        val result = FlutterSecureStorageReader.readAll(new.files(), keystore, PACKAGE)
        assertEq(mapOf("identity/ed25519Seed" to "old-generation", "auth/session/app.forkmesh.com" to "new-generation"), result.values)
        assertEq(emptyList<String>(), result.undecryptable)
    }

    fun testUndecryptableEntriesAreReportedNotGuessed() {
        val writer = PluginWriter(FakeKeystore(), FssKeyAlgorithm.RSA_ECB_OAEPwithSHA_256andMGF1Padding, FssStorageAlgorithm.AES_GCM_NoPadding)
        writer.write("identity/ed25519Seed", "good")
        writer.data[FlutterSecureStorageReader.prefixed("broken")] = Base64s.encodeDefault(ByteArray(40) { it.toByte() })
        val result = FlutterSecureStorageReader.readAll(writer.files(), writer.keystore, PACKAGE)
        assertEq(mapOf("identity/ed25519Seed" to "good"), result.values)
        assertEq(listOf("broken"), result.undecryptable)
    }

    fun testMissingKeystoreKeyMakesEverythingUndecryptable() {
        val writer = PluginWriter(FakeKeystore(), FssKeyAlgorithm.RSA_ECB_OAEPwithSHA_256andMGF1Padding, FssStorageAlgorithm.AES_GCM_NoPadding)
        writer.write("identity/ed25519Seed", "x")
        val result = FlutterSecureStorageReader.readAll(writer.files(), FakeKeystore(), PACKAGE)
        assertEq(emptyMap<String, String>(), result.values)
        assertEq(listOf("identity/ed25519Seed"), result.undecryptable)
    }

    fun testLockedKeystoreThrowsSoNothingIsDecided() {
        val writer = PluginWriter(FakeKeystore(), FssKeyAlgorithm.RSA_ECB_OAEPwithSHA_256andMGF1Padding, FssStorageAlgorithm.AES_GCM_NoPadding)
        writer.write("identity/ed25519Seed", "x")
        writer.keystore.unavailable = true
        assertFails<FlutterKeystoreUnavailable> { FlutterSecureStorageReader.readAll(writer.files(), writer.keystore, PACKAGE) }
    }

    fun testNoDataNeedsNoKeystore() {
        val keystore = FakeKeystore()
        val result = FlutterSecureStorageReader.readAll(FlutterSecureStorageFiles(emptyMap(), emptyMap()), keystore, PACKAGE)
        assertEq(0, result.values.size)
        assertEq(0, keystore.calls)
    }

    private fun espFixture(): Pair<Map<String, Any?>, Map<String, Any?>> {
        val fixture = Json.parseObject(resourceText("migration/esp_fixture.json"))
        @Suppress("UNCHECKED_CAST")
        return (fixture["prefs"] as Map<String, Any?>) to (fixture["expected"] as Map<String, Any?>)
    }

    private fun espKeystore(): FakeKeystore {
        val fixture = Json.parseObject(resourceText("migration/esp_fixture.json"))
        return FakeKeystore().apply { aes[TinkEsp.MASTER_KEY_ALIAS] = bytesOfHex(fixture["masterKeyHex"] as String) }
    }

    fun testEncryptedSharedPreferencesWrittenByRealTink() {
        val (prefs, expected) = espFixture()
        val result = FlutterSecureStorageReader.readAll(FlutterSecureStorageFiles(prefs, emptyMap()), espKeystore(), PACKAGE)
        assertEq(expected, result.values)
        assertEq(emptyList<String>(), result.undecryptable)
        expect(result.source.contains("EncryptedSharedPreferences"))
        assertEq(3, result.storedNames.size)
        for (names in result.storedNames.values) expect(names.single() in prefs.keys)
    }

    fun testPluginStorageWinsOverAnEspLeftover() {
        val (prefs, _) = espFixture()
        val keystore = espKeystore()
        val writer = PluginWriter(keystore, FssKeyAlgorithm.RSA_ECB_OAEPwithSHA_256andMGF1Padding, FssStorageAlgorithm.AES_GCM_NoPadding, data = LinkedHashMap(prefs))
        writer.write("identity/ed25519Seed", "migrated-by-flutter-10")
        val result = FlutterSecureStorageReader.readAll(writer.files(), keystore, PACKAGE)
        assertEq("migrated-by-flutter-10", result.values["identity/ed25519Seed"])
        assertEq(3, result.values.size)
        assertEq(2, result.storedNames["identity/ed25519Seed"]?.size)
    }

    fun testEspWithoutItsMasterKeyIsUndecryptable() {
        val (prefs, _) = espFixture()
        val result = FlutterSecureStorageReader.readAll(FlutterSecureStorageFiles(prefs, emptyMap()), FakeKeystore(), PACKAGE)
        assertEq(0, result.values.size)
        expect(result.undecryptable.isNotEmpty())
    }

    fun testAesSivRfc5297DeterministicVector() {
        val key = bytesOfHex("fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff")
        val ad = bytesOfHex("101112131415161718191a1b1c1d1e1f2021222324252627")
        val plain = bytesOfHex("112233445566778899aabbccddee")
        val sealed = bytesOfHex("85632d07c6e8f37f950acd320a2ecc9340c02b9690c4dc04daef7f6afe5c")
        assertHex("85632d07c6e8f37f950acd320a2ecc9340c02b9690c4dc04daef7f6afe5c", AesSiv.encrypt(key, plain, ad))
        assertHex("112233445566778899aabbccddee", AesSiv.decrypt(key, sealed, ad))
        val tampered = sealed.copyOf().also { it[20] = (it[20].toInt() xor 1).toByte() }
        assertFails<Exception> { AesSiv.decrypt(key, tampered, ad) }
    }

    fun testAesCmacRfc4493Vectors() {
        val key = bytesOfHex("2b7e151628aed2a6abf7158809cf4f3c")
        assertHex("bb1d6929e95937287fa37d129b756746", AesSiv.cmac(key, ByteArray(0)))
        assertHex("070a16b46b4d4144f79bdd9dd04a287c", AesSiv.cmac(key, bytesOfHex("6bc1bee22e409f96e93d7e117393172a")))
    }

    fun testAndroidBase64DefaultWrapsAndDecodes() {
        val bytes = ByteArray(100) { it.toByte() }
        val text = Base64s.encodeDefault(bytes)
        expect(text.contains('\n'))
        assertEq(bytes.toList(), Base64s.decode(text).toList())
    }
}
