import Foundation

public enum NearbyKind: String, CaseIterable, Sendable {
    case phone
    case tablet
    case computer
    case headphones
    case hearingAid
    case speaker
    case tv
    case watch
    case glasses
    case fitness
    case health
    case scale
    case input
    case gaming
    case tag
    case car
    case ride
    case drone
    case camera
    case printer
    case lock
    case smartHome
    case light
    case appliance
    case other

    public var label: String {
        switch self {
        case .phone: return "Phone"
        case .tablet: return "Tablet or e-reader"
        case .computer: return "Computer"
        case .headphones: return "Headphones"
        case .hearingAid: return "Hearing aid"
        case .speaker: return "Speaker"
        case .tv: return "TV or streamer"
        case .watch: return "Watch or band"
        case .glasses: return "Other smart glasses"
        case .fitness: return "Fitness sensor"
        case .health: return "Health monitor"
        case .scale: return "Scale"
        case .input: return "Keyboard, mouse or remote"
        case .gaming: return "Game controller"
        case .tag: return "Tracker tag or beacon"
        case .car: return "Car"
        case .ride: return "Scooter or e-bike"
        case .drone: return "Drone"
        case .camera: return "Camera"
        case .printer: return "Printer"
        case .lock: return "Smart lock"
        case .smartHome: return "Smart home"
        case .light: return "Light"
        case .appliance: return "Appliance"
        case .other: return "Bluetooth device"
        }
    }

    public var icon: String {
        switch self {
        case .phone: return "smartphone"
        case .tablet: return "tablet"
        case .computer: return "laptop"
        case .headphones: return "headphones"
        case .hearingAid: return "hearing"
        case .speaker: return "speaker"
        case .tv: return "tv"
        case .watch: return "watch"
        case .glasses: return "visibility"
        case .fitness: return "fitness_center"
        case .health: return "monitor_heart"
        case .scale: return "monitor_weight"
        case .input: return "keyboard"
        case .gaming: return "sports_esports"
        case .tag: return "location_on"
        case .car: return "directions_car"
        case .ride: return "electric_scooter"
        case .drone: return "flight"
        case .camera: return "photo_camera"
        case .printer: return "print"
        case .lock: return "lock"
        case .smartHome: return "home"
        case .light: return "lightbulb"
        case .appliance: return "kitchen"
        case .other: return "bluetooth"
        }
    }
}

public struct NearbyDevice: Equatable, Identifiable, Sendable {
    public let id: String
    public let name: String?
    public let maker: String?
    public let kind: NearbyKind
    public let rssi: Int
    public let seenAt: Date

    public init(id: String, name: String?, maker: String?, kind: NearbyKind, rssi: Int, seenAt: Date) {
        self.id = id
        self.name = name
        self.maker = maker
        self.kind = kind
        self.rssi = rssi
        self.seenAt = seenAt
    }

    public var label: String {
        if let name = name { return name }
        if let maker = maker { return "\(maker) device" }
        return kind.label
    }

    public var distance: String { GlassesSignatures.distance(rssi) }
}

public enum NearbyKinds {
    public static let makers: [Int: String] = [
        0x004C: "Apple",
        0x0075: "Samsung",
        0x00E0: "Google",
        0x0006: "Microsoft",
        0x009E: "Bose",
        0x012D: "Sony",
        0x0087: "Garmin",
        0x0157: "Huami",
        0x038F: "Xiaomi",
        0x027D: "Huawei",
        0x0059: "Nordic",
        0x000F: "Broadcom",
        0x0171: "Amazon",
        0x0310: "JBL",
        0x0057: "Harman",
        0x00D2: "Tile",
        0x0822: "Tile",
        0x0499: "Ruuvi",
        0x006B: "Polar",
        0x05A7: "Sonos",
        0x01DA: "Logitech",
    ]

    private static let nameKinds: [(NearbyKind, [String])] = [
        (.headphones, ["airpods"]),
        (.phone, ["iphone"]),
        (.tablet, ["ipad"]),
        (.computer, ["macbook", "imac", "mac mini", "mac studio"]),
        (.glasses, ["frames", "glasses", "xreal", "nreal", "rokid", "vuzix", "even g1", "rayneo", "solos"]),
        (.hearingAid, [
            "hearing", "phonak", "oticon", "resound", "signia", "starkey", "widex", "jabra enhance", "cochlear",
        ]),
        (.headphones, [
            "buds", "headphone", "headset", "earbud", "beats", "wh-", "wf-", "jabra", "bose qc", "bose nc", "pods", "liberty",
            "jlab", "skullcandy", "shokz", "sennheiser", "momentum", "quietcomfort", "nothing ear", "earfun",
        ]),
        (.fitness, [
            "polar h", "hrm", "heart rate", "cadence", "kickr", "wahoo", "peloton", "treadmill", "rower", "concept2", "stryd",
            "power meter", "zwift", "echelon",
        ]),
        (.watch, [
            "watch", "band", "fitbit", "garmin", "mi smart", "amazfit", "whoop", "oura", "versa", "inspire", "forerunner",
            "fenix", "venu", "vivoactive", "instinct", "polar", "suunto", "coros", "ticwatch", "fossil", "galaxy fit",
        ]),
        (.speaker, [
            "speaker", "jbl", "soundlink", "sonos", "echo", "homepod", "boombox", "soundbar", "sound bar", "ue boom", "megaboom",
            "wonderboom", "soundcore", "marshall", "beosound", "ultimate ears", "revolve",
        ]),
        (.tv, [
            "tv", "roku", "fire stick", "firetv", "chromecast", "bravia", "tcl", "hisense", "vizio", "webos", "projector",
            "shield", "mi box",
        ]),
        (.tablet, ["galaxy tab", "fire hd", "tablet", "kindle", "kobo", "remarkable", "boox"]),
        (.computer, [
            "laptop", "desktop", "thinkpad", "chromebook", "surface", "xps", "dell", "lenovo", "elitebook", "zenbook",
            "vivobook",
        ]),
        (.gaming, [
            "controller", "gamepad", "xbox", "dualsense", "dualshock", "joy-con", "8bitdo", "nintendo", "playstation",
            "steam deck",
        ]),
        (.input, ["keyboard", "mouse", "trackpad", "mx ", "pencil", "stylus", "remote", "presenter", "logi"]),
        (.tag, [
            "tile", "airtag", "smarttag", "tracker", "chipolo", "find my", "pebblebee", "beacon", "estimote", "kontakt",
            "tractive", "fi collar", "itag",
        ]),
        (.drone, ["mavic", "drone", "tello", "skydio", "anafi", "dji mini", "dji air", "avata"]),
        (.camera, ["gopro", "insta360", "camera", "osmo", "canon eos", "fujifilm", "lumix", "nikon", "ricoh", "theta"]),
        (.printer, [
            "printer", "deskjet", "officejet", "laserjet", "epson", "phomemo", "niimbot", "brother", "zebra", "instax",
            "sprocket",
        ]),
        (.ride, ["scooter", "segway", "ninebot", "e-bike", "ebike", "vanmoof", "onewheel", "boosted"]),
        (.car, [
            "my car", "carplay", "tesla", "toyota", "honda", "uconnect", "ford sync", "bmw", "audi mmi", "my audi", "mercedes",
            "hyundai", "kia ", "subaru", "nissan", "mazda", "volvo", "chevrolet", "jeep", "rivian", "polestar", "my vw", "obd",
            "car multimedia",
        ]),
        (.health, [
            "glucose", "dexcom", "libre", "omron", "blood pressure", "thermometer", "oximeter", "withings", "qardio", "ihealth",
            "cgm", "inhaler",
        ]),
        (.scale, ["scale", "renpho", "body comp", "fitindex"]),
        (.lock, ["smart lock", "yale", "schlage", "nuki", "lockly", "ultraloq", "kwikset", "level bolt"]),
        (.smartHome, [
            "thermostat", "ecobee", "nest ", "switchbot", "smart plug", "meross", "kasa", "tuya", "sensor", "aqara", "ruuvi",
            "inkbird", "gvh5", "govee_h5", "eve energy", "eve motion", "eve door", "eve room", "eve thermo", "doorbell",
            "hygrometer",
        ]),
        (.light, [
            "govee", "gbk_", "ihoment", "philips hue", "hue ", "lifx", "nanoleaf", "bulb", "lamp", "led strip", "ledble",
            "elk-bledom", "triones", "light strip", "lightstrip", "smart light", "twinkly",
        ]),
        (.appliance, [
            "vertuo", "nespresso", "coffee", "kettle", "fridge", "washer", "dryer", "oven", "roomba", "vacuum", "air fryer",
            "airfryer", "anova", "instant pot", "toothbrush", "oral-b", "sonicare", "shaver", "dyson", "irobot", "roborock",
            "robovac", "ecovacs", "thermomix", "traeger", "meater", "microwave", "purifier", "humidifier",
        ]),
        (.phone, [
            "galaxy", "pixel", "phone", "oneplus", "moto", "redmi", "xperia", "nokia", "oppo", "vivo", "realme", "poco",
        ]),
    ]

    private static let appearanceKinds: [Int: NearbyKind] = [
        1: .phone, 2: .computer, 3: .watch, 5: .tv, 6: .input, 7: .glasses,
        8: .tag, 9: .tag, 10: .speaker, 12: .health, 13: .fitness, 14: .health,
        16: .health, 17: .fitness, 18: .fitness, 21: .smartHome, 22: .light, 23: .appliance,
        24: .smartHome, 25: .appliance, 26: .appliance, 27: .smartHome, 28: .lock, 31: .light,
        32: .smartHome, 33: .speaker, 35: .car, 36: .appliance, 37: .headphones, 38: .drone,
        39: .tv, 40: .tv, 41: .hearingAid, 42: .gaming, 49: .health, 50: .scale,
        51: .ride, 52: .health, 53: .health, 54: .health,
    ]

    private static let serviceKinds: [Int: NearbyKind] = [
        0x1854: .hearingAid, 0xFDF0: .hearingAid, 0x184E: .headphones, 0x1850: .headphones, 0x1844: .headphones,
        0x1812: .input, 0x180D: .fitness, 0x1814: .fitness, 0x1816: .fitness, 0x1818: .fitness,
        0x1826: .fitness, 0x1810: .health, 0x1808: .health, 0x181F: .health, 0x1809: .health,
        0x1822: .health, 0x183A: .health, 0x181D: .scale, 0x181B: .scale, 0xFEAA: .tag,
        0xFEED: .tag, 0xFD6F: .phone,
    ]

    private static let companyKinds: [([Int], NearbyKind)] = [
        ([0x00D2, 0x0822], .tag),
        ([0x009E, 0x0310, 0x0057, 0x05A7], .speaker),
        ([0x0087, 0x0157], .watch),
        ([0x006B], .fitness),
        ([0x01DA], .input),
        ([0x0499], .smartHome),
        ([0x004C, 0x0075, 0x00E0], .phone),
        ([0x0006], .computer),
    ]

    public static func kind(name: String?, companyIds: [Int], services: [Int] = [], appearance: Int? = nil) -> NearbyKind {
        let lowered: String = name?.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() ?? ""
        if !lowered.isEmpty, let match = nameKinds.first(where: { $0.1.contains { lowered.contains($0) } }) {
            return match.0
        }
        if let appearance = appearance {
            if [0x03C3, 0x03C4].contains(appearance) { return .gaming }
            if let kind = appearanceKinds[appearance >> 6] { return kind }
        }
        if let kind = services.lazy.compactMap({ serviceKinds[$0] }).first { return kind }
        if let match = companyKinds.first(where: { !Set($0.0).isDisjoint(with: companyIds) }) { return match.1 }
        return .other
    }

    public static func shortUuid(_ text: String) -> Int? {
        let upper: String = text.uppercased()
        if upper.count == 4 { return Int(upper, radix: 16) }
        guard upper.count == 36, upper.hasPrefix("0000"), upper.hasSuffix("-0000-1000-8000-00805F9B34FB") else { return nil }
        return Int(upper.dropFirst(4).prefix(4), radix: 16)
    }
    public static func maker(_ companyIds: [Int]) -> String? {
        companyIds.lazy.compactMap { makers[$0] }.first
    }

    public static func device(_ advert: GlassesAdvert, now: Date) -> NearbyDevice? {
        guard advert.rssi < 0, advert.rssi >= -127 else { return nil }
        let trimmed: String? = advert.name?.trimmingCharacters(in: .whitespacesAndNewlines)
        let name: String? = (trimmed?.isEmpty ?? true) ? nil : trimmed
        return NearbyDevice(
            id: advert.identifier, name: name, maker: maker(advert.companyIds), kind: kind(name: name, companyIds: advert.companyIds, services: advert.serviceUuids, appearance: advert.appearance),
            rssi: advert.rssi, seenAt: now)
    }
}

public struct GlassesCensus: Sendable {
    public let forgetAfter: TimeInterval
    public let limit: Int
    private var seen: [String: NearbyDevice] = [:]

    public init(forgetAfter: TimeInterval = 45, limit: Int = 60) {
        self.forgetAfter = forgetAfter
        self.limit = limit
    }

    public mutating func observe(_ device: NearbyDevice) {
        let rssi: Int = seen[device.id].map { ($0.rssi * 3 + device.rssi) / 4 } ?? device.rssi
        if let previous = seen[device.id], device.name == nil {
            seen[device.id] = NearbyDevice(
                id: device.id, name: previous.name, maker: device.maker ?? previous.maker, kind: previous.kind == .other ? device.kind : previous.kind, rssi: rssi,
                seenAt: device.seenAt)
        } else {
            seen[device.id] = NearbyDevice(
                id: device.id, name: device.name, maker: device.maker, kind: device.kind, rssi: rssi, seenAt: device.seenAt)
        }
        if seen.count > limit * 2 { prune(device.seenAt) }
    }

    public mutating func devices(_ now: Date) -> [NearbyDevice] {
        prune(now)
        let sorted: [NearbyDevice] = seen.values.sorted { $0.rssi == $1.rssi ? $0.id < $1.id : $0.rssi > $1.rssi }
        return Array(sorted.prefix(limit))
    }

    public mutating func reset() {
        seen.removeAll()
    }

    private mutating func prune(_ now: Date) {
        seen = seen.filter { now.timeIntervalSince($0.value.seenAt) <= forgetAfter }
        if seen.count > limit * 2 {
            let keep: Set<String> = Set(seen.values.sorted { $0.seenAt > $1.seenAt }.prefix(limit).map(\.id))
            seen = seen.filter { keep.contains($0.key) }
        }
    }
}
