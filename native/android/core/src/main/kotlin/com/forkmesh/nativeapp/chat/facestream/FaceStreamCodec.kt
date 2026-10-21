package com.forkmesh.nativeapp.chat.facestream

import com.forkmesh.nativeapp.chat.face.CallFacePoint

class FaceStreamException(val code: String) : Exception("face stream: $code")

data class FaceStreamPose(val rotation: DoubleArray, val x: Double, val y: Double, val scale: Double) {
    val values: DoubleArray get() = doubleArrayOf(rotation[0], rotation[1], rotation[2], rotation[3], x, y, scale)

    override fun equals(other: Any?): Boolean = other is FaceStreamPose && values.contentEquals(other.values)

    override fun hashCode(): Int = values.contentHashCode()
}

data class FaceStreamExpression(
    val lidLeft: Double = 0.0,
    val lidRight: Double = 0.0,
    val gazeX: Double = 0.0,
    val gazeY: Double = 0.0,
    val mouth: Double = 0.0,
    val smile: Double = 0.0,
    val tongue: Double = 0.0,
) {
    val values: DoubleArray get() = doubleArrayOf(lidLeft, lidRight, gazeX, gazeY, mouth, smile, tongue)

    companion object {
        fun of(v: DoubleArray) = FaceStreamExpression(v[0], v[1], v[2], v[3], v[4], v[5], v[6])
    }
}

data class FaceStreamInput(val time: Long, val points: List<CallFacePoint>, val pose: FaceStreamPose, val expression: FaceStreamExpression)

data class FaceStreamFrame(
    val key: Boolean,
    val stale: Boolean,
    val seq: Int,
    val keyId: Int,
    val time: Long,
    val width: Int,
    val height: Int,
    val precision: Int,
    val pose: FaceStreamPose,
    val expression: FaceStreamExpression,
    val points: List<CallFacePoint>,
)

data class FaceStreamConfig(val width: Int, val height: Int, val fps: Int = 0, val precision: Int = 0, val keyInterval: Int = 0) {
    internal fun normalized(strict: Boolean = false): FaceStreamConfig? {
        if (strict && (fps == 0 || precision == 0)) return null
        val c = FaceStreamConfig(
            width, height,
            if (fps == 0) FaceStream.DEFAULT_FPS else fps,
            if (precision == 0) FaceStream.DEFAULT_PRECISION else precision,
            if (keyInterval == 0) FaceStream.DEFAULT_KEY_INTERVAL else keyInterval,
        )
        val ok = c.width in 1..FaceStream.MAX_SIDE && c.height in 1..FaceStream.MAX_SIDE && c.fps in 1..120 &&
            c.precision in FaceStream.MIN_PRECISION..FaceStream.MAX_PRECISION && c.keyInterval in 1..255
        return if (ok) c else null
    }
}

object FaceStream {
    const val VERSION = 1
    const val KIND_DELTA = 0
    const val KIND_KEY = 1
    const val KIND_REFERENCE = 2
    const val DEFAULT_PRECISION = 96
    const val DEFAULT_KEY_INTERVAL = 30
    const val DEFAULT_FPS = 30
    const val MAX_FRAME_PACKET = 1024
    const val MIN_PRECISION = 16
    const val MAX_PRECISION = 255
    const val MAX_SIDE = 4096
    const val LANDMARK_COUNT = 68
    const val LABEL = "Face stream"
    const val DETAIL = "Animated from a recent photo while the connection is weak"
    const val STALE_DETAIL = "Face stream: catching up"
    internal const val KEY_HEADER = 12
    internal const val QUAT_STEPS = 512.0
    internal const val TRANS_STEPS = 2.0
    internal const val SCALE_STEPS = 8.0
    internal const val MAX_TIME_JUMP = 4000

    val TEMPLATE: List<DoubleArray> = listOf(
        doubleArrayOf(-1.0496, 0.0765, -0.95), doubleArrayOf(-1.0473, 0.3567, -0.9), doubleArrayOf(-1.032, 0.6366, -0.8), doubleArrayOf(-0.9939, 0.914, -0.65),
        doubleArrayOf(-0.9103, 1.1805, -0.45), doubleArrayOf(-0.7622, 1.4155, -0.25), doubleArrayOf(-0.5366, 1.5709, -0.08), doubleArrayOf(-0.2751, 1.6607, 0.05),
        doubleArrayOf(0.0, 1.7019, 0.1), doubleArrayOf(0.2751, 1.6607, 0.05), doubleArrayOf(0.5366, 1.5709, -0.08), doubleArrayOf(0.7622, 1.4155, -0.25),
        doubleArrayOf(0.9103, 1.1805, -0.45), doubleArrayOf(0.9939, 0.914, -0.65), doubleArrayOf(1.032, 0.6366, -0.8), doubleArrayOf(1.0473, 0.3567, -0.9),
        doubleArrayOf(1.0496, 0.0765, -0.95), doubleArrayOf(-0.8602, -0.2609, -0.25), doubleArrayOf(-0.7013, -0.3102, -0.08), doubleArrayOf(-0.5424, -0.3296, 0.02),
        doubleArrayOf(-0.3836, -0.3117, 0.05), doubleArrayOf(-0.2248, -0.3094, 0.05), doubleArrayOf(0.2248, -0.3094, 0.05), doubleArrayOf(0.3836, -0.3117, 0.05),
        doubleArrayOf(0.5424, -0.3296, 0.02), doubleArrayOf(0.7013, -0.3102, -0.08), doubleArrayOf(0.8602, -0.2609, -0.25), doubleArrayOf(0.0, -0.0138, 0.0),
        doubleArrayOf(0.0, 0.1764, 0.12), doubleArrayOf(0.0, 0.365, 0.25), doubleArrayOf(0.0, 0.554, 0.38), doubleArrayOf(-0.2372, 0.6484, 0.12),
        doubleArrayOf(-0.1297, 0.6943, 0.2), doubleArrayOf(0.0, 0.7127, 0.25), doubleArrayOf(0.1297, 0.6943, 0.2), doubleArrayOf(0.2372, 0.6484, 0.12),
        doubleArrayOf(-0.7439, -0.0089, -0.12), doubleArrayOf(-0.5813, -0.07, -0.04), doubleArrayOf(-0.4077, -0.0609, -0.04), doubleArrayOf(-0.2572, 0.0273, -0.03),
        doubleArrayOf(-0.4211, 0.0591, -0.04), doubleArrayOf(-0.5888, 0.0535, -0.04), doubleArrayOf(0.2572, 0.0273, -0.03), doubleArrayOf(0.4077, -0.0609, -0.04),
        doubleArrayOf(0.5813, -0.07, -0.04), doubleArrayOf(0.7439, -0.0089, -0.12), doubleArrayOf(0.5888, 0.0535, -0.04), doubleArrayOf(0.4211, 0.0591, -0.04),
        doubleArrayOf(-0.4809, 0.9814, 0.02), doubleArrayOf(-0.3228, 0.9409, 0.12), doubleArrayOf(-0.1623, 0.9114, 0.17), doubleArrayOf(0.0, 0.9221, 0.19),
        doubleArrayOf(0.1623, 0.9114, 0.17), doubleArrayOf(0.3228, 0.9409, 0.12), doubleArrayOf(0.4809, 0.9814, 0.02), doubleArrayOf(0.3384, 1.0858, 0.12),
        doubleArrayOf(0.1765, 1.1543, 0.15), doubleArrayOf(0.0, 1.1789, 0.16), doubleArrayOf(-0.1765, 1.1543, 0.15), doubleArrayOf(-0.3384, 1.0858, 0.12),
        doubleArrayOf(-0.4495, 0.9876, 0.04), doubleArrayOf(-0.2256, 1.021, 0.13), doubleArrayOf(0.0, 1.0353, 0.14), doubleArrayOf(0.2256, 1.021, 0.13),
        doubleArrayOf(0.4495, 0.9876, 0.04), doubleArrayOf(0.2261, 1.021, 0.13), doubleArrayOf(0.0, 1.0353, 0.14), doubleArrayOf(-0.2261, 1.021, 0.13),
    )

    internal class Channel(val steps: Double, val low: Int, val high: Int, val signed: Boolean, val prior: Int)

    internal val CHANNELS = listOf(
        Channel(16.0, 0, 16, false, 3600),
        Channel(16.0, 0, 16, false, 3600),
        Channel(64.0, -32, 32, true, FaceStreamBits.PROB_HALF),
        Channel(64.0, -32, 32, true, FaceStreamBits.PROB_HALF),
        Channel(16.0, 0, 16, false, FaceStreamBits.PROB_HALF),
        Channel(16.0, 0, 16, false, 2800),
        Channel(16.0, 0, 16, false, 3900),
    )

    internal fun floorHalf(value: Double): Int {
        val f = Math.floor(value + 0.5)
        if (f > 1e9) return 1_000_000_000
        if (f < -1e9) return -1_000_000_000
        return f.toInt()
    }

    internal fun group(i: Int): Int = when {
        i <= 16 -> 0
        i <= 26 -> 1
        i <= 35 -> 2
        i <= 47 -> 3
        i <= 59 -> 4
        else -> 5
    }

    fun rotationRows(q: DoubleArray): Array<DoubleArray> {
        val w = q[0]
        val x = q[1]
        val y = q[2]
        val z = q[3]
        return arrayOf(
            doubleArrayOf(w * w + x * x - y * y - z * z, 2 * (x * y - w * z), 2 * (x * z + w * y)),
            doubleArrayOf(2 * (x * y + w * z), w * w - x * x + y * y - z * z, 2 * (y * z - w * x)),
            doubleArrayOf(2 * (x * z - w * y), 2 * (y * z + w * x), w * w - x * x - y * y + z * z),
        )
    }

    internal fun dot3(r: DoubleArray, v: DoubleArray): Double = r[0] * v[0] + r[1] * v[1] + r[2] * v[2]

    fun project(pose: FaceStreamPose): List<CallFacePoint> {
        val r = rotationRows(pose.rotation)
        return TEMPLATE.map { m -> CallFacePoint(pose.x + pose.scale * dot3(r[0], m), pose.y + pose.scale * dot3(r[1], m)) }
    }

    internal fun nominal(fps: Int): Int = (1000 + fps / 2) / fps

    internal fun header(kind: Int, id: Int): Byte = ((kind shl 6) or (VERSION shl 4) or (id and 15)).toByte()

    fun packetKind(packet: ByteArray): Int = if (packet.isEmpty()) -1 else (packet[0].toInt() and 0xFF) shr 6
}

internal data class FaceStreamQPose(val qx: Int, val qy: Int, val qz: Int, val tx: Int, val ty: Int, val s: Int) {
    val values: IntArray get() = intArrayOf(qx, qy, qz, tx, ty, s)

    val pose: FaceStreamPose
        get() {
            val x = qx / FaceStream.QUAT_STEPS
            val y = qy / FaceStream.QUAT_STEPS
            val z = qz / FaceStream.QUAT_STEPS
            val ss = x * x + y * y + z * z
            val w = if (ss < 1) Math.sqrt(1 - ss) else 0.0
            val n = Math.sqrt(w * w + x * x + y * y + z * z)
            return FaceStreamPose(doubleArrayOf(w / n, x / n, y / n, z / n), tx / FaceStream.TRANS_STEPS, ty / FaceStream.TRANS_STEPS, s / FaceStream.SCALE_STEPS)
        }

    companion object {
        fun of(p: FaceStreamPose): FaceStreamQPose {
            val q = p.rotation
            val n = Math.sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3])
            var x = 0.0
            var y = 0.0
            var z = 0.0
            if (n > 0) {
                x = q[1] / n
                y = q[2] / n
                z = q[3] / n
                if (q[0] < 0) {
                    x = -x
                    y = -y
                    z = -z
                }
            }
            return FaceStreamQPose(
                clampInt(FaceStream.floorHalf(x * FaceStream.QUAT_STEPS), -512, 512),
                clampInt(FaceStream.floorHalf(y * FaceStream.QUAT_STEPS), -512, 512),
                clampInt(FaceStream.floorHalf(z * FaceStream.QUAT_STEPS), -512, 512),
                clampInt(FaceStream.floorHalf(p.x * FaceStream.TRANS_STEPS), -32768, 32767),
                clampInt(FaceStream.floorHalf(p.y * FaceStream.TRANS_STEPS), -32768, 32767),
                clampInt(FaceStream.floorHalf(p.scale * FaceStream.SCALE_STEPS), 1, 32767),
            )
        }
    }
}

private fun quantize(e: FaceStreamExpression): IntArray {
    val v = e.values
    return IntArray(v.size) { clampInt(FaceStream.floorHalf(v[it] * FaceStream.CHANNELS[it].steps), FaceStream.CHANNELS[it].low, FaceStream.CHANNELS[it].high) }
}

private fun expressionOf(q: IntArray) = FaceStreamExpression.of(DoubleArray(q.size) { q[it] / FaceStream.CHANNELS[it].steps })

private fun models(count: Int) = Array(count) { FaceStreamIntModel() }

private fun zeros() = Array(FaceStream.LANDMARK_COUNT) { DoubleArray(2) }

class FaceStreamEncoder(config: FaceStreamConfig) {
    val config: FaceStreamConfig = config.normalized() ?: throw FaceStreamException("config")
    private var keyId = 15
    private var keyPose = FaceStreamQPose(0, 0, 0, 0, 0, 0)
    private var seq = 255
    private var time = 0L
    private var chain = models(12)
    private var recon = zeros()
    private var projected: List<CallFacePoint> = List(FaceStream.LANDMARK_COUNT) { CallFacePoint(0.0, 0.0) }
    private var started = false
    private var sinceKey = 0
    private var forceKey = false
    var last: FaceStreamFrame? = null
        private set

    fun requestKey() {
        forceKey = true
    }

    private fun valid(input: FaceStreamInput): Boolean {
        if (input.points.size < FaceStream.LANDMARK_COUNT || input.pose.rotation.size != 4 || input.time < 0 || input.time > 0xFFFFFFFFL) return false
        if (input.points.take(FaceStream.LANDMARK_COUNT).any { !it.x.isFinite() || !it.y.isFinite() }) return false
        return input.pose.values.all { it.isFinite() } && input.expression.values.all { it.isFinite() }
    }

    fun encode(input: FaceStreamInput): ByteArray {
        if (!valid(input)) throw FaceStreamException("input")
        val nominal = FaceStream.nominal(config.fps)
        val dt = ((input.time - time) and 0xFFFFFFFFL).toInt()
        val key = !started || forceKey || sinceKey >= config.keyInterval || dt - nominal > FaceStream.MAX_TIME_JUMP || dt - nominal < -FaceStream.MAX_TIME_JUMP
        val qp = FaceStreamQPose.of(input.pose)
        val pose = qp.pose
        val now = FaceStream.project(pose)
        val nextSeq = (seq + 1) and 255
        val id = if (key) (keyId + 1) and 15 else keyId
        val prefix = ArrayList<Byte>()
        prefix.add(FaceStream.header(if (key) FaceStream.KIND_KEY else FaceStream.KIND_DELTA, id))
        prefix.add(nextSeq.toByte())
        if (key) {
            for (v in intArrayOf(config.width shr 8, config.width, config.height shr 8, config.height, config.fps, config.precision)) prefix.add(v.toByte())
            for (shift in intArrayOf(24, 16, 8, 0)) prefix.add((input.time ushr shift).toByte())
        }
        val enc = FaceStreamRangeEncoder(prefix)
        if (key) {
            enc.direct((qp.qx + 512).toLong(), 11)
            enc.direct((qp.qy + 512).toLong(), 11)
            enc.direct((qp.qz + 512).toLong(), 11)
            enc.direct((qp.tx + 32768).toLong(), 16)
            enc.direct((qp.ty + 32768).toLong(), 16)
            enc.direct(qp.s.toLong(), 15)
        } else {
            enc.int(FaceStreamIntModel(), 0, dt - nominal)
            val base = keyPose.values
            val poseModels = models(6)
            qp.values.forEachIndexed { i, v -> enc.int(poseModels[i], 0, v - base[i]) }
        }
        val qx = quantize(input.expression)
        FaceStream.CHANNELS.forEachIndexed { i, c ->
            val model = FaceStreamIntModel(c.prior)
            if (c.signed) enc.int(model, 0, qx[i]) else enc.uint(model, 0, qx[i])
        }
        if (key) chain = models(12)
        val landmarks = if (key) models(12) else chain
        val step = pose.scale / config.precision
        val next = zeros()
        val nonzero = IntArray(12)
        for (i in 0 until FaceStream.LANDMARK_COUNT) {
            for (axis in 0..1) {
                val current = if (axis == 0) now[i].x else now[i].y
                val before = if (axis == 0) projected[i].x else projected[i].y
                val pred = if (key) current else recon[i][axis] + (current - before)
                val value = if (axis == 0) input.points[i].x else input.points[i].y
                val r = clampInt(FaceStream.floorHalf((value - pred) / step), -FaceStreamBits.MAX_INT, FaceStreamBits.MAX_INT)
                val cls = FaceStream.group(i) * 2 + axis
                enc.int(landmarks[cls], nonzero[cls], r)
                nonzero[cls] = if (r != 0) 1 else 0
                next[i][axis] = pred + r * step
            }
        }
        val packet = enc.finish()
        if (key) {
            keyId = id
            keyPose = qp
            sinceKey = 0
            forceKey = false
        }
        started = true
        sinceKey += 1
        seq = nextSeq
        time = input.time
        recon = next
        projected = now
        last = FaceStreamFrame(key, false, nextSeq, id, input.time, config.width, config.height, config.precision, pose, expressionOf(qx),
            next.map { CallFacePoint(it[0], it[1]) })
        return packet
    }
}

class FaceStreamDecoder {
    private var config = FaceStreamConfig(1, 1)
    private var keyId = 0
    private var keyPose = FaceStreamQPose(0, 0, 0, 0, 0, 0)
    private var seq = 0
    private var time = 0L
    private var chain = models(12)
    private var recon = zeros()
    private var projected: List<CallFacePoint> = List(FaceStream.LANDMARK_COUNT) { CallFacePoint(0.0, 0.0) }
    private var haveKey = false
    private var chainOK = false
    private var lastGood = zeros()
    private var goodProjected: List<CallFacePoint> = projected

    val ready: Boolean get() = haveKey

    fun reset() {
        haveKey = false
        chainOK = false
    }

    private fun u8(packet: ByteArray, at: Int) = packet[at].toInt() and 0xFF

    fun decode(packet: ByteArray): FaceStreamFrame {
        if (packet.size < 2) throw FaceStreamException("short")
        if (packet.size > FaceStream.MAX_FRAME_PACKET) throw FaceStreamException("long")
        if ((u8(packet, 0) shr 4) and 3 != FaceStream.VERSION) throw FaceStreamException("version")
        val kind = u8(packet, 0) shr 6
        if (kind != FaceStream.KIND_KEY && kind != FaceStream.KIND_DELTA) throw FaceStreamException(if (kind == FaceStream.KIND_REFERENCE) "not-a-frame" else "kind")
        val id = u8(packet, 0) and 15
        val frameSeq = u8(packet, 1)
        val key = kind == FaceStream.KIND_KEY
        var cfg = config
        var t = 0L
        val start: Int
        if (key) {
            if (packet.size < FaceStream.KEY_HEADER) throw FaceStreamException("short")
            cfg = FaceStreamConfig((u8(packet, 2) shl 8) or u8(packet, 3), (u8(packet, 4) shl 8) or u8(packet, 5), u8(packet, 6), u8(packet, 7), 1)
                .normalized(strict = true) ?: throw FaceStreamException("header")
            t = (u8(packet, 8).toLong() shl 24) or (u8(packet, 9).toLong() shl 16) or (u8(packet, 10).toLong() shl 8) or u8(packet, 11).toLong()
            start = FaceStream.KEY_HEADER
        } else {
            if (!haveKey || id != keyId) throw FaceStreamException("need-key")
            val ahead = (frameSeq - seq) and 255
            if (ahead == 0 || ahead > 128) throw FaceStreamException("old")
            start = 2
        }
        val dec = FaceStreamRangeDecoder(packet, start)
        val inSeq = key || (chainOK && frameSeq == ((seq + 1) and 255))
        val qp: FaceStreamQPose
        if (key) {
            qp = FaceStreamQPose(
                dec.direct(11).toInt() - 512, dec.direct(11).toInt() - 512, dec.direct(11).toInt() - 512,
                dec.direct(16).toInt() - 32768, dec.direct(16).toInt() - 32768, dec.direct(15).toInt(),
            )
            if (qp.qx > 512 || qp.qy > 512 || qp.qz > 512 || qp.s < 1) throw FaceStreamException("corrupt")
        } else {
            val nominal = FaceStream.nominal(cfg.fps)
            val dt = dec.int(FaceStreamIntModel(), 0) + nominal
            val gap = (frameSeq - seq) and 255
            t = (time + ((gap - 1) * nominal + dt).toLong()) and 0xFFFFFFFFL
            val poseModels = models(6)
            val base = keyPose.values
            val v = IntArray(6) { base[it] + dec.int(poseModels[it], 0) }
            qp = FaceStreamQPose(v[0], v[1], v[2], v[3], v[4], v[5])
            if (qp.qx !in -512..512 || qp.qy !in -512..512 || qp.qz !in -512..512 || qp.tx !in -32768..32767 || qp.ty !in -32768..32767 || qp.s !in 1..32767) {
                throw FaceStreamException("corrupt")
            }
        }
        val qx = IntArray(FaceStream.CHANNELS.size)
        FaceStream.CHANNELS.forEachIndexed { i, c ->
            val model = FaceStreamIntModel(c.prior)
            val value = if (c.signed) dec.int(model, 0) else dec.uint(model, 0)
            if (value < c.low || value > c.high) throw FaceStreamException("corrupt")
            qx[i] = value
        }
        val pose = qp.pose
        val now = FaceStream.project(pose)
        val points: List<CallFacePoint>
        var stale = false
        if (inSeq) {
            val landmarks = if (key) models(12) else Array(12) { chain[it].copy() }
            val step = pose.scale / cfg.precision
            val nonzero = IntArray(12)
            val next = zeros()
            for (i in 0 until FaceStream.LANDMARK_COUNT) {
                for (axis in 0..1) {
                    val current = if (axis == 0) now[i].x else now[i].y
                    val before = if (axis == 0) projected[i].x else projected[i].y
                    val pred = if (key) current else recon[i][axis] + (current - before)
                    val cls = FaceStream.group(i) * 2 + axis
                    val r = dec.int(landmarks[cls], nonzero[cls])
                    nonzero[cls] = if (r != 0) 1 else 0
                    next[i][axis] = pred + r * step
                }
            }
            if (dec.overrun) throw FaceStreamException("corrupt")
            chain = if (key) models(12) else landmarks
            recon = next
            projected = now
            lastGood = next
            goodProjected = now
            chainOK = true
            points = next.map { CallFacePoint(it[0], it[1]) }
        } else {
            if (dec.overrun) throw FaceStreamException("corrupt")
            chainOK = false
            stale = true
            points = List(FaceStream.LANDMARK_COUNT) { i ->
                CallFacePoint(lastGood[i][0] + (now[i].x - goodProjected[i].x), lastGood[i][1] + (now[i].y - goodProjected[i].y))
            }
        }
        if (key) {
            haveKey = true
            config = cfg
            keyId = id
            keyPose = qp
        }
        seq = frameSeq
        time = t
        return FaceStreamFrame(key, stale, frameSeq, id, t, cfg.width, cfg.height, cfg.precision, pose, expressionOf(qx), points)
    }
}

class FaceStreamReference(
    val id: Int,
    val format: Int,
    val time: Long,
    val cropX: Int,
    val cropY: Int,
    val cropW: Int,
    val cropH: Int,
    val width: Int,
    val height: Int,
    val points: List<CallFacePoint>,
    val maskW: Int = 0,
    val maskH: Int = 0,
    val mask: ByteArray? = null,
    val image: ByteArray,
) {
    private val valid: Boolean
        get() = id in 0..15 && format in 0..2 && width in 1..MAX_SIDE && height in 1..MAX_SIDE && cropW >= 1 && cropH >= 1 && cropX >= 0 && cropY >= 0 &&
            cropX + cropW <= 65535 && cropY + cropH <= 65535 && (maskW == 0) == (maskH == 0) && maskW in 0..MAX_MASK_SIDE && maskH in 0..MAX_MASK_SIDE &&
            image.isNotEmpty()

    fun encoded(): ByteArray {
        if (!valid || points.size < FaceStream.LANDMARK_COUNT || (maskW > 0 && (mask?.size ?: 0) < maskW * maskH)) throw FaceStreamException("input")
        val out = ArrayList<Byte>()
        out.add(FaceStream.header(FaceStream.KIND_REFERENCE, id))
        for (shift in intArrayOf(24, 16, 8, 0)) out.add((time ushr shift).toByte())
        for (v in intArrayOf(cropX, cropY, cropW, cropH, width, height)) {
            out.add((v shr 8).toByte())
            out.add(v.toByte())
        }
        for (p in points.take(FaceStream.LANDMARK_COUNT)) {
            for (v in doubleArrayOf(p.x, p.y)) {
                if (!v.isFinite()) throw FaceStreamException("input")
                val q = clampInt(FaceStream.floorHalf(v * POINT_STEPS), -32768, 32767) and 0xFFFF
                out.add((q shr 8).toByte())
                out.add(q.toByte())
            }
        }
        out.add(format.toByte())
        out.add(maskW.toByte())
        out.add(maskH.toByte())
        val coded = if (maskW > 0) encodeMask(mask!!, maskW, maskH) else ByteArray(0)
        out.add((coded.size shr 8).toByte())
        out.add(coded.size.toByte())
        val packet = ByteArray(out.size + coded.size + image.size)
        out.forEachIndexed { i, b -> packet[i] = b }
        coded.copyInto(packet, out.size)
        image.copyInto(packet, out.size + coded.size)
        if (packet.size > MAX_PACKET) throw FaceStreamException("long")
        return packet
    }

    companion object {
        const val JPEG = 0
        const val HEIC = 1
        const val WEBP = 2
        const val MAX_SIDE = 1024
        const val MAX_MASK_SIDE = 128
        const val MAX_PACKET = 256 * 1024
        private const val FIXED = 1 + 4 + 12 + 68 * 4 + 3 + 2
        private const val POINT_STEPS = 16.0

        private fun context(bits: ByteArray, w: Int, x: Int, y: Int): Int {
            fun at(px: Int, py: Int): Int = if (px < 0 || py < 0 || px >= w) 0 else bits[py * w + px].toInt()
            return at(x - 1, y) or (at(x, y - 1) shl 1) or (at(x - 1, y - 1) shl 2) or (at(x + 1, y - 1) shl 3)
        }

        internal fun encodeMask(mask: ByteArray, w: Int, h: Int): ByteArray {
            val probs = IntArray(16) { FaceStreamBits.PROB_HALF }
            val bits = ByteArray(w * h) { if (mask[it].toInt() != 0) 1 else 0 }
            val enc = FaceStreamRangeEncoder()
            for (y in 0 until h) for (x in 0 until w) enc.bit(probs, context(bits, w, x, y), bits[y * w + x].toInt())
            return enc.finish()
        }

        internal fun decodeMask(data: ByteArray, w: Int, h: Int): ByteArray {
            val probs = IntArray(16) { FaceStreamBits.PROB_HALF }
            val out = ByteArray(w * h)
            val dec = FaceStreamRangeDecoder(data, 0)
            for (y in 0 until h) for (x in 0 until w) out[y * w + x] = dec.bit(probs, context(out, w, x, y)).toByte()
            return out
        }

        fun decode(packet: ByteArray): FaceStreamReference {
            if (packet.size < FIXED + 1) throw FaceStreamException("short")
            if (packet.size > MAX_PACKET) throw FaceStreamException("long")
            val first = packet[0].toInt() and 0xFF
            if ((first shr 4) and 3 != FaceStream.VERSION) throw FaceStreamException("version")
            if (first shr 6 != FaceStream.KIND_REFERENCE) throw FaceStreamException("not-a-reference")
            var at = 5
            fun u8(i: Int) = packet[i].toInt() and 0xFF
            fun u16(): Int {
                val v = (u8(at) shl 8) or u8(at + 1)
                at += 2
                return v
            }
            val time = (u8(1).toLong() shl 24) or (u8(2).toLong() shl 16) or (u8(3).toLong() shl 8) or u8(4).toLong()
            val cropX = u16()
            val cropY = u16()
            val cropW = u16()
            val cropH = u16()
            val width = u16()
            val height = u16()
            val points = List(FaceStream.LANDMARK_COUNT) {
                val x = u16().toShort() / POINT_STEPS
                val y = u16().toShort() / POINT_STEPS
                CallFacePoint(x, y)
            }
            val format = u8(at)
            val maskW = u8(at + 1)
            val maskH = u8(at + 2)
            at += 3
            val maskLength = u16()
            if (at + maskLength >= packet.size) throw FaceStreamException("corrupt")
            var mask: ByteArray? = null
            if (maskW > 0 && maskH > 0) {
                mask = decodeMask(packet.copyOfRange(at, at + maskLength), maskW, maskH)
            } else if (maskLength != 0) {
                throw FaceStreamException("corrupt")
            }
            at += maskLength
            val reference = FaceStreamReference(first and 15, format, time, cropX, cropY, cropW, cropH, width, height, points, maskW, maskH, mask,
                packet.copyOfRange(at, packet.size))
            if (!reference.valid) throw FaceStreamException("corrupt")
            return reference
        }
    }
}
