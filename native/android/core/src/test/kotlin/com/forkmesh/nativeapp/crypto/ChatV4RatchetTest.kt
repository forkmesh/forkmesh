package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.random.Random

class ChatV4RatchetTest {
    private val v = ChatV4Vectors
    private val now = v.now
    private val room = v.room

    private fun pair(): Pair<RoomSeal, RoomSeal> = v.device("alice") to v.device("bob")

    private fun chat(text: String): Map<String, Any?> = linkedMapOf("type" to "chat", "id" to text, "text" to text)

    private fun opened(result: SealOpen, label: String = ""): SealOpen.Opened {
        expect(result is SealOpen.Opened, "$label expected opened but was ${result.javaClass.simpleName}")
        return result as SealOpen.Opened
    }

    private fun copy(frame: Map<String, Any?>): MutableMap<String, Any?> = Json.parseObject(Json.stringify(frame))

    fun testConversationWithDropsAndReordering() {
        val (alice, bob) = pair()
        val rng = Random(4)
        var at = now
        for (turn in 0 until 6) {
            val sender = if (turn % 2 == 0) alice else bob
            val receiver = if (turn % 2 == 0) bob else alice
            val frames = (0 until 5).map { sender.seal(chat("$turn-$it"), at + it) }.shuffled(rng)
            for (frame in frames) opened(receiver.open(frame, at + 10), "turn $turn")
            at += 100
        }
    }

    fun testAMessageLostInAnOldChainOpensAfterANewChain() {
        val (alice, bob) = pair()
        val late = alice.seal(chat("late"), now)
        opened(bob.open(alice.seal(chat("a2"), now + 1), now + 2))
        opened(alice.open(bob.seal(chat("b1"), now + 3), now + 4))
        opened(bob.open(alice.seal(chat("a3"), now + 5), now + 6))
        assertEq("late", opened(bob.open(late, now + 7)).plain["text"])
        expect(bob.open(late, now + 7) is SealOpen.Replay)
    }

    fun testSimultaneousFirstMessagesConverge() {
        val (alice, bob) = pair()
        val fromAlice = alice.seal(chat("a"), now)
        val fromBob = bob.seal(chat("b"), now)
        opened(bob.open(fromAlice, now + 1))
        opened(alice.open(fromBob, now + 1))
        for (i in 0 until 3) {
            opened(bob.open(alice.seal(chat("a$i"), now + 10L * i), now + 10L * i), "a$i")
            opened(alice.open(bob.seal(chat("b$i"), now + 10L * i + 5), now + 10L * i + 5), "b$i")
        }
    }

    fun testResetRecoversALostSession() {
        val (alice, bob) = pair()
        opened(bob.open(alice.seal(chat("a1"), now), now))
        opened(alice.open(bob.seal(chat("b1"), now + 1), now + 1))
        bob.importRatchets(mapOf("v" to 4L, "peers" to emptyMap<String, Any?>()))
        val broken = alice.seal(chat("a2"), now + 2)
        val lost = bob.open(broken, now + 3)
        expect(lost is SealOpen.NoSession && lost.kid == alice.keyId, "a lost session gives noSession")
        expect(!bob.openedSince(alice.keyId, ChatV3.integer(broken["ctr"])!!), "nothing newer opened yet")
        bob.resetSessions(alice.keyId)
        val reset = bob.seal(mapOf("type" to ChatV4.RESET_TYPE, "senderId" to "node-bob-phone"), now + 4, only = listOf(alice.keyId))
        val entry = (reset["keys"] as List<*>).single() as Map<*, *>
        assertEq(entry["sid"], ChatV4.sessionId(ChatV3.unb64u(entry["dh"])!!, ChatV3.unb64(entry["ct"])!!))
        assertEq(ChatV4.RESET_TYPE, opened(alice.open(reset, now + 5)).plain["type"])
        val again = alice.seal(chat("a2 again"), now + 6)
        opened(bob.open(again, now + 7))
        expect(bob.openedSince(alice.keyId, ChatV3.integer(broken["ctr"])!!), "a newer frame from alice opened")
    }

    fun testCommitFalseLeavesTheStateAlone() {
        val (alice, bob) = pair()
        val frame = alice.seal(chat("a1"), now)
        val before = Json.stringify(bob.exportRatchets())
        opened(bob.open(frame, now, commit = false))
        assertEq(before, Json.stringify(bob.exportRatchets()))
        opened(bob.open(frame, now))
        expect(bob.open(frame, now) is SealOpen.Replay)
        expect(bob.open(alice.seal(chat("a2"), now + 1), now + 1, commit = false) is SealOpen.Opened)
        val after = Json.stringify(bob.exportRatchets())
        opened(bob.open(alice.seal(chat("a3"), now + 2), now + 2))
        expect(after != Json.stringify(bob.exportRatchets()))
    }

    fun testTooManySkippedKeysNeedAReset() {
        val (alice, bob) = pair()
        repeat(ChatV4.MAX_SKIP + 1) { alice.seal(chat("lost"), now) }
        val far = alice.seal(chat("far"), now)
        expect(bob.open(far, now) is SealOpen.NoSession)
    }

    fun testSkippedKeysAreKeptUpToTheLimit() {
        val (alice, bob) = pair()
        val first = alice.seal(chat("first"), now)
        repeat(ChatV4.MAX_SKIP - 1) { alice.seal(chat("lost"), now) }
        opened(bob.open(alice.seal(chat("last"), now), now))
        val peers = bob.exportRatchets()["peers"] as Map<*, *>
        val session = ((peers[alice.keyId] as Map<*, *>)["sessions"] as List<*>).single() as Map<*, *>
        assertEq(ChatV4.MAX_SKIP, (session["skipped"] as List<*>).size)
        assertEq("first", opened(bob.open(first, now)).plain["text"])
    }

    fun testSkippedKeysAreWipedAfterEightDays() {
        val (alice, bob) = pair()
        alice.seal(chat("late"), now)
        opened(bob.open(alice.seal(chat("next"), now), now))
        assertEq(1, skippedAtBob(bob, alice))
        val later = now + ChatV4.SKIPPED_LIFE_MS + 1
        opened(bob.open(alice.seal(chat("much later"), now + 1), later))
        assertEq(0, skippedAtBob(bob, alice))
    }

    private fun skippedAtBob(bob: RoomSeal, alice: RoomSeal): Int {
        val peers = bob.exportRatchets()["peers"] as Map<*, *>
        val session = ((peers[alice.keyId] as Map<*, *>)["sessions"] as List<*>).single() as Map<*, *>
        return (session["skipped"] as List<*>).size
    }

    fun testExpiredPrekeyCannotStartASession() {
        val (alice, bob) = pair()
        val frame = alice.seal(chat("a1"), now)
        val prekeyId = ((frame["keys"] as List<*>).single() as Map<*, *>)["spk"] as String
        bob.keys.wipeExpired(bob.keys.prekey(prekeyId)!!.exp + 1)
        expect(bob.open(frame, now) is SealOpen.NoSession)
    }

    fun testOtherRoomTamperedHeaderAndSelfEntryAreInvalid() {
        val (alice, bob) = pair()
        val frame = alice.seal(chat("a1"), now)
        val elsewhere = RoomSeal.create("another-room", v.scope, SealPolicy.STRICT, "bob", bob.keys, bob.trust, clock = { now })
        expect(elsewhere.open(frame, now) is SealOpen.Invalid)
        val withSelf = copy(frame)
        val entries = (withSelf["keys"] as List<*>).map { it as Map<*, *> }.toMutableList()
        entries.add(LinkedHashMap(entries[0]).also { it["kid"] = alice.keyId })
        withSelf["keys"] = entries.sortedBy { it["kid"] as String }
        expect(bob.open(withSelf, now) is SealOpen.Invalid)
        val tampered = copy(frame)
        val first = LinkedHashMap((tampered["keys"] as List<*>)[0] as Map<*, *>)
        first["n"] = 7L
        tampered["keys"] = listOf(first)
        expect(bob.open(tampered, now) is SealOpen.Invalid)
        opened(bob.open(frame, now))
    }

    fun testOwnFramesZeroEntriesAndNoSelfEntry() {
        val (alice, bob) = pair()
        val frame = alice.seal(chat("a1"), now)
        assertEq(listOf(bob.keyId), RoomSeal.wrappedKeyIds(frame))
        expect(alice.open(frame, now) is SealOpen.Own)
        val empty = alice.seal(chat("nobody"), now, only = emptyList())
        assertEq(emptyList<String>(), RoomSeal.wrappedKeyIds(empty))
        expect(bob.open(empty, now) is SealOpen.NotForMe)
        assertEq(listOf(bob.keyId), alice.recipientKeyIds(now = now))
    }

    fun testSessionsAreBounded() {
        val (alice, bob) = pair()
        for (i in 0 until ChatV4.SESSIONS_KEPT + 3) {
            alice.resetSessions(bob.keyId)
            opened(bob.open(alice.seal(chat("$i"), now + i), now + i), "session $i")
        }
        val peers = bob.exportRatchets()["peers"] as Map<*, *>
        assertEq(ChatV4.SESSIONS_KEPT, ((peers[alice.keyId] as Map<*, *>)["sessions"] as List<*>).size)
    }

    fun testStrictRoomsIgnoreV3AndOpenRoomsIgnoreV4() {
        val (alice, bob) = pair()
        val openAlice = RoomSeal.create(room, "open-scope", SealPolicy.OPEN, "alice", alice.keys, clock = { now }, nodeKey = ChatNodeKey.generate())
        val openBob = RoomSeal.create(room, "open-scope", SealPolicy.OPEN, "bob", bob.keys, clock = { now }, nodeKey = ChatNodeKey.generate())
        openBob.acceptKeyFrame(openAlice.keyFrame(now), now)
        openAlice.acceptKeyFrame(openBob.keyFrame(now), now)
        val v3 = openAlice.seal(chat("v3"), now)
        assertEq(3L, v3["v"])
        opened(openBob.open(v3, now))
        expect(bob.open(v3, now) is SealOpen.Invalid, "v3 in a strict room")
        expect(!bob.accepts(v3))
        val v4 = alice.seal(chat("v4"), now)
        expect(openBob.open(v4, now) is SealOpen.Invalid, "v4 in an open room")
        expect(!openBob.accepts(v4))
        expect(bob.accepts(v4))
    }

    fun testTheRatchetIsStoredInTheSecureStoreAndReloads() {
        val store = MemorySecretStore()
        val keys = v.keys("bob")
        store.write(ChatDeviceKeys.IDENTITY_STORE_KEY, ChatV3.b64(ChatV3.unb64(v.obj("devices", "bob")["identitySeed"])!!))
        val loaded = ChatDeviceKeys.forStore(store)
        for (id in keys.prekeyIds()) {
            val prekey = keys.prekey(id)!!
            loaded.addPrekey(prekey.xPrivate, prekey.pqSeed, prekey.ts)
        }
        fun bobFrom(target: MemorySecretStore): RoomSeal {
            val seal = RoomSeal.loadOrCreate(room, v.scope, SealPolicy.STRICT, "bob", target, clock = { now }, node = "node-bob-phone")
            seal.participants = v.participants
            for (bundle in v.obj("bundles").values) {
                @Suppress("UNCHECKED_CAST")
                seal.acceptKeyFrame(bundle as Map<String, Any?>, now)
            }
            return seal
        }
        val alice = v.device("alice")
        val bob = bobFrom(store)
        opened(bob.open(alice.seal(chat("a1"), now), now))
        val stored = store.read(ChatV4.storeKey(room))
        expect(stored != null, "ratchet stored")
        assertEq("chat/e2ee/ratchetV4/${ChatV3.b64u(room.toByteArray())}", ChatV4.storeKey(room))
        val reply = bob.seal(chat("b1"), now + 1)
        assertEq(Json.parse(store.read(ChatV4.storeKey(room))!!), Json.parse(Json.stringify(bob.exportRatchets())), "stored before the frame leaves")
        opened(alice.open(reply, now + 2))
        val restarted = bobFrom(MemorySecretStore(store.snapshot()))
        opened(restarted.open(alice.seal(chat("a2"), now + 3), now + 3))
    }
}
