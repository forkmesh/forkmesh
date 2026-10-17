package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.SecretStore

class ChatPrekey internal constructor(
    val id: String,
    internal val xPrivate: ByteArray,
    val x: ByteArray,
    internal val pqSeed: ByteArray,
    val ts: Long,
    val exp: Long,
    pqPublic: ByteArray? = null,
) {
    private var pair: MlKem768.KeyPair? = null
    private var publicPq: ByteArray? = pqPublic

    val pq: ByteArray
        @Synchronized get() = publicPq ?: keyPair().publicKey.also { publicPq = it }

    @Synchronized
    internal fun keyPair(): MlKem768.KeyPair = pair ?: MlKem768.generateKeyPair(pqSeed).also { pair = it }

    internal fun decapsulate(ciphertext: ByteArray): ByteArray = MlKem768.decapsulate(keyPair().secretKey, ciphertext)

    fun publicJson(): Map<String, Any?> = linkedMapOf(
        "id" to id,
        "x" to ChatV3.b64u(x),
        "pq" to ChatV3.b64u(pq),
        "ts" to ts,
        "exp" to exp,
    )

    internal fun storedJson(): Map<String, Any?> = linkedMapOf(
        "id" to id,
        "xPriv" to ChatV3.b64(xPrivate),
        "pqSeed" to ChatV3.b64(pqSeed),
        "ts" to ts,
        "exp" to exp,
    )

    @Synchronized
    internal fun wipe() {
        Bytes.wipe(xPrivate, pqSeed)
        pair?.let { Bytes.wipe(it.secretKey) }
        pair = null
    }

    companion object {
        fun make(xPrivate: ByteArray, pqSeed: ByteArray, ts: Long): ChatPrekey {
            require(xPrivate.size == X25519.KEY_BYTES && pqSeed.size == MlKem768.SEED_BYTES) { "bad prekey material" }
            val x = X25519.publicKey(xPrivate)
            val pair = MlKem768.generateKeyPair(pqSeed)
            val prekey = ChatPrekey(
                ChatV3.id16(Bytes.concat(x, pair.publicKey)),
                xPrivate.copyOf(),
                x,
                pqSeed.copyOf(),
                ts,
                ts + ChatV3.PREKEY_LIFE_MS,
                pair.publicKey,
            )
            prekey.pair = pair
            return prekey
        }

        internal fun fromStored(value: Map<*, *>): ChatPrekey? {
            val id = value["id"] as? String ?: return null
            val xPrivate = ChatV3.unb64(value["xPriv"]) ?: return null
            val pqSeed = ChatV3.unb64(value["pqSeed"]) ?: return null
            val ts = ChatV3.integer(value["ts"]) ?: return null
            val exp = ChatV3.integer(value["exp"]) ?: return null
            if (!ChatV3.isKid(id) || xPrivate.size != X25519.KEY_BYTES || pqSeed.size != MlKem768.SEED_BYTES) return null
            return ChatPrekey(id, xPrivate, X25519.publicKey(xPrivate), pqSeed, ts, exp)
        }
    }
}

class ChatDeviceKeys private constructor(private val store: SecretStore?, identitySeed: ByteArray) {
    private val seed = identitySeed.copyOf()
    val idk: ByteArray = Ed25519.publicKey(seed)
    val idkText: String = ChatV3.b64u(idk)
    val kid: String = ChatV3.id16(idk)
    private val prekeys = ArrayList<ChatPrekey>()
    private var rotations = 0

    val generation: Int
        @Synchronized get() = rotations

    fun sign(message: ByteArray): ByteArray = Ed25519.sign(seed, message, idk)

    @Synchronized
    fun prekeyIds(): List<String> = prekeys.map { it.id }

    @Synchronized
    fun prekey(id: String): ChatPrekey? = prekeys.firstOrNull { it.id == id }

    @Synchronized
    fun addPrekey(xPrivate: ByteArray, pqSeed: ByteArray, ts: Long): ChatPrekey {
        val made = ChatPrekey.make(xPrivate, pqSeed, ts)
        prekeys.add(made)
        trim()
        rotations++
        persist()
        return made
    }

    @Synchronized
    fun wipeExpired(now: Long): Boolean {
        var changed = false
        val iterator = prekeys.iterator()
        while (iterator.hasNext()) {
            val prekey = iterator.next()
            if (now > prekey.exp) {
                prekey.wipe()
                iterator.remove()
                changed = true
            }
        }
        if (changed) persist()
        return changed
    }

    @Synchronized
    fun refresh(now: Long): Boolean {
        wipeExpired(now)
        val newest = prekeys.maxByOrNull { it.ts }
        if (newest != null && now - newest.ts < ChatV3.PREKEY_ROTATE_MS) return false
        prekeys.add(ChatPrekey.make(X25519.generatePrivateKey(), Bytes.random(MlKem768.SEED_BYTES), now))
        trim()
        rotations++
        persist()
        return true
    }

    @Synchronized
    fun newest(now: Long): ChatPrekey {
        refresh(now)
        return prekeys.maxByOrNull { it.ts }!!
    }

    @Synchronized
    fun newestWithoutRotation(): ChatPrekey? = prekeys.maxByOrNull { it.ts }

    private fun trim() {
        prekeys.sortBy { it.ts }
        while (prekeys.size > ChatV3.MAX_PREKEYS) prekeys.removeAt(0).wipe()
    }

    private fun persist() {
        val target = store ?: return
        try {
            target.write(PREKEYS_STORE_KEY, Json.stringify(prekeys.map { it.storedJson() }))
        } catch (_: Exception) {
        }
    }

    private fun load() {
        val target = store ?: return
        try {
            val decoded = Json.parse(target.read(PREKEYS_STORE_KEY) ?: return) as? List<*> ?: return
            for (value in decoded) {
                val prekey = (value as? Map<*, *>)?.let(ChatPrekey::fromStored) ?: continue
                if (prekeys.none { it.id == prekey.id }) prekeys.add(prekey)
            }
            trim()
        } catch (_: Exception) {
        }
    }

    companion object {
        const val IDENTITY_STORE_KEY = "chat/e2ee/ed25519IdentityV3"
        const val PREKEYS_STORE_KEY = "chat/e2ee/prekeysV3"

        private const val LEGACY_PRIVATE_KEY = "chat/e2ee/x25519PrivateV2"
        private val cache = HashMap<StoreIdentity, ChatDeviceKeys>()

        fun forStore(store: SecretStore): ChatDeviceKeys = synchronized(cache) {
            val identity = StoreIdentity.of(store)
            cache[identity]?.let { return it }
            val stored = try {
                store.read(IDENTITY_STORE_KEY)
            } catch (_: Exception) {
                null
            }
            val loaded = if (stored.isNullOrEmpty()) null else ChatV3.unb64(stored)?.takeIf { it.size == Ed25519.SEED_BYTES }
            val seed: ByteArray = loaded ?: Ed25519.generateSeed().also { fresh ->
                store.write(IDENTITY_STORE_KEY, ChatV3.b64(fresh))
                try {
                    store.delete(LEGACY_PRIVATE_KEY)
                } catch (_: Exception) {
                }
            }
            val keys = ChatDeviceKeys(store, seed)
            synchronized(keys) { keys.load() }
            cache[identity] = keys
            keys
        }

        fun existing(store: SecretStore): ChatDeviceKeys? = synchronized(cache) {
            val identity = StoreIdentity.of(store)
            cache[identity]?.let { return it }
            val stored = try {
                store.read(IDENTITY_STORE_KEY)
            } catch (_: Exception) {
                null
            }
            if (stored.isNullOrEmpty()) return null
            val seed = ChatV3.unb64(stored)?.takeIf { it.size == Ed25519.SEED_BYTES } ?: return null
            val keys = ChatDeviceKeys(store, seed)
            synchronized(keys) { keys.load() }
            cache[identity] = keys
            keys
        }

        fun fromSeed(identitySeed: ByteArray): ChatDeviceKeys {
            require(identitySeed.size == Ed25519.SEED_BYTES) { "identity seed must be 32 bytes" }
            return ChatDeviceKeys(null, identitySeed)
        }
    }
}
