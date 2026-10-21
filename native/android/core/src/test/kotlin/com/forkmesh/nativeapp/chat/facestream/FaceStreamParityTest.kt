package com.forkmesh.nativeapp.chat.facestream

import com.forkmesh.nativeapp.chat.face.CallFacePoint
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.bytesOfHex
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.hexOf
import com.forkmesh.nativeapp.testing.resourceText

@Suppress("UNCHECKED_CAST")
class FaceStreamParityTest {
    private val fixture: Map<String, Any?> by lazy { Json.parseObject(resourceText("chat/face-stream-parity.json")) }
    private var worst = 0.0

    private fun obj(v: Any?) = v as Map<String, Any?>
    private fun list(v: Any?) = (v as List<Any?>?) ?: emptyList()
    private fun num(v: Any?) = (v as Number).toDouble()
    private fun int(v: Any?) = (v as Number).toInt()
    private fun nums(v: Any?) = list(v).map(::num)
    private fun points(v: Any?): List<CallFacePoint> {
        val flat = nums(v)
        return List(flat.size / 2) { CallFacePoint(flat[it * 2], flat[it * 2 + 1]) }
    }

    private fun close(a: Double, b: Double, label: String) {
        val e = Math.abs(a - b) / maxOf(1.0, Math.abs(b))
        worst = maxOf(worst, e)
        expect(e <= 1e-9, "$label: expected $b but was $a")
    }

    fun testTemplateMatchesTheGoReference() {
        val template = list(fixture["template"])
        assertEq(68, template.size)
        template.forEachIndexed { i, row -> expect(nums(row).toDoubleArray().contentEquals(FaceStream.TEMPLATE[i]), "template $i") }
    }

    fun testPacketsAndFramesMatchTheGoReference() {
        var packets = 0
        for (stream in list(fixture["streams"]).map(::obj)) {
            val c = obj(stream["config"])
            val encoder = FaceStreamEncoder(FaceStreamConfig(int(c["width"]), int(c["height"]), int(c["fps"]), int(c["precision"]), int(c["keyInterval"])))
            val decoder = FaceStreamDecoder()
            val requests = list(stream["requestKeyAt"]).map(::int).toSet()
            val drops = list(stream["drop"]).map(::int).toSet()
            val from = int(stream["decodeFrom"])
            val decoded = list(stream["decoded"]).map(::obj).iterator()
            val expected = list(stream["packets"]).map { it as String }
            list(stream["inputs"]).map(::obj).forEachIndexed { i, raw ->
                if (i in requests) encoder.requestKey()
                val pose = nums(raw["pose"])
                val input = FaceStreamInput(num(raw["time"]).toLong(), points(raw["points"]),
                    FaceStreamPose(doubleArrayOf(pose[0], pose[1], pose[2], pose[3]), pose[4], pose[5], pose[6]),
                    FaceStreamExpression.of(nums(raw["expression"]).toDoubleArray()))
                assertEq(expected[i], hexOf(encoder.encode(input)), "${stream["name"]} packet $i")
                packets += 1
                if (i < from || i in drops) return@forEachIndexed
                val want = decoded.next()
                try {
                    val got = decoder.decode(bytesOfHex(expected[i]))
                    expect(want["error"] == null, "frame $i should fail with ${want["error"]}")
                    val frame = obj(want["frame"])
                    assertEq(frame["key"], got.key, "key $i")
                    assertEq(frame["stale"], got.stale, "stale $i")
                    assertEq(int(frame["seq"]), got.seq, "seq $i")
                    assertEq(int(frame["keyId"]), got.keyId, "keyId $i")
                    assertEq(num(frame["time"]).toLong(), got.time, "time $i")
                    assertEq(int(frame["precision"]), got.precision, "precision $i")
                    got.pose.values.forEachIndexed { k, v -> close(v, nums(frame["pose"])[k], "pose $i.$k") }
                    got.expression.values.forEachIndexed { k, v -> close(v, nums(frame["expression"])[k], "expression $i.$k") }
                    val flat = nums(frame["points"])
                    got.points.forEachIndexed { k, p ->
                        close(p.x, flat[k * 2], "x $i.$k")
                        close(p.y, flat[k * 2 + 1], "y $i.$k")
                    }
                } catch (error: FaceStreamException) {
                    assertEq(want["error"], error.code, "frame $i")
                }
            }
        }
        println("FaceStream parity: $packets packets byte-identical, worst decoded difference $worst")
    }

    fun testReferencesMatchTheGoReference() {
        for (entry in list(fixture["references"]).map(::obj)) {
            val input = obj(entry["input"])
            val crop = list(input["crop"]).map(::int)
            val maskText = input["mask"] as String
            val reference = FaceStreamReference(int(input["id"]), int(input["format"]), num(input["time"]).toLong(), crop[0], crop[1], crop[2], crop[3],
                int(input["width"]), int(input["height"]), points(input["points"]), int(input["maskW"]), int(input["maskH"]),
                if (maskText.isEmpty()) null else ByteArray(maskText.length) { if (maskText[it] == '1') 1 else 0 }, bytesOfHex(input["image"] as String))
            assertEq(entry["packet"], hexOf(reference.encoded()), "reference packet")
            val got = FaceStreamReference.decode(bytesOfHex(entry["packet"] as String))
            val want = obj(entry["decoded"])
            assertEq(want["mask"], got.mask?.joinToString("") { it.toString() } ?: "", "mask")
            nums(want["points"]).forEachIndexed { k, v -> close(if (k % 2 == 0) got.points[k / 2].x else got.points[k / 2].y, v, "reference point $k") }
            assertEq(want["image"], hexOf(got.image), "image")
            var refused = false
            try {
                FaceStreamDecoder().decode(bytesOfHex(entry["packet"] as String))
            } catch (error: FaceStreamException) {
                refused = error.code == "not-a-frame" || error.code == "long"
            }
            expect(refused, "frame decoder must refuse a reference")
        }
    }

    fun testRandomBytesNeverCrash() {
        var state = 0x2390
        fun next(): Byte {
            state = state xor (state shl 13)
            state = state xor (state ushr 17)
            state = state xor (state shl 5)
            return state.toByte()
        }
        val key = bytesOfHex(list(obj(list(fixture["streams"])[0])["packets"])[0] as String)
        for (round in 0 until 400) {
            val decoder = FaceStreamDecoder()
            decoder.decode(key)
            val packet = ByteArray(2 + round % 90) { next() }
            if (round % 3 == 0) packet[0] = ((packet[0].toInt() and 0x4F) or 0x10).toByte()
            try {
                val frame = decoder.decode(packet)
                expect(frame.points.all { it.x.isFinite() && it.y.isFinite() }, "finite points")
            } catch (_: FaceStreamException) {
            }
            try {
                FaceStreamReference.decode(packet + ByteArray(round * 3) { next() })
            } catch (_: FaceStreamException) {
            }
        }
    }
}
