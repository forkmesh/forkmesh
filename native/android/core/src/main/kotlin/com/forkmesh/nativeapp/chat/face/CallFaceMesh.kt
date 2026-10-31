package com.forkmesh.nativeapp.chat.face

data class CallFaceDetection(val score: Double, val cx: Double, val cy: Double, val w: Double, val h: Double, val keypoints: List<CallFacePoint>)

data class CallFaceLetterbox(val scale: Double, val x: Double, val y: Double) {
    companion object {
        fun of(width: Double, height: Double): CallFaceLetterbox {
            val side = CallFaceJS.max(width, height)
            return CallFaceLetterbox(side, (side - width) / 2 / side, (side - height) / 2 / side)
        }
    }
}

data class CallFaceRoi(val cx: Double, val cy: Double, val size: Double, val angle: Double)

object CallFaceMesh {
    const val DETECT_SIZE = 128.0
    const val MESH_SIZE = 192.0
    const val DETECT_SCORE = 0.5
    const val FACE_SCORE = 0.5
    const val ROI_SCALE = 1.5

    val regionNames = listOf("faceContour", "rightEyebrow", "leftEyebrow", "rightEye", "leftEye", "noseCrest", "nose", "outerLips", "innerLips")
    val regions: Map<String, List<Int>> = mapOf(
        "faceContour" to listOf(127, 234, 93, 132, 58, 172, 136, 150, 149, 176, 148, 152, 377, 400, 378, 379, 365, 397, 288, 361, 323, 454, 356),
        "rightEyebrow" to listOf(70, 63, 105, 66, 107),
        "leftEyebrow" to listOf(336, 296, 334, 293, 300),
        "rightEye" to listOf(33, 246, 161, 160, 159, 158, 157, 173, 133, 155, 154, 153, 145, 144, 163, 7),
        "leftEye" to listOf(263, 466, 388, 387, 386, 385, 384, 398, 362, 382, 381, 380, 374, 373, 390, 249),
        "noseCrest" to listOf(168, 6, 197, 195, 5, 4),
        "nose" to listOf(6, 197, 195, 5, 64, 98, 97, 2, 326, 327, 294),
        "outerLips" to listOf(61, 185, 40, 39, 37, 0, 267, 269, 270, 409, 291, 375, 321, 405, 314, 17, 84, 181, 91, 146),
        "innerLips" to listOf(78, 191, 80, 81, 82, 13, 312, 311, 310, 415, 308, 324, 318, 402, 317, 14, 87, 178, 88, 95),
    )

    fun blazeAnchors(): List<CallFacePoint> {
        val anchors = ArrayList<CallFacePoint>()
        for ((grid, count) in listOf(16 to 2, 8 to 6)) {
            for (y in 0 until grid) {
                for (x in 0 until grid) {
                    repeat(count) { anchors.add(CallFacePoint((x + 0.5) / grid, (y + 0.5) / grid)) }
                }
            }
        }
        return anchors
    }

    fun sigmoid(logit: Double, limit: Double): Double = 1 / (1 + StrictMath.exp(-CallFaceJS.max(-limit, CallFaceJS.min(limit, logit))))

    fun decodeDetections(scores: FloatArray, boxes: FloatArray, anchors: List<CallFacePoint>): CallFaceDetection? {
        var best: CallFaceDetection? = null
        val found = ArrayList<CallFaceDetection>()
        for (i in anchors.indices) {
            val score = sigmoid(scores[i].toDouble(), 100.0)
            if (score < DETECT_SCORE) continue
            val r = DoubleArray(16) { boxes[i * 16 + it].toDouble() }
            val ax = anchors[i].x
            val ay = anchors[i].y
            val box = CallFaceDetection(
                score,
                r[0] / DETECT_SIZE + ax,
                r[1] / DETECT_SIZE + ay,
                r[2] / DETECT_SIZE,
                r[3] / DETECT_SIZE,
                List(6) { k -> CallFacePoint(r[4 + k * 2] / DETECT_SIZE + ax, r[5 + k * 2] / DETECT_SIZE + ay) },
            )
            found.add(box)
            if (best == null || score > best.score) best = box
        }
        val top = best ?: return null
        fun overlap(a: CallFaceDetection, b: CallFaceDetection): Double {
            val x0 = CallFaceJS.max(a.cx - a.w / 2, b.cx - b.w / 2)
            val x1 = CallFaceJS.min(a.cx + a.w / 2, b.cx + b.w / 2)
            val y0 = CallFaceJS.max(a.cy - a.h / 2, b.cy - b.h / 2)
            val y1 = CallFaceJS.min(a.cy + a.h / 2, b.cy + b.h / 2)
            val inter = CallFaceJS.max(0.0, x1 - x0) * CallFaceJS.max(0.0, y1 - y0)
            return inter / CallFaceJS.or(a.w * a.h + b.w * b.h - inter, 1.0)
        }
        val group = found.filter { overlap(it, top) > 0.3 }
        val total = group.fold(0.0) { sum, box -> sum + box.score }
        fun mean(pick: (CallFaceDetection) -> Double): Double = group.fold(0.0) { sum, box -> sum + pick(box) * box.score } / total
        return CallFaceDetection(
            top.score,
            mean { it.cx },
            mean { it.cy },
            mean { it.w },
            mean { it.h },
            top.keypoints.indices.map { k -> CallFacePoint(mean { it.keypoints[k].x }, mean { it.keypoints[k].y }) },
        )
    }

    fun roiFromDetection(detection: CallFaceDetection, letterbox: CallFaceLetterbox): CallFaceRoi {
        fun toFrame(p: CallFacePoint): CallFacePoint = CallFacePoint((p.x - letterbox.x) * letterbox.scale, (p.y - letterbox.y) * letterbox.scale)
        val center = toFrame(CallFacePoint(detection.cx, detection.cy))
        val right = toFrame(detection.keypoints[0])
        val left = toFrame(detection.keypoints[1])
        val angle = StrictMath.atan2(left.y - right.y, left.x - right.x)
        val size = CallFaceJS.max(detection.w, detection.h) * letterbox.scale * ROI_SCALE
        return CallFaceRoi(center.x, center.y, size, angle)
    }

    fun roiFromMesh(points: List<CallFacePoint>): CallFaceRoi {
        val a = points[33]
        val b = points[263]
        val angle = StrictMath.atan2(b.y - a.y, b.x - a.x)
        val c = StrictMath.cos(-angle)
        val s = StrictMath.sin(-angle)
        var minX = Double.POSITIVE_INFINITY
        var minY = Double.POSITIVE_INFINITY
        var maxX = Double.NEGATIVE_INFINITY
        var maxY = Double.NEGATIVE_INFINITY
        val ox = points[1].x
        val oy = points[1].y
        for (point in points) {
            val rx = (point.x - ox) * c - (point.y - oy) * s
            val ry = (point.x - ox) * s + (point.y - oy) * c
            minX = CallFaceJS.min(minX, rx)
            maxX = CallFaceJS.max(maxX, rx)
            minY = CallFaceJS.min(minY, ry)
            maxY = CallFaceJS.max(maxY, ry)
        }
        val mx = (minX + maxX) / 2
        val my = (minY + maxY) / 2
        val ci = StrictMath.cos(angle)
        val si = StrictMath.sin(angle)
        return CallFaceRoi(ox + mx * ci - my * si, oy + mx * si + my * ci, CallFaceJS.max(maxX - minX, maxY - minY) * ROI_SCALE, angle)
    }

    fun meshPoints(raw: FloatArray, roi: CallFaceRoi): List<CallFacePoint> {
        val c = StrictMath.cos(roi.angle)
        val s = StrictMath.sin(roi.angle)
        val points = ArrayList<CallFacePoint>()
        var i = 0
        while (i + 2 < raw.size) {
            val x = (raw[i].toDouble() / MESH_SIZE - 0.5) * roi.size
            val y = (raw[i + 1].toDouble() / MESH_SIZE - 0.5) * roi.size
            points.add(CallFacePoint(roi.cx + x * c - y * s, roi.cy + x * s + y * c, (raw[i + 2].toDouble() / MESH_SIZE) * roi.size))
            i += 3
        }
        return points
    }

    fun meshRegions(points: List<CallFacePoint>): CallFaceRegions {
        val out = LinkedHashMap<String, List<List<Double>>>()
        for (name in regionNames) {
            out[name] = regions.getValue(name).map { i ->
                val p = points[i]
                val z = p.z
                if (z != null && z.isFinite()) listOf(p.x, p.y, -z) else listOf(p.x, p.y)
            }
        }
        return out
    }

    fun meshToLandmarks(points: List<CallFacePoint>): List<CallFacePoint>? = CallFaceGeometry.landmarksFromRegions(meshRegions(points))
}
