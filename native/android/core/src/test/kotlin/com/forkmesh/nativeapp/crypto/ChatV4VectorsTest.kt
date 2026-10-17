package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.resourceText
import java.io.File

internal object ChatV4Vectors {
    val vectors: Map<String, Any?> by lazy { Json.parseObject(load()) }
    val room: String get() = vectors["room"] as String
    val scope: String get() = vectors["scope"] as String
    val now: Long get() = vectors["now"] as Long
    val participants: List<String> get() = (vectors["participants"] as List<*>).map { it as String }

    private fun load(): String {
        val root = runCatching { CryptoInterop.repoRoot }.getOrNull()
        val canonical = root?.let { File(it, "conformance/chat/v4/vectors.json") }
        if (canonical != null && canonical.isFile) return canonical.readText(Charsets.UTF_8)
        return resourceText("chat/v4/vectors.json")
    }

    @Suppress("UNCHECKED_CAST")
    fun obj(vararg path: String): Map<String, Any?> {
        var current: Map<String, Any?> = vectors
        for (key in path) current = current[key] as Map<String, Any?>
        return current
    }

    fun kid(name: String): String = obj("devices", name)["kid"] as String

    fun keys(name: String): ChatDeviceKeys {
        val secrets = obj("devices", name)
        val keys = ChatDeviceKeys.fromSeed(ChatV3.unb64(secrets["identitySeed"])!!)
        assertEq(secrets["kid"], keys.kid, "$name kid")
        for (value in secrets["prekeys"] as List<*>) {
            val spk = value as Map<*, *>
            val made = keys.addPrekey(ChatV3.unb64(spk["xPriv"])!!, ChatV3.unb64(spk["pqSeed"])!!, spk["ts"] as Long)
            assertEq(spk["id"], made.id, "$name prekey id")
        }
        return keys
    }

    fun device(name: String, state: String? = null): RoomSeal {
        val node = obj("nodes")[name] as String
        val seal = RoomSeal.create(room, scope, SealPolicy.STRICT, name, keys(name), clock = { now }, node = node)
        seal.participants = participants
        for (bundle in obj("bundles").values) {
            @Suppress("UNCHECKED_CAST")
            assertEq(TrustState.APPROVED, seal.acceptKeyFrame(bundle as Map<String, Any?>, now)?.state, "$name accepts bundle")
        }
        if (state != null) seal.importRatchets(obj("states", state))
        return seal
    }

    fun frame(name: String): Map<String, Any?> = obj("frames", name)
}

class ChatV4VectorsTest {
    private val v = ChatV4Vectors

    private fun b(text: Any?): ByteArray = ChatV3.unb64(text)!!

    fun testKdfVectors() {
        val k = v.obj("kdf")
        @Suppress("UNCHECKED_CAST")
        val s = k["sessionId"] as Map<String, Any?>
        assertEq(s["sid"], ChatV4.sessionId(ChatV3.unb64u(s["dh"])!!, b(s["ct"])))
        @Suppress("UNCHECKED_CAST")
        val i = k["init"] as Map<String, Any?>
        val init = ChatV4.kdfInit(
            i["room"] as String, i["sender"] as String, i["recipient"] as String, i["spk"] as String, i["sid"] as String,
            b(i["ss1"]), b(i["ss2"]),
        )
        assertEq(i["rk"], ChatV3.b64(init.first), "init rk")
        assertEq(i["ck"], ChatV3.b64(init.second), "init ck")
        @Suppress("UNCHECKED_CAST")
        val r = k["rootStep"] as Map<String, Any?>
        val step = ChatV4.kdfRk(b(r["rk"]), b(r["dhOut"]), b(r["ss"]))
        assertEq(r["nextRk"], ChatV3.b64(step.first), "root step rk")
        assertEq(r["ck"], ChatV3.b64(step.second), "root step ck")
        @Suppress("UNCHECKED_CAST")
        val c = k["chainStep"] as Map<String, Any?>
        val chain = ChatV4.kdfCk(b(c["ck"]))
        assertEq(c["mk"], ChatV3.b64(chain.first), "chain mk")
        assertEq(c["nextCk"], ChatV3.b64(chain.second), "chain ck")
    }

    fun testVectorSequence() {
        val devices = HashMap<String, RoomSeal>()
        for (value in v.vectors["sequence"] as List<*>) {
            val step = value as Map<*, *>
            val name = step["device"] as String
            if (step.containsKey("state")) devices[name] = v.device(name, step["state"] as String?)
            val dev = devices.getValue(name)
            if (step["clearSeen"] == true) dev.clearSeen()
            val result = dev.open(v.frame(step["frame"] as String), step["at"] as Long)
            val label = "${step["device"]} ${step["frame"]}"
            val kind = when (result) {
                is SealOpen.Opened -> "opened"
                is SealOpen.Held -> "held"
                is SealOpen.Own -> "own"
                is SealOpen.NotForMe -> "notForMe"
                is SealOpen.NoSession -> "noSession"
                is SealOpen.Replay -> "replay"
                is SealOpen.Invalid -> "invalid"
            }
            assertEq(step["expect"], kind, label)
            if (result is SealOpen.Opened) {
                assertEq(v.obj("plain")[step["plain"] as String], result.plain, "$label plain")
                assertEq(v.kid(if (name == "alice") "bob" else "alice"), result.senderKid, "$label sender")
            }
            if (result is SealOpen.NoSession) assertEq(v.kid("alice"), result.kid, "$label kid")
            if (step["frame"] == "m1" && result is SealOpen.Opened) {
                val peers = dev.exportRatchets()["peers"] as Map<*, *>
                val session = ((peers[v.kid("alice")] as Map<*, *>)["sessions"] as List<*>)[0] as Map<*, *>
                val want = v.obj("bobAfterM1")
                assertEq(want["rk"], session["rk"], "bobAfterM1 rk")
                assertEq(want["ckr"], session["ckr"], "bobAfterM1 ckr")
                assertEq(want["nr"], session["nr"], "bobAfterM1 nr")
                assertEq(want["skipped"], (session["skipped"] as List<*>).size.toLong(), "bobAfterM1 skipped")
            }
        }
    }

    fun testStoredStatesRoundTripInTheCanonicalFormat() {
        for (name in listOf("aliceAfterM2", "bobAfterM1", "bobAfterR1")) {
            val device = v.device(if (name.startsWith("alice")) "alice" else "bob", name)
            assertEq(v.obj("states", name), device.exportRatchets(), name)
        }
    }

    fun testHeadersFollowTheRatchet() {
        val h = v.obj("headers")
        fun header(name: String, key: String): Any? = (h[name] as Map<*, *>)[key]
        expect(header("m1", "sid") == header("m3", "sid") && header("m1", "sid") == header("r1", "sid"))
        assertEq(listOf(0L, 1L, 0L, 1L), listOf("m1", "m2", "m3", "m4").map { header(it, "n") })
        assertEq(2L, header("m3", "pn"))
        assertEq(emptyList<Any?>(), v.frame("empty")["keys"])
        assertEq(listOf(v.kid("bob")), RoomSeal.wrappedKeyIds(v.frame("m1")))
        expect(RoomSeal.isRatchetFrame(v.frame("empty")))
        expect(!RoomSeal.isSealedFrame(v.frame("m1")))
        expect(!RoomSeal.isLegacyFrame(v.frame("m1")))
    }

    fun testKotlinFramesHaveTheVectorShape() {
        val alice = v.device("alice")
        val frame = alice.seal(mapOf("type" to "chat", "id" to "k1", "text" to "hi"), v.now)
        assertEq(v.frame("m1").keys, frame.keys)
        val entry = (frame["keys"] as List<*>).single() as Map<*, *>
        assertEq((((v.frame("m1")["keys"] as List<*>)[0]) as Map<*, *>).keys, entry.keys)
        assertEq(v.kid("bob"), entry["kid"])
        assertEq(entry["sid"], ChatV4.sessionId(ChatV3.unb64u(entry["dh"])!!, ChatV3.unb64(entry["ct"])!!))
        val stored = alice.exportRatchets()
        val session = (((stored["peers"] as Map<*, *>)[v.kid("bob")] as Map<*, *>)["sessions"] as List<*>).single() as Map<*, *>
        assertEq(listOf("sid", "rk", "dhs", "dhr", "cks", "ns", "pn", "sq", "ckr", "nr", "skipped", "created", "used"), session.keys.toList())
    }
}
