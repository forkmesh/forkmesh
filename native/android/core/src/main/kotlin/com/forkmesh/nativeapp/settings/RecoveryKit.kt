package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.coves.CoveJson
import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.crypto.Ed25519
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.security.PortableIdentity
import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter

class RecoveryKitException(message: String) : Exception(message) {
    companion object {
        val shortPassphrase get() = RecoveryKitException("Use a recovery passphrase of at least ${RecoveryKit.MIN_PASSPHRASE} characters.")
        val mismatchedPassphrase get() = RecoveryKitException("The two passphrases do not match.")
        val noIdentity get() = RecoveryKitException("This device has no identity key yet.")
        val notASeed get() = RecoveryKitException("That kit does not hold an Ed25519 identity.")
        fun differentKey(key: String) = RecoveryKitException("That kit holds a different key (${RecoveryKit.shortKey(key)}).")
    }
}

data class RecoveryKitCheck(val publicKey: String, val createdAt: Long, val matchesThisDevice: Boolean)

object RecoveryKit {
    const val SEED_TYPE = "forkmesh.ed25519-seed"
    const val MIN_PASSPHRASE = 12
    const val FILE_EXTENSION = "json"

    private val DAY = DateTimeFormatter.ofPattern("yyyy-MM-dd").withZone(ZoneOffset.UTC)

    fun validatePassphrase(passphrase: String, confirm: String): RecoveryKitException? {
        if (passphrase.length < MIN_PASSPHRASE) return RecoveryKitException.shortPassphrase
        if (passphrase != confirm) return RecoveryKitException.mismatchedPassphrase
        return null
    }

    fun strength(passphrase: String): Int {
        val count = passphrase.length
        if (count < MIN_PASSPHRASE) return 0
        var classes = 0
        if (passphrase.any { it.isLowerCase() }) classes++
        if (passphrase.any { it.isUpperCase() }) classes++
        if (passphrase.any { it.isDigit() }) classes++
        if (passphrase.any { !it.isLetter() && !it.isDigit() }) classes++
        if (count >= 20 && classes >= 2) return 3
        if (count >= 16 || classes >= 3) return 2
        return 1
    }

    fun payload(seed: ByteArray, publicKey: ByteArray): Map<String, Any?> = linkedMapOf(
        "type" to SEED_TYPE,
        "schemaVersion" to 1L,
        "publicKey" to B64.encodeUrlNoPadding(publicKey),
        "seed" to B64.encodeUrlNoPadding(seed),
    )

    fun export(identity: DeviceIdentity, passphrase: String, confirm: String, createdAt: Long): String {
        validatePassphrase(passphrase, confirm)?.let { throw it }
        val seed = identity.exportSeed()
        try {
            if (seed.size != 32) throw RecoveryKitException.noIdentity
            val container = PortableIdentity.export(payload(seed, identity.publicKeyBytes), passphrase, createdAt)
            return CoveJson.pretty(container, indent = "  ")
        } finally {
            seed.fill(0)
        }
    }

    fun check(kit: String, passphrase: String, currentPublicKey: String): RecoveryKitCheck {
        if (passphrase.length < MIN_PASSPHRASE) throw RecoveryKitException.shortPassphrase
        if (kit.toByteArray().size > 256 * 1024) throw RecoveryKitException("That file is not a ForkMesh recovery kit.")
        @Suppress("UNCHECKED_CAST")
        val container = try {
            Json.parse(kit) as? Map<String, Any?>
        } catch (_: Exception) {
            null
        } ?: throw RecoveryKitException("That file is not a ForkMesh recovery kit.")
        val clear = try {
            PortableIdentity.import(container, passphrase)
        } catch (failure: PortableIdentity.FormatException) {
            throw RecoveryKitException(failure.message ?: "That file is not a ForkMesh recovery kit.")
        } catch (failure: IllegalArgumentException) {
            throw RecoveryKitException(failure.message ?: "That file is not a ForkMesh recovery kit.")
        }
        if (clear.text("type") != SEED_TYPE) throw RecoveryKitException.notASeed
        val seed = decodeUnpadded(clear.text("seed")) ?: throw RecoveryKitException.notASeed
        try {
            if (seed.size != 32) throw RecoveryKitException.notASeed
            val derived = try {
                Ed25519.publicKey(seed)
            } catch (_: Exception) {
                throw RecoveryKitException.notASeed
            }
            val publicKey = B64.encodeUrlNoPadding(derived)
            val recorded = clear.text("publicKey")
            if (recorded.isNotEmpty() && recorded != publicKey) throw RecoveryKitException.notASeed
            return RecoveryKitCheck(publicKey, container.long("createdAt"), publicKey == currentPublicKey)
        } finally {
            seed.fill(0)
        }
    }

    fun fileName(account: String, createdAt: Long): String {
        val cleaned = account.lowercase().filter { it.isLetterOrDigit() || it == '-' }
        val stem = if (cleaned.isEmpty()) "forkmesh" else "forkmesh-$cleaned"
        return "$stem-recovery-kit-${DAY.format(Instant.ofEpochMilli(createdAt))}.$FILE_EXTENSION"
    }

    private fun decodeUnpadded(text: String): ByteArray? {
        if (text.isEmpty() || !text.all { it.isLetterOrDigit() && it.code < 128 || it == '-' || it == '_' }) return null
        return try {
            java.util.Base64.getUrlDecoder().decode(text + "=".repeat((4 - text.length % 4) % 4))
        } catch (_: IllegalArgumentException) {
            null
        }
    }

    fun shortKey(key: String): String = if (key.length <= 16) key else "${key.take(8)}...${key.takeLast(8)}"
}

data class IdentityBackupState(val backedUpKey: String, val backedUpAt: Long, val snoozedUntil: Long) {
    fun needsBackup(currentKey: String): Boolean = currentKey.isNotEmpty() && backedUpKey != currentKey

    fun showsNag(currentKey: String, nowMillis: Long): Boolean = needsBackup(currentKey) && nowMillis >= snoozedUntil
}

class IdentityBackupStore(private val prefs: PrefStore) {
    val state: IdentityBackupState
        get() = IdentityBackupState(
            prefs.getString(KEY_KEY).orEmpty(),
            (prefs.getDouble(AT_KEY) ?: 0.0).toLong(),
            (prefs.getDouble(SNOOZE_KEY) ?: 0.0).toLong(),
        )

    fun markBackedUp(publicKey: String, atMillis: Long) {
        prefs.putString(KEY_KEY, publicKey)
        prefs.putDouble(AT_KEY, atMillis.toDouble())
        prefs.remove(SNOOZE_KEY)
    }

    fun snooze(nowMillis: Long) {
        prefs.putDouble(SNOOZE_KEY, (nowMillis + SNOOZE_MILLIS).toDouble())
    }

    companion object {
        const val KEY_KEY = "identity/backedUpKey"
        const val AT_KEY = "identity/backedUpAt"
        const val SNOOZE_KEY = "identity/backupSnoozedUntil"
        const val SNOOZE_MILLIS = 7L * 86_400_000L
    }
}
