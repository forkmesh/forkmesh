package com.forkmesh.nativeapp.chat.face

class CallFaceOneEuroFilter(val minCutoff: Double = 1.2, val beta: Double = 0.02, val derivativeCutoff: Double = 1.0) {
    var value: Double? = null
        private set
    var slope = 0.0
        private set
    var time = 0.0
        private set

    fun reset() {
        value = null
        slope = 0.0
    }

    fun filter(input: Double, now: Double, scale: Double = 1.0): Double {
        val current = value
        if (current == null || !(now > time)) {
            value = input
            time = now
            return input
        }
        val elapsed = (now - time) / 1000
        time = now
        fun smoothing(cutoff: Double): Double {
            val tau = 1 / (2 * Math.PI * cutoff)
            return 1 / (1 + tau / elapsed)
        }
        val rate = (input - current) / elapsed
        slope += smoothing(derivativeCutoff) * (rate - slope)
        val cutoff = minCutoff + (beta * Math.abs(slope)) / scale
        val next = current + smoothing(cutoff) * (input - current)
        value = next
        return next
    }
}

class CallFacePointSmoother(count: Int, minCutoff: Double = 1.2, beta: Double = 0.6) {
    private val filters = List(count * 3) { CallFaceOneEuroFilter(minCutoff, beta) }

    fun reset() {
        for (filter in filters) filter.reset()
    }

    fun smooth(points: List<CallFacePoint>, time: Double, scale: Double = 1.0): List<CallFacePoint> = points.mapIndexed { i, point ->
        val values = point.values
        CallFacePoint.of(DoubleArray(values.size) { k ->
            if (i * 3 + k < filters.size) filters[i * 3 + k].filter(values[k], time, scale) else values[k]
        })
    }
}

class CallFaceAnchorSmoother {
    private val eye = List(4) { CallFaceOneEuroFilter(1.5, 1.5) }
    private val mouth = List(2) { CallFaceOneEuroFilter(1.5, 1.5) }
    private val open = CallFaceOneEuroFilter(3.0, 0.5)
    private val yawFilter = CallFaceOneEuroFilter(0.6, 0.3)
    private var yaw = 0.0

    fun reset() {
        for (filter in eye + mouth + listOf(open, yawFilter)) filter.reset()
        yaw = 0.0
    }

    fun smooth(anchors: CallFaceAnchors, time: Double): CallFaceAnchors {
        val span = CallFaceJS.or(CallFaceJS.hypot(anchors.eyeR.x - anchors.eyeL.x, anchors.eyeR.y - anchors.eyeL.y), 1.0)
        anchors.yaw?.let { yaw = yawFilter.filter(it, time) }
        val eyeL = CallFacePoint(eye[0].filter(anchors.eyeL.x, time, span), eye[1].filter(anchors.eyeL.y, time, span))
        val eyeR = CallFacePoint(eye[2].filter(anchors.eyeR.x, time, span), eye[3].filter(anchors.eyeR.y, time, span))
        val lips = anchors.mouth?.let { CallFacePoint(mouth[0].filter(it.x, time, span), mouth[1].filter(it.y, time, span)) }
        val opened = open.filter(if (CallFaceJS.truthy(anchors.open)) anchors.open!! else 0.0, time)
        return CallFaceAnchors(eyeL = eyeL, eyeR = eyeR, mouth = lips, open = CallFaceGeometry.clampValue(opened, 0.0, 1.0), yaw = yaw)
    }
}

class CallFaceGroupFilter(val minCutoff: Double = 1.2, val beta: Double = 0.6, val derivativeCutoff: Double = 1.0) {
    var values: DoubleArray? = null
        private set
    var speed = 0.0
        private set
    var time = 0.0
        private set

    fun reset() {
        values = null
        speed = 0.0
    }

    fun filter(input: DoubleArray, now: Double, scale: Double = 1.0): DoubleArray {
        val current = values
        if (current == null || current.size != input.size || !(now > time)) {
            values = input.copyOf()
            time = now
            return input.copyOf()
        }
        val elapsed = (now - time) / 1000
        time = now
        fun smoothing(cutoff: Double): Double {
            val tau = 1 / (2 * Math.PI * cutoff)
            return 1 / (1 + tau / elapsed)
        }
        var fastest = 0.0
        for (i in input.indices) fastest = Math.max(fastest, Math.abs(input[i] - current[i]) / elapsed)
        speed += smoothing(derivativeCutoff) * (fastest / CallFaceJS.or(scale, 1.0) - speed)
        val k = smoothing(minCutoff + beta * speed)
        for (i in input.indices) current[i] += k * (input[i] - current[i])
        return current.copyOf()
    }
}

data class CallFaceSmoothGroup(val points: List<Int>, val minCutoff: Double, val beta: Double, val derivativeCutoff: Double)

class CallFaceLandmarkSmoother {
    companion object {
        val HEAD = CallFaceSmoothGroup(emptyList(), 3.0, 5.0, 4.0)
        val GROUPS = listOf(
            CallFaceSmoothGroup(CallFaceGeometry.jaw, 1.2, 2.0, 2.0),
            CallFaceSmoothGroup((17 until 27).toList(), 2.0, 3.0, 3.0),
            CallFaceSmoothGroup((27 until 36).toList(), 1.5, 2.0, 2.0),
            CallFaceSmoothGroup((36 until 48).toList(), 2.5, 4.0, 3.0),
            CallFaceSmoothGroup((48 until 68).toList(), 2.5, 6.0, 4.0),
        )
    }

    private val eyes = CallFaceGroupFilter(HEAD.minCutoff, HEAD.beta, HEAD.derivativeCutoff)
    private val filters = GROUPS.map { it.points to CallFaceGroupFilter(it.minCutoff, it.beta, it.derivativeCutoff) }

    fun reset() {
        eyes.reset()
        for ((_, filter) in filters) filter.reset()
    }

    fun smooth(points: List<CallFacePoint>, time: Double): List<CallFacePoint> {
        if (points.size < CallFaceGeometry.LANDMARK_COUNT) return points
        val raw = CallFaceGeometry.faceFrame(points)
        val eye = eyes.filter(doubleArrayOf(raw.left.x, raw.left.y, raw.right.x, raw.right.y), time, raw.span)
        val steady = CallFaceGeometry.faceFrame(CallFacePoint(eye[0], eye[1]), CallFacePoint(eye[2], eye[3]))
        val out = points.toMutableList()
        for ((indices, filter) in filters) {
            val flat = ArrayList<Double>()
            for (i in indices) {
                val local = CallFaceGeometry.toFrame(raw, points[i])
                flat.add(local.x / raw.span)
                flat.add(local.y / raw.span)
                points[i].z?.let { flat.add(it / raw.span) }
            }
            val smooth = filter.filter(flat.toDoubleArray(), time)
            var at = 0
            for (i in indices) {
                var point = CallFaceGeometry.fromFrame(steady, smooth[at] * steady.span, smooth[at + 1] * steady.span)
                at += 2
                if (points[i].z != null) {
                    point = point.copy(z = smooth[at] * steady.span)
                    at += 1
                }
                out[i] = point
            }
        }
        return out
    }
}
