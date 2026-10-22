package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.resourceText
import java.io.File

class ChatV3VectorsTest {
    private val vectors: Map<String, Any?> = Json.parseObject(loadVectors())
    private val room = vectors["room"] as String
    private val scope = vectors["scope"] as String
    private val now = vectors["now"] as Long
    private val participants: List<String> = (vectors["participants"] as List<*>).map { it as String }

    private fun node(name: String): String = obj(vectors, "nodes")[name] as String

    private fun loadVectors(): String {
        val root = runCatching { CryptoInterop.repoRoot }.getOrNull()
        val canonical = root?.let { File(it, "conformance/chat/v3/vectors.json") }
        if (canonical != null && canonical.isFile) return canonical.readText(Charsets.UTF_8)
        return resourceText("chat/v3/vectors.json")
    }

    @Suppress("UNCHECKED_CAST")
    private fun obj(map: Map<String, Any?>, vararg path: String): Map<String, Any?> {
        var current: Map<String, Any?> = map
        for (key in path) current = current[key] as Map<String, Any?>
        return current
    }

    private fun str(map: Map<String, Any?>, key: String): String = map[key] as String

    private fun keysFor(name: String): ChatDeviceKeys {
        val secrets = obj(vectors, "devices", name)
        val keys = ChatDeviceKeys.fromSeed(ChatV3.unb64(str(secrets, "identitySeed"))!!)
        assertEq(str(secrets, "idk"), keys.idkText, "$name idk")
        assertEq(str(secrets, "kid"), keys.kid, "$name kid")
        for (value in secrets["prekeys"] as List<*>) {
            @Suppress("UNCHECKED_CAST")
            val spk = value as Map<String, Any?>
            val made = keys.addPrekey(ChatV3.unb64(str(spk, "xPriv"))!!, ChatV3.unb64(str(spk, "pqSeed"))!!, spk["ts"] as Long)
            assertEq(str(spk, "id"), made.id, "$name prekey id")
            assertEq(str(spk, "x"), ChatV3.b64u(made.x), "$name prekey x")
            assertEq(str(spk, "pq"), ChatV3.b64u(made.pq), "$name prekey pq")
            assertEq(spk["exp"], made.exp, "$name prekey exp")
        }
        return keys
    }

    private fun seal(name: String, acct: String, keys: ChatDeviceKeys = keysFor(name), at: Long = now, node: String = acct): RoomSeal =
        RoomSeal.create(room, scope, SealPolicy.STRICT, acct, keys, clock = { at }, version = ChatV3.VERSION, node = node).also { it.participants = participants }

    private fun vectorSeal(name: String, acct: String): RoomSeal = seal(name, acct, node = node(name))

    private fun bundle(name: String): Map<String, Any?> = obj(vectors, "bundles", name)

    private fun bobWithTrust(): RoomSeal {
        val bob = vectorSeal("bob", "bob")
        assertEq(TrustState.APPROVED, bob.acceptKeyFrame(bundle("alice"), now)?.state)
        assertEq(TrustState.APPROVED, bob.acceptKeyFrame(bundle("bob"), now)?.state)
        assertEq(TrustState.PENDING, bob.acceptKeyFrame(bundle("alice2"), now)?.state)
        assertEq(obj(vectors, "bundleChecks")["pendingStateForAlice2AtBob"], TrustState.PENDING.wire)
        return bob
    }

    private fun sealed(name: String): Map<String, Any?> = obj(vectors, "sealed", name)

    private fun frame(name: String): Map<String, Any?> = obj(sealed(name), "frame")

    private fun kidOf(name: String): String = str(obj(vectors, "devices", name), "kid")

    fun testBundlesVerify() {
        for (name in listOf("alice", "alice2", "bob", "bobOld", "injectedOutsider", "injectedAsBob")) {
            expect(ChatV3.verifyBundle(bundle(name), room, now), name)
        }
        assertEq(node("alice"), bundle("alice")["node"])
        for ((name, value) in obj(vectors, "bundles", "invalid")) {
            @Suppress("UNCHECKED_CAST")
            expect(!ChatV3.verifyBundle(value as Map<String, Any?>, room, now), name)
        }
        expect(!ChatV3.verifyBundle(bundle("alice"), room, obj(vectors, "bundleChecks")["expiredAt"] as Long))
    }

    fun testKotlinBundlesMatchTheReferenceByteForByte() {
        assertEq(bundle("alice"), vectorSeal("alice", "alice").keyFrame(now))
        assertEq(bundle("bob"), vectorSeal("bob", "bob").keyFrame(now))
        assertEq(bundle("alice2"), vectorSeal("alice2", "alice").keyFrame(now))
    }

    fun testBundleForKnownKidCannotChangeAccount() {
        val bob = bobWithTrust()
        val changed = LinkedHashMap(bundle("alice")).also { it["acct"] = "mallory" }
        assertEq(null, bob.acceptKeyFrame(changed, now))
        assertEq("alice", bob.trust.record(kidOf("alice"))?.acct)
        assertEq(node("alice"), bob.trust.record(kidOf("alice"))?.node)
    }

    fun testServerInjectedDevicesAreRefusedOrHeld() {
        val bob = bobWithTrust()
        assertEq(null, bob.acceptKeyFrame(bundle("injectedOutsider"), now), "an account outside the DM is dropped")
        assertEq(TrustState.PENDING, bob.acceptKeyFrame(bundle("injectedAsBob"), now)?.state, "a new device for bob waits")
        val open = RoomSeal.create(room, "open-scope", SealPolicy.OPEN, "bob", bob.keys, clock = { now })
        assertEq(null, open.acceptKeyFrame(bundle("injectedOutsider"), now), "an open room needs the node signature")
    }

    fun testOpenRelaySequence() {
        val bob = bobWithTrust()
        val first = bob.open(frame("first"), now)
        expect(first is SealOpen.Opened, "first opens")
        first as SealOpen.Opened
        assertEq(sealed("first")["plain"], first.plain)
        assertEq(sealed("first")["senderKid"], first.senderKid)
        assertEq("alice", first.senderAcct)
        assertEq(node("alice"), first.plain["senderId"])
        assertEq(node("alice"), first.senderNode)
        expect(bob.open(frame("replay"), now) is SealOpen.Replay, "replay")
        val second = bob.open(frame("second"), now + 1000)
        expect(second is SealOpen.Opened && second.plain == sealed("second")["plain"], "second opens")
        expect(bob.open(frame("tampered"), now) is SealOpen.Invalid, "tampered")
        expect(bob.open(frame("forgedSenderId"), now + 2000) is SealOpen.Invalid, "forged sender id")
        val pending = sealed("fromPendingDevice")
        @Suppress("UNCHECKED_CAST")
        assertEq(pending["keys"], RoomSeal.wrappedKeyIds(pending["frame"] as Map<String, Any?>))
        val held = bob.open(frame("fromPendingDevice"), now + 3000)
        expect(held is SealOpen.Held && held.kid == kidOf("alice2"), "pending device is held")
        assertEq(1, bob.trust.heldCount(room))
        bob.approve(kidOf("alice2"))
        val released = bob.takeHeld(kidOf("alice2"))
        assertEq(1, released.size)
        expect(bob.open(released.single().frame, now + 3000) is SealOpen.Opened, "opens after approval")
    }

    fun testForgedFrameDoesNotBurnCounter() {
        val bob = bobWithTrust()
        expect(bob.open(frame("tampered"), now) is SealOpen.Invalid)
        expect(bob.open(frame("first"), now) is SealOpen.Opened)
    }

    fun testBlockedSenderIsHeldNotOpened() {
        val bob = bobWithTrust()
        bob.block(kidOf("alice"))
        val result = bob.open(frame("first"), now)
        expect(result is SealOpen.Held && result.blocked, "blocked")
        assertEq(0, bob.trust.heldCount(room), "blocked frames are dropped")
    }

    fun testWipedPrekeyCannotOpen() {
        val bob = bobWithTrust()
        val later = obj(vectors, "afterBobWipesOldPrekey")["now"] as Long
        bob.keys.wipeExpired(later)
        expect(bob.open(frame("first"), now) is SealOpen.NotForMe)
    }

    fun testOtherRoomCannotOpen() {
        val bob = bobWithTrust()
        val other = RoomSeal.create("another-room", scope, SealPolicy.STRICT, "bob", bob.keys, trust = bob.trust, clock = { now }, version = ChatV3.VERSION, node = node("bob"))
        expect(other.open(frame("first"), now) is SealOpen.Invalid)
    }

    fun testHistoryReplayReopensWithoutBurningAgain() {
        val bob = bobWithTrust()
        expect(bob.open(frame("first"), now) is SealOpen.Opened)
        val again = bob.open(frame("first"), now, history = true)
        expect(again is SealOpen.Opened && again.replayed, "a history copy of a seen frame opens, marked replayed")
        expect(bob.open(frame("first"), now) is SealOpen.Replay)
    }

    fun testKekAndMlKem() {
        val k = obj(vectors, "kek")
        val ct = ChatV3.unb64(str(k, "ct"))!!
        val bob = keysFor("bob")
        val spk = bob.prekey(str(k, "spkId"))!!
        assertBytes(ChatV3.unb64(str(k, "ss2"))!!, spk.decapsulate(ct))
        assertBytes(ChatV3.unb64(str(k, "ss1"))!!, ChatV3.x25519(ChatV3.unb64(str(k, "ephPriv"))!!, spk.x))
        assertBytes(ChatV3.unb64(str(k, "ss1"))!!, ChatV3.x25519(spk.xPrivate, ChatV3.unb64u(str(k, "ephPub"))!!))
        assertEq(str(k, "ephPub"), ChatV3.b64u(X25519.publicKey(ChatV3.unb64(str(k, "ephPriv"))!!)))
        val derived = ChatV3.kek(
            str(k, "room"), str(k, "sender"), str(k, "recipient"), str(k, "spkId"),
            ChatV3.unb64u(str(k, "ephPub"))!!, ct, ChatV3.unb64(str(k, "ss1"))!!, ChatV3.unb64(str(k, "ss2"))!!,
        )
        assertEq(str(k, "kek"), ChatV3.b64(derived))
    }

    fun testSafetyNumbers() {
        for (name in listOf("twoDevices", "threeDevices")) {
            val s = obj(vectors, "safety", name)
            val idks = (s["idks"] as List<*>).map { ChatV3.unb64u(it)!! }
            val number = ChatV3.safetyNumber(str(s, "scope"), idks)
            assertEq(str(s, "digits"), number.digits, name)
            assertEq(60, number.digits.length)
            assertEq(12, number.groups.size)
            assertEq(str(s, "qr"), number.qr, name)
            assertEq(number.digits, ChatV3.safetyNumber(str(s, "scope"), idks.reversed()).digits)
        }
        expect(obj(vectors, "safety", "twoDevices")["digits"] != obj(vectors, "safety", "threeDevices")["digits"])
    }

    fun testSafetyNumberFromTrustMatchesTheVector() {
        val bob = bobWithTrust()
        assertEq(obj(vectors, "safety", "twoDevices")["digits"], bob.safetyNumber().digits)
        bob.approve(kidOf("alice2"))
        assertEq(obj(vectors, "safety", "threeDevices")["digits"], bob.safetyNumber().digits)
    }

    fun testLinkHandshake() {
        val link = obj(vectors, "link")
        val initiator = MeshLinkCrypto.parseHello(obj(link, "initiatorHello"))!!
        val responder = MeshLinkCrypto.parseHello(obj(link, "responderHello"))!!
        val transcript = MeshLinkCrypto.transcript(initiator, responder)
        assertEq(str(link, "transcript"), ChatV3.b64u(transcript))
        val iSig = ChatV3.unb64u(str(link, "initiatorAuthSig"))!!
        val rSig = ChatV3.unb64u(str(link, "responderAuthSig"))!!
        expect(Ed25519.verify(initiator.idkBytes, MeshLinkCrypto.authMessage(true, transcript), iSig))
        expect(Ed25519.verify(responder.idkBytes, MeshLinkCrypto.authMessage(false, transcript), rSig))
        expect(!Ed25519.verify(initiator.idkBytes, MeshLinkCrypto.authMessage(false, transcript), iSig))
        val iKeys = ChatDeviceKeys.fromSeed(ChatV3.unb64(str(link, "initiatorIdentitySeed"))!!)
        assertEq(str(link, "initiatorAuthSig"), ChatV3.b64u(iKeys.sign(MeshLinkCrypto.authMessage(true, transcript))))
        val iEph = ChatV3.unb64(str(link, "initiatorEphPriv"))!!
        val rEph = ChatV3.unb64(str(link, "responderEphPriv"))!!
        assertEq(initiator.eph, ChatV3.b64u(X25519.publicKey(iEph)))
        val keys = MeshLinkCrypto.keys(iEph, X25519.publicKey(rEph), transcript)
        val mirror = MeshLinkCrypto.keys(rEph, X25519.publicKey(iEph), transcript)
        assertBytes(keys.i2r, mirror.i2r)
        assertBytes(keys.r2i, mirror.r2i)
        assertEq(str(link, "keyI2R"), ChatV3.b64(keys.i2r))
        assertEq(str(link, "keyR2I"), ChatV3.b64(keys.r2i))
        for (name in listOf("frame0", "frame1")) {
            val entry = obj(link, name)
            val data = ChatV3.unb64(str(entry, "data"))!!
            val opened = MeshLinkCrypto.open(keys.i2r, entry["counter"] as Long, data)!!
            assertEq(entry["plain"], Json.parse(Utf8.decode(opened)))
            val ordered = linkedMapOf("type" to "hello", "senderId" to node("alice"))
            assertEq(entry["plain"], ordered)
            assertEq(str(entry, "data"), ChatV3.b64(MeshLinkCrypto.seal(keys.i2r, entry["counter"] as Long, Utf8.encode(Json.stringify(ordered)))))
        }
        assertEq(null, MeshLinkCrypto.open(keys.i2r, 1, ChatV3.unb64(str(obj(link, "frame0"), "data"))!!))
        assertEq(null, MeshLinkCrypto.open(keys.r2i, 0, ChatV3.unb64(str(obj(link, "frame0"), "data"))!!))
    }

    fun testSealOpenRoundTripBetweenKotlinDevices() {
        val alice = seal("alice", "alice")
        val bob = seal("bob", "bob")
        expect(alice.acceptKeyFrame(bob.keyFrame(now), now) != null)
        expect(bob.acceptKeyFrame(alice.keyFrame(now), now) != null)
        val plain = mapOf("type" to "chat", "id" to "k1", "senderId" to "alice", "channel" to scope, "text" to "round trip ✓")
        val frame = alice.seal(plain, now)
        assertEq(listOf(kidOf("alice"), kidOf("bob")).sorted(), RoomSeal.wrappedKeyIds(frame))
        expect(!Json.stringify(frame).contains("round trip"))
        @Suppress("UNCHECKED_CAST")
        val wire = Json.parse(Json.stringify(frame)) as Map<String, Any?>
        val opened = bob.open(wire, now)
        expect(opened is SealOpen.Opened && opened.plain == plain && opened.senderAcct == "alice", "bob opens")
        val own = alice.open(wire, now)
        expect(own is SealOpen.Opened && own.plain == plain, "the sender opens its own copy")
        expect(bob.open(wire, now) is SealOpen.Replay)
        val next = alice.seal(plain + ("id" to "k2"), now)
        assertEq((frame["ctr"] as Long) + 1, next["ctr"])
        expect(bob.open(next, now) is SealOpen.Opened)
        val lying = alice.seal(plain + ("senderId" to "bob"), now)
        expect(bob.open(lying, now) is SealOpen.Invalid, "acct check")
        val onlyAlice = alice.seal(plain, now, only = emptyList())
        assertEq(listOf(kidOf("alice")), RoomSeal.wrappedKeyIds(onlyAlice))
        expect(bob.open(onlyAlice, now) is SealOpen.NotForMe)
        val stale = alice.seal(plain, now - ChatV3.PREKEY_LIFE_MS - 1)
        expect(bob.open(stale, now) is SealOpen.Invalid, "too old")
    }

    fun testPendingDeviceGetsNothingSealedAndIsReleasedOnApproval() {
        val bob = seal("bob", "bob")
        val alice = seal("alice", "alice")
        val alice2 = seal("alice2", "alice")
        bob.acceptKeyFrame(alice.keyFrame(now), now)
        assertEq(TrustState.PENDING, bob.acceptKeyFrame(alice2.keyFrame(now), now)?.state)
        assertEq(listOf(kidOf("alice"), kidOf("bob")).sorted(), RoomSeal.wrappedKeyIds(bob.seal(mapOf("type" to "chat"), now)))
        alice2.acceptKeyFrame(bob.keyFrame(now), now)
        val fromPending = alice2.seal(mapOf("type" to "chat", "senderId" to "alice", "text" to "new phone"), now)
        expect(bob.open(fromPending, now) is SealOpen.Held)
        bob.approve(kidOf("alice2"))
        assertEq(listOf(kidOf("alice"), kidOf("alice2"), kidOf("bob")).sorted(), RoomSeal.wrappedKeyIds(bob.seal(mapOf("type" to "chat"), now)))
        val released = bob.takeHeld(kidOf("alice2"))
        assertEq("new phone", (bob.open(released.single().frame, now) as SealOpen.Opened).plain["text"])
    }

    fun testStrictScopeTrustsAccountsNotDevices() {
        val bob = seal("bob", "bob", node = "bob-phone")
        val alice = seal("alice", "alice", node = "alice-phone")
        val stranger = RoomSeal.create(room, scope, SealPolicy.STRICT, "mallory", ChatDeviceKeys.fromSeed(ByteArray(32) { 7 }), clock = { now }, version = ChatV3.VERSION, node = "mallory-node")
        val bobTablet = RoomSeal.create(room, scope, SealPolicy.STRICT, "bob", ChatDeviceKeys.fromSeed(ByteArray(32) { 8 }), clock = { now }, version = ChatV3.VERSION, node = "bob-tablet")
        bob.keyFrame(now)
        assertEq(TrustState.APPROVED, bob.acceptKeyFrame(alice.keyFrame(now), now)?.state)
        assertEq(null, bob.acceptKeyFrame(stranger.keyFrame(now), now))
        assertEq(TrustState.PENDING, bob.acceptKeyFrame(bobTablet.keyFrame(now), now)?.state, "the user's own new device waits too")
        alice.acceptKeyFrame(bob.keyFrame(now), now)
        expect(bob.open(alice.seal(mapOf("type" to "chat", "senderId" to "alice-phone"), now), now) is SealOpen.Opened)
        expect(bob.open(alice.seal(mapOf("type" to "chat", "senderId" to "alice"), now), now) is SealOpen.Invalid, "senderId is checked against node")
    }

    fun testReplayWindowSlidesAndRejectsOldCounters() {
        val alice = seal("alice", "alice")
        val bob = seal("bob", "bob")
        alice.acceptKeyFrame(bob.keyFrame(now), now)
        bob.acceptKeyFrame(alice.keyFrame(now), now)
        val frames = (1..3).map { alice.seal(mapOf("type" to "typing", "n" to it.toLong()), now) }
        expect(bob.open(frames[2], now) is SealOpen.Opened)
        expect(bob.open(frames[0], now) is SealOpen.Opened, "out of order inside the window")
        expect(bob.open(frames[0], now) is SealOpen.Replay)
        repeat(ChatV3.REPLAY_WINDOW) { alice.seal(mapOf("type" to "typing"), now, only = emptyList()) }
        val late = alice.seal(mapOf("type" to "typing"), now)
        expect(bob.open(late, now) is SealOpen.Opened)
        expect(bob.open(frames[1], now) is SealOpen.Replay, "below the window")
    }

    fun testStateSurvivesARestart() {
        val store = MemorySecretStore()
        val first = RoomSeal.loadOrCreate(room, scope, SealPolicy.STRICT, "bob", store, clock = { now })
        val peer = RoomSeal.create(room, scope, SealPolicy.STRICT, "alice", keysFor("alice"), clock = { now })
        first.acceptKeyFrame(peer.keyFrame(now), now)
        peer.acceptKeyFrame(first.keyFrame(now), now)
        val sent = first.seal(mapOf("type" to "chat"), now)
        val received = peer.seal(mapOf("type" to "chat", "senderId" to "alice"), now)
        expect(first.open(received, now) is SealOpen.Opened)
        expect(store.read(ChatDeviceKeys.IDENTITY_STORE_KEY) != null)
        expect(store.read(ChatDeviceKeys.PREKEYS_STORE_KEY) != null)
        expect(store.read(first.trust.storeKey) != null)
        expect(store.read(ChatV4.storeKey(room)) != null)
        val copy = MemorySecretStore(store.snapshot())
        val restarted = RoomSeal.loadOrCreate(room, scope, SealPolicy.STRICT, "bob", copy, clock = { now })
        assertEq(first.keyId, restarted.keyId)
        assertEq(TrustState.APPROVED, restarted.trust.record(peer.keyId)?.state)
        expect(restarted.open(received, now) is SealOpen.Replay, "seen counters persist")
        expect((restarted.seal(mapOf("type" to "chat"), now)["ctr"] as Long) > (sent["ctr"] as Long), "counters never repeat")
        expect(peer.open(restarted.seal(mapOf("type" to "chat"), now), now) is SealOpen.Opened)
    }

    fun testPrekeysRotateDailyAndAreWipedAfterEightDays() {
        val keys = ChatDeviceKeys.fromSeed(ByteArray(32) { 9 })
        val first = keys.newest(now)
        val generation = keys.generation
        assertEq(first.id, keys.newest(now + ChatV3.DAY_MS - 1).id)
        val second = keys.newest(now + ChatV3.DAY_MS)
        expect(second.id != first.id)
        expect(keys.generation > generation)
        expect(keys.prekey(first.id) != null, "the old prekey is kept until it expires")
        keys.wipeExpired(first.exp + 1)
        assertEq(null, keys.prekey(first.id))
        repeat(12) { keys.newest(now + (it + 2) * ChatV3.DAY_MS) }
        expect(keys.prekeyIds().size <= ChatV3.MAX_PREKEYS)
    }

    fun testLinkFramesRoundTripWithCounters() {
        val key = Bytes.random(32)
        val sealed = MeshLinkCrypto.seal(key, 7, Utf8.encode("""{"type":"focus","on":true}"""))
        assertEq("""{"type":"focus","on":true}""", Utf8.decode(MeshLinkCrypto.open(key, 7, sealed)!!))
        assertEq(null, MeshLinkCrypto.open(key, 8, sealed))
        val flipped = sealed.copyOf().also { it[0] = (it[0].toInt() xor 1).toByte() }
        assertEq(null, MeshLinkCrypto.open(key, 7, flipped))
        assertBytes(byteArrayOf(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2), MeshLinkCrypto.nonce(258))
    }
}
