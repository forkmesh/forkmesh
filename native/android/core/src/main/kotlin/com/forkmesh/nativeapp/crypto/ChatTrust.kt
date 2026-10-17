package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.account.projects.ScopedSecretStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.SecretStore

enum class SealPolicy { OPEN, STRICT }

internal class StoreIdentity private constructor(private val base: SecretStore, private val prefix: String) {
    override fun equals(other: Any?): Boolean = other is StoreIdentity && other.base === base && other.prefix == prefix

    override fun hashCode(): Int = System.identityHashCode(base) * 31 + prefix.hashCode()

    companion object {
        fun of(store: SecretStore): StoreIdentity {
            if (store !is ScopedSecretStore) return StoreIdentity(store, "")
            val inner = of(store.base)
            return StoreIdentity(inner.base, inner.prefix + store.prefix)
        }
    }
}

enum class TrustState(val wire: String) {
    APPROVED("approved"),
    PENDING("pending"),
    BLOCKED("blocked");

    companion object {
        fun of(wire: Any?): TrustState? = entries.firstOrNull { it.wire == wire }
    }
}

class TrustRecord internal constructor(
    val kid: String,
    val idk: String,
    val acct: String,
    val node: String,
    spk: Map<String, Any?>,
    state: TrustState,
    val firstSeen: Long,
    verified: Boolean,
    lastSeen: Long,
) {
    var spk: Map<String, Any?> = spk
        internal set
    var state: TrustState = state
        internal set
    var verified: Boolean = verified
        internal set
    var lastSeen: Long = lastSeen
        internal set

    val idkBytes: ByteArray get() = ChatV3.unb64u(idk) ?: ByteArray(0)

    val spkExp: Long get() = ChatV3.integer(spk["exp"]) ?: 0L

    val spkTs: Long get() = ChatV3.integer(spk["ts"]) ?: 0L

    fun copy(): TrustRecord = TrustRecord(kid, idk, acct, node, spk, state, firstSeen, verified, lastSeen)

    internal fun toJson(): Map<String, Any?> = linkedMapOf(
        "kid" to kid,
        "idk" to idk,
        "acct" to acct,
        "node" to node,
        "spk" to spk,
        "state" to state.wire,
        "firstSeen" to firstSeen,
        "verified" to verified,
        "lastSeen" to lastSeen,
    )

    internal companion object {
        fun fromJson(value: Map<*, *>): TrustRecord? {
            val kid = value["kid"] as? String ?: return null
            val idk = value["idk"] as? String ?: return null
            val acct = value["acct"] as? String ?: return null
            val node = value["node"] as? String ?: return null
            val spk = ChatV3.publicPrekey(value["spk"]) ?: return null
            val state = TrustState.of(value["state"]) ?: return null
            val idkBytes = ChatV3.unb64u(idk) ?: return null
            if (idkBytes.size != Ed25519.PUBLIC_KEY_BYTES || kid != ChatV3.id16(idkBytes) || !ChatV3.validAcct(acct) || !ChatV3.validAcct(node)) return null
            val firstSeen = ChatV3.integer(value["firstSeen"]) ?: 0L
            return TrustRecord(kid, idk, acct, node, spk, state, firstSeen, value["verified"] == true, ChatV3.integer(value["lastSeen"]) ?: firstSeen)
        }
    }
}

class HeldFrame(val room: String, val kid: String, val frame: Map<String, Any?>, val meta: Map<String, Any?>) {
    internal fun toJson(): Map<String, Any?> = linkedMapOf("room" to room, "kid" to kid, "frame" to frame, "meta" to meta)
}

class BundleOutcome(val kid: String, val acct: String, val node: String, val state: TrustState, val isNew: Boolean, val updated: Boolean)

class TrustChange(val approvedBefore: Set<String>, val approvedAfter: Set<String>, val hadOtherApproved: Boolean) {
    val changed: Boolean get() = approvedBefore != approvedAfter
}

class ChatTrust private constructor(val scope: String, private val store: SecretStore?) {
    private val records = LinkedHashMap<String, TrustRecord>()
    private val held = ArrayList<HeldFrame>()
    private var ownKid = ""
    private var revisionCount = 0

    val storeKey: String get() = "$STORE_PREFIX${ChatV3.b64u(Utf8.encode(scope))}"

    val revision: Int
        @Synchronized get() = revisionCount

    @Synchronized
    fun record(kid: String): TrustRecord? = records[kid]?.copy()

    @Synchronized
    fun records(): List<TrustRecord> = records.values.map { it.copy() }

    @Synchronized
    fun approved(): List<TrustRecord> = records.values.filter { it.state == TrustState.APPROVED }.map { it.copy() }

    @Synchronized
    fun pending(): List<TrustRecord> = records.values.filter { it.state == TrustState.PENDING }.map { it.copy() }

    @Synchronized
    fun ensureOwn(kid: String, idk: String, acct: String, node: String, spk: Map<String, Any?>, now: Long) {
        ownKid = kid
        val known = records[kid]
        if (known != null && known.idk == idk && known.acct == acct && known.node == node && known.state == TrustState.APPROVED &&
            known.spk["id"] == spk["id"]
        ) {
            return
        }
        records[kid] = TrustRecord(kid, idk, acct, node, spk, TrustState.APPROVED, known?.firstSeen ?: now, known?.verified ?: false, now)
        changed()
    }

    @Synchronized
    fun accept(
        bundle: Map<String, Any?>,
        policy: SealPolicy,
        selfKid: String,
        now: Long,
        participants: Collection<String>? = null,
    ): BundleOutcome? {
        val kid = bundle["kid"] as String
        val idk = bundle["idk"] as String
        val acct = bundle["acct"] as String
        val node = bundle["node"] as String
        val spk = ChatV3.publicPrekey(bundle["spk"]) ?: return null
        if (participants != null && acct !in participants) return null
        val known = records[kid]
        if (known != null) {
            if (known.idk != idk || known.acct != acct || known.node != node) return null
            val newer = (spk["ts"] as Long) > known.spkTs
            if (newer) known.spk = spk
            known.lastSeen = now
            if (newer) changed()
            return BundleOutcome(kid, acct, node, known.state, isNew = false, updated = newer)
        }
        val state = when {
            policy == SealPolicy.OPEN || kid == selfKid -> TrustState.APPROVED
            records.values.any { it.state == TrustState.APPROVED && it.acct == acct } -> TrustState.PENDING
            else -> TrustState.APPROVED
        }
        records[kid] = TrustRecord(kid, idk, acct, node, spk, state, now, false, now)
        if (state == TrustState.APPROVED) clearVerified()
        evict(selfKid)
        changed()
        return BundleOutcome(kid, acct, node, state, isNew = true, updated = true)
    }

    private fun evict(selfKid: String) {
        while (records.size > MAX_RECORDS) {
            val oldest = records.values
                .filter { it.kid != selfKid && it.kid != ownKid && it.state != TrustState.BLOCKED }
                .minByOrNull { it.lastSeen } ?: return
            records.remove(oldest.kid)
            held.removeAll { it.kid == oldest.kid }
        }
    }

    private fun clearVerified() {
        for (record in records.values) record.verified = false
    }

    private fun approvedKids(): Set<String> = records.values.filter { it.state == TrustState.APPROVED }.map { it.kid }.toSet()

    private fun otherApproved(selfAcct: String): Boolean =
        records.values.any { it.state == TrustState.APPROVED && it.kid != ownKid && it.acct != selfAcct }

    @Synchronized
    fun approve(kid: String, selfAcct: String = ""): TrustChange? {
        val record = records[kid] ?: return null
        val before = approvedKids()
        val had = otherApproved(selfAcct)
        if (record.state != TrustState.APPROVED) {
            record.state = TrustState.APPROVED
            clearVerified()
            changed()
        }
        return TrustChange(before, approvedKids(), had)
    }

    @Synchronized
    fun block(kid: String, selfAcct: String = ""): TrustChange? {
        if (kid == ownKid) return null
        val record = records[kid] ?: return null
        val before = approvedKids()
        val had = otherApproved(selfAcct)
        if (record.state != TrustState.BLOCKED) {
            if (record.state == TrustState.APPROVED) clearVerified()
            record.state = TrustState.BLOCKED
            held.removeAll { it.kid == kid }
            changed()
        }
        return TrustChange(before, approvedKids(), had)
    }

    @Synchronized
    fun markVerified(): Boolean {
        var any = false
        for (record in records.values) {
            if (record.state == TrustState.APPROVED && !record.verified) {
                record.verified = true
                any = true
            }
        }
        if (any) changed()
        return any
    }

    @Synchronized
    fun verified(): Boolean {
        val approved = records.values.filter { it.state == TrustState.APPROVED && it.kid != ownKid }
        return approved.isNotEmpty() && approved.all { it.verified }
    }

    @Synchronized
    fun hold(frame: HeldFrame) {
        val record = records[frame.kid]
        if (record?.state == TrustState.BLOCKED) return
        val ctr = frame.frame["ctr"]
        if (held.any { it.room == frame.room && it.kid == frame.kid && it.frame["ctr"] == ctr }) return
        held.add(frame)
        val inRoom = held.filter { it.room == frame.room }
        if (inRoom.size > ChatV3.MAX_HELD) held.remove(inRoom.first())
        changed()
    }

    @Synchronized
    fun takeHeld(room: String, kid: String): List<HeldFrame> {
        val taken = held.filter { it.room == room && it.kid == kid }
        if (taken.isEmpty()) return emptyList()
        held.removeAll(taken.toSet())
        changed()
        return taken
    }

    @Synchronized
    fun heldCount(room: String): Int = held.count { it.room == room }

    @Synchronized
    fun heldKids(room: String): Set<String> = held.filter { it.room == room }.map { it.kid }.toSet()

    private fun changed() {
        revisionCount++
        persist()
    }

    private fun persist() {
        val target = store ?: return
        try {
            val keptHeld = held.filter { Json.stringify(it.frame).length <= MAX_PERSISTED_HELD_CHARS }
            val value = linkedMapOf<String, Any?>(
                "v" to ChatV3.VERSION,
                "records" to records.values.map { it.toJson() },
                "held" to keptHeld.map { it.toJson() },
            )
            target.write(storeKey, Json.stringify(value))
        } catch (_: Exception) {
        }
    }

    private fun load() {
        val target = store ?: return
        try {
            val root = Json.parse(target.read(storeKey) ?: return) as? Map<*, *> ?: return
            for (value in root["records"] as? List<*> ?: emptyList<Any?>()) {
                val record = (value as? Map<*, *>)?.let(TrustRecord::fromJson) ?: continue
                records[record.kid] = record
            }
            for (value in root["held"] as? List<*> ?: emptyList<Any?>()) {
                val entry = value as? Map<*, *> ?: continue
                val room = entry["room"] as? String ?: continue
                val kid = entry["kid"] as? String ?: continue
                @Suppress("UNCHECKED_CAST")
                val frame = entry["frame"] as? Map<String, Any?> ?: continue
                @Suppress("UNCHECKED_CAST")
                val meta = entry["meta"] as? Map<String, Any?> ?: emptyMap()
                held.add(HeldFrame(room, kid, frame, meta))
            }
        } catch (_: Exception) {
        }
    }

    companion object {
        const val STORE_PREFIX = "chat/e2ee/trustV3/"
        const val MAX_RECORDS = 256
        const val MAX_PERSISTED_HELD_CHARS = 96 * 1024

        private val cache = HashMap<StoreIdentity, HashMap<String, ChatTrust>>()

        fun forStore(store: SecretStore, scope: String): ChatTrust = synchronized(cache) {
            val scopes = cache.getOrPut(StoreIdentity.of(store)) { HashMap() }
            scopes[scope]?.let { return it }
            val trust = ChatTrust(scope, store)
            synchronized(trust) { trust.load() }
            scopes[scope] = trust
            trust
        }

        fun inMemory(scope: String): ChatTrust = ChatTrust(scope, null)
    }
}
