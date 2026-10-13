package com.forkmesh.nativeapp.migration

import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.IvParameterSpec
import javax.crypto.spec.SecretKeySpec

enum class FssKeyAlgorithm {
    RSA_ECB_PKCS1Padding,
    RSA_ECB_OAEPwithSHA_256andMGF1Padding,

    AES_GCM_NoPadding,
    ;

    companion object {
        fun parse(name: String?): FssKeyAlgorithm? = when (name) {
            null -> null
            "AES_GCM_NoPadding_BIOMETRIC" -> AES_GCM_NoPadding
            else -> entries.firstOrNull { it.name == name }
        }
    }
}

enum class FssStorageAlgorithm {
    AES_CBC_PKCS7Padding,
    AES_GCM_NoPadding,
    ;

    companion object {
        fun parse(name: String?): FssStorageAlgorithm? = when (name) {
            null -> null
            "AES_GCM_NoPadding_BIOMETRIC" -> AES_GCM_NoPadding
            else -> entries.firstOrNull { it.name == name }
        }
    }
}

class FlutterKeystoreUnavailable(message: String, cause: Throwable? = null) : Exception(message, cause)

interface FlutterKeystore {
    fun rsaUnwrap(alias: String, oaep: Boolean, wrapped: ByteArray): ByteArray?

    fun aesGcmOpen(alias: String, sealed: ByteArray, aad: ByteArray): ByteArray?
}

class FlutterSecureStorageFiles(
    val data: Map<String, Any?>,
    val keyStorage: Map<String, Any?>,
    val config: Map<String, Any?> = emptyMap(),
    val legacyConfig: Map<String, Any?> = emptyMap(),
) {
    val isEmpty: Boolean get() = data.isEmpty()
}

class FlutterSecrets(
    val values: Map<String, String>,

    val undecryptable: List<String>,

    val source: String,

    val storedNames: Map<String, List<String>> = emptyMap(),
)

object FlutterSecureStorageReader {
    const val DATA_PREFS = "FlutterSecureStorage"
    const val KEY_PREFS = "FlutterSecureKeyStorage"
    const val CONFIG_PREFS = "FlutterSecureStorageConfiguration:FlutterSecureStorage"
    const val LEGACY_CONFIG_PREFS = "FlutterSecureStorageConfiguration"

    const val KEY_PREFIX = "VGhpcyBpcyB0aGUgcHJlZml4IGZvciBhIHNlY3VyZSBzdG9yYWdlCg"

    const val MARKER_KEY_ALGORITHM = "FlutterSecureSAlgorithmKey"
    const val MARKER_STORAGE_ALGORITHM = "FlutterSecureSAlgorithmStorage"

    const val WRAPPED_KEY_CBC = "VGhpcyBpcyB0aGUga2V5IGZvciBhIHNlY3VyZSBzdG9yYWdlIEFFUyBLZXkK"

    const val WRAPPED_KEY_GCM = "AESVGhpcyBpcyB0aGUga2V5IGZvciBhIHNlY3VyZSBzdG9yYWdlIEFFUyBLZXkK"

    const val WRAPPED_KEY_GCM_V9 = "VGhpcyBpcyB0aGUga2V5IGZvcihBIHNlY3XyZZBzdG9yYWdlIEFFUyBLZXkK"

    fun rsaAlias(packageName: String, algorithm: FssKeyAlgorithm): String = when (algorithm) {
        FssKeyAlgorithm.RSA_ECB_PKCS1Padding -> "$packageName.FlutterSecureStoragePluginKey"
        FssKeyAlgorithm.RSA_ECB_OAEPwithSHA_256andMGF1Padding -> "$packageName.FlutterSecureStoragePluginKeyOAEP"
        FssKeyAlgorithm.AES_GCM_NoPadding -> "$packageName.FlutterSecureStoragePluginKey"
    }

    fun prefixed(key: String): String = KEY_PREFIX + "_" + key

    fun plainKey(stored: String): String? {
        if (!stored.contains(KEY_PREFIX)) return null
        return stored.replaceFirst(KEY_PREFIX + "_", "")
    }

    fun savedAlgorithms(files: FlutterSecureStorageFiles): Pair<FssKeyAlgorithm, FssStorageAlgorithm> {
        for (source in listOf(files.config, files.legacyConfig, files.data)) {
            val key = FssKeyAlgorithm.parse(source[MARKER_KEY_ALGORITHM] as? String)
            val storage = FssStorageAlgorithm.parse(source[MARKER_STORAGE_ALGORITHM] as? String)
            if (key != null && storage != null) return key to storage
        }
        return FssKeyAlgorithm.RSA_ECB_PKCS1Padding to FssStorageAlgorithm.AES_CBC_PKCS7Padding
    }

    fun readAll(files: FlutterSecureStorageFiles, keystore: FlutterKeystore, packageName: String): FlutterSecrets {
        val values = LinkedHashMap<String, String>()
        val undecryptable = ArrayList<String>()
        val sources = ArrayList<String>()

        val storedNames = LinkedHashMap<String, MutableList<String>>()
        val entries = LinkedHashMap<String, String>()
        for ((stored, raw) in files.data) {
            val plain = plainKey(stored) ?: continue
            val text = raw as? String ?: continue
            entries[plain] = text
            storedNames.getOrPut(plain) { ArrayList() }.add(stored)
        }
        if (entries.isNotEmpty()) {
            val (savedKey, savedStorage) = savedAlgorithms(files)
            if (savedKey == FssKeyAlgorithm.AES_GCM_NoPadding) {
                undecryptable.addAll(entries.keys)
            } else {
                val combos = buildList {
                    add(savedKey to savedStorage)
                    for (k in listOf(FssKeyAlgorithm.RSA_ECB_OAEPwithSHA_256andMGF1Padding, FssKeyAlgorithm.RSA_ECB_PKCS1Padding)) {
                        for (s in listOf(FssStorageAlgorithm.AES_GCM_NoPadding, FssStorageAlgorithm.AES_CBC_PKCS7Padding)) {
                            if (!(k == savedKey && s == savedStorage)) add(k to s)
                        }
                    }
                }
                val pending = LinkedHashMap(entries)
                for ((keyAlg, storageAlg) in combos) {
                    if (pending.isEmpty()) break
                    val aesKey = storageKey(files, keystore, packageName, keyAlg, storageAlg) ?: continue
                    val opened = ArrayList<String>()
                    for ((plain, text) in pending) {
                        val value = runCatching { decryptValue(aesKey, storageAlg, Base64s.decode(text)) }.getOrNull() ?: continue
                        values[plain] = value
                        opened.add(plain)
                    }
                    if (opened.isNotEmpty()) sources.add("$keyAlg/$storageAlg")
                    for (plain in opened) pending.remove(plain)
                }
                undecryptable.addAll(pending.keys)
            }
        }

        val espCandidates = files.data.filterKeys { plainKey(it) == null }
        val esp = TinkEsp.decryptAll(espCandidates, DATA_PREFS) { sealed ->
            keystore.aesGcmOpen(TinkEsp.MASTER_KEY_ALIAS, sealed, ByteArray(0))
        }
        if (esp != null) {
            var espCount = 0
            for (entry in esp.entries) {
                val stored = entry.key ?: continue
                val plain = plainKey(stored) ?: continue
                val text = entry.value as? String ?: continue

                if (!values.containsKey(plain)) {
                    values[plain] = text
                    espCount++
                }
                storedNames.getOrPut(plain) { ArrayList() }.add(entry.storedName)
                undecryptable.remove(plain)
            }
            undecryptable.addAll(esp.failures.map { "esp:$it" })
            if (espCount > 0) sources.add("EncryptedSharedPreferences")
        }

        return FlutterSecrets(values, undecryptable, sources.joinToString(", ").ifEmpty { "none" }, storedNames)
    }

    private fun storageKey(
        files: FlutterSecureStorageFiles,
        keystore: FlutterKeystore,
        packageName: String,
        keyAlg: FssKeyAlgorithm,
        storageAlg: FssStorageAlgorithm,
    ): ByteArray? {
        val names = when (storageAlg) {
            FssStorageAlgorithm.AES_GCM_NoPadding -> listOf(WRAPPED_KEY_GCM, WRAPPED_KEY_GCM_V9)
            FssStorageAlgorithm.AES_CBC_PKCS7Padding -> listOf(WRAPPED_KEY_CBC)
        }
        for (name in names) {
            val wrappedText = files.keyStorage[name] as? String ?: continue
            val wrapped = runCatching { Base64s.decode(wrappedText) }.getOrNull() ?: continue
            val oaep = keyAlg == FssKeyAlgorithm.RSA_ECB_OAEPwithSHA_256andMGF1Padding
            val key = try {
                keystore.rsaUnwrap(rsaAlias(packageName, keyAlg), oaep, wrapped)
            } catch (unavailable: FlutterKeystoreUnavailable) {
                throw unavailable
            } catch (_: Exception) {
                null
            }
            if (key != null && (key.size == 16 || key.size == 24 || key.size == 32)) return key
        }
        return null
    }

    fun decryptValue(key: ByteArray, algorithm: FssStorageAlgorithm, input: ByteArray): String {
        val plain = when (algorithm) {
            FssStorageAlgorithm.AES_CBC_PKCS7Padding -> {
                require(input.size > 16 && (input.size - 16) % 16 == 0) { "not a CBC payload" }
                val cipher = Cipher.getInstance("AES/CBC/PKCS5Padding")
                cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), IvParameterSpec(input, 0, 16))
                cipher.doFinal(input, 16, input.size - 16)
            }
            FssStorageAlgorithm.AES_GCM_NoPadding -> {
                require(input.size >= 12 + 16) { "not a GCM payload" }
                val cipher = Cipher.getInstance("AES/GCM/NoPadding")
                cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, input, 0, 12))
                cipher.doFinal(input, 12, input.size - 12)
            }
        }

        val decoder = Charsets.UTF_8.newDecoder()
            .onMalformedInput(java.nio.charset.CodingErrorAction.REPORT)
            .onUnmappableCharacter(java.nio.charset.CodingErrorAction.REPORT)
        return decoder.decode(java.nio.ByteBuffer.wrap(plain)).toString()
    }
}
