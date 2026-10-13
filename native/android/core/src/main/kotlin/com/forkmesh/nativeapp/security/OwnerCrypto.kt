package com.forkmesh.nativeapp.security

import com.forkmesh.nativeapp.crypto.AesGcm
import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.crypto.Hkdf
import com.forkmesh.nativeapp.crypto.MlKem768
import com.forkmesh.nativeapp.crypto.Sha256
import com.forkmesh.nativeapp.crypto.Utf8
import com.forkmesh.nativeapp.crypto.X25519
import com.forkmesh.nativeapp.crypto.dartNumEquals
import com.forkmesh.nativeapp.json.Json

class OwnerCrypto private constructor(
    private val x25519PublicKey: ByteArray,
    private val x25519PrivateKey: ByteArray,
    private val mlkemPublicKey: ByteArray,
    private val mlkemPrivateKey: ByteArray,
) : OwnerKeys {
    val x25519Public: ByteArray get() = x25519PublicKey.copyOf()

    val mlkemPublic: ByteArray get() = mlkemPublicKey.copyOf()

    override val keyId: String = B64.encodeUrlNoPadding(Sha256.hash(x25519PublicKey, mlkemPublicKey))

    override val publicBundle: Map<String, Any?>
        get() = linkedMapOf(
            "v" to 1L,
            "x25519" to B64.encodeUrlNoPadding(x25519PublicKey),
            "mlkem768" to B64.encodeUrlNoPadding(mlkemPublicKey),
        )

    private fun serialize(): String = Json.stringify(
        linkedMapOf(
            "v" to 1L,
            "x25519Pub" to B64.encode(x25519PublicKey),
            "x25519Priv" to B64.encode(x25519PrivateKey),
            "mlkemPub" to B64.encode(mlkemPublicKey),
            "mlkemPriv" to B64.encode(mlkemPrivateKey),
        ),
    )

    override fun isRecipientOf(envelope: Map<String, Any?>): Boolean = recipientKeyIds(envelope).contains(keyId)

    override fun open(envelope: Map<String, Any?>): ByteArray? {
        val ids = recipientKeyIds(envelope)
        val index = ids.indexOf(keyId)
        if (index < 0) return null

        val payloadNonce = fromB64UrlExact(envelope["nonce"], NONCE_BYTES)
        val payloadTag = fromB64UrlExact(envelope["tag"], TAG_BYTES)
        val payloadBody = fromB64UrlBounded(envelope["body"], MAX_BODY_BYTES)
        val recipient = (envelope["recipients"] as List<*>)[index] as Map<*, *>
        val ephemeral = fromB64UrlExact(recipient["x25519"], X25519_BYTES)
        val kemCiphertext = fromB64UrlExact(recipient["mlkem768"], MLKEM_CIPHERTEXT_BYTES)
        val wrapNonce = fromB64UrlExact(recipient["nonce"], NONCE_BYTES)
        val wrapTag = fromB64UrlExact(recipient["tag"], TAG_BYTES)
        val wrappedKey = fromB64UrlExact(recipient["key"], KEY_BYTES)
        if (payloadNonce == null ||
            payloadTag == null ||
            payloadBody == null ||
            ephemeral == null ||
            kemCiphertext == null ||
            wrapNonce == null ||
            wrapTag == null ||
            wrappedKey == null
        ) {
            return null
        }

        return try {
            val contentKey = unwrapContentKey(ephemeral, kemCiphertext, wrapNonce, wrapTag, wrappedKey) ?: return null
            AesGcm.decrypt(payloadBody, payloadNonce, payloadTag, contentKey)
        } catch (_: Exception) {
            null
        }
    }

    override fun openJson(envelope: Map<String, Any?>): Map<String, Any?>? {
        val plain = open(envelope) ?: return null
        return try {
            @Suppress("UNCHECKED_CAST")
            Json.parse(Utf8.decode(plain)) as? Map<String, Any?>
        } catch (_: Exception) {
            null
        }
    }

    private fun unwrapContentKey(
        ephemeral: ByteArray,
        kemCiphertext: ByteArray,
        wrapNonce: ByteArray,
        wrapTag: ByteArray,
        wrappedKey: ByteArray,
    ): ByteArray? {
        val ssX = X25519.sharedSecret(x25519PrivateKey, ephemeral)
        val ssM = MlKem768.decapsulate(mlkemPrivateKey, kemCiphertext)
        val kek = deriveKek(ssX, ssM, Bytes.concat(ephemeral, kemCiphertext))
        Bytes.wipe(ssX, ssM)
        val unwrapped = AesGcm.decryptOrNull(wrappedKey, wrapNonce, wrapTag, kek) ?: return null
        return if (unwrapped.size == KEY_BYTES) unwrapped else null
    }

    fun seal(plaintext: ByteArray, recipientBundles: List<Map<String, Any?>>): Map<String, Any?>? {
        if (recipientBundles.isEmpty() || recipientBundles.size > MAX_RECIPIENTS) return null
        val contentKey = Bytes.random(KEY_BYTES)
        val sealed = AesGcm.encrypt(plaintext, contentKey)

        val recipients = ArrayList<Map<String, Any?>>()
        val seen = HashSet<String>()
        for (bundle in recipientBundles) {
            val xPub = fromB64UrlExact(bundle["x25519"], X25519_BYTES)
            val mPub = fromB64UrlExact(bundle["mlkem768"], MLKEM_PUBLIC_BYTES)
            if (!dartNumEquals(bundle["v"], 1) || xPub == null || mPub == null) return null
            val kid = B64.encodeUrlNoPadding(Sha256.hash(xPub, mPub))
            if (!seen.add(kid)) return null

            val ephemeralPrivate = X25519.generatePrivateKey()
            val ephemeralPublic = X25519.publicKey(ephemeralPrivate)
            val ssX = X25519.sharedSecret(ephemeralPrivate, xPub)
            val encapsulation = MlKem768.encapsulate(mPub)
            val kek = deriveKek(ssX, encapsulation.sharedSecret, Bytes.concat(ephemeralPublic, encapsulation.ciphertext))
            val wrapped = AesGcm.encrypt(contentKey, kek)
            Bytes.wipe(ephemeralPrivate, ssX, encapsulation.sharedSecret, kek)
            recipients.add(
                linkedMapOf(
                    "kid" to kid,
                    "x25519" to B64.encodeUrlNoPadding(ephemeralPublic),
                    "mlkem768" to B64.encodeUrlNoPadding(encapsulation.ciphertext),
                    "nonce" to B64.encodeUrlNoPadding(wrapped.nonce),
                    "tag" to B64.encodeUrlNoPadding(wrapped.mac),
                    "key" to B64.encodeUrlNoPadding(wrapped.cipherText),
                ),
            )
        }
        Bytes.wipe(contentKey)

        return linkedMapOf(
            "kind" to ENVELOPE_KIND,
            "v" to 1L,
            "alg" to ENVELOPE_ALG,
            "nonce" to B64.encodeUrlNoPadding(sealed.nonce),
            "tag" to B64.encodeUrlNoPadding(sealed.mac),
            "body" to B64.encodeUrlNoPadding(sealed.cipherText),
            "recipients" to recipients,
        )
    }

    fun sealJson(payload: Map<String, Any?>, recipientBundles: List<Map<String, Any?>>): Map<String, Any?>? =
        seal(Utf8.encode(Json.stringify(payload)), recipientBundles)

    companion object {
        const val SEED_KEY = "ownerCrypto/hybridV1"
        const val ENVELOPE_KIND = "forkmesh.owner-sealed"
        const val ENVELOPE_ALG = "x25519+mlkem768/aes256gcm"

        const val MAX_RECIPIENTS = 6

        const val HKDF_SALT = "forkmesh-mirror-kem-v1"
        const val KEY_BYTES = 32
        const val NONCE_BYTES = 12
        const val TAG_BYTES = 16
        const val X25519_BYTES = 32
        const val MLKEM_PUBLIC_BYTES = 1184
        const val MLKEM_PRIVATE_BYTES = 2400
        const val MLKEM_CIPHERTEXT_BYTES = 1088
        const val MAX_BODY_BYTES = 1024 * 1024

        private val B64URL_TEXT = Regex("[A-Za-z0-9_-]+")

        fun loadOrCreate(store: SecretStore): OwnerCrypto {
            val stored = store.read(SEED_KEY)
            if (!stored.isNullOrEmpty()) {
                restore(stored)?.let { return it }
            }
            val created = generate()
            store.write(SEED_KEY, created.serialize())
            return created
        }

        fun forget(store: SecretStore) {
            store.delete(SEED_KEY)
        }

        private fun generate(): OwnerCrypto {
            val xPriv = X25519.generatePrivateKey()
            val xPub = X25519.publicKey(xPriv)
            val mlkem = MlKem768.generateKeyPair()
            return OwnerCrypto(xPub, xPriv, mlkem.publicKey, mlkem.secretKey)
        }

        private fun restore(stored: String): OwnerCrypto? = try {
            val data = Json.parse(stored)
            if (data !is Map<*, *> || !dartNumEquals(data["v"], 1)) {
                null
            } else {
                val xPub = B64.decode(data["x25519Pub"] as String)
                val xPriv = B64.decode(data["x25519Priv"] as String)
                val mPub = B64.decode(data["mlkemPub"] as String)
                val mPriv = B64.decode(data["mlkemPriv"] as String)
                if (xPub.size != X25519_BYTES ||
                    xPriv.size != X25519_BYTES ||
                    mPub.size != MLKEM_PUBLIC_BYTES ||
                    mPriv.size != MLKEM_PRIVATE_BYTES
                ) {
                    null
                } else {
                    OwnerCrypto(xPub, xPriv, mPub, mPriv)
                }
            }
        } catch (_: Exception) {
            null
        }

        fun recipientKeyIds(envelope: Map<String, Any?>): List<String> {
            if (envelope["kind"] != ENVELOPE_KIND || !dartNumEquals(envelope["v"], 1) || envelope["alg"] != ENVELOPE_ALG) {
                return emptyList()
            }
            val recipients = envelope["recipients"]
            if (recipients !is List<*> || recipients.isEmpty() || recipients.size > MAX_RECIPIENTS) return emptyList()
            val ids = ArrayList<String>()
            for (entry in recipients) {
                if (entry !is Map<*, *>) return emptyList()
                val kid = entry["kid"]
                if (kid !is String || kid.length < 16 || kid.length > 128 || ids.contains(kid) || !B64URL_TEXT.matches(kid)) {
                    return emptyList()
                }
                ids.add(kid)
            }
            return ids
        }

        private fun deriveKek(ssX: ByteArray, ssM: ByteArray, info: ByteArray): ByteArray =
            Hkdf.sha256(Bytes.concat(ssX, ssM), Utf8.encode(HKDF_SALT), info, KEY_BYTES)

        private fun fromB64UrlExact(value: Any?, exactLength: Int): ByteArray? {
            val decoded = fromB64Url(value)
            return if (decoded != null && decoded.size == exactLength) decoded else null
        }

        private fun fromB64UrlBounded(value: Any?, maxLength: Int): ByteArray? {
            val decoded = fromB64Url(value)
            return if (decoded != null && decoded.isNotEmpty() && decoded.size <= maxLength) decoded else null
        }

        private fun fromB64Url(value: Any?): ByteArray? {
            if (value !is String || value.isEmpty() || value.contains('=')) return null
            if (!B64URL_TEXT.matches(value)) return null
            return B64.decodeOrNull(value.padEnd((value.length + 3) and 3.inv(), '='))
        }
    }
}
