package com.forkmesh.nativeapp.glasses

enum class GlassesMaker(val companyId: Int, val label: String) {
    META(0x01AB, "Meta Platforms"),
    META_TECHNOLOGIES(0x058E, "Meta Platforms Technologies"),
    LUXOTTICA(0x0D53, "Luxottica (Ray-Ban, Oakley)");

    companion object {
        fun of(companyId: Int): GlassesMaker? = entries.firstOrNull { it.companyId == companyId }
    }
}

enum class GlassesRange(val wire: String, val label: String, val minRssi: Int) {
    NEAR("near", "Close by", -65),
    ROOM("room", "Same room", -75),
    WIDE("wide", "Anywhere in range", -90);

    companion object {
        val DEFAULT = ROOM

        fun of(wire: String?): GlassesRange = entries.firstOrNull { it.wire == wire } ?: DEFAULT
    }
}

data class GlassesAdvert(
    val address: String,
    val name: String?,
    val rssi: Int,
    val companyIds: List<Int>,
    val serviceUuids: List<Int> = emptyList(),
    val appearance: Int? = null,
)

data class GlassesSighting(
    val address: String,
    val label: String,
    val maker: GlassesMaker?,
    val namedAsGlasses: Boolean,
    val rssi: Int,
    val seenAtMs: Long,
) {
    val distance: String get() = GlassesSignatures.distance(rssi)
}

object GlassesSignatures {
    val COMPANY_IDS: Set<Int> = GlassesMaker.entries.map { it.companyId }.toSet()
    val NAME_KEYWORDS: List<String> = listOf("ray-ban", "rayban", "ray ban", "oakley meta", "meta ray", "meta glasses")

    fun companyId(manufacturerData: ByteArray): Int? {
        if (manufacturerData.size < 2) return null
        return (manufacturerData[0].toInt() and 0xff) or ((manufacturerData[1].toInt() and 0xff) shl 8)
    }

    fun nameMatches(name: String?): Boolean {
        val lowered = name?.trim()?.lowercase() ?: return false
        return lowered.isNotEmpty() && NAME_KEYWORDS.any { it in lowered }
    }

    fun match(advert: GlassesAdvert, range: GlassesRange, nowMs: Long): GlassesSighting? {
        if (advert.rssi < range.minRssi || advert.rssi >= 0) return null
        val maker = advert.companyIds.firstNotNullOfOrNull(GlassesMaker::of)
        val named = nameMatches(advert.name)
        if (maker == null && !named) return null
        val label = when {
            named -> advert.name!!.trim()
            maker == GlassesMaker.LUXOTTICA -> "Ray-Ban or Oakley Meta glasses"
            else -> "Meta glasses or headset"
        }
        return GlassesSighting(advert.address, label, maker, named, advert.rssi, nowMs)
    }

    fun distance(rssi: Int): String = when {
        rssi >= -60 -> "Very close"
        rssi >= -70 -> "Close"
        else -> "In the area"
    }

    fun alertText(sighting: GlassesSighting): String =
        "${sighting.label} · ${sighting.distance.lowercase()} (${sighting.rssi} dBm)"
}

class GlassesTracker(val forgetAfterMs: Long = 120_000L, val cooldownMs: Long = 30_000L) {
    private val seen = LinkedHashMap<String, GlassesSighting>()
    private var lastAlertMs: Long? = null

    fun observe(sighting: GlassesSighting): Boolean {
        prune(sighting.seenAtMs)
        val fresh = sighting.address !in seen
        seen[sighting.address] = sighting
        if (!fresh) return false
        val last = lastAlertMs
        if (last != null && sighting.seenAtMs - last < cooldownMs) return false
        lastAlertMs = sighting.seenAtMs
        return true
    }

    fun nearby(nowMs: Long): List<GlassesSighting> {
        prune(nowMs)
        return seen.values.sortedByDescending { it.rssi }
    }

    fun reset() {
        seen.clear()
        lastAlertMs = null
    }

    private fun prune(nowMs: Long) {
        seen.entries.removeAll { nowMs - it.value.seenAtMs > forgetAfterMs }
    }
}
