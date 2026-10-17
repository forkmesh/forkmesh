package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.SecretStore

object ChatV4 {
    const val VERSION = 4L
    const val MAX_SKIP = 256
    const val SKIPPED_KEPT = 512
    const val SKIPPED_LIFE_MS = 8 * ChatV3.DAY_MS
    const val SESSIONS_KEPT = 4
    const val RESET_TYPE = "e2ee-reset"
    const val RESET_INTERVAL_MS = 60_000L
    const val STORE_PREFIX = "chat/e2ee/ratchetV4/"
    private const val MAX_COUNT = 1L shl 31

    private const val SALT_PREFIX = "ForkMesh chat e2ee salt v4:"
    private const val INIT_LABEL = "forkmesh-chat-e2ee-init-v4"
    private const val RATCHET_LABEL = "forkmesh-chat-e2ee-ratchet-v4"
    private const val CONTENT_LABEL = "forkmesh-chat-e2ee-content-v4"
    private const val WRAP_LABEL = "forkmesh-chat-e2ee-wrap-v4"
    private const val SEALED_LABEL = "forkmesh-chat-e2ee-sealed-v4"

    private val KEY32 = Regex("[A-Za-z0-9_-]{43}")
    private val MESSAGE_KEY = byteArrayOf(1)
    private val CHAIN_KEY = byteArrayOf(2)

    class Pair32(val first: ByteArray, val second: ByteArray)

    private fun kdf64(ikm: ByteArray, salt: ByteArray, info: ByteArray): Pair32 {
        val out = Hkdf.sha256(ikm, salt, info, 64)
        try {
            return Pair32(out.copyOfRange(0, 32), out.copyOfRange(32, 64))
        } finally {
            Bytes.wipe(out)
        }
    }

    fun kdfInit(room: String, sender: String, recipient: String, spkId: String, sid: String, ss1: ByteArray, ss2: ByteArray): Pair32 {
        val salt = Sha256.hash(Utf8.encode(SALT_PREFIX + room))
        val info = Utf8.encode(listOf(INIT_LABEL, room, sender, recipient, spkId, sid).joinToString("\n"))
        val ikm = Bytes.concat(ss1, ss2)
        try {
            return kdf64(ikm, salt, info)
        } finally {
            Bytes.wipe(ikm)
        }
    }

    fun kdfRk(rk: ByteArray, dhOut: ByteArray, ss: ByteArray): Pair32 {
        val ikm = Bytes.concat(dhOut, ss)
        try {
            return kdf64(ikm, rk, Utf8.encode(RATCHET_LABEL))
        } finally {
            Bytes.wipe(ikm)
        }
    }

    fun kdfCk(ck: ByteArray): Pair32 = Pair32(HmacSha256.mac(ck, MESSAGE_KEY), HmacSha256.mac(ck, CHAIN_KEY))

    fun sessionId(dhPub: ByteArray, ct: ByteArray): String = ChatV3.id16(Bytes.concat(dhPub, ct))

    fun contentAad(room: String, sender: String, ctr: Long, ts: Long): ByteArray =
        Utf8.encode("$CONTENT_LABEL\n$room\n$sender\n$ctr\n$ts")

    private fun ctHash(entry: Map<*, *>): String {
        val ct = ChatV3.unb64(entry["ct"]) ?: throw IllegalArgumentException("bad ct")
        return ChatV3.b64u(Sha256.hash(ct))
    }

    fun wrapAad(room: String, sender: String, entry: Map<*, *>, bodyNonce: String, bodyTag: String): ByteArray =
        Utf8.encode(
            listOf(
                WRAP_LABEL, room, sender, entry["kid"] as String, entry["sid"] as String, entry["spk"] as String, ctHash(entry),
                entry["dh"] as String, ChatV3.integer(entry["pn"]).toString(), ChatV3.integer(entry["n"]).toString(), bodyNonce, bodyTag,
            ).joinToString("\n"),
        )

    fun sealedMessage(room: String, frame: Map<String, Any?>): ByteArray {
        val body = ChatV3.unb64(frame["body"]) ?: throw IllegalArgumentException("bad body")
        val out = StringBuilder()
        out.append(SEALED_LABEL).append('\n').append(room).append('\n')
            .append(frame["kid"] as String).append('\n').append(frame["idk"] as String).append('\n')
            .append(ChatV3.integer(frame["ctr"])).append('\n').append(ChatV3.integer(frame["ts"])).append('\n')
            .append(frame["nonce"] as String).append('\n').append(frame["tag"] as String).append('\n')
            .append(ChatV3.b64u(Sha256.hash(body)))
        for (value in frame["keys"] as List<*>) {
            val entry = value as Map<*, *>
            out.append('\n')
                .append(entry["kid"] as String).append('|')
                .append(entry["sid"] as String).append('|')
                .append(entry["spk"] as String).append('|')
                .append(ctHash(entry)).append('|')
                .append(entry["dh"] as String).append('|')
                .append(ChatV3.integer(entry["pn"])).append('|')
                .append(ChatV3.integer(entry["n"])).append('|')
                .append(entry["nonce"] as String).append('|')
                .append(entry["tag"] as String).append('|')
                .append(entry["key"] as String)
        }
        return Utf8.encode(out.toString())
    }

    fun count(value: Any?): Long? {
        val number = ChatV3.integer(value) ?: return null
        return if (number in 0 until MAX_COUNT) number else null
    }

    fun validEntry(value: Any?): Boolean {
        val entry = value as? Map<*, *> ?: return false
        if (!ChatV3.isKid(entry["kid"]) || !ChatV3.isKid(entry["sid"]) || !ChatV3.isKid(entry["spk"])) return false
        val dh = entry["dh"] as? String ?: return false
        if (!KEY32.matches(dh) || ChatV3.unb64u(dh)?.size != X25519.KEY_BYTES) return false
        if (count(entry["pn"]) == null || count(entry["n"]) == null) return false
        return ChatV3.unb64(entry["ct"])?.size == MlKem768.CIPHERTEXT_BYTES &&
            ChatV3.unb64(entry["nonce"])?.size == ChatV3.NONCE_BYTES &&
            ChatV3.unb64(entry["tag"])?.size == ChatV3.TAG_BYTES &&
            ChatV3.unb64(entry["key"])?.size == ChatV3.KEY_BYTES
    }

    fun storeKey(room: String): String = STORE_PREFIX + ChatV3.b64u(Utf8.encode(room))
}

internal class RatchetPair(val priv: ByteArray, val pub: String) {
    fun copy(): RatchetPair = RatchetPair(priv.copyOf(), pub)
}

internal class RatchetSkipped(val dh: String, val n: Long, val mk: ByteArray, val ts: Long)

internal class RatchetSession(
    val sid: String,
    var rk: ByteArray,
    var dhs: RatchetPair?,
    var dhr: String?,
    var cks: ByteArray?,
    var ns: Long,
    var pn: Long,
    var sqSpk: String?,
    var sqCt: ByteArray?,
    var ckr: ByteArray?,
    var nr: Long,
    val skipped: ArrayList<RatchetSkipped>,
    val created: Long,
    var used: Long,
) {
    fun copy(): RatchetSession = RatchetSession(
        sid, rk.copyOf(), dhs?.copy(), dhr, cks?.copyOf(), ns, pn, sqSpk, sqCt?.copyOf(), ckr?.copyOf(), nr,
        ArrayList(skipped.map { RatchetSkipped(it.dh, it.n, it.mk.copyOf(), it.ts) }), created, used,
    )

    fun wipe() {
        Bytes.wipe(rk)
        dhs?.let { Bytes.wipe(it.priv) }
        cks?.let { Bytes.wipe(it) }
        ckr?.let { Bytes.wipe(it) }
        for (key in skipped) Bytes.wipe(key.mk)
    }

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "sid" to sid,
        "rk" to ChatV3.b64(rk),
        "dhs" to dhs?.let { linkedMapOf("priv" to ChatV3.b64(it.priv), "pub" to it.pub) },
        "dhr" to dhr,
        "cks" to cks?.let(ChatV3::b64),
        "ns" to ns,
        "pn" to pn,
        "sq" to if (sqSpk != null && sqCt != null) linkedMapOf("spk" to sqSpk, "ct" to ChatV3.b64(sqCt!!)) else null,
        "ckr" to ckr?.let(ChatV3::b64),
        "nr" to nr,
        "skipped" to skipped.map { linkedMapOf("dh" to it.dh, "n" to it.n, "mk" to ChatV3.b64(it.mk), "ts" to it.ts) },
        "created" to created,
        "used" to used,
    )

    companion object {
        private fun key32(value: Any?): ByteArray? = ChatV3.unb64(value)?.takeIf { it.size == ChatV3.KEY_BYTES }

        private fun pub(value: Any?): String? =
            (value as? String)?.takeIf { ChatV3.unb64u(it)?.size == X25519.KEY_BYTES }

        fun fromJson(value: Any?): RatchetSession? {
            val map = value as? Map<*, *> ?: return null
            val sid = map["sid"] as? String ?: return null
            if (!ChatV3.isKid(sid)) return null
            val rk = key32(map["rk"]) ?: return null
            val dhs = when (val raw = map["dhs"]) {
                null -> null
                is Map<*, *> -> RatchetPair(key32(raw["priv"]) ?: return null, pub(raw["pub"]) ?: return null)
                else -> return null
            }
            val dhr = if (map["dhr"] == null) null else pub(map["dhr"]) ?: return null
            val cks = if (map["cks"] == null) null else key32(map["cks"]) ?: return null
            val ckr = if (map["ckr"] == null) null else key32(map["ckr"]) ?: return null
            var sqSpk: String? = null
            var sqCt: ByteArray? = null
            when (val raw = map["sq"]) {
                null -> {}
                is Map<*, *> -> {
                    sqSpk = (raw["spk"] as? String)?.takeIf(ChatV3::isKid) ?: return null
                    sqCt = ChatV3.unb64(raw["ct"])?.takeIf { it.size == MlKem768.CIPHERTEXT_BYTES } ?: return null
                }
                else -> return null
            }
            val skipped = ArrayList<RatchetSkipped>()
            for (raw in map["skipped"] as? List<*> ?: emptyList<Any?>()) {
                val entry = raw as? Map<*, *> ?: continue
                val dh = pub(entry["dh"]) ?: continue
                val n = ChatV4.count(entry["n"]) ?: continue
                val mk = key32(entry["mk"]) ?: continue
                val ts = ChatV3.integer(entry["ts"]) ?: continue
                skipped.add(RatchetSkipped(dh, n, mk, ts))
            }
            return RatchetSession(
                sid, rk, dhs, dhr, cks,
                ChatV4.count(map["ns"]) ?: return null,
                ChatV4.count(map["pn"]) ?: return null,
                sqSpk, sqCt, ckr,
                ChatV4.count(map["nr"]) ?: return null,
                skipped,
                ChatV3.integer(map["created"]) ?: 0L,
                ChatV3.integer(map["used"]) ?: 0L,
            )
        }
    }
}

internal class RatchetPeer(var current: String?, val sessions: ArrayList<RatchetSession>) {
    fun copy(): RatchetPeer = RatchetPeer(current, ArrayList(sessions.map { it.copy() }))

    fun wipe() {
        for (session in sessions) session.wipe()
    }

    fun toJson(): Map<String, Any?> = linkedMapOf("current" to current, "sessions" to sessions.map { it.toJson() })

    companion object {
        fun fromJson(value: Any?): RatchetPeer? {
            val map = value as? Map<*, *> ?: return null
            val sessions = ArrayList<RatchetSession>()
            for (raw in map["sessions"] as? List<*> ?: emptyList<Any?>()) {
                val session = RatchetSession.fromJson(raw) ?: continue
                if (sessions.none { it.sid == session.sid }) sessions.add(session)
            }
            return RatchetPeer(map["current"] as? String, sessions)
        }
    }
}

internal class RoomRatchets private constructor(private val room: String, private val store: SecretStore?) {
    private var peers = LinkedHashMap<String, RatchetPeer>()

    @Synchronized
    fun peer(kid: String): RatchetPeer = peers[kid]?.copy() ?: RatchetPeer(null, ArrayList())

    @Synchronized
    fun created(kid: String): Long? {
        val peer = peers[kid] ?: return null
        return peer.sessions.firstOrNull { it.sid == peer.current }?.created
    }

    @Synchronized
    fun commit(updates: Map<String, RatchetPeer>, now: Long) {
        if (updates.isEmpty()) return
        val next = LinkedHashMap(peers)
        for ((kid, peer) in updates) next[kid] = trimmed(peer, now)
        persist(next)
        val old = peers
        peers = next
        for (kid in updates.keys) old[kid]?.let { if (next[kid] !== it) it.wipe() }
    }

    @Synchronized
    fun reset(kid: String) {
        if (!peers.containsKey(kid)) return
        val next = LinkedHashMap(peers)
        val gone = next.remove(kid)
        persist(next)
        peers = next
        gone?.wipe()
    }

    @Synchronized
    fun export(): Map<String, Any?> {
        val out = LinkedHashMap<String, Any?>()
        for ((kid, peer) in peers) out[kid] = peer.toJson()
        return linkedMapOf("v" to ChatV4.VERSION, "peers" to out)
    }

    @Synchronized
    fun import(state: Map<*, *>) {
        val next = parse(state)
        persist(next)
        peers = next
    }

    private fun trimmed(peer: RatchetPeer, now: Long): RatchetPeer {
        val sorted = peer.sessions.sortedByDescending { it.used }
        val current = sorted.firstOrNull { it.sid == peer.current }
        val kept = ArrayList<RatchetSession>()
        if (current != null) kept.add(current)
        for (session in sorted) {
            if (kept.size >= ChatV4.SESSIONS_KEPT) break
            if (session !== current) kept.add(session)
        }
        for (session in sorted) if (session !in kept) session.wipe()
        for (session in kept) {
            val fresh = session.skipped.filter { it.ts >= now - ChatV4.SKIPPED_LIFE_MS }.takeLast(ChatV4.SKIPPED_KEPT)
            val keep = fresh.toHashSet()
            for (key in session.skipped) if (key !in keep) Bytes.wipe(key.mk)
            session.skipped.clear()
            session.skipped.addAll(fresh)
        }
        return RatchetPeer(peer.current, kept)
    }

    private fun persist(value: Map<String, RatchetPeer>) {
        val target = store ?: return
        val out = LinkedHashMap<String, Any?>()
        for ((kid, peer) in value) out[kid] = peer.toJson()
        target.write(ChatV4.storeKey(room), Json.stringify(linkedMapOf("v" to ChatV4.VERSION, "peers" to out)))
    }

    private fun parse(root: Map<*, *>): LinkedHashMap<String, RatchetPeer> {
        val out = LinkedHashMap<String, RatchetPeer>()
        if (ChatV3.integer(root["v"]) != ChatV4.VERSION) return out
        val raw = root["peers"] as? Map<*, *> ?: return out
        for ((kid, value) in raw) {
            if (!ChatV3.isKid(kid)) continue
            val peer = RatchetPeer.fromJson(value) ?: continue
            out[kid as String] = peer
        }
        return out
    }

    private fun load() {
        val target = store ?: return
        try {
            val root = Json.parse(target.read(ChatV4.storeKey(room)) ?: return) as? Map<*, *> ?: return
            peers = parse(root)
        } catch (_: Exception) {
        }
    }

    companion object {
        private val cache = HashMap<StoreIdentity, HashMap<String, RoomRatchets>>()

        fun forStore(store: SecretStore?, room: String): RoomRatchets {
            if (store == null) return RoomRatchets(room, null)
            return synchronized(cache) {
                val rooms = cache.getOrPut(StoreIdentity.of(store)) { HashMap() }
                rooms.getOrPut(room) { RoomRatchets(room, store).also { synchronized(it) { it.load() } } }
            }
        }
    }
}
