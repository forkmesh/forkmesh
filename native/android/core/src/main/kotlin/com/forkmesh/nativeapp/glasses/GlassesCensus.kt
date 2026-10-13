package com.forkmesh.nativeapp.glasses

import java.util.UUID

enum class NearbyKind(val label: String, val icon: String) {
    PHONE("Phone", "smartphone"),
    TABLET("Tablet or e-reader", "tablet"),
    COMPUTER("Computer", "laptop"),
    HEADPHONES("Headphones", "headphones"),
    HEARING_AID("Hearing aid", "hearing"),
    SPEAKER("Speaker", "speaker"),
    TV("TV or streamer", "tv"),
    WATCH("Watch or band", "watch"),
    GLASSES("Other smart glasses", "visibility"),
    FITNESS("Fitness sensor", "fitness_center"),
    HEALTH("Health monitor", "monitor_heart"),
    SCALE("Scale", "monitor_weight"),
    INPUT("Keyboard, mouse or remote", "keyboard"),
    GAMING("Game controller", "sports_esports"),
    TAG("Tracker tag or beacon", "location_on"),
    CAR("Car", "directions_car"),
    RIDE("Scooter or e-bike", "electric_scooter"),
    DRONE("Drone", "flight"),
    CAMERA("Camera", "photo_camera"),
    PRINTER("Printer", "print"),
    LOCK("Smart lock", "lock"),
    SMART_HOME("Smart home", "home"),
    LIGHT("Light", "lightbulb"),
    APPLIANCE("Appliance", "kitchen"),
    OTHER("Bluetooth device", "bluetooth"),
}

data class NearbyDevice(
    val address: String,
    val name: String?,
    val maker: String?,
    val kind: NearbyKind,
    val rssi: Int,
    val seenAtMs: Long,
) {
    val label: String get() = name ?: maker?.let { "$it device" } ?: kind.label
    val distance: String get() = GlassesSignatures.distance(rssi)
}

object NearbyKinds {
    val MAKERS: Map<Int, String> = mapOf(
        0x004C to "Apple",
        0x0075 to "Samsung",
        0x00E0 to "Google",
        0x0006 to "Microsoft",
        0x009E to "Bose",
        0x012D to "Sony",
        0x0087 to "Garmin",
        0x0157 to "Huami",
        0x038F to "Xiaomi",
        0x027D to "Huawei",
        0x0059 to "Nordic",
        0x000F to "Broadcom",
        0x0171 to "Amazon",
        0x0310 to "JBL",
        0x0057 to "Harman",
        0x00D2 to "Tile",
        0x0822 to "Tile",
        0x0499 to "Ruuvi",
        0x006B to "Polar",
        0x05A7 to "Sonos",
        0x01DA to "Logitech",
    )

    private val NAME_KINDS: List<Pair<NearbyKind, List<String>>> = listOf(
        NearbyKind.HEADPHONES to listOf("airpods"),
        NearbyKind.PHONE to listOf("iphone"),
        NearbyKind.TABLET to listOf("ipad"),
        NearbyKind.COMPUTER to listOf("macbook", "imac", "mac mini", "mac studio"),
        NearbyKind.GLASSES to listOf(
            "frames", "glasses", "xreal", "nreal", "rokid", "vuzix", "even g1", "rayneo", "solos",
        ),
        NearbyKind.HEARING_AID to listOf(
            "hearing", "phonak", "oticon", "resound", "signia", "starkey", "widex", "jabra enhance", "cochlear",
        ),
        NearbyKind.HEADPHONES to listOf(
            "buds", "headphone", "headset", "earbud", "beats", "wh-", "wf-", "jabra", "bose qc", "bose nc", "pods", "liberty",
            "jlab", "skullcandy", "shokz", "sennheiser", "momentum", "quietcomfort", "nothing ear", "earfun",
        ),
        NearbyKind.FITNESS to listOf(
            "polar h", "hrm", "heart rate", "cadence", "kickr", "wahoo", "peloton", "treadmill", "rower", "concept2", "stryd",
            "power meter", "zwift", "echelon",
        ),
        NearbyKind.WATCH to listOf(
            "watch", "band", "fitbit", "garmin", "mi smart", "amazfit", "whoop", "oura", "versa", "inspire", "forerunner",
            "fenix", "venu", "vivoactive", "instinct", "polar", "suunto", "coros", "ticwatch", "fossil", "galaxy fit",
        ),
        NearbyKind.SPEAKER to listOf(
            "speaker", "jbl", "soundlink", "sonos", "echo", "homepod", "boombox", "soundbar", "sound bar", "ue boom", "megaboom",
            "wonderboom", "soundcore", "marshall", "beosound", "ultimate ears", "revolve",
        ),
        NearbyKind.TV to listOf(
            "tv", "roku", "fire stick", "firetv", "chromecast", "bravia", "tcl", "hisense", "vizio", "webos", "projector",
            "shield", "mi box",
        ),
        NearbyKind.TABLET to listOf("galaxy tab", "fire hd", "tablet", "kindle", "kobo", "remarkable", "boox"),
        NearbyKind.COMPUTER to listOf(
            "laptop", "desktop", "thinkpad", "chromebook", "surface", "xps", "dell", "lenovo", "elitebook", "zenbook",
            "vivobook",
        ),
        NearbyKind.GAMING to listOf(
            "controller", "gamepad", "xbox", "dualsense", "dualshock", "joy-con", "8bitdo", "nintendo", "playstation",
            "steam deck",
        ),
        NearbyKind.INPUT to listOf(
            "keyboard", "mouse", "trackpad", "mx ", "pencil", "stylus", "remote", "presenter", "logi",
        ),
        NearbyKind.TAG to listOf(
            "tile", "airtag", "smarttag", "tracker", "chipolo", "find my", "pebblebee", "beacon", "estimote", "kontakt",
            "tractive", "fi collar", "itag",
        ),
        NearbyKind.DRONE to listOf("mavic", "drone", "tello", "skydio", "anafi", "dji mini", "dji air", "avata"),
        NearbyKind.CAMERA to listOf(
            "gopro", "insta360", "camera", "osmo", "canon eos", "fujifilm", "lumix", "nikon", "ricoh", "theta",
        ),
        NearbyKind.PRINTER to listOf(
            "printer", "deskjet", "officejet", "laserjet", "epson", "phomemo", "niimbot", "brother", "zebra", "instax",
            "sprocket",
        ),
        NearbyKind.RIDE to listOf("scooter", "segway", "ninebot", "e-bike", "ebike", "vanmoof", "onewheel", "boosted"),
        NearbyKind.CAR to listOf(
            "my car", "carplay", "tesla", "toyota", "honda", "uconnect", "ford sync", "bmw", "audi mmi", "my audi", "mercedes",
            "hyundai", "kia ", "subaru", "nissan", "mazda", "volvo", "chevrolet", "jeep", "rivian", "polestar", "my vw", "obd",
            "car multimedia",
        ),
        NearbyKind.HEALTH to listOf(
            "glucose", "dexcom", "libre", "omron", "blood pressure", "thermometer", "oximeter", "withings", "qardio", "ihealth",
            "cgm", "inhaler",
        ),
        NearbyKind.SCALE to listOf("scale", "renpho", "body comp", "fitindex"),
        NearbyKind.LOCK to listOf(
            "smart lock", "yale", "schlage", "nuki", "lockly", "ultraloq", "kwikset", "level bolt",
        ),
        NearbyKind.SMART_HOME to listOf(
            "thermostat", "ecobee", "nest ", "switchbot", "smart plug", "meross", "kasa", "tuya", "sensor", "aqara", "ruuvi",
            "inkbird", "gvh5", "govee_h5", "eve energy", "eve motion", "eve door", "eve room", "eve thermo", "doorbell",
            "hygrometer",
        ),
        NearbyKind.LIGHT to listOf(
            "govee", "gbk_", "ihoment", "philips hue", "hue ", "lifx", "nanoleaf", "bulb", "lamp", "led strip", "ledble",
            "elk-bledom", "triones", "light strip", "lightstrip", "smart light", "twinkly",
        ),
        NearbyKind.APPLIANCE to listOf(
            "vertuo", "nespresso", "coffee", "kettle", "fridge", "washer", "dryer", "oven", "roomba", "vacuum", "air fryer",
            "airfryer", "anova", "instant pot", "toothbrush", "oral-b", "sonicare", "shaver", "dyson", "irobot", "roborock",
            "robovac", "ecovacs", "thermomix", "traeger", "meater", "microwave", "purifier", "humidifier",
        ),
        NearbyKind.PHONE to listOf(
            "galaxy", "pixel", "phone", "oneplus", "moto", "redmi", "xperia", "nokia", "oppo", "vivo", "realme", "poco",
        ),
    )

    private val APPEARANCE_KINDS: Map<Int, NearbyKind> = mapOf(
        1 to NearbyKind.PHONE, 2 to NearbyKind.COMPUTER, 3 to NearbyKind.WATCH, 5 to NearbyKind.TV, 6 to NearbyKind.INPUT,
        7 to NearbyKind.GLASSES, 8 to NearbyKind.TAG, 9 to NearbyKind.TAG, 10 to NearbyKind.SPEAKER, 12 to NearbyKind.HEALTH,
        13 to NearbyKind.FITNESS, 14 to NearbyKind.HEALTH, 16 to NearbyKind.HEALTH, 17 to NearbyKind.FITNESS, 18 to NearbyKind.FITNESS,
        21 to NearbyKind.SMART_HOME, 22 to NearbyKind.LIGHT, 23 to NearbyKind.APPLIANCE, 24 to NearbyKind.SMART_HOME, 25 to NearbyKind.APPLIANCE,
        26 to NearbyKind.APPLIANCE, 27 to NearbyKind.SMART_HOME, 28 to NearbyKind.LOCK, 31 to NearbyKind.LIGHT, 32 to NearbyKind.SMART_HOME,
        33 to NearbyKind.SPEAKER, 35 to NearbyKind.CAR, 36 to NearbyKind.APPLIANCE, 37 to NearbyKind.HEADPHONES, 38 to NearbyKind.DRONE,
        39 to NearbyKind.TV, 40 to NearbyKind.TV, 41 to NearbyKind.HEARING_AID, 42 to NearbyKind.GAMING, 49 to NearbyKind.HEALTH,
        50 to NearbyKind.SCALE, 51 to NearbyKind.RIDE, 52 to NearbyKind.HEALTH, 53 to NearbyKind.HEALTH, 54 to NearbyKind.HEALTH,
    )

    private val SERVICE_KINDS: Map<Int, NearbyKind> = mapOf(
        0x1854 to NearbyKind.HEARING_AID, 0xFDF0 to NearbyKind.HEARING_AID, 0x184E to NearbyKind.HEADPHONES, 0x1850 to NearbyKind.HEADPHONES,
        0x1844 to NearbyKind.HEADPHONES, 0x1812 to NearbyKind.INPUT, 0x180D to NearbyKind.FITNESS, 0x1814 to NearbyKind.FITNESS,
        0x1816 to NearbyKind.FITNESS, 0x1818 to NearbyKind.FITNESS, 0x1826 to NearbyKind.FITNESS, 0x1810 to NearbyKind.HEALTH,
        0x1808 to NearbyKind.HEALTH, 0x181F to NearbyKind.HEALTH, 0x1809 to NearbyKind.HEALTH, 0x1822 to NearbyKind.HEALTH,
        0x183A to NearbyKind.HEALTH, 0x181D to NearbyKind.SCALE, 0x181B to NearbyKind.SCALE, 0xFEAA to NearbyKind.TAG,
        0xFEED to NearbyKind.TAG, 0xFD6F to NearbyKind.PHONE,
    )

    private val COMPANY_KINDS: List<Pair<List<Int>, NearbyKind>> = listOf(
        listOf(0x00D2, 0x0822) to NearbyKind.TAG,
        listOf(0x009E, 0x0310, 0x0057, 0x05A7) to NearbyKind.SPEAKER,
        listOf(0x0087, 0x0157) to NearbyKind.WATCH,
        listOf(0x006B) to NearbyKind.FITNESS,
        listOf(0x01DA) to NearbyKind.INPUT,
        listOf(0x0499) to NearbyKind.SMART_HOME,
        listOf(0x004C, 0x0075, 0x00E0) to NearbyKind.PHONE,
        listOf(0x0006) to NearbyKind.COMPUTER,
    )

    private val GAMING_APPEARANCES: Set<Int> = setOf(0x03C3, 0x03C4)
    private const val BASE_UUID_LOW = -0x7fffff7fa064cb05L

    fun kind(name: String?, companyIds: List<Int>, services: List<Int> = emptyList(), appearance: Int? = null): NearbyKind {
        val lowered = name?.trim()?.lowercase().orEmpty()
        if (lowered.isNotEmpty()) {
            NAME_KINDS.firstOrNull { (_, words) -> words.any { it in lowered } }?.let { return it.first }
        }
        if (appearance != null) {
            if (appearance in GAMING_APPEARANCES) return NearbyKind.GAMING
            APPEARANCE_KINDS[appearance shr 6]?.let { return it }
        }
        services.firstNotNullOfOrNull { SERVICE_KINDS[it] }?.let { return it }
        COMPANY_KINDS.firstOrNull { (ids, _) -> ids.any { it in companyIds } }?.let { return it.second }
        return NearbyKind.OTHER
    }

    fun shortUuid(uuid: UUID): Int? {
        if (uuid.leastSignificantBits != BASE_UUID_LOW || (uuid.mostSignificantBits and 0xFFFFFFFFL) != 0x1000L) return null
        val top = uuid.mostSignificantBits ushr 32
        return if (top <= 0xFFFF) top.toInt() else null
    }

    fun appearance(record: ByteArray?): Int? {
        if (record == null) return null
        var at = 0
        while (at < record.size) {
            val length = record[at].toInt() and 0xFF
            if (length == 0 || at + length >= record.size) return null
            if ((record[at + 1].toInt() and 0xFF) == 0x19 && length >= 3) {
                return (record[at + 2].toInt() and 0xFF) or ((record[at + 3].toInt() and 0xFF) shl 8)
            }
            at += length + 1
        }
        return null
    }
    fun maker(companyIds: List<Int>): String? = companyIds.firstNotNullOfOrNull { MAKERS[it] }

    fun device(advert: GlassesAdvert, nowMs: Long): NearbyDevice? {
        if (advert.rssi >= 0 || advert.rssi < -127) return null
        val name = advert.name?.trim()?.takeIf { it.isNotEmpty() }
        return NearbyDevice(advert.address, name, maker(advert.companyIds), kind(name, advert.companyIds, advert.serviceUuids, advert.appearance), advert.rssi, nowMs)
    }
}

class GlassesCensus(val forgetAfterMs: Long = 45_000L, val limit: Int = 60) {
    private val seen = LinkedHashMap<String, NearbyDevice>()

    fun observe(device: NearbyDevice) {
        val previous = seen[device.address]
        val rssi = previous?.let { ((it.rssi * 3 + device.rssi) / 4.0).toInt() } ?: device.rssi
        seen[device.address] = if (previous != null && device.name == null) device.copy(name = previous.name, maker = device.maker ?: previous.maker, kind = if (previous.kind == NearbyKind.OTHER) device.kind else previous.kind, rssi = rssi) else device.copy(rssi = rssi)
        if (seen.size > limit * 2) prune(device.seenAtMs)
    }

    fun devices(nowMs: Long): List<NearbyDevice> {
        prune(nowMs)
        return seen.values.sortedWith(compareByDescending<NearbyDevice> { it.rssi }.thenBy { it.address }).take(limit)
    }

    fun reset() = seen.clear()

    private fun prune(nowMs: Long) {
        seen.entries.removeAll { nowMs - it.value.seenAtMs > forgetAfterMs }
        if (seen.size > limit * 2) {
            val keep = seen.values.sortedByDescending { it.seenAtMs }.take(limit).map { it.address }.toSet()
            seen.keys.retainAll(keep)
        }
    }
}
