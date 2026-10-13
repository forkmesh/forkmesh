package com.forkmesh.nativeapp.feature.tools.vault

import android.app.KeyguardManager
import android.content.Context
import android.os.Build
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.nio.ByteBuffer
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.PrivateKey
import java.security.SecureRandom
import java.security.spec.MGF1ParameterSpec
import java.security.spec.X509EncodedKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.OAEPParameterSpec
import javax.crypto.spec.PSource
import javax.crypto.spec.SecretKeySpec

data class VaultLogin(val id: String, val title: String, val username: String, val host: String)

class VaultAutofillStore(context: Context) {
    private val context = context.applicationContext
    private val directory = File(this.context.noBackupFilesDir, "vault-autofill")
    private val keyStore: KeyStore = KeyStore.getInstance(KEYSTORE).apply { load(null) }

    class NeedsResync(message: String) : Exception(message)

    val deviceSecure: Boolean
        get() = (context.getSystemService(Context.KEYGUARD_SERVICE) as KeyguardManager).isDeviceSecure

    fun logins(): List<VaultLogin> {
        val root = readSealed(INDEX_FILE) ?: return emptyList()
        val rows = root.optJSONArray("logins") ?: return emptyList()
        return (0 until rows.length()).map { rows.getJSONObject(it) }.map {
            VaultLogin(it.optString("id"), it.optString("title"), it.optString("username"), it.optString("host"))
        }
    }

    val account: String get() = readSealed(INDEX_FILE)?.optString("account").orEmpty()
    val enabled: Boolean get() = File(directory, INDEX_FILE).isFile

    fun sync(account: String, logins: List<Map<String, String>>): Boolean {
        if (!deviceSecure) return false
        directory.mkdirs()
        val index = JSONArray()
        val passwords = JSONObject()
        for (login in logins) {
            val id = login["id"].orEmpty()
            if (id.isEmpty() || login["password"].isNullOrEmpty()) continue
            index.put(JSONObject().put("id", id).put("title", login["title"].orEmpty())
                .put("username", login["username"].orEmpty()).put("host", login["host"].orEmpty()))
            passwords.put(id, login["password"])
        }
        writeSecrets(passwords.toString().toByteArray())
        writeSealed(INDEX_FILE, JSONObject().put("account", account).put("logins", index))
        val known = (0 until index.length()).map { index.getJSONObject(it).getString("id") }.toSet()
        val links = links()
        val pruned = links.mapValues { (_, ids) -> ids.filter { it in known } }.filterValues { it.isNotEmpty() }
        if (pruned != links) writeLinks(pruned)
        return true
    }

    fun clear() {
        directory.listFiles()?.forEach { it.delete() }
        for (alias in listOf(INDEX_KEY, SECRETS_KEY)) {
            if (keyStore.containsAlias(alias)) keyStore.deleteEntry(alias)
        }
    }

    fun password(id: String): String {
        val file = File(directory, SECRETS_FILE)
        if (!file.isFile) throw NeedsResync("no autofill cache")
        val buffer = ByteBuffer.wrap(file.readBytes())
        val wrapped = ByteArray(buffer.short.toInt()).also { buffer.get(it) }
        val iv = ByteArray(GCM_IV).also { buffer.get(it) }
        val sealed = ByteArray(buffer.remaining()).also { buffer.get(it) }
        val private = try {
            keyStore.getKey(SECRETS_KEY, null) as? PrivateKey
        } catch (error: Exception) {
            null
        } ?: throw NeedsResync("the autofill key is gone")
        val unwrap = Cipher.getInstance(RSA_OAEP)
        try {
            unwrap.init(Cipher.DECRYPT_MODE, private, OAEP)
        } catch (error: android.security.keystore.KeyPermanentlyInvalidatedException) {
            clear()
            throw NeedsResync("the autofill key was invalidated")
        }
        val dataKey = unwrap.doFinal(wrapped)
        try {
            val cipher = Cipher.getInstance(AES_GCM)
            cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(dataKey, "AES"), GCMParameterSpec(GCM_TAG_BITS, iv))
            val plain = cipher.doFinal(sealed)
            try {
                return JSONObject(String(plain)).optString(id)
            } finally {
                plain.fill(0)
            }
        } finally {
            dataKey.fill(0)
        }
    }

    fun secretsKeyProtection(): String {
        val key = keyStore.getKey(SECRETS_KEY, null) as? PrivateKey ?: return "missing"
        val info = KeyFactory.getInstance(key.algorithm, KEYSTORE)
            .getKeySpec(key, android.security.keystore.KeyInfo::class.java)
        @Suppress("DEPRECATION")
        return "authRequired=${info.isUserAuthenticationRequired} " +
            "validity=${info.userAuthenticationValidityDurationSeconds} " +
            "hardwareEnforced=${info.isUserAuthenticationRequirementEnforcedBySecureHardware} " +
            "insideSecureHardware=${info.isInsideSecureHardware}"
    }

    private fun writeSecrets(plain: ByteArray) {
        val dataKey = ByteArray(32).also { SecureRandom().nextBytes(it) }
        try {
            val iv = ByteArray(GCM_IV).also { SecureRandom().nextBytes(it) }
            val cipher = Cipher.getInstance(AES_GCM)
            cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(dataKey, "AES"), GCMParameterSpec(GCM_TAG_BITS, iv))
            val sealed = cipher.doFinal(plain)
            val public = KeyFactory.getInstance("RSA")
                .generatePublic(X509EncodedKeySpec(secretsCertificateKey()))
            val wrap = Cipher.getInstance(RSA_OAEP)
            wrap.init(Cipher.ENCRYPT_MODE, public, OAEP)
            val wrapped = wrap.doFinal(dataKey)
            val out = ByteBuffer.allocate(2 + wrapped.size + iv.size + sealed.size)
            out.putShort(wrapped.size.toShort()).put(wrapped).put(iv).put(sealed)
            File(directory, SECRETS_FILE).writeBytes(out.array())
        } finally {
            dataKey.fill(0)
            plain.fill(0)
        }
    }

    private fun secretsCertificateKey(): ByteArray {
        if (!keyStore.containsAlias(SECRETS_KEY)) {
            val spec = KeyGenParameterSpec.Builder(SECRETS_KEY, KeyProperties.PURPOSE_DECRYPT)
                .setKeySize(2048)
                .setDigests(KeyProperties.DIGEST_SHA256)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_RSA_OAEP)
                .setUserAuthenticationRequired(true)
                .apply {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                        setUserAuthenticationParameters(
                            AUTH_SECONDS,
                            KeyProperties.AUTH_BIOMETRIC_STRONG or KeyProperties.AUTH_DEVICE_CREDENTIAL,
                        )
                    } else {
                        @Suppress("DEPRECATION")
                        setUserAuthenticationValidityDurationSeconds(AUTH_SECONDS)
                    }
                }
                .build()
            KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_RSA, KEYSTORE).apply {
                initialize(spec)
                generateKeyPair()
            }
        }
        return keyStore.getCertificate(SECRETS_KEY).publicKey.encoded
    }

    fun links(): Map<String, List<String>> {
        val root = readSealed(LINKS_FILE) ?: return emptyMap()
        return root.keys().asSequence().associateWith { name ->
            val ids = root.getJSONArray(name)
            (0 until ids.length()).map(ids::getString)
        }
    }

    fun remember(packageName: String, id: String) {
        val links = links().toMutableMap()
        links[packageName] = (listOf(id) + links[packageName].orEmpty()).distinct().take(MAX_LINKS_PER_APP)
        writeLinks(links)
    }

    private fun writeLinks(links: Map<String, List<String>>) {
        val root = JSONObject()
        for ((name, ids) in links) root.put(name, JSONArray(ids))
        writeSealed(LINKS_FILE, root)
    }

    fun addPending(save: Map<String, String>) {
        val root = readSealed(PENDING_FILE) ?: JSONObject()
        val list = root.optJSONArray("saves") ?: JSONArray()
        if (list.length() >= MAX_PENDING) return
        list.put(JSONObject(save))
        writeSealed(PENDING_FILE, root.put("saves", list))
    }

    fun takePending(): List<Map<String, String>> {
        val list = readSealed(PENDING_FILE)?.optJSONArray("saves") ?: return emptyList()
        File(directory, PENDING_FILE).delete()
        return (0 until list.length()).map { list.getJSONObject(it) }.map { row ->
            row.keys().asSequence().associateWith { row.optString(it) }
        }
    }

    private fun indexKey(): SecretKey {
        (keyStore.getKey(INDEX_KEY, null) as? SecretKey)?.let { return it }
        val spec = KeyGenParameterSpec.Builder(
            INDEX_KEY, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
        ).setKeySize(256)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .build()
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE).run {
            init(spec)
            generateKey()
        }
    }

    private fun writeSealed(name: String, value: JSONObject) {
        directory.mkdirs()
        val cipher = Cipher.getInstance(AES_GCM)
        cipher.init(Cipher.ENCRYPT_MODE, indexKey())
        File(directory, name).writeBytes(cipher.iv + cipher.doFinal(value.toString().toByteArray()))
    }

    private fun readSealed(name: String): JSONObject? {
        val file = File(directory, name)
        if (!file.isFile) return null
        return try {
            val bytes = file.readBytes()
            val cipher = Cipher.getInstance(AES_GCM)
            cipher.init(Cipher.DECRYPT_MODE, indexKey(), GCMParameterSpec(GCM_TAG_BITS, bytes, 0, GCM_IV))
            JSONObject(String(cipher.doFinal(bytes, GCM_IV, bytes.size - GCM_IV)))
        } catch (error: Exception) {
            null
        }
    }

    companion object {
        const val AUTH_SECONDS = 30
        private const val KEYSTORE = "AndroidKeyStore"
        private const val INDEX_KEY = "forkmesh.vault.autofill.index"
        private const val SECRETS_KEY = "forkmesh.vault.autofill.secrets"
        private const val INDEX_FILE = "index.bin"
        private const val SECRETS_FILE = "secrets.bin"
        private const val LINKS_FILE = "links.bin"
        private const val PENDING_FILE = "pending.bin"
        private const val AES_GCM = "AES/GCM/NoPadding"
        private const val RSA_OAEP = "RSA/ECB/OAEPWithSHA-256AndMGF1Padding"
        private const val GCM_IV = 12
        private const val GCM_TAG_BITS = 128
        private const val MAX_LINKS_PER_APP = 5
        private const val MAX_PENDING = 20
        private val OAEP = OAEPParameterSpec("SHA-256", "MGF1", MGF1ParameterSpec.SHA1, PSource.PSpecified.DEFAULT)
    }
}
