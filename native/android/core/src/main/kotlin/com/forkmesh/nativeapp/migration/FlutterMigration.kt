package com.forkmesh.nativeapp.migration

import com.forkmesh.nativeapp.security.SecretStore

interface FlutterSource {
    val packageName: String

    val keystore: FlutterKeystore

    fun secureFiles(): FlutterSecureStorageFiles

    fun preferences(): Map<String, Any?>

    fun deleteSecureEntries(storedNames: Collection<String>)

    fun deletePreferences(keys: Collection<String>)
}

interface NativePreferences {
    fun read(key: String): PreferenceValue?

    fun write(key: String, value: PreferenceValue)

    fun remove(key: String)
}

sealed class FlutterMigrationOutcome {
    object AlreadyDone : FlutterMigrationOutcome()

    object NothingToMigrate : FlutterMigrationOutcome()

    data class Migrated(val secrets: Int, val preferences: Int, val undecryptable: Int) : FlutterMigrationOutcome()

    data class Deferred(val reason: String) : FlutterMigrationOutcome()
}

class FlutterMigration(
    private val flutter: FlutterSource,
    private val secrets: SecretStore,
    private val prefs: NativePreferences,
    private val clock: () -> Long = System::currentTimeMillis,
) {
    companion object {
        const val DONE_KEY = "migration/flutter/done"
        const val DONE_AT_KEY = "migration/flutter/doneAt"
        const val SECRET_KEYS_KEY = "migration/flutter/secretKeys"

        const val SECRET_ENTRIES_KEY = "migration/flutter/secretEntries"
        const val PREFERENCE_KEYS_KEY = "migration/flutter/preferenceKeys"
        const val UNDECRYPTABLE_KEY = "migration/flutter/undecryptable"
        const val RETIRED_KEY = "migration/flutter/retired"
        const val ATTEMPTS_KEY = "migration/flutter/attempts"

        const val UPGRADED_KEY = "migration/flutter/upgraded"

        const val ACTIVE_PROJECT_KEY = "relays/active"

        const val UPGRADE_PROJECT_ID = "app.forkmesh.com"

        private val DECIMAL_STRING_KEYS = setOf("ads/hiddenUntilMs")

        private fun isBookkeeping(key: String) = key.startsWith("migration/flutter/")

        fun nativeForm(key: String, value: PreferenceValue): PreferenceValue =
            if (key in DECIMAL_STRING_KEYS && value is PreferenceValue.Int64) PreferenceValue.Text(value.value.toString()) else value
    }

    val isDone: Boolean get() = (prefs.read(DONE_KEY) as? PreferenceValue.Bool)?.value == true
    val isRetired: Boolean get() = (prefs.read(RETIRED_KEY) as? PreferenceValue.Bool)?.value == true

    private fun list(key: String): List<String> = (prefs.read(key) as? PreferenceValue.TextList)?.value ?: emptyList()

    val migratedSecretKeys: List<String> get() = list(SECRET_KEYS_KEY)
    val migratedPreferenceKeys: List<String> get() = list(PREFERENCE_KEYS_KEY)
    val undecryptableSecretKeys: List<String> get() = list(UNDECRYPTABLE_KEY)

    val migratedAnySession: Boolean
        get() = migratedSecretKeys.any { it == "auth/session" || it.startsWith("auth/session/") }

    val migratedSessionProjects: List<String>
        get() = migratedSecretKeys.filter { it.startsWith("auth/session/") }.map { it.removePrefix("auth/session/") }.sorted()

    fun run(): FlutterMigrationOutcome {
        if (isDone) return FlutterMigrationOutcome.AlreadyDone
        val attempts = (prefs.read(ATTEMPTS_KEY) as? PreferenceValue.Int64)?.value ?: 0L
        prefs.write(ATTEMPTS_KEY, PreferenceValue.Int64(attempts + 1))

        val files = try {
            flutter.secureFiles()
        } catch (error: Exception) {
            return FlutterMigrationOutcome.Deferred("Flutter secure storage unreadable: $error")
        }
        val source = try {
            FlutterSecureStorageReader.readAll(files, flutter.keystore, flutter.packageName)
        } catch (error: FlutterKeystoreUnavailable) {
            return FlutterMigrationOutcome.Deferred("Keystore unavailable: ${error.message}")
        } catch (error: Exception) {
            return FlutterMigrationOutcome.Deferred("Flutter secure storage unreadable: $error")
        }
        val sourcePrefs = try {
            FlutterPreferences.decode(flutter.preferences()).filterKeys { !isBookkeeping(it) }
        } catch (error: Exception) {
            return FlutterMigrationOutcome.Deferred("Flutter preferences unreadable: $error")
        }
        val sourceSecrets = source.values

        if (sourceSecrets.isEmpty() && sourcePrefs.isEmpty() && source.undecryptable.isEmpty()) {
            markDone(emptyList(), emptyList(), emptyList(), emptyList())
            return FlutterMigrationOutcome.NothingToMigrate
        }

        for (key in sourceSecrets.keys.sorted()) {
            val value = sourceSecrets.getValue(key)
            try {
                if (secrets.read(key) != value) secrets.write(key, value)
            } catch (error: Exception) {
                return FlutterMigrationOutcome.Deferred("native secret write failed for $key: $error")
            }
        }
        for (key in sourcePrefs.keys.sorted()) {
            val value = nativeForm(key, sourcePrefs.getValue(key))
            if (prefs.read(key) != value) prefs.write(key, value)
        }

        val active = (sourcePrefs[ACTIVE_PROJECT_KEY] as? PreferenceValue.Text)?.value
        if (active.isNullOrEmpty()) prefs.write(ACTIVE_PROJECT_KEY, PreferenceValue.Text(UPGRADE_PROJECT_ID))

        for ((key, value) in sourceSecrets) {
            val stored = try {
                secrets.read(key)
            } catch (error: Exception) {
                return FlutterMigrationOutcome.Deferred("native secret read-back failed for $key: $error")
            }
            if (stored != value) return FlutterMigrationOutcome.Deferred("native secret read-back mismatch for $key")
        }
        for ((key, value) in sourcePrefs) {
            if (prefs.read(key) != nativeForm(key, value)) return FlutterMigrationOutcome.Deferred("preference read-back mismatch for $key")
        }

        val entries = sourceSecrets.keys.flatMap { source.storedNames[it] ?: listOf(FlutterSecureStorageReader.prefixed(it)) }
        markDone(sourceSecrets.keys.sorted(), entries.sorted(), sourcePrefs.keys.sorted(), source.undecryptable.sorted())
        prefs.write(UPGRADED_KEY, PreferenceValue.Bool(true))
        return FlutterMigrationOutcome.Migrated(sourceSecrets.size, sourcePrefs.size, source.undecryptable.size)
    }

    private fun markDone(secretKeys: List<String>, entries: List<String>, preferenceKeys: List<String>, undecryptable: List<String>) {
        prefs.write(SECRET_KEYS_KEY, PreferenceValue.TextList(secretKeys))
        prefs.write(SECRET_ENTRIES_KEY, PreferenceValue.TextList(entries))
        prefs.write(PREFERENCE_KEYS_KEY, PreferenceValue.TextList(preferenceKeys))
        prefs.write(UNDECRYPTABLE_KEY, PreferenceValue.TextList(undecryptable))
        prefs.write(DONE_AT_KEY, PreferenceValue.Int64(clock()))
        prefs.write(DONE_KEY, PreferenceValue.Bool(true))
    }

    fun canRetire(sessionAccepted: Boolean): Boolean {
        if (!isDone || isRetired) return false
        if (undecryptableSecretKeys.isNotEmpty()) return false
        if (list(SECRET_ENTRIES_KEY).isEmpty() && migratedPreferenceKeys.isEmpty()) return true
        return sessionAccepted && migratedAnySession
    }

    fun retireFlutterData(sessionAccepted: Boolean): Boolean {
        if (!canRetire(sessionAccepted)) return false
        try {
            flutter.deleteSecureEntries(list(SECRET_ENTRIES_KEY))
            flutter.deletePreferences(migratedPreferenceKeys)
        } catch (_: Exception) {
            return false
        }
        prefs.write(RETIRED_KEY, PreferenceValue.Bool(true))
        return true
    }
}
