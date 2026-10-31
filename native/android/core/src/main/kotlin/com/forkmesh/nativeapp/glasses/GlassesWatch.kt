package com.forkmesh.nativeapp.glasses

enum class SightingKind(val title: String, val icon: String) {
    GLASSES("Smart glasses nearby", "visibility"),
    CAMERA("Camera nearby", "photo_camera"),
}

enum class GlassesMaker(
    val companyId: Int?,
    val label: String,
    val shown: String,
    val kind: SightingKind = SightingKind.CAMERA,
    val services: List<Int> = emptyList(),
) {
    META(0x01AB, "Meta Platforms", "Meta glasses or headset", SightingKind.GLASSES),
    META_TECHNOLOGIES(0x058E, "Meta Platforms Technologies", "Meta glasses or headset", SightingKind.GLASSES),
    LUXOTTICA(0x0D53, "Luxottica (Ray-Ban, Oakley)", "Ray-Ban or Oakley Meta glasses", SightingKind.GLASSES),
    VUZIX(0x060C, "Vuzix", "Vuzix camera glasses", SightingKind.GLASSES),
    SNAP(null, "Snap", "Snap Spectacles", SightingKind.GLASSES, listOf(0xFE45)),
    GOPRO(0x02F2, "GoPro", "GoPro camera", services = listOf(0xFEA5, 0xFEA6)),
    INSTA360(0x10D7, "Arashi Vision (Insta360)", "Insta360 camera", services = listOf(0xFC2D, 0xFC2E, 0xFC2F, 0xFC30)),
    DJI(0x08AA, "DJI", "DJI camera or drone"),
    CANON(0x01A9, "Canon", "Canon camera"),
    NIKON(0x0399, "Nikon", "Nikon camera"),
    FUJIFILM(0x04D8, "Fujifilm", "Fujifilm camera"),
    OLYMPUS(0x04D0, "Olympus", "Olympus camera"),
    OM_DIGITAL(0x09F1, "OM Digital Solutions", "OM System camera"),
    RICOH(0x065F, "Ricoh (Pentax, Theta)", "Ricoh, Pentax or Theta camera"),
    LEICA(0x051A, "Leica Camera", "Leica camera"),
    HASSELBLAD(0x10D4, "Hasselblad", "Hasselblad camera"),
    POLAROID(0x02A0, "Impossible Camera (Polaroid)", "Polaroid camera"),
    BLINCAM(0x04D7, "Blincam", "Blincam wearable camera"),
    OPAL(0x0F30, "Opal Camera", "Opal webcam"),
    AXON(null, "Axon", "Axon body camera", services = listOf(0xFC81)),
    SEVENTY_MAI(0x0909, "70mai", "70mai dash cam", services = listOf(0xFD4D, 0xFD4E)),
    ARLO(0x0C19, "Arlo", "Arlo security camera"),
    HIKVISION(0x0E25, "Hikvision", "Hikvision security camera");

    companion object {
        fun of(companyId: Int): GlassesMaker? = entries.firstOrNull { it.companyId == companyId }

        fun serving(service: Int): GlassesMaker? = entries.firstOrNull { service in it.services }
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
    val kind: SightingKind = SightingKind.GLASSES,
) {
    val distance: String get() = GlassesSignatures.distance(rssi)
}

object GlassesSignatures {
    val COMPANY_IDS: Set<Int> = GlassesMaker.entries.mapNotNull { it.companyId }.toSet()
    val SERVICE_UUIDS: Set<Int> = GlassesMaker.entries.flatMap { it.services }.toSet()
    val NAME_KEYWORDS: List<String> = listOf("ray-ban", "rayban", "ray ban", "oakley meta", "meta ray", "meta glasses")
    val CAMERA_KEYWORDS: List<String> = listOf(
        "camera", "webcam", "dashcam", "dash cam", "bodycam", "body cam", "body-cam", "trail cam", "eufycam", "wyze cam",
        "gopro", "insta360", "insta 360", "osmo action", "osmo pocket", "osmo 360", "osmo nano", "dji action", "dji pocket",
        "canon", "powershot", "eos r", "eos m", "nikon", "coolpix", "fujifilm", "lumix", "olympus", "om system", "ricoh",
        "pentax", "theta", "leica", "hasselblad", "ilce-", "zv-e", "zv-1", "dsc-rx", "dsc-hx", "fdr-x", "hdr-as", "kodak",
        "polaroid", "instax mini evo", "instax pal", "akaso", "sjcam", "yi action", "blincam", "spectacles", "axon body",
        "nextbase", "viofo", "70mai", "blackvue", "thinkware", "vantrue", "garmin dash", "brinno", "tactacam", "spypoint",
        "mavic", "dji mini", "dji air", "dji neo", "dji flip", "avata", "skydio", "anafi", "hover x1", "vuzix", "rokid",
    )
    val PRINTER_KEYWORDS: List<String> = listOf(
        "printer", "pixma", "maxify", "selphy", "imageclass", "instax link", "instax mini link", "instax square link",
        "instax wide link", "instax share",
    )

    fun companyId(manufacturerData: ByteArray): Int? {
        if (manufacturerData.size < 2) return null
        return (manufacturerData[0].toInt() and 0xff) or ((manufacturerData[1].toInt() and 0xff) shl 8)
    }

    fun nameKind(name: String?): SightingKind? {
        val lowered = name?.trim()?.lowercase() ?: return null
        return when {
            lowered.isEmpty() -> null
            NAME_KEYWORDS.any { it in lowered } -> SightingKind.GLASSES
            CAMERA_KEYWORDS.any { it in lowered } && !printerName(lowered) -> SightingKind.CAMERA
            else -> null
        }
    }

    fun nameMatches(name: String?): Boolean = nameKind(name) != null

    fun printerName(name: String?): Boolean {
        val lowered = name?.lowercase() ?: return false
        return PRINTER_KEYWORDS.any { it in lowered }
    }

    fun maker(advert: GlassesAdvert): GlassesMaker? =
        advert.companyIds.firstNotNullOfOrNull(GlassesMaker::of) ?: advert.serviceUuids.firstNotNullOfOrNull(GlassesMaker::serving)

    fun watched(advert: GlassesAdvert): Boolean =
        advert.companyIds.any { it in COMPANY_IDS } || advert.serviceUuids.any { it in SERVICE_UUIDS }

    fun kind(advert: GlassesAdvert): SightingKind? {
        val kind = maker(advert)?.kind ?: return nameKind(advert.name)
        return if (kind == SightingKind.CAMERA && printerName(advert.name)) null else kind
    }

    fun match(advert: GlassesAdvert, range: GlassesRange, nowMs: Long): GlassesSighting? {
        if (advert.rssi < range.minRssi || advert.rssi >= 0) return null
        val kind = kind(advert) ?: return null
        val maker = maker(advert)
        val named = nameMatches(advert.name)
        val label = if (named) advert.name!!.trim() else maker?.shown ?: kind.title
        return GlassesSighting(advert.address, label, maker, named, advert.rssi, nowMs, kind)
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
