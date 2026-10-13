package com.forkmesh.nativeapp.feature.account.projects

import android.content.Context
import android.util.Log
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.migration.FlutterKeystore
import com.forkmesh.nativeapp.migration.FlutterKeystoreUnavailable
import com.forkmesh.nativeapp.migration.FlutterMigration
import com.forkmesh.nativeapp.migration.FlutterMigrationOutcome
import com.forkmesh.nativeapp.migration.FlutterPreferences
import com.forkmesh.nativeapp.migration.FlutterSecureStorageFiles
import com.forkmesh.nativeapp.migration.FlutterSecureStorageReader
import com.forkmesh.nativeapp.migration.FlutterSource
import com.forkmesh.nativeapp.migration.NativePreferences
import com.forkmesh.nativeapp.migration.PreferenceValue
import com.forkmesh.nativeapp.platform.Prefs
import com.forkmesh.nativeapp.security.SecretStore
import java.security.KeyStore
import java.security.KeyStoreException
import java.security.PrivateKey
import java.security.UnrecoverableKeyException
import java.security.spec.MGF1ParameterSpec
import javax.crypto.Cipher
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.OAEPParameterSpec
import javax.crypto.spec.PSource

class AndroidFlutterKeystore : FlutterKeystore {
    private fun keyStore(): KeyStore = try {
        KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
    } catch (error: Exception) {
        throw FlutterKeystoreUnavailable("AndroidKeyStore did not load", error)
    }

    private fun key(alias: String): java.security.Key? = try {
        keyStore().getKey(alias, null)
    } catch (error: KeyStoreException) {
        throw FlutterKeystoreUnavailable("AndroidKeyStore refused $alias", error)
    } catch (error: UnrecoverableKeyException) {
        throw FlutterKeystoreUnavailable("AndroidKeyStore could not recover $alias", error)
    }

    override fun rsaUnwrap(alias: String, oaep: Boolean, wrapped: ByteArray): ByteArray? {
        val key = key(alias) as? PrivateKey ?: return null
        val cipher = if (oaep) {
            Cipher.getInstance("RSA/ECB/OAEPPadding", "AndroidKeyStoreBCWorkaround").apply {
                init(Cipher.UNWRAP_MODE, key, OAEPParameterSpec("SHA-256", "MGF1", MGF1ParameterSpec.SHA1, PSource.PSpecified.DEFAULT))
            }
        } else {
            Cipher.getInstance("RSA/ECB/PKCS1Padding", "AndroidKeyStoreBCWorkaround").apply { init(Cipher.UNWRAP_MODE, key) }
        }
        return cipher.unwrap(wrapped, "AES", Cipher.SECRET_KEY).encoded
    }

    override fun aesGcmOpen(alias: String, sealed: ByteArray, aad: ByteArray): ByteArray? {
        val key = key(alias) as? SecretKey ?: return null
        if (sealed.size < 12 + 16) return null
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(128, sealed, 0, 12))
        if (aad.isNotEmpty()) cipher.updateAAD(aad)
        return cipher.doFinal(sealed, 12, sealed.size - 12)
    }
}

class AndroidFlutterSource(private val context: Context) : FlutterSource {
    override val packageName: String = context.packageName
    override val keystore: FlutterKeystore = AndroidFlutterKeystore()

    private fun file(name: String) = context.applicationContext.getSharedPreferences(name, Context.MODE_PRIVATE)

    override fun secureFiles(): FlutterSecureStorageFiles = FlutterSecureStorageFiles(
        data = file(FlutterSecureStorageReader.DATA_PREFS).all,
        keyStorage = file(FlutterSecureStorageReader.KEY_PREFS).all,
        config = file(FlutterSecureStorageReader.CONFIG_PREFS).all,
        legacyConfig = file(FlutterSecureStorageReader.LEGACY_CONFIG_PREFS).all,
    )

    override fun preferences(): Map<String, Any?> = file(FlutterPreferences.FILE).all

    override fun deleteSecureEntries(storedNames: Collection<String>) {
        val editor = file(FlutterSecureStorageReader.DATA_PREFS).edit()
        for (name in storedNames) editor.remove(name)
        check(editor.commit()) { "Flutter secure storage entries were not removed" }
    }

    override fun deletePreferences(keys: Collection<String>) {
        val editor = file(FlutterPreferences.FILE).edit()
        for (key in keys) editor.remove(FlutterPreferences.PREFIX + key)
        check(editor.commit()) { "Flutter preferences were not removed" }
    }
}

class NativePrefsAdapter(private val prefs: Prefs) : NativePreferences {
    private val doubles = HashSet<String>()

    override fun read(key: String): PreferenceValue? {
        if (key in doubles && prefs.contains(key)) return PreferenceValue.Real(prefs.getDouble(key, 0.0))
        return when (val raw = prefs.raw(key)) {
            null -> null
            is Boolean -> PreferenceValue.Bool(raw)
            is Long -> PreferenceValue.Int64(raw)
            is Int -> PreferenceValue.Int64(raw.toLong())
            is Float -> PreferenceValue.Real(raw.toDouble())
            is Set<*> -> PreferenceValue.TextList(raw.map { it.toString() })
            is String -> if (raw.startsWith(Prefs.LIST_MARKER)) PreferenceValue.TextList(prefs.getStringList(key).orEmpty()) else PreferenceValue.Text(raw)
            else -> null
        }
    }

    override fun write(key: String, value: PreferenceValue) {
        when (value) {
            is PreferenceValue.Bool -> prefs.commitRaw(key, value.value)
            is PreferenceValue.Int64 -> prefs.commitRaw(key, value.value)
            is PreferenceValue.Real -> {
                prefs.commitRaw(key, java.lang.Double.doubleToRawLongBits(value.value))
                doubles.add(key)
            }
            is PreferenceValue.Text -> prefs.commitRaw(key, value.value)
            is PreferenceValue.TextList -> prefs.putStringListNow(key, value.value)
        }
    }

    override fun remove(key: String) {
        prefs.commitRaw(key, null)
        doubles.remove(key)
    }
}

object FlutterDataMigration {
    private const val TAG = "ForkMeshMigration"

    fun migration(context: Context, secrets: SecretStore, prefs: Prefs): FlutterMigration =
        FlutterMigration(AndroidFlutterSource(context), secrets, NativePrefsAdapter(prefs))

    fun runBeforeLaunch(context: Context, secrets: SecretStore, prefs: Prefs): FlutterMigrationOutcome {
        val outcome = try {
            migration(context, secrets, prefs).run()
        } catch (error: Exception) {
            FlutterMigrationOutcome.Deferred("migration failed: $error")
        }
        when (outcome) {
            FlutterMigrationOutcome.AlreadyDone -> Unit
            is FlutterMigrationOutcome.Deferred -> Log.w(TAG, "Flutter data not migrated yet: ${outcome.reason}")
            else -> Log.i(TAG, "Flutter data migration: $outcome")
        }
        return outcome
    }

    fun isUpgraded(prefs: Prefs): Boolean = prefs.getBool(FlutterMigration.UPGRADED_KEY) == true

    fun retireWhenVerified(context: Context, secrets: SecretStore, prefs: Prefs, api: ApiService, signedIn: Boolean) {
        val migration = migration(context, secrets, prefs)
        if (!migration.isDone || migration.isRetired) return
        var accepted = false
        if (signedIn) {
            accepted = try {
                api.listDevices()
                true
            } catch (error: Exception) {
                Log.i(TAG, "Session not verified yet; the Flutter copies stay: $error")
                false
            }
        }
        if (migration.retireFlutterData(accepted)) Log.i(TAG, "Flutter copies removed after the native session was verified")
    }
}
