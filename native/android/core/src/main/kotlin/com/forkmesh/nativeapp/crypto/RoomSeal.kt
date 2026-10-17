package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.SecretStore
import java.util.BitSet

sealed class SealOpen {
    class Opened(
        val plain: Map<String, Any?>,
        val senderKid: String,
        val senderAcct: String?,
        val replayed: Boolean,
        val senderNode: String? = null,
    ) : SealOpen()

    class Held(val kid: String, val blocked: Boolean) : SealOpen()

    object NotForMe : SealOpen()

    object Replay : SealOpen()

    object Invalid : SealOpen()

    object Own : SealOpen()

    class NoSession(val kid: String) : SealOpen()
}

internal class RoomCounters private constructor(private val room: String, private val store: SecretStore?) {
    private class Window(var max: Long, var bits: BitSet)

    private var next = 1L
    private var reserved = 0L
    private val windows = object : LinkedHashMap<String, Window>(16, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, Window>?): Boolean = size > MAX_SENDERS
    }
    private var unsaved = 0

    private val suffix: String get() = ChatV3.b64u(Utf8.encode(room))

    private val counterKey: String get() = "chat/e2ee/ctrV3/$suffix"

    private val seenKey: String get() = "chat/e2ee/seenV3/$suffix"

    @Synchronized
    fun nextCounter(): Long {
        if (next > reserved) {
            reserved = next + RESERVE - 1
            store?.write(counterKey, reserved.toString())
        }
        return next++
    }

    @Synchronized
    fun seen(kid: String, ctr: Long): Boolean {
        val window = windows[kid] ?: return false
        if (ctr <= window.max - ChatV3.REPLAY_WINDOW) return true
        if (ctr > window.max) return false
        return window.bits.get((window.max - ctr).toInt())
    }

    @Synchronized
    fun highest(kid: String): Long = windows[kid]?.max ?: 0L

    @Synchronized
    fun clearSeen() {
        windows.clear()
    }

    @Synchronized
    fun record(kid: String, ctr: Long, deferPersist: Boolean): Boolean {
        val window: Window? = windows[kid]
        val fresh: Boolean
        if (window == null) {
            windows[kid] = Window(ctr, BitSet(ChatV3.REPLAY_WINDOW).also { it.set(0) })
            fresh = true
        } else if (ctr > window.max) {
            val shift = ctr - window.max
            val moved = BitSet(ChatV3.REPLAY_WINDOW)
            if (shift < ChatV3.REPLAY_WINDOW.toLong()) {
                var i = window.bits.nextSetBit(0)
                while (i >= 0) {
                    val j = i.toLong() + shift
                    if (j < ChatV3.REPLAY_WINDOW.toLong()) moved.set(j.toInt())
                    i = window.bits.nextSetBit(i + 1)
                }
            }
            moved.set(0)
            window.max = ctr
            window.bits = moved
            fresh = true
        } else if (ctr <= window.max - ChatV3.REPLAY_WINDOW) {
            fresh = false
        } else {
            val index = (window.max - ctr).toInt()
            if (window.bits.get(index)) {
                fresh = false
            } else {
                window.bits.set(index)
                fresh = true
            }
        }
        if (fresh) {
            unsaved++
            if (!deferPersist || unsaved >= DEFERRED_SAVES) flush()
        }
        return fresh
    }

    @Synchronized
    fun flush() {
        if (unsaved == 0) return
        unsaved = 0
        val target = store ?: return
        try {
            val value = LinkedHashMap<String, Any?>()
            for ((kid, window) in windows) {
                val bytes = window.bits.toByteArray().copyOf(ChatV3.REPLAY_WINDOW / 8)
                value[kid] = linkedMapOf("m" to window.max, "b" to ChatV3.b64(bytes))
            }
            target.write(seenKey, Json.stringify(value))
        } catch (_: Exception) {
        }
    }

    private fun load() {
        val target = store ?: return
        try {
            reserved = target.read(counterKey)?.trim()?.toLongOrNull() ?: 0L
            next = reserved + 1
        } catch (_: Exception) {
        }
        try {
            val root = Json.parse(target.read(seenKey) ?: return) as? Map<*, *> ?: return
            for ((kid, raw) in root) {
                if (kid !is String || raw !is Map<*, *>) continue
                val max = ChatV3.integer(raw["m"]) ?: continue
                val bytes = ChatV3.unb64(raw["b"]) ?: continue
                windows[kid] = Window(max, BitSet.valueOf(bytes))
            }
        } catch (_: Exception) {
        }
    }

    companion object {
        private const val RESERVE = 32
        private const val MAX_SENDERS = 512
        private const val DEFERRED_SAVES = 32
        private val cache = HashMap<StoreIdentity, HashMap<String, RoomCounters>>()

        fun forStore(store: SecretStore?, room: String): RoomCounters {
            if (store == null) return RoomCounters(room, null)
            return synchronized(cache) {
                val rooms = cache.getOrPut(StoreIdentity.of(store)) { HashMap() }
                rooms.getOrPut(room) { RoomCounters(room, store).also { synchronized(it) { it.load() } } }
            }
        }
    }
}

class RoomSeal private constructor(
    val room: String,
    val scope: String,
    val policy: SealPolicy,
    acct: String,
    node: String,
    val keys: ChatDeviceKeys,
    val trust: ChatTrust,
    private val counters: RoomCounters,
    private val ratchets: RoomRatchets,
    private val store: SecretStore?,
    private val clock: () -> Long,
    val ratchetVersion: Boolean = policy == SealPolicy.STRICT,
) {
    @Volatile
    var acct: String = acct
        private set

    @Volatile
    var node: String = node
        private set

    @Volatile
    var participants: Collection<String>? = null

    @Volatile
    private var advertisedGeneration = -1

    val keyId: String get() = keys.kid

    fun setAcct(value: String) {
        if (ChatV3.validAcct(value)) acct = value
    }

    fun setIdentity(account: String, sender: String) {
        if (ChatV3.validAcct(account)) acct = account
        if (ChatV3.validAcct(sender)) node = sender
    }

    val peerKeyIds: List<String>
        get() = trust.approved().filter { it.kid != keys.kid }.map { it.kid }

    val peerCount: Int get() = peerKeyIds.size

    fun keyFrame(now: Long = clock()): Map<String, Any?> {
        val spk = keys.newest(now).publicJson()
        advertisedGeneration = keys.generation
        val idk = keys.idkText
        if (ChatV3.validAcct(acct) && ChatV3.validAcct(node)) trust.ensureOwn(keys.kid, idk, acct, node, spk, now)
        val signature = keys.sign(ChatV3.bundleMessage(room, keys.kid, idk, acct, node, spk))
        return linkedMapOf(
            "type" to ChatV3.KEY_TYPE,
            "v" to ChatV3.VERSION,
            "kid" to keys.kid,
            "idk" to idk,
            "acct" to acct,
            "node" to node,
            "spk" to spk,
            "sig" to ChatV3.b64u(signature),
        )
    }

    fun needsAdvertisement(): Boolean = advertisedGeneration != keys.generation

    fun shouldPersistAdvertisement(now: Long = clock()): Boolean {
        val target = store ?: return true
        val key = "chat/e2ee/announcedV3/${ChatV3.b64u(Utf8.encode(room))}"
        val newest = keys.newest(now).id
        val previous = try {
            target.read(key).orEmpty()
        } catch (_: Exception) {
            ""
        }
        val at = previous.substringBefore('|').trim().toLongOrNull() ?: 0L
        val id = previous.substringAfter('|', "")
        if (id == newest && now - at < ChatV3.DAY_MS) return false
        try {
            target.write(key, "$now|$newest")
        } catch (_: Exception) {
        }
        return true
    }

    fun acceptKeyFrame(frame: Map<String, Any?>, now: Long = clock()): BundleOutcome? {
        if (!ChatV3.verifyBundle(frame, room, now)) return null
        if (frame["kid"] == keys.kid && frame["idk"] != keys.idkText) return null
        return trust.accept(frame, policy, keys.kid, now, participants)
    }

    fun approve(kid: String): TrustChange? = trust.approve(kid, acct)

    fun block(kid: String): TrustChange? = trust.block(kid, acct)

    fun takeHeld(kid: String): List<HeldFrame> = trust.takeHeld(room, kid)

    fun flush() = counters.flush()

    fun safetyNumber(): ChatV3.SafetyNumber {
        val idks = LinkedHashMap<String, ByteArray>()
        idks[keys.kid] = keys.idk
        for (record in trust.approved()) idks[record.kid] = record.idkBytes
        return ChatV3.safetyNumber(scope, idks.values)
    }

    private fun recipients(now: Long, only: Collection<String>?): List<Pair<String, Map<String, Any?>>> {
        val own = keys.newest(now).publicJson()
        val candidates = trust.approved()
            .filter { it.kid != keys.kid && it.spkExp > now && (only == null || it.kid in only) }
            .sortedByDescending { it.lastSeen }
            .take(ChatV3.MAX_RECIPIENTS - 1)
        val targets = sortedMapOf<String, Map<String, Any?>>()
        targets[keys.kid] = own
        for (record in candidates) targets[record.kid] = record.spk
        return targets.entries.map { it.key to it.value }
    }

    private fun ratchetRecipients(now: Long, only: Collection<String>?): List<TrustRecord> =
        trust.approved()
            .filter { it.kid != keys.kid && it.spkExp > now && (only == null || it.kid in only) }
            .sortedByDescending { it.lastSeen }
            .take(ChatV3.MAX_RECIPIENTS)
            .sortedBy { it.kid }

    fun recipientKeyIds(only: Collection<String>? = null, now: Long = clock()): List<String> =
        if (policy == SealPolicy.STRICT) ratchetRecipients(now, only).map { it.kid } else recipients(now, only).map { it.first }

    fun accepts(frame: Map<String, Any?>): Boolean = if (ratchetVersion) isRatchetFrame(frame) else isSealedFrame(frame)

    fun resetSessions(kid: String) = ratchets.reset(kid)

    fun openedSince(kid: String, ctr: Long): Boolean = counters.highest(kid) > ctr

    internal fun exportRatchets(): Map<String, Any?> = ratchets.export()

    internal fun importRatchets(state: Map<*, *>) = ratchets.import(state)

    internal fun clearSeen() = counters.clearSeen()

    fun seal(plain: Map<String, Any?>, now: Long = clock(), only: Collection<String>? = null): Map<String, Any?> {
        if (ratchetVersion) return sealRatchet(plain, now, only)
        val targets = recipients(now, only)
        val ctr = counters.nextCounter()
        val contentKey = Bytes.random(ChatV3.KEY_BYTES)
        try {
            val content = AesGcm.encrypt(
                Utf8.encode(Json.stringify(plain)),
                contentKey,
                aad = ChatV3.contentAad(room, keys.kid, ctr, now),
            )
            val nonce = ChatV3.b64(content.nonce)
            val tag = ChatV3.b64(content.mac)
            val entries = ArrayList<Map<String, Any?>>()
            for ((recipient, spk) in targets) {
                val entry = try {
                    wrapFor(recipient, spk, contentKey, nonce, tag)
                } catch (error: RuntimeException) {
                    if (recipient == keys.kid) throw error
                    null
                } ?: continue
                entries.add(entry)
            }
            val frame = linkedMapOf<String, Any?>(
                "type" to ChatV3.SEALED_TYPE,
                "v" to ChatV3.VERSION,
                "kid" to keys.kid,
                "idk" to keys.idkText,
                "ctr" to ctr,
                "ts" to now,
                "nonce" to nonce,
                "tag" to tag,
                "body" to ChatV3.b64(content.cipherText),
                "keys" to entries,
            )
            frame["sig"] = ChatV3.b64u(keys.sign(ChatV3.sealedMessage(room, frame)))
            return frame
        } finally {
            Bytes.wipe(contentKey)
        }
    }

    private fun wrapFor(recipient: String, spk: Map<String, Any?>, contentKey: ByteArray, nonce: String, tag: String): Map<String, Any?> {
        val x = ChatV3.unb64u(spk["x"]) ?: throw IllegalArgumentException("bad prekey")
        val pq = ChatV3.unb64u(spk["pq"]) ?: throw IllegalArgumentException("bad prekey")
        val spkId = spk["id"] as String
        val eph = X25519.generatePrivateKey()
        val ephPub = X25519.publicKey(eph)
        var ss1 = ByteArray(0)
        var ss2 = ByteArray(0)
        var kek = ByteArray(0)
        try {
            ss1 = ChatV3.x25519(eph, x)
            val encapsulated = MlKem768.encapsulate(pq)
            ss2 = encapsulated.sharedSecret
            kek = ChatV3.kek(room, keys.kid, recipient, spkId, ephPub, encapsulated.ciphertext, ss1, ss2)
            val wrapped = AesGcm.encrypt(contentKey, kek, aad = ChatV3.wrapAad(room, keys.kid, recipient, nonce, tag))
            return linkedMapOf(
                "kid" to recipient,
                "spk" to spkId,
                "eph" to ChatV3.b64u(ephPub),
                "ct" to ChatV3.b64(encapsulated.ciphertext),
                "nonce" to ChatV3.b64(wrapped.nonce),
                "tag" to ChatV3.b64(wrapped.mac),
                "key" to ChatV3.b64(wrapped.cipherText),
            )
        } finally {
            Bytes.wipe(eph, ss1, ss2, kek)
        }
    }

    private class Shape(
        val kid: String,
        val idk: String,
        val idkBytes: ByteArray,
        val ctr: Long,
        val ts: Long,
        val bodyNonce: ByteArray,
        val bodyTag: ByteArray,
        val body: ByteArray,
        val sig: ByteArray,
        val entries: List<Map<*, *>>,
    )

    private fun shape(frame: Map<String, Any?>): Shape? {
        if (frame["type"] != ChatV3.SEALED_TYPE || ChatV3.integer(frame["v"]) != ChatV3.VERSION) return null
        val kid = frame["kid"] as? String ?: return null
        val idk = frame["idk"] as? String ?: return null
        val idkBytes = ChatV3.unb64u(idk) ?: return null
        if (idkBytes.size != Ed25519.PUBLIC_KEY_BYTES || kid != ChatV3.id16(idkBytes)) return null
        val list = frame["keys"] as? List<*> ?: return null
        if (list.isEmpty() || list.size > ChatV3.MAX_RECIPIENTS) return null
        val entries = ArrayList<Map<*, *>>()
        var previous = ""
        for (value in list) {
            val entry = value as? Map<*, *> ?: return null
            val entryKid = entry["kid"]
            if (!ChatV3.isKid(entryKid) || !ChatV3.isKid(entry["spk"])) return null
            entryKid as String
            if (entries.isNotEmpty() && entryKid <= previous) return null
            previous = entryKid
            if (ChatV3.unb64u(entry["eph"])?.size != X25519.KEY_BYTES) return null
            if (ChatV3.unb64(entry["ct"])?.size != MlKem768.CIPHERTEXT_BYTES) return null
            if (ChatV3.unb64(entry["nonce"])?.size != ChatV3.NONCE_BYTES) return null
            if (ChatV3.unb64(entry["tag"])?.size != ChatV3.TAG_BYTES) return null
            if (ChatV3.unb64(entry["key"])?.size != ChatV3.KEY_BYTES) return null
            entries.add(entry)
        }
        val ctr = ChatV3.integer(frame["ctr"]) ?: return null
        val ts = ChatV3.integer(frame["ts"]) ?: return null
        if (ctr < 1) return null
        val nonce = ChatV3.unb64(frame["nonce"]) ?: return null
        val tag = ChatV3.unb64(frame["tag"]) ?: return null
        val body = ChatV3.unb64(frame["body"]) ?: return null
        val sig = ChatV3.unb64u(frame["sig"]) ?: return null
        if (nonce.size != ChatV3.NONCE_BYTES || tag.size != ChatV3.TAG_BYTES || sig.size != ChatV3.SIGNATURE_BYTES) return null
        return Shape(kid, idk, idkBytes, ctr, ts, nonce, tag, body, sig, entries)
    }

    fun open(
        frame: Map<String, Any?>,
        now: Long = clock(),
        history: Boolean = false,
        meta: Map<String, Any?> = emptyMap(),
        hold: Boolean = true,
        commit: Boolean = true,
    ): SealOpen {
        return try {
            if (ratchetVersion) openRatchet(frame, now, meta, hold, commit) else openChecked(frame, now, history, meta, hold, commit)
        } catch (_: RuntimeException) {
            SealOpen.Invalid
        } catch (_: AesGcm.AuthenticationException) {
            SealOpen.Invalid
        }
    }

    private fun openChecked(
        frame: Map<String, Any?>,
        now: Long,
        history: Boolean,
        meta: Map<String, Any?>,
        hold: Boolean,
        commit: Boolean,
    ): SealOpen {
        val shape = shape(frame) ?: return SealOpen.Invalid
        val own = shape.kid == keys.kid
        val record = if (own) null else trust.record(shape.kid)
        val recordIdk = if (own) keys.idkText else record?.idk
        val acct = if (own) null else record?.acct
        val senderNode = if (own) null else record?.node
        if (!own && (record == null || record.state != TrustState.APPROVED)) {
            val blocked = record?.state == TrustState.BLOCKED
            if (hold && !blocked && shape.entries.any { it["kid"] == keys.kid } &&
                Ed25519.verify(shape.idkBytes, ChatV3.sealedMessage(room, frame), shape.sig)
            ) {
                trust.hold(HeldFrame(room, shape.kid, frame, meta))
            }
            return SealOpen.Held(shape.kid, blocked)
        }
        if (recordIdk != shape.idk) return SealOpen.Invalid
        if (!Ed25519.verify(shape.idkBytes, ChatV3.sealedMessage(room, frame), shape.sig)) return SealOpen.Invalid
        if (!history && counters.seen(shape.kid, shape.ctr)) return SealOpen.Replay
        if (shape.ts < now - ChatV3.PREKEY_LIFE_MS || shape.ts > now + ChatV3.DAY_MS) return SealOpen.Invalid
        val entry = shape.entries.firstOrNull { it["kid"] == keys.kid } ?: return SealOpen.NotForMe
        keys.wipeExpired(now)
        val prekey = keys.prekey(entry["spk"] as String) ?: return SealOpen.NotForMe
        val ephPub = ChatV3.unb64u(entry["eph"])!!
        val ct = ChatV3.unb64(entry["ct"])!!
        val ss1 = ChatV3.x25519(prekey.xPrivate, ephPub)
        val ss2 = prekey.decapsulate(ct)
        val kek = ChatV3.kek(room, shape.kid, keys.kid, prekey.id, ephPub, ct, ss1, ss2)
        val contentKey = try {
            AesGcm.decrypt(
                ChatV3.unb64(entry["key"])!!,
                ChatV3.unb64(entry["nonce"])!!,
                ChatV3.unb64(entry["tag"])!!,
                kek,
                ChatV3.wrapAad(room, shape.kid, keys.kid, frame["nonce"] as String, frame["tag"] as String),
            )
        } finally {
            Bytes.wipe(ss1, ss2, kek)
        }
        if (contentKey.size != ChatV3.KEY_BYTES) return SealOpen.Invalid
        val clear = try {
            AesGcm.decrypt(shape.body, shape.bodyNonce, shape.bodyTag, contentKey, ChatV3.contentAad(room, shape.kid, shape.ctr, shape.ts))
        } finally {
            Bytes.wipe(contentKey)
        }
        @Suppress("UNCHECKED_CAST")
        val plain = Json.parse(Utf8.decode(clear)) as? Map<String, Any?> ?: return SealOpen.Invalid
        if (senderNode != null && plain.containsKey("senderId") && plain["senderId"] != senderNode) return SealOpen.Invalid
        if (!commit) return SealOpen.Opened(plain, shape.kid, acct, replayed = false, senderNode = senderNode)
        val fresh = counters.record(shape.kid, shape.ctr, deferPersist = history)
        if (!fresh && !history) return SealOpen.Replay
        return SealOpen.Opened(plain, shape.kid, acct, replayed = !fresh, senderNode = senderNode)
    }

    fun unseal(frame: Map<String, Any?>, now: Long = clock()): Map<String, Any?>? = (open(frame, now) as? SealOpen.Opened)?.plain

    private class RatchetReplay : Exception()

    private class RatchetLost : Exception()

    private class SentEntry(val entry: LinkedHashMap<String, Any?>, val mk: ByteArray)

    private fun sealRatchet(plain: Map<String, Any?>, now: Long, only: Collection<String>?): Map<String, Any?> = synchronized(ratchets) {
        val targets = ratchetRecipients(now, only)
        val ctr = counters.nextCounter()
        val contentKey = Bytes.random(ChatV3.KEY_BYTES)
        val working = LinkedHashMap<String, RatchetPeer>()
        var committed = false
        try {
            val content = AesGcm.encrypt(
                Utf8.encode(Json.stringify(plain)),
                contentKey,
                aad = ChatV4.contentAad(room, keys.kid, ctr, now),
            )
            val nonce = ChatV3.b64(content.nonce)
            val tag = ChatV3.b64(content.mac)
            val entries = ArrayList<Map<String, Any?>>()
            for (record in targets) {
                val peer = ratchets.peer(record.kid)
                val sent = try {
                    sendEntry(peer, record.kid, record.spk, now)
                } catch (_: RuntimeException) {
                    peer.wipe()
                    continue
                }
                try {
                    val wrapped = AesGcm.encrypt(contentKey, sent.mk, aad = ChatV4.wrapAad(room, keys.kid, sent.entry, nonce, tag))
                    sent.entry["nonce"] = ChatV3.b64(wrapped.nonce)
                    sent.entry["tag"] = ChatV3.b64(wrapped.mac)
                    sent.entry["key"] = ChatV3.b64(wrapped.cipherText)
                } finally {
                    Bytes.wipe(sent.mk)
                }
                entries.add(sent.entry)
                working[record.kid] = peer
            }
            val frame = linkedMapOf<String, Any?>(
                "type" to ChatV3.SEALED_TYPE,
                "v" to ChatV4.VERSION,
                "kid" to keys.kid,
                "idk" to keys.idkText,
                "ctr" to ctr,
                "ts" to now,
                "nonce" to nonce,
                "tag" to tag,
                "body" to ChatV3.b64(content.cipherText),
                "keys" to entries,
            )
            frame["sig"] = ChatV3.b64u(keys.sign(ChatV4.sealedMessage(room, frame)))
            ratchets.commit(working, now)
            committed = true
            frame
        } finally {
            Bytes.wipe(contentKey)
            if (!committed) for (peer in working.values) peer.wipe()
        }
    }

    private fun sendEntry(peer: RatchetPeer, recipient: String, spk: Map<String, Any?>, now: Long): SentEntry {
        val spkId = spk["id"] as String
        val x = ChatV3.unb64u(spk["x"]) ?: throw IllegalArgumentException("bad prekey")
        val pq = ChatV3.unb64u(spk["pq"]) ?: throw IllegalArgumentException("bad prekey")
        var session = peer.sessions.firstOrNull { it.sid == peer.current }
        if (session != null && session.cks == null && session.dhr == null) session = null
        if (session == null) {
            val eph = X25519.generatePrivateKey()
            val ephPub = X25519.publicKey(eph)
            val ss1 = ChatV3.x25519(eph, x)
            val encapsulated = MlKem768.encapsulate(pq)
            try {
                val sid = ChatV4.sessionId(ephPub, encapsulated.ciphertext)
                val init = ChatV4.kdfInit(room, keys.kid, recipient, spkId, sid, ss1, encapsulated.sharedSecret)
                session = RatchetSession(
                    sid, init.first, RatchetPair(eph, ChatV3.b64u(ephPub)), null, init.second, 0, 0,
                    spkId, encapsulated.ciphertext, null, 0, ArrayList(), now, now,
                )
                peer.sessions.add(session)
                peer.current = sid
            } finally {
                Bytes.wipe(ss1, encapsulated.sharedSecret)
            }
        } else if (session.cks == null) {
            val dhs = X25519.generatePrivateKey()
            val encapsulated = MlKem768.encapsulate(pq)
            val dhOut = ChatV3.x25519(dhs, ChatV3.unb64u(session.dhr)!!)
            try {
                val step = ChatV4.kdfRk(session.rk, dhOut, encapsulated.sharedSecret)
                session.dhs?.let { Bytes.wipe(it.priv) }
                Bytes.wipe(session.rk)
                session.rk = step.first
                session.cks = step.second
                session.dhs = RatchetPair(dhs, ChatV3.b64u(X25519.publicKey(dhs)))
                session.sqSpk = spkId
                session.sqCt = encapsulated.ciphertext
            } finally {
                Bytes.wipe(dhOut, encapsulated.sharedSecret)
            }
        }
        val chain = session.cks!!
        val step = ChatV4.kdfCk(chain)
        val entry = linkedMapOf<String, Any?>(
            "kid" to recipient,
            "sid" to session.sid,
            "spk" to session.sqSpk!!,
            "ct" to ChatV3.b64(session.sqCt!!),
            "dh" to session.dhs!!.pub,
            "pn" to session.pn,
            "n" to session.ns,
        )
        Bytes.wipe(chain)
        session.cks = step.second
        session.ns += 1
        session.used = now
        return SentEntry(entry, step.first)
    }

    private fun prekeyFor(id: String, now: Long): ChatPrekey {
        keys.wipeExpired(now)
        return keys.prekey(id) ?: throw RatchetLost()
    }

    private fun skipTo(session: RatchetSession, until: Long, now: Long) {
        if (until < session.nr) throw RatchetReplay()
        if (until - session.nr > ChatV4.MAX_SKIP) throw RatchetLost()
        var chain = session.ckr ?: throw RatchetLost()
        while (session.nr < until) {
            val step = ChatV4.kdfCk(chain)
            Bytes.wipe(chain)
            session.skipped.add(RatchetSkipped(session.dhr!!, session.nr, step.first, now))
            chain = step.second
            session.nr += 1
        }
        session.ckr = chain
    }

    private fun receive(session: RatchetSession, entry: Map<*, *>, now: Long): ByteArray {
        val dh = entry["dh"] as String
        val n = ChatV3.integer(entry["n"])!!
        val index = session.skipped.indexOfFirst { it.dh == dh && it.n == n }
        if (index >= 0) return session.skipped.removeAt(index).mk
        if (dh != session.dhr) {
            val dhs = session.dhs ?: throw RatchetLost()
            if (session.ckr != null) skipTo(session, ChatV3.integer(entry["pn"])!!, now)
            val prekey = prekeyFor(entry["spk"] as String, now)
            val ss = prekey.decapsulate(ChatV3.unb64(entry["ct"])!!)
            val dhOut = ChatV3.x25519(dhs.priv, ChatV3.unb64u(dh)!!)
            try {
                val step = ChatV4.kdfRk(session.rk, dhOut, ss)
                Bytes.wipe(session.rk)
                session.ckr?.let { Bytes.wipe(it) }
                session.cks?.let { Bytes.wipe(it) }
                session.rk = step.first
                session.ckr = step.second
                session.nr = 0
                session.dhr = dh
                session.pn = session.ns
                session.ns = 0
                session.cks = null
                session.sqSpk = null
                session.sqCt = null
            } finally {
                Bytes.wipe(ss, dhOut)
            }
        }
        if (session.ckr == null) throw RatchetLost()
        skipTo(session, n, now)
        val chain = session.ckr!!
        val step = ChatV4.kdfCk(chain)
        Bytes.wipe(chain)
        session.ckr = step.second
        session.nr += 1
        return step.first
    }

    private fun openEntry(peer: RatchetPeer, sender: String, entry: Map<*, *>, now: Long): ByteArray {
        val sid = entry["sid"] as String
        var session = peer.sessions.firstOrNull { it.sid == sid }
        if (session == null) {
            val dh = ChatV3.unb64u(entry["dh"])!!
            val ct = ChatV3.unb64(entry["ct"])!!
            if (sid != ChatV4.sessionId(dh, ct) || ChatV3.integer(entry["pn"]) != 0L) throw RatchetLost()
            val prekey = prekeyFor(entry["spk"] as String, now)
            val ss1 = ChatV3.x25519(prekey.xPrivate, dh)
            val ss2 = prekey.decapsulate(ct)
            try {
                val init = ChatV4.kdfInit(room, sender, keys.kid, prekey.id, sid, ss1, ss2)
                session = RatchetSession(
                    sid, init.first, null, entry["dh"] as String, null, 0, 0, null, null, init.second, 0, ArrayList(), now, now,
                )
                peer.sessions.add(session)
            } finally {
                Bytes.wipe(ss1, ss2)
            }
        }
        val mk = receive(session, entry, now)
        session.used = now
        peer.current = session.sid
        return mk
    }

    private fun ratchetShape(frame: Map<String, Any?>): Shape? {
        if (frame["type"] != ChatV3.SEALED_TYPE || ChatV3.integer(frame["v"]) != ChatV4.VERSION) return null
        val kid = frame["kid"] as? String ?: return null
        val idk = frame["idk"] as? String ?: return null
        val idkBytes = ChatV3.unb64u(idk) ?: return null
        if (idkBytes.size != Ed25519.PUBLIC_KEY_BYTES || kid != ChatV3.id16(idkBytes)) return null
        val list = frame["keys"] as? List<*> ?: return null
        if (list.size > ChatV3.MAX_RECIPIENTS) return null
        val entries = ArrayList<Map<*, *>>()
        var previous = ""
        for (value in list) {
            if (!ChatV4.validEntry(value)) return null
            val entry = value as Map<*, *>
            val entryKid = entry["kid"] as String
            if (entries.isNotEmpty() && entryKid <= previous) return null
            if (entryKid == kid) return null
            previous = entryKid
            entries.add(entry)
        }
        val ctr = ChatV4.count(frame["ctr"]) ?: return null
        val ts = ChatV3.integer(frame["ts"]) ?: return null
        if (ctr < 1) return null
        val nonce = ChatV3.unb64(frame["nonce"]) ?: return null
        val tag = ChatV3.unb64(frame["tag"]) ?: return null
        val body = ChatV3.unb64(frame["body"]) ?: return null
        val sig = ChatV3.unb64u(frame["sig"]) ?: return null
        if (nonce.size != ChatV3.NONCE_BYTES || tag.size != ChatV3.TAG_BYTES || sig.size != ChatV3.SIGNATURE_BYTES) return null
        return Shape(kid, idk, idkBytes, ctr, ts, nonce, tag, body, sig, entries)
    }

    private fun openRatchet(frame: Map<String, Any?>, now: Long, meta: Map<String, Any?>, hold: Boolean, commit: Boolean): SealOpen {
        val shape = ratchetShape(frame) ?: return SealOpen.Invalid
        if (shape.kid == keys.kid) return SealOpen.Own
        val record = trust.record(shape.kid)
        if (record == null || record.state != TrustState.APPROVED) {
            val blocked = record?.state == TrustState.BLOCKED
            if (hold && commit && !blocked && shape.entries.any { it["kid"] == keys.kid } &&
                Ed25519.verify(shape.idkBytes, ChatV4.sealedMessage(room, frame), shape.sig)
            ) {
                trust.hold(HeldFrame(room, shape.kid, frame, meta))
            }
            return SealOpen.Held(shape.kid, blocked)
        }
        if (record.idk != shape.idk) return SealOpen.Invalid
        if (!Ed25519.verify(shape.idkBytes, ChatV4.sealedMessage(room, frame), shape.sig)) return SealOpen.Invalid
        return synchronized(ratchets) { openRatchetLocked(frame, shape, record, now, commit) }
    }

    private fun openRatchetLocked(frame: Map<String, Any?>, shape: Shape, record: TrustRecord, now: Long, commit: Boolean): SealOpen {
        if (counters.seen(shape.kid, shape.ctr)) return SealOpen.Replay
        if (shape.ts < now - ChatV3.PREKEY_LIFE_MS || shape.ts > now + ChatV3.DAY_MS) return SealOpen.Invalid
        val entry = shape.entries.firstOrNull { it["kid"] == keys.kid } ?: return SealOpen.NotForMe
        val peer = ratchets.peer(shape.kid)
        var committed = false
        try {
            val contentKey = try {
                val mk = openEntry(peer, shape.kid, entry, now)
                try {
                    AesGcm.decrypt(
                        ChatV3.unb64(entry["key"])!!,
                        ChatV3.unb64(entry["nonce"])!!,
                        ChatV3.unb64(entry["tag"])!!,
                        mk,
                        ChatV4.wrapAad(room, shape.kid, entry, frame["nonce"] as String, frame["tag"] as String),
                    )
                } finally {
                    Bytes.wipe(mk)
                }
            } catch (_: RatchetReplay) {
                return SealOpen.Replay
            } catch (_: RatchetLost) {
                return SealOpen.NoSession(shape.kid)
            } catch (_: AesGcm.AuthenticationException) {
                return SealOpen.NoSession(shape.kid)
            } catch (_: RuntimeException) {
                return SealOpen.NoSession(shape.kid)
            }
            if (contentKey.size != ChatV3.KEY_BYTES) return SealOpen.NoSession(shape.kid)
            val clear = try {
                AesGcm.decrypt(shape.body, shape.bodyNonce, shape.bodyTag, contentKey, ChatV4.contentAad(room, shape.kid, shape.ctr, shape.ts))
            } finally {
                Bytes.wipe(contentKey)
            }
            @Suppress("UNCHECKED_CAST")
            val plain = Json.parse(Utf8.decode(clear)) as? Map<String, Any?> ?: return SealOpen.Invalid
            if (plain.containsKey("senderId") && plain["senderId"] != record.node) return SealOpen.Invalid
            if (!commit) return SealOpen.Opened(plain, shape.kid, record.acct, replayed = false, senderNode = record.node)
            ratchets.commit(mapOf(shape.kid to peer), now)
            committed = true
            counters.record(shape.kid, shape.ctr, deferPersist = false)
            return SealOpen.Opened(plain, shape.kid, record.acct, replayed = false, senderNode = record.node)
        } finally {
            if (!committed) peer.wipe()
        }
    }

    companion object {
        fun loadOrCreate(
            roomName: String,
            scope: String,
            policy: SealPolicy,
            acct: String,
            secureStore: SecretStore,
            clock: () -> Long = System::currentTimeMillis,
            node: String = acct,
        ): RoomSeal {
            val room = dartTrim(roomName)
            if (room.isEmpty()) throw IllegalArgumentException("A chat room is required.")
            val trustScope = dartTrim(scope).ifEmpty { room }
            return RoomSeal(
                room,
                trustScope,
                policy,
                acct,
                node,
                ChatDeviceKeys.forStore(secureStore),
                ChatTrust.forStore(secureStore, trustScope),
                RoomCounters.forStore(secureStore, room),
                RoomRatchets.forStore(secureStore, room),
                secureStore,
                clock,
            )
        }

        fun existing(
            roomName: String,
            scope: String,
            policy: SealPolicy,
            acct: String,
            secureStore: SecretStore,
            clock: () -> Long = System::currentTimeMillis,
        ): RoomSeal? {
            val room = dartTrim(roomName)
            if (room.isEmpty()) return null
            val keys = ChatDeviceKeys.existing(secureStore) ?: return null
            val trustScope = dartTrim(scope).ifEmpty { room }
            return RoomSeal(
                room,
                trustScope,
                policy,
                acct,
                acct,
                keys,
                ChatTrust.forStore(secureStore, trustScope),
                RoomCounters.forStore(secureStore, room),
                RoomRatchets.forStore(secureStore, room),
                secureStore,
                clock,
            )
        }

        fun create(
            roomName: String,
            scope: String,
            policy: SealPolicy,
            acct: String,
            keys: ChatDeviceKeys,
            trust: ChatTrust = ChatTrust.inMemory(dartTrim(scope).ifEmpty { dartTrim(roomName) }),
            clock: () -> Long = System::currentTimeMillis,
            node: String = acct,
            version: Long = if (policy == SealPolicy.STRICT) ChatV4.VERSION else ChatV3.VERSION,
        ): RoomSeal {
            val room = dartTrim(roomName)
            if (room.isEmpty()) throw IllegalArgumentException("A chat room is required.")
            return RoomSeal(
                room, trust.scope, policy, acct, node, keys, trust, RoomCounters.forStore(null, room), RoomRatchets.forStore(null, room), null, clock,
                version == ChatV4.VERSION,
            )
        }

        fun isKeyFrame(frame: Map<String, Any?>): Boolean =
            frame["type"] == ChatV3.KEY_TYPE && ChatV3.integer(frame["v"]) == ChatV3.VERSION && ChatV3.isKid(frame["kid"])

        fun isSealedFrame(frame: Map<String, Any?>): Boolean {
            val keys = frame["keys"]
            return frame["type"] == ChatV3.SEALED_TYPE && ChatV3.integer(frame["v"]) == ChatV3.VERSION &&
                ChatV3.isKid(frame["kid"]) && keys is List<*> && keys.isNotEmpty() && keys.size <= ChatV3.MAX_RECIPIENTS
        }

        fun isRatchetFrame(frame: Map<String, Any?>): Boolean {
            val keys = frame["keys"]
            return frame["type"] == ChatV3.SEALED_TYPE && ChatV3.integer(frame["v"]) == ChatV4.VERSION &&
                ChatV3.isKid(frame["kid"]) && keys is List<*> && keys.size <= ChatV3.MAX_RECIPIENTS
        }

        fun frameKeyId(frame: Map<String, Any?>): String =
            if (isSealedFrame(frame) || isRatchetFrame(frame)) frame["kid"] as String else ""

        fun wrappedKeyIds(frame: Map<String, Any?>): List<String> {
            if (!isSealedFrame(frame) && !isRatchetFrame(frame)) return emptyList()
            return (frame["keys"] as List<*>).mapNotNull { (it as? Map<*, *>)?.get("kid") as? String }
        }

        fun isLegacyFrame(frame: Map<String, Any?>): Boolean {
            val version = ChatV3.integer(frame["v"])
            return when (frame["type"]) {
                ChatV3.KEY_TYPE -> version != ChatV3.VERSION
                ChatV3.SEALED_TYPE -> version != ChatV3.VERSION && version != ChatV4.VERSION
                else -> false
            }
        }
    }
}
