package com.forkmesh.nativeapp.security

import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.crypto.Ed25519

class Identity private constructor(keys: Keys, private val store: SecretStore?) : DeviceIdentity {
    private class Keys(val seed: ByteArray, val publicKey: ByteArray)

    @Volatile
    private var keys: Keys = keys

    override val publicKeyBytes: ByteArray
        get() = keys.publicKey.copyOf()

    override val publicKeyB64url: String
        get() = B64.encodeUrlNoPadding(keys.publicKey)

    override val nodeId: String
        get() = publicKeyB64url

    override val shortKey: String
        get() {
            val k = publicKeyB64url
            return if (k.length <= 16) k else "${k.substring(0, 8)}...${k.substring(k.length - 8)}"
        }

    override fun sign(data: ByteArray): String {
        val current = keys
        return B64.encodeUrlNoPadding(Ed25519.sign(current.seed, data, current.publicKey))
    }

    override fun exportSeed(): ByteArray = keys.seed.copyOf()

    override fun rotate() {
        rotate(store ?: throw IllegalStateException("This identity was not loaded from a SecretStore; use rotate(store)."))
    }

    fun rotate(secureStore: SecretStore) {
        val seed = Ed25519.generateSeed()
        secureStore.write(SEED_KEY, B64.encode(seed))
        keys = Keys(seed, Ed25519.publicKey(seed))
    }

    companion object {
        const val SEED_KEY = "identity/ed25519Seed"

        fun loadOrCreate(store: SecretStore): Identity {
            val stored = store.read(SEED_KEY)
            val seed = if (!stored.isNullOrEmpty()) {
                B64.decode(stored).also { require(it.size == Ed25519.SEED_BYTES) { "Seed must have 32 bytes" } }
            } else {
                Ed25519.generateSeed().also { store.write(SEED_KEY, B64.encode(it)) }
            }
            return Identity(Keys(seed, Ed25519.publicKey(seed)), store)
        }

        fun fromSeed(seed: ByteArray): Identity {
            require(seed.size == Ed25519.SEED_BYTES) { "Seed must have 32 bytes" }
            val copy = seed.copyOf()
            return Identity(Keys(copy, Ed25519.publicKey(copy)), null)
        }

        fun clearStoredSeed(store: SecretStore) {
            store.delete(SEED_KEY)
        }
    }
}
