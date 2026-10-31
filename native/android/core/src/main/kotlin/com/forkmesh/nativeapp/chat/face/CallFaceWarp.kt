package com.forkmesh.nativeapp.chat.face

data class CallFaceAxisFrame(val center: CallFacePoint, val x: CallFacePoint, val y: CallFacePoint)

data class CallFaceHairRegion(val center: CallFacePoint, val size: CallFacePoint)

object CallFaceWarp {
    const val VERTEX_FLOATS = 7
    const val HAIR_SIDE = 6.4
    val hairCenter = CallFacePoint(0.0, 0.5, -0.3)
    val zoneCenter = CallFacePoint(0.0, 0.0, -0.35)
    const val EYE_UV_HALF_X = 0.62
    const val EYE_UV_HALF_Y = 0.36
    const val LOOK_GAIN = 0.55

    fun vertexRegion(v: Int, extraStart: Int = Int.MAX_VALUE): Double {
        if (v >= extraStart) return 1.0
        if (v >= 68) return 0.0
        return if (v <= 26) 0.25 else 1.0
    }

    private fun signedArea(points: List<CallFacePoint>, v: IntArray): Double {
        val p = points[v[0]]
        val q = points[v[1]]
        val r = points[v[2]]
        return (q.x - p.x) * (r.y - p.y) - (r.x - p.x) * (q.y - p.y)
    }

    fun warpVertices(mesh: CallFaceHeadMesh, layout: List<CallFacePoint>, live: List<CallFacePoint>, photoSize: Double, depths: DoubleArray? = null): FloatArray {
        val out = FloatArray(mesh.triangles.size * 3 * VERTEX_FLOATS)
        var at = 0
        for (triangle in mesh.triangles) {
            val folded = triangle.cull && signedArea(layout, triangle.v) * signedArea(live, triangle.v) < 0
            triangle.v.forEachIndexed { k, v ->
                val placed = live[if (folded) triangle.v[0] else v]
                out[at] = placed.x.toFloat()
                out[at + 1] = placed.y.toFloat()
                out[at + 2] = (layout[v].x / photoSize).toFloat()
                out[at + 3] = (layout[v].y / photoSize).toFloat()
                out[at + 4] = triangle.hole[k].toFloat()
                out[at + 5] = vertexRegion(v, mesh.extraStart).toFloat()
                out[at + 6] = (depths?.get(v) ?: 0.0).toFloat()
                at += VERTEX_FLOATS
            }
        }
        return out
    }

    fun coverRect(imageWidth: Double, imageHeight: Double, width: Double, height: Double): DoubleArray {
        val image = imageWidth / CallFaceJS.max(1.0, imageHeight)
        val frame = width / CallFaceJS.max(1.0, height)
        if (image > frame) {
            val span = frame / image
            return doubleArrayOf((1 - span) / 2, 0.0, span, 1.0)
        }
        val span = image / frame
        return doubleArrayOf(0.0, (1 - span) / 2, 1.0, span)
    }

    fun hairRegion(pose: CallFacePose, width: Double, height: Double): CallFaceHairRegion {
        val center = CallFaceGeometry.projectPose(pose, hairCenter)
        val side = HAIR_SIDE * pose.scale
        val size = CallFacePoint(CallFaceJS.min(side, width), CallFaceJS.min(side, height))
        fun fit(value: Double, extent: Double, limit: Double): Double = CallFaceJS.min(CallFaceJS.max(value, extent / 2), limit - extent / 2)
        return CallFaceHairRegion(CallFacePoint(fit(center.x, size.x, width), fit(center.y, size.y, height)), size)
    }

    fun photoEyes(layout: List<CallFacePoint>, photoSize: Double): List<DoubleArray> =
        listOf(Triple(36, 39, listOf(36, 37, 38, 39, 40, 41)), Triple(42, 45, listOf(42, 43, 44, 45, 46, 47))).map { (a, b, ring) ->
            val width = CallFaceJS.hypot(layout[b].x - layout[a].x, layout[b].y - layout[a].y) / photoSize
            val center = CallFaceGeometry.centroid(layout, ring)
            doubleArrayOf(center.x / photoSize, center.y / photoSize, EYE_UV_HALF_X * width, EYE_UV_HALF_Y * width)
        }

    fun lookUniform(look: List<CallFacePoint?>?): DoubleArray {
        if (look == null) return doubleArrayOf(0.0, 0.0, 0.0, 0.0)
        fun pick(side: Int, k: Int): Double {
            val point = look.getOrNull(side) ?: return 0.0
            val value = if (k == 0) point.x else point.y
            return if (value.isFinite()) value * LOOK_GAIN else 0.0
        }
        return doubleArrayOf(pick(0, 0), pick(0, 1), pick(1, 0), pick(1, 1))
    }

    fun axisFrame(box: CallFaceBox): CallFaceAxisFrame =
        CallFaceAxisFrame(box.center, CallFacePoint(box.ux.x / box.half.x, box.ux.y / box.half.x), CallFacePoint(box.uy.x / box.half.y, box.uy.y / box.half.y))

    fun zoneFrame(head: List<CallFacePoint>, pose: CallFacePose): CallFaceAxisFrame {
        val center = CallFaceGeometry.projectPose(pose, zoneCenter)
        val frame = CallFaceGeometry.faceFrame(head)
        val scale = CallFaceJS.or(pose.scale, frame.span)
        val ux = CallFacePoint(StrictMath.cos(pose.roll), StrictMath.sin(pose.roll))
        val uy = CallFacePoint(-ux.y, ux.x)
        return CallFaceAxisFrame(center, CallFacePoint(ux.x / scale, ux.y / scale), CallFacePoint(uy.x / scale, uy.y / scale))
    }
}
