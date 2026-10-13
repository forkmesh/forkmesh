package com.forkmesh.nativeapp.migration

import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.SecureRandom
import java.security.spec.MGF1ParameterSpec
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.IvParameterSpec
import javax.crypto.spec.OAEPParameterSpec
import javax.crypto.spec.PSource
import javax.crypto.spec.SecretKeySpec

const val PACKAGE = "com.forkmesh.forkmesh"

object RsaSpecs {
    fun cipher(oaep: Boolean): Cipher = Cipher.getInstance(if (oaep) "RSA/ECB/OAEPPadding" else "RSA/ECB/PKCS1Padding")

    fun spec(oaep: Boolean): OAEPParameterSpec? =
        if (oaep) OAEPParameterSpec("SHA-256", "MGF1", MGF1ParameterSpec.SHA1, PSource.PSpecified.DEFAULT) else null
}

class FakeKeystore : FlutterKeystore {
    val rsa = HashMap<String, KeyPair>()
    val aes = HashMap<String, ByteArray>()
    var unavailable = false
    var calls = 0

    override fun rsaUnwrap(alias: String, oaep: Boolean, wrapped: ByteArray): ByteArray? {
        calls++
        if (unavailable) throw FlutterKeystoreUnavailable("keystore locked")
        val pair = rsa[alias] ?: return null
        val cipher = RsaSpecs.cipher(oaep)
        val spec = RsaSpecs.spec(oaep)
        if (spec == null) cipher.init(Cipher.UNWRAP_MODE, pair.private) else cipher.init(Cipher.UNWRAP_MODE, pair.private, spec)
        return cipher.unwrap(wrapped, "AES", Cipher.SECRET_KEY).encoded
    }

    override fun aesGcmOpen(alias: String, sealed: ByteArray, aad: ByteArray): ByteArray? {
        calls++
        if (unavailable) throw FlutterKeystoreUnavailable("keystore locked")
        val key = aes[alias] ?: return null
        return TinkEsp.aesGcmDecrypt(key, sealed, aad)
    }

    fun rsaPair(alias: String): KeyPair = rsa.getOrPut(alias) {
        KeyPairGenerator.getInstance("RSA").apply { initialize(2048) }.generateKeyPair()
    }
}

class PluginWriter(
    val keystore: FakeKeystore,
    private val keyAlgorithm: FssKeyAlgorithm,
    private val storageAlgorithm: FssStorageAlgorithm,
    private val markers: MarkerPlace = MarkerPlace.NAMESPACED_CONFIG,
    private val gcmKeyName: String = FlutterSecureStorageReader.WRAPPED_KEY_GCM,
    val data: MutableMap<String, Any?> = LinkedHashMap(),
    val keyStorage: MutableMap<String, Any?> = LinkedHashMap(),
    val config: MutableMap<String, Any?> = LinkedHashMap(),
    val legacyConfig: MutableMap<String, Any?> = LinkedHashMap(),
) {
    enum class MarkerPlace { NONE, NAMESPACED_CONFIG, LEGACY_CONFIG, DATA }

    private val random = SecureRandom()
    private val aesKey: ByteArray

    init {
        val wrappedName = when (storageAlgorithm) {
            FssStorageAlgorithm.AES_GCM_NoPadding -> gcmKeyName
            FssStorageAlgorithm.AES_CBC_PKCS7Padding -> FlutterSecureStorageReader.WRAPPED_KEY_CBC
        }
        aesKey = ByteArray(16).also { random.nextBytes(it) }
        val oaep = keyAlgorithm == FssKeyAlgorithm.RSA_ECB_OAEPwithSHA_256andMGF1Padding
        val pair = keystore.rsaPair(FlutterSecureStorageReader.rsaAlias(PACKAGE, keyAlgorithm))
        val cipher = RsaSpecs.cipher(oaep)
        val spec = RsaSpecs.spec(oaep)
        if (spec == null) cipher.init(Cipher.WRAP_MODE, pair.public) else cipher.init(Cipher.WRAP_MODE, pair.public, spec)
        keyStorage[wrappedName] = Base64s.encodeDefault(cipher.wrap(SecretKeySpec(aesKey, "AES")))
        val target = when (markers) {
            MarkerPlace.NONE -> null
            MarkerPlace.NAMESPACED_CONFIG -> config
            MarkerPlace.LEGACY_CONFIG -> legacyConfig
            MarkerPlace.DATA -> data
        }
        target?.set(FlutterSecureStorageReader.MARKER_KEY_ALGORITHM, keyAlgorithm.name)
        target?.set(FlutterSecureStorageReader.MARKER_STORAGE_ALGORITHM, storageAlgorithm.name)
    }

    fun write(key: String, value: String) {
        val input = value.toByteArray(Charsets.UTF_8)
        val combined = when (storageAlgorithm) {
            FssStorageAlgorithm.AES_CBC_PKCS7Padding -> {
                val iv = ByteArray(16).also { random.nextBytes(it) }
                val cipher = Cipher.getInstance("AES/CBC/PKCS5Padding")
                cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(aesKey, "AES"), IvParameterSpec(iv))
                iv + cipher.doFinal(input)
            }
            FssStorageAlgorithm.AES_GCM_NoPadding -> {
                val iv = ByteArray(12).also { random.nextBytes(it) }
                val cipher = Cipher.getInstance("AES/GCM/NoPadding")
                cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(aesKey, "AES"), GCMParameterSpec(128, iv))
                iv + cipher.doFinal(input)
            }
        }
        data[FlutterSecureStorageReader.prefixed(key)] = Base64s.encodeDefault(combined)
    }

    fun files(): FlutterSecureStorageFiles = FlutterSecureStorageFiles(data, keyStorage, config, legacyConfig)
}

class FakeFlutterSource(
    override val keystore: FakeKeystore,
    var files: FlutterSecureStorageFiles,
    val prefs: MutableMap<String, Any?> = LinkedHashMap(),
) : FlutterSource {
    override val packageName: String = PACKAGE
    val deletedEntries = ArrayList<String>()
    val deletedPrefs = ArrayList<String>()

    override fun secureFiles(): FlutterSecureStorageFiles = files

    override fun preferences(): Map<String, Any?> = prefs

    override fun deleteSecureEntries(storedNames: Collection<String>) {
        val data = LinkedHashMap(files.data)
        for (name in storedNames) {
            data.remove(name)
            deletedEntries.add(name)
        }
        files = FlutterSecureStorageFiles(data, files.keyStorage, files.config, files.legacyConfig)
    }

    override fun deletePreferences(keys: Collection<String>) {
        for (key in keys) {
            prefs.remove(FlutterPreferences.PREFIX + key)
            deletedPrefs.add(key)
        }
    }
}

class MemoryNativePreferences : NativePreferences {
    val values = LinkedHashMap<String, PreferenceValue>()
    var writes = 0

    override fun read(key: String): PreferenceValue? = values[key]

    override fun write(key: String, value: PreferenceValue) {
        writes++
        values[key] = value
    }

    override fun remove(key: String) {
        values.remove(key)
    }
}
