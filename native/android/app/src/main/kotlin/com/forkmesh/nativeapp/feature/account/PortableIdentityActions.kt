package com.forkmesh.nativeapp.feature.account

import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.PortableIdentity
import com.forkmesh.nativeapp.security.SecretStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.util.Base64

sealed class PortableIdentityResult {
    abstract val message: String

    data class Exported(val document: String) : PortableIdentityResult() {
        override val message: String get() = "Identity key saved."
    }

    data class Imported(val shortKey: String) : PortableIdentityResult() {
        override val message: String
            get() = "Restored identity $shortKey. Restart ForkMesh so every panel picks up the new key."
    }

    data class Failed(override val message: String) : PortableIdentityResult()

    data object NotAvailable : PortableIdentityResult() {
        override val message: String get() = PortableIdentityActions.NOT_AVAILABLE_MESSAGE
    }
}

object PortableIdentityActions {
    const val NOT_AVAILABLE_MESSAGE = "Portable identity export and import are not available in this build yet."

    const val FILE_NAME = "forkmesh-portable-identity.json"

    private const val SEED_PAYLOAD_TYPE = "forkmesh.ed25519-seed"
    private const val WRONG_PASSPHRASE = "Recovery passphrase is incorrect or the file was changed."
    private const val KEY_MISMATCH = "Portable identity key material does not match its public key."

    val available: Boolean
        get() = Services.get<DeviceIdentity>() != null && Services.get<SecretStore>() != null

    suspend fun exportIdentity(passphrase: String): PortableIdentityResult = withContext(Dispatchers.Default) {
        val identity = Services.get<DeviceIdentity>() ?: return@withContext PortableIdentityResult.NotAvailable
        val seed = identity.exportSeed()
        try {
            val payload = linkedMapOf<String, Any?>(
                "type" to SEED_PAYLOAD_TYPE,
                "schemaVersion" to 1L,
                "publicKey" to identity.publicKeyB64url,
                "seed" to Base64.getUrlEncoder().withoutPadding().encodeToString(seed),
            )
            PortableIdentityResult.Exported(Json.stringify(PortableIdentity.export(payload, passphrase)))
        } catch (error: IllegalArgumentException) {
            PortableIdentityResult.Failed(error.message ?: "Could not save the identity key.")
        } finally {
            seed.fill(0)
        }
    }

    suspend fun importIdentity(document: String, passphrase: String): PortableIdentityResult =
        withContext(Dispatchers.Default) {
            val store = Services.get<SecretStore>() ?: return@withContext PortableIdentityResult.NotAvailable
            val payload = try {
                PortableIdentity.import(Json.parseObject(document), passphrase)
            } catch (error: PortableIdentity.FormatException) {
                return@withContext PortableIdentityResult.Failed(error.message ?: WRONG_PASSPHRASE)
            } catch (error: IllegalArgumentException) {
                return@withContext PortableIdentityResult.Failed(error.message ?: WRONG_PASSPHRASE)
            }
            if (payload["type"] != SEED_PAYLOAD_TYPE || jsonLong(payload["schemaVersion"], -1L) != 1L) {
                return@withContext PortableIdentityResult.Failed(WRONG_PASSPHRASE)
            }
            val seed = decodeSeed(payload.str("seed")) ?: return@withContext PortableIdentityResult.Failed(WRONG_PASSPHRASE)
            try {
                val expected = payload.str("publicKey")
                if (expected.isEmpty() || Identity.fromSeed(seed).publicKeyB64url != expected) {
                    return@withContext PortableIdentityResult.Failed(KEY_MISMATCH)
                }
                store.write(Identity.SEED_KEY, Base64.getEncoder().encodeToString(seed))
                val identity = Identity.loadOrCreate(store)
                Services.put(DeviceIdentity::class.java, identity)
                Services.get<AuthService>()?.let { auth -> if (auth.isAuthenticated) auth.logout() }
                PortableIdentityResult.Imported(identity.shortKey)
            } finally {
                seed.fill(0)
            }
        }

    private fun decodeSeed(value: String): ByteArray? {
        if (value.isEmpty() || !value.all { it.isLetterOrDigit() || it == '-' || it == '_' }) return null
        val bytes = try {
            Base64.getUrlDecoder().decode(value)
        } catch (_: IllegalArgumentException) {
            return null
        }
        if (bytes.size != 32 || Base64.getUrlEncoder().withoutPadding().encodeToString(bytes) != value) return null
        return bytes
    }
}
