package com.forkmesh.nativeapp.chat.face

data class CallOutfit(val id: String, val label: String)

data class CallOutfitFrame(val center: CallFacePoint, val x: CallFacePoint, val y: CallFacePoint, val unit: Double, val roll: Double)

object CallOutfits {
    const val STORAGE_KEY = "forkmesh.chat.callOutfit"
    const val SIZE = 1024.0
    const val UNIT = 96.0
    val neck = CallFacePoint(512.0, 176.0)
    const val DROP = 1.35
    const val CHIN_SPANS = 1.9
    const val ROLL_SHARE = 0.3
    const val TILE = 256
    const val REACH = 0.35

    val all: List<CallOutfit> = listOf(
        CallOutfit("", "Your own clothes"),
        CallOutfit("suit", "Suit and tie"),
        CallOutfit("hoodie", "Hoodie"),
        CallOutfit("sweater", "Cable-knit sweater"),
        CallOutfit("denim", "Denim jacket"),
        CallOutfit("flannel", "Flannel shirt"),
        CallOutfit("turtleneck", "Turtleneck"),
    )

    fun byId(id: String): CallOutfit? = all.firstOrNull { it.id == id }

    fun frame(points: List<CallFacePoint>?): CallOutfitFrame? {
        if (points == null || points.size < 68) return null
        val face = CallFaceGeometry.faceFrame(points)
        val chin = points[8]
        val unit = CallFaceJS.max(face.span, CallFaceJS.hypot(chin.x - face.mid.x, chin.y - face.mid.y) / CHIN_SPANS)
        val roll = face.roll * ROLL_SHARE
        val ux = CallFacePoint(StrictMath.cos(roll), StrictMath.sin(roll))
        val uy = CallFacePoint(-ux.y, ux.x)
        val base = CallFacePoint((face.mid.x + chin.x) / 2, chin.y)
        return CallOutfitFrame(
            CallFacePoint(base.x + uy.x * DROP * unit, base.y + uy.y * DROP * unit),
            CallFacePoint(ux.x / unit, ux.y / unit),
            CallFacePoint(uy.x / unit, uy.y / unit),
            unit,
            roll,
        )
    }

    fun uv(frame: CallOutfitFrame, pixel: CallFacePoint): CallFacePoint {
        val dx = pixel.x - frame.center.x
        val dy = pixel.y - frame.center.y
        val qx = dx * frame.x.x + dy * frame.x.y
        val qy = dx * frame.y.x + dy * frame.y.y
        return CallFacePoint((neck.x + qx * UNIT) / SIZE, (neck.y + qy * UNIT) / SIZE)
    }
}

class CallOutfitSmoother(val rate: Double = 0.35) {
    private var last: CallOutfitFrame? = null

    fun smooth(frame: CallOutfitFrame?): CallOutfitFrame? {
        if (frame == null) return last
        val previous = last
        if (previous == null) {
            last = frame
            return frame
        }
        fun mix(a: CallFacePoint, b: CallFacePoint): CallFacePoint = CallFacePoint(a.x + (b.x - a.x) * rate, a.y + (b.y - a.y) * rate)
        val unit = previous.unit + (frame.unit - previous.unit) * rate
        val roll = previous.roll + (frame.roll - previous.roll) * rate
        val ux = CallFacePoint(StrictMath.cos(roll), StrictMath.sin(roll))
        val uy = CallFacePoint(-ux.y, ux.x)
        val next = CallOutfitFrame(mix(previous.center, frame.center), CallFacePoint(ux.x / unit, ux.y / unit), CallFacePoint(uy.x / unit, uy.y / unit), unit, roll)
        last = next
        return next
    }

    fun reset() {
        last = null
    }
}
