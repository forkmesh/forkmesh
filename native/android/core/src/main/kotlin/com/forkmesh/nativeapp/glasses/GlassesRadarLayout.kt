package com.forkmesh.nativeapp.glasses

data class GlassesRadarBlip(
    val address: String,
    val angle: Double,
    val distance: Double,
    val glasses: Boolean,
)

object GlassesRadarLayout {
    const val NEAREST = 0.2
    const val FARTHEST = 0.92

    fun angle(address: String): Double {
        var hash = 2_166_136_261u
        for (byte in address.encodeToByteArray()) {
            hash = (hash xor byte.toUByte().toUInt()) * 16_777_619u
        }
        return (hash % 3600u).toInt() / 10.0
    }

    fun distance(rssi: Int): Double = (1 - GlassesHoming.closeness(rssi)).coerceIn(NEAREST, FARTHEST)

    fun blips(others: List<NearbyDevice>, glasses: List<GlassesSighting>, homing: GlassesHomingReading?, heading: Double?): List<GlassesRadarBlip> {
        val devices = others.map { GlassesRadarBlip(it.address, angle(it.address), distance(it.rssi), false) }
        val found = glasses.map { sighting ->
            val followed = homing?.takeIf { it.address == sighting.address && it.live }
            GlassesRadarBlip(
                sighting.address,
                followed?.arrow(heading) ?: angle(sighting.address),
                distance(followed?.rssi ?: sighting.rssi),
                true,
            )
        }
        return devices + found
    }
}
