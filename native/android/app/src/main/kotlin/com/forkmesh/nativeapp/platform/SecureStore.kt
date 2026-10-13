package com.forkmesh.nativeapp.platform

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import com.forkmesh.nativeapp.security.SecretStore
import java.nio.ByteBuffer
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

class SecureStore(context: Context) : SecretStore {
    private val prefs = context.applicationContext.getSharedPreferences("forkmesh.secure", Context.MODE_PRIVATE)

    @Synchronized
    override fun read(key: String): String? {
        val sealed = prefs.getString(key, null) ?: return null
        return try {
            val packed = ByteBuffer.wrap(Base64.decode(sealed, Base64.NO_WRAP))
            val iv = ByteArray(IV_BYTES).also { packed.get(it) }
            val ciphertext = ByteArray(packed.remaining()).also { packed.get(it) }
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(Cipher.DECRYPT_MODE, secretKey(), GCMParameterSpec(TAG_BITS, iv))
            cipher.updateAAD(key.toByteArray(Charsets.UTF_8))
            String(cipher.doFinal(ciphertext), Charsets.UTF_8)
        } catch (_: Exception) {
            null
        }
    }

    @Synchronized
    override fun write(key: String, value: String) {
        writeChecked(key, value)
    }

    @Synchronized
    fun writeChecked(key: String, value: String): Boolean {
        return try {
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(Cipher.ENCRYPT_MODE, secretKey())
            cipher.updateAAD(key.toByteArray(Charsets.UTF_8))
            val ciphertext = cipher.doFinal(value.toByteArray(Charsets.UTF_8))
            val iv = cipher.iv
            val packed = ByteBuffer.allocate(iv.size + ciphertext.size).put(iv).put(ciphertext).array()
            prefs.edit().putString(key, Base64.encodeToString(packed, Base64.NO_WRAP)).commit()
        } catch (_: Exception) {
            false
        }
    }

    @Synchronized
    override fun delete(key: String) {
        prefs.edit().remove(key).commit()
    }

    @Synchronized
    fun contains(key: String): Boolean = prefs.contains(key)

    @Synchronized
    fun keys(): Set<String> = prefs.all.keys.toSet()

    @Synchronized
    fun deleteAll() {
        prefs.edit().clear().commit()
    }

    private fun secretKey(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getEntry(KEY_ALIAS, null) as? KeyStore.SecretKeyEntry)?.let { return it.secretKey }
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        generator.init(
            KeyGenParameterSpec.Builder(KEY_ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build(),
        )
        return generator.generateKey()
    }

    private companion object {
        const val KEY_ALIAS = "forkmesh.secure.v1"
        const val TRANSFORMATION = "AES/GCM/NoPadding"
        const val IV_BYTES = 12
        const val TAG_BITS = 128
    }
}
