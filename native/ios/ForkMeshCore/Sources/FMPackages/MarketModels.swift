import FMBase
import Foundation

public enum PackageDataScope: String, Sendable, Equatable {
    case organization
    case account
    case none = ""
}

public struct PackagePermission: Equatable, Sendable, Identifiable {
    public var id: String
    public var label: String
    public var risk: String

    public init(id: String, label: String, risk: String) {
        self.id = id
        self.label = label
        self.risk = risk
    }
}

public struct PackageScreenshot: Equatable, Sendable, Identifiable {
    public var url: URL
    public var alt: String

    public var id: String { url.absoluteString }

    public init(url: URL, alt: String = "") {
        self.url = url
        self.alt = alt
    }

    public init?(json: JSONObject) {
        guard let url = URL(string: json.str("url")), url.scheme == "https", url.host?.isEmpty == false else { return nil }
        self.init(url: url, alt: PackagesText.bounded(json.str("alt"), 160))
    }
}

public enum PackageSettingType: String, Equatable, Sendable {
    case boolean
    case choice
}

public struct PackageSettingOption: Equatable, Sendable, Identifiable {
    public var value: String
    public var label: String

    public var id: String { value }

    public init(value: String, label: String) {
        self.value = value
        self.label = label
    }
}

public struct PackageSetting: Equatable, Sendable, Identifiable {
    public var key: String
    public var type: PackageSettingType
    public var label: String
    public var defaultValue: JSONValue
    public var options: [PackageSettingOption]

    public var id: String { key }

    public init(key: String, type: PackageSettingType, label: String, defaultValue: JSONValue, options: [PackageSettingOption] = []) {
        self.key = key
        self.type = type
        self.label = label
        self.defaultValue = defaultValue
        self.options = options
    }

    public init?(json: JSONObject) {
        let key = json.str("key")
        guard !key.isEmpty, key.count <= 63, let type = PackageSettingType(rawValue: json.str("type")) else { return nil }
        let label = PackagesText.bounded(json.str("label"), 120)
        switch type {
        case .boolean:
            self.init(key: key, type: type, label: label.isEmpty ? key : label, defaultValue: .bool(json.bool("default")))
        case .choice:
            let options = json.maps("options").prefix(40).compactMap { (option: JSONObject) -> PackageSettingOption? in
                let value = option.str("value")
                guard !value.isEmpty else { return nil }
                let text = PackagesText.bounded(option.str("label"), 80)
                return PackageSettingOption(value: value, label: text.isEmpty ? value : text)
            }
            guard let first = options.first else { return nil }
            let wanted = json.str("default")
            let fallback = options.contains { $0.value == wanted } ? wanted : first.value
            self.init(key: key, type: type, label: label.isEmpty ? key : label, defaultValue: .string(fallback), options: options)
        }
    }

    public var json: JSONObject {
        var out: JSONObject = ["key": .string(key), "type": .string(type.rawValue), "label": .string(label), "default": defaultValue]
        if type == .choice {
            out["options"] = .array(options.map { .object(["value": .string($0.value), "label": .string($0.label)]) })
        }
        return out
    }

    public func accepts(_ value: JSONValue) -> Bool {
        switch type {
        case .boolean: return value.boolValue != nil
        case .choice: return options.contains { .string($0.value) == value }
        }
    }
}

public struct PackageMonitors: Equatable, Sendable {
    public var systems: [String]
    public var prefixes: [String]

    public init(systems: [String] = [], prefixes: [String] = []) {
        self.systems = systems
        self.prefixes = prefixes
    }

    public init(json: JSONObject) {
        self.init(
            systems: json.strings("systems").filter { !$0.isEmpty }.prefix(80).map { $0 },
            prefixes: json.strings("prefixes").filter { !$0.isEmpty }.prefix(20).map { $0 })
    }

    public var isEmpty: Bool { systems.isEmpty && prefixes.isEmpty }

    public var json: JSONObject {
        ["systems": .array(systems.map(JSONValue.string)), "prefixes": .array(prefixes.map(JSONValue.string))]
    }
}

public enum PackagePlace: String, CaseIterable, Equatable, Hashable, Sendable {
    case mobile
    case desktop
    case web

    public static let here: PackagePlace = .mobile

    public var title: String {
        switch self {
        case .mobile: return "Phone"
        case .desktop: return "Desktop"
        case .web: return "Web"
        }
    }

    public var symbol: String {
        switch self {
        case .mobile: return "iphone"
        case .desktop: return "desktopcomputer"
        case .web: return "globe"
        }
    }

    public static func list(_ json: JSONObject, _ key: String) -> [PackagePlace] {
        guard json.contains(key) else { return allCases }
        let wanted = Set(json.strings(key).compactMap(PackagePlace.init(rawValue:)))
        let ordered = allCases.filter(wanted.contains)
        return ordered.isEmpty ? allCases : ordered
    }

    public static func ordered(_ places: some Sequence<PackagePlace>) -> [PackagePlace] {
        let wanted = Set(places)
        return allCases.filter(wanted.contains)
    }

    public static func json(_ places: [PackagePlace]) -> JSONValue { .array(places.map { .string($0.rawValue) }) }
}

public enum PackagePhoneSurface: Equatable, Sendable {
    case native(String)
    case web(String)
    case desktopOnly
    case worldOnly
    case everyDevice
}

public struct MarketPackage: Equatable, Sendable, Identifiable {
    public var id: String
    public var market: String
    public var name: String
    public var version: String
    public var summary: String
    public var category: String
    public var type: String
    public var desktop: Bool
    public var webRoute: String
    public var worker: Bool
    public var world: Bool
    public var api: String
    public var dataScope: PackageDataScope
    public var bundled: Bool
    public var permissions: [PackagePermission]
    public var screenshots: [PackageScreenshot]
    public var defaultOn: Bool
    public var installedBefore: String
    public var accountSync: Bool
    public var dependencies: [String]
    public var settings: [PackageSetting]
    public var monitors: PackageMonitors
    public var places: [PackagePlace]
    public var free: Bool
    public var publisher: String
    public var verified: Bool

    public init(
        id: String, market: String = "forkmesh", name: String, version: String = "", summary: String = "",
        category: String = "", type: String = "", desktop: Bool = false, webRoute: String = "", worker: Bool = false,
        world: Bool = false, api: String = "", dataScope: PackageDataScope = .none, bundled: Bool = false,
        permissions: [PackagePermission] = [], screenshots: [PackageScreenshot] = [], defaultOn: Bool = false,
        installedBefore: String = "", accountSync: Bool = false, dependencies: [String] = [], settings: [PackageSetting] = [],
        monitors: PackageMonitors = PackageMonitors(), places: [PackagePlace] = PackagePlace.allCases, free: Bool = true,
        publisher: String = "", verified: Bool = false
    ) {
        self.id = id
        self.market = market
        self.name = name
        self.version = version
        self.summary = summary
        self.category = category
        self.type = type
        self.desktop = desktop
        self.webRoute = webRoute
        self.worker = worker
        self.world = world
        self.api = api
        self.dataScope = dataScope
        self.bundled = bundled
        self.permissions = permissions
        self.screenshots = screenshots
        self.defaultOn = defaultOn
        self.installedBefore = installedBefore
        self.accountSync = accountSync
        self.dependencies = dependencies
        self.settings = settings
        self.monitors = monitors
        self.places = PackagePlace.ordered(places)
        self.free = free
        self.publisher = publisher
        self.verified = verified
    }

    public init?(json: JSONObject) {
        let id = json.str("id")
        guard PackagesText.isPackageId(id) else { return nil }
        let surfaces = json.obj("surfaces")
        self.init(
            id: id, market: json.str("market", "forkmesh"), name: PackagesText.bounded(json.str("name"), 80),
            version: json.str("version"), summary: PackagesText.bounded(json.str("summary"), 600),
            category: json.str("category"), type: json.str("type"), desktop: surfaces.bool("desktop"),
            webRoute: surfaces.str("web"), worker: surfaces.bool("worker"), world: surfaces.bool("world"),
            api: json.str("api"), dataScope: PackageDataScope(rawValue: json.str("dataScope")) ?? .none,
            bundled: json.bool("bundled"),
            permissions: json.maps("permissions").prefix(40).map {
                PackagePermission(id: $0.str("id"), label: $0.str("label"), risk: $0.str("risk", "sensitive"))
            },
            screenshots: json.maps("screenshots").prefix(8).compactMap(PackageScreenshot.init(json:)),
            defaultOn: json.bool("default"), installedBefore: PackagesText.installDate(json.str("installedBefore")),
            accountSync: json.bool("accountSync"),
            dependencies: json.list("dependencies").prefix(20).compactMap { (entry: JSONValue) -> String? in
                let dependency = entry.stringValue ?? entry.str("id")
                return PackagesText.isPackageId(dependency) && dependency != id ? dependency : nil
            },
            settings: json.maps("settings").prefix(20).compactMap(PackageSetting.init(json:)),
            monitors: PackageMonitors(json: json.obj("monitors")), places: PackagePlace.list(json, "places"),
            free: json.optBool("free") ?? true, publisher: PackagesText.bounded(json.str("publisher"), 80),
            verified: json.bool("verified"))
        if name.isEmpty { name = id }
    }

    public static let nativeDestinations: [String: String] = [
        "crm": "crm", "marketing": "marketing", "analytics": "analytics", "competitor": "competitors",
        "ledger": "ledger", "vault": "vault", "calories": "calories", "notes": "notes", "tasks": "tasks",
        "calendar": "calendar", "timers": "timers", "steps": "steps", "water": "water", "sleep": "sleep",
        "sugar-break": "sugarBreak", "stretch": "stretch", "food-guide": "foodGuide", "speech": "speech", "status-page": "status", "tour": "tour", "aquarium": "aquarium", "capture": "capture",
        "glasses-alert": "glassesAlert", "phone-security": "phoneSecurity", "eye-control": "eyeControl", "ember": "ember", "strata": "strata", "synapse": "synapse", "openwrt": "openWrtRouter", "files": "fileExplorer", "face-changer": "faceChanger", "video-mix-dj-studio": "videoMixDjStudio", "app-store-status": "appStoreStatus", "usage": "usage", "rss-reader": "rssReader", "instance-feed": "instanceFeed", "secure-drive": "secureDrive", "spider-room": "spiderRoom", "people": "people", "docs": "docs", "design-system": "designSystem",
        "agents": "Agents", "inbox": "Inbox", "chat": "Chat", "code": "currentRepo", "bug-reports": "reportBug",
        "issues": "repo-issues", "commits": "repo-commits", "branches": "repo-branches", "pull-requests": "repo-pulls",
        "releases": "repo-releases", "actions": "repo-actions", "discussions": "repo-discussions",
        "desktops": "desktops", "special-agents": "teammates", "shortcuts": "shortcuts", "hosts": "hosts",
        "network": "network", "coves": "coves", "on-device-ai": "localAI", "activity": "activity", "logs": "logs", "diagnostics": "diagnostics", "stats": "stats",
        "organizations": "orgs", "world": "world", "sign-in": "signIn", "workflows": "workflows",
    ]

    public var phoneSurface: PackagePhoneSurface {
        if let destination = MarketPackage.nativeDestinations[id] { return .native(destination) }
        if !webRoute.isEmpty { return .web(webRoute) }
        if accountSync { return .everyDevice }
        if world && !desktop { return .worldOnly }
        return .desktopOnly
    }

    public var installsPerOrganization: Bool { dataScope == .organization }

    public var hasSensitivePermissions: Bool { permissions.contains { $0.risk == "high" || $0.risk == "sensitive" } }

    public func runs(on place: MarketRunsOn) -> Bool {
        switch place {
        case .any: return true
        case .phone:
            if case .native = phoneSurface { return true }
            return false
        case .web: return !webRoute.isEmpty
        case .desktop: return desktop
        case .world: return world
        }
    }
}

public struct PackageCounts: Equatable, Sendable {
    public var installs: Int
    public var downloads: Int
    public var accounts: Int
    public var organizations: Int

    public init(installs: Int = 0, downloads: Int = 0, accounts: Int = 0, organizations: Int = 0) {
        self.installs = installs
        self.downloads = downloads
        self.accounts = accounts
        self.organizations = organizations
    }

    public init(json: JSONObject) {
        self.init(
            installs: max(0, json.int("installs")), downloads: max(0, json.int("downloads")),
            accounts: max(0, json.int("accounts")), organizations: max(0, json.int("organizations")))
    }

    public var total: Int { installs + accounts + organizations }
}

public struct PackageUsage: Equatable, Sendable {
    public var views: Int
    public var opens: Int
    public var people: Int

    public init(views: Int = 0, opens: Int = 0, people: Int = 0) {
        self.views = views
        self.opens = opens
        self.people = people
    }

    public init(json: JSONObject) {
        self.init(views: max(0, json.int("views")), opens: max(0, json.int("opens")), people: max(0, json.int("people")))
    }
}

public struct MarketSummary: Equatable, Sendable {
    public var ratings: [String: RatingSummary]
    public var usage: [String: PackageUsage]

    public init(ratings: [String: RatingSummary] = [:], usage: [String: PackageUsage] = [:]) {
        self.ratings = ratings
        self.usage = usage
    }

    public init(json: JSONObject) {
        var ratings: [String: RatingSummary] = [:]
        for (key, value) in json.obj("packages") where PackagesText.isPackageId(key) {
            ratings[key] = RatingSummary(json: value.objectOrEmpty)
        }
        var usage: [String: PackageUsage] = [:]
        for (key, value) in json.obj("usage") where PackagesText.isPackageId(key) {
            usage[key] = PackageUsage(json: value.objectOrEmpty)
        }
        self.init(ratings: ratings, usage: usage)
    }
}

public struct RatingSummary: Equatable, Sendable {
    public var average: Double
    public var count: Int
    public var histogram: [Int]

    public init(average: Double = 0, count: Int = 0, histogram: [Int] = [0, 0, 0, 0, 0]) {
        self.average = average
        self.count = count
        self.histogram = histogram
    }

    public init(json: JSONObject) {
        let bars = json.obj("histogram")
        self.init(
            average: min(5, max(0, json.double("average"))), count: max(0, json.int("count")),
            histogram: (1...5).map { max(0, bars.int(String($0))) })
    }

    public var label: String {
        count == 0 ? "No ratings yet" : String(format: "%.1f (%d)", average, count)
    }
}

public struct PluginInstall: Equatable, Sendable, Identifiable {
    public var id: String
    public var installed: Bool
    public var installedBy: String
    public var installedAt: Int64
    public var serverData: Bool
    public var hasData: Bool

    public init(json: JSONObject) {
        id = json.str("id")
        installed = json.bool("installed")
        installedBy = json.str("installedBy")
        installedAt = json.int64("installedAt")
        serverData = json.bool("serverData")
        hasData = json.bool("hasData")
    }

    public init(id: String, installed: Bool, installedBy: String = "", installedAt: Int64 = 0) {
        self.id = id
        self.installed = installed
        self.installedBy = installedBy
        self.installedAt = installedAt
        serverData = false
        hasData = false
    }
}

public struct OrgPackageState: Equatable, Sendable, Identifiable {
    public var organization: String
    public var role: String
    public var canManage: Bool
    public var plugins: [PluginInstall]

    public var id: String { organization }

    public init(organization: String, role: String, canManage: Bool, plugins: [PluginInstall]) {
        self.organization = organization
        self.role = role
        self.canManage = canManage
        self.plugins = plugins
    }

    public init(json: JSONObject) {
        self.init(
            organization: json.str("organization"), role: json.str("role", "member"), canManage: json.bool("canManage"),
            plugins: json.maps("plugins").map(PluginInstall.init(json:)))
    }

    public func isInstalled(_ packageId: String) -> Bool {
        plugins.contains { $0.id == packageId && $0.installed }
    }

    public func removing(_ packageId: String) -> OrgPackageState {
        var state = self
        for index in state.plugins.indices where state.plugins[index].id == packageId {
            state.plugins[index] = PluginInstall(id: packageId, installed: false)
        }
        return state
    }
}

public struct PackageRemoval: Equatable, Sendable {
    public var packageId: String
    public var organization: String
    public var filename: String
    public var export: Data?

    public init(packageId: String, organization: String, filename: String = "", export: Data? = nil) {
        self.packageId = packageId
        self.organization = organization
        self.filename = filename
        self.export = export
    }

    public init(json: JSONObject, packageId: String, organization: String) {
        let export = json["export"].flatMap { $0.isNull ? nil : JSON.encodeData($0) }
        self.init(
            packageId: packageId, organization: organization, filename: "forkmesh-\(packageId)-\(organization).json",
            export: json.bool("hadData") ? export : nil)
    }
}

public enum MarketFilter: String, CaseIterable, Sendable {
    case all
    case installed
    case notInstalled = "not-installed"

    public init(route: String?) {
        self = MarketFilter(rawValue: (route ?? "").lowercased()) ?? .all
    }

    public var title: String {
        switch self {
        case .all: return "All"
        case .installed: return "Installed"
        case .notInstalled: return "Not installed"
        }
    }
}

public enum MarketSort: String, CaseIterable, Sendable {
    case featured
    case name
    case rating
    case installs
    case downloads
    case used
    case opened
    case installedFirst = "installed-first"

    public var title: String {
        switch self {
        case .featured: return "Featured"
        case .name: return "Name A–Z"
        case .rating: return "Top rated"
        case .installs: return "Most installed"
        case .downloads: return "Most downloaded"
        case .used: return "Most used"
        case .opened: return "Most opened"
        case .installedFirst: return "Installed first"
        }
    }

    public var symbol: String {
        switch self {
        case .featured: return "sparkles"
        case .name: return "textformat"
        case .rating: return "star"
        case .installs: return "arrow.down.app"
        case .downloads: return "arrow.down.circle"
        case .used: return "person.2"
        case .opened: return "hand.tap"
        case .installedFirst: return "checkmark.circle"
        }
    }
}

public enum MarketRunsOn: String, CaseIterable, Sendable {
    case any
    case phone
    case web
    case desktop
    case world

    public var title: String {
        switch self {
        case .any: return "Anywhere"
        case .phone: return "This phone"
        case .web: return "Web"
        case .desktop: return "Desktop"
        case .world: return "World"
        }
    }
}

public enum MarketPrice: String, CaseIterable, Sendable {
    case any
    case free
    case paid

    public var title: String {
        switch self {
        case .any: return "Any price"
        case .free: return "Free"
        case .paid: return "Paid"
        }
    }
}

public enum MarketInstallScope: String, CaseIterable, Sendable {
    case account
    case organization
    case device
    case desktop

    public var title: String {
        switch self {
        case .account: return "Your account"
        case .organization: return "An organization"
        case .device: return "This phone only"
        case .desktop: return "Your desktop"
        }
    }
}

public struct MarketFilters: Equatable, Sendable {
    public var category: String?
    public var kind: String?
    public var runsOn: MarketRunsOn
    public var price: MarketPrice
    public var scope: MarketInstallScope?
    public var publisher: String?
    public var verifiedOnly: Bool
    public var ratedOnly: Bool

    public init(
        category: String? = nil, kind: String? = nil, runsOn: MarketRunsOn = .any, price: MarketPrice = .any,
        scope: MarketInstallScope? = nil, publisher: String? = nil, verifiedOnly: Bool = false, ratedOnly: Bool = false
    ) {
        self.category = category
        self.kind = kind
        self.runsOn = runsOn
        self.price = price
        self.scope = scope
        self.publisher = publisher
        self.verifiedOnly = verifiedOnly
        self.ratedOnly = ratedOnly
    }

    public var activeCount: Int {
        [category != nil, kind != nil, runsOn != .any, price != .any, scope != nil, publisher != nil, verifiedOnly, ratedOnly]
            .filter { $0 }.count
    }
}

public enum MarketCardAction: Equatable, Sendable {
    case open
    case installed
    case installAccount
    case installDevice
    case installPhone
    case installOrganization(String)
    case chooseOrganization([String])
    case createOrganization
    case askAdmin
    case signIn
    case desktop
    case busy
}

public struct MarketInstallAllPlan: Equatable, Sendable {
    public var targets: [MarketPackage]
    public var skipped: Int

    public init(targets: [MarketPackage], skipped: Int) {
        self.targets = targets
        self.skipped = skipped
    }
}

public struct MarketInstallAllResult: Equatable, Sendable {
    public var installed: Int
    public var skipped: Int
    public var failed: Int

    public init(installed: Int, skipped: Int, failed: Int) {
        self.installed = installed
        self.skipped = skipped
        self.failed = failed
    }
}

public enum MarketUninstall: Equatable, Sendable {
    case account
    case device
    case phone
    case organization([String])
}

public enum PackagesText {
    static func isSlug(_ value: String, limit: Int) -> Bool {
        guard let first = value.unicodeScalars.first, value.count <= limit, CharacterSet.lowercaseLetters.contains(first)
        else { return false }
        return value.unicodeScalars.allSatisfy {
            ($0.value >= 97 && $0.value <= 122) || ($0.value >= 48 && $0.value <= 57) || $0 == "-"
        }
    }

    public static func isPackageId(_ value: String) -> Bool {
        guard value.count <= 96 else { return false }
        if !value.contains(".") { return isSlug(value, limit: 96) }
        let parts = value.split(separator: ".", omittingEmptySubsequences: false)
        guard let head = parts.first, isSlug(String(head), limit: 96) else { return false }
        return parts.allSatisfy { (part: Substring) -> Bool in
            guard let first = part.first, let last = part.last else { return false }
            return first != "-" && last != "-" && !part.contains("--")
                && part.unicodeScalars.allSatisfy {
                    ($0.value >= 97 && $0.value <= 122) || ($0.value >= 48 && $0.value <= 57) || $0 == "-"
                }
        }
    }

    public static func isOrgName(_ value: String) -> Bool {
        isSlug(value, limit: 63) && !value.hasSuffix("-")
    }

    public static func isHexId(_ value: String) -> Bool {
        value.count == 32 && value.unicodeScalars.allSatisfy {
            ($0.value >= 48 && $0.value <= 57) || ($0.value >= 97 && $0.value <= 102)
        }
    }

    public static func installDate(_ value: String) -> String {
        let parts = value.split(separator: "-", omittingEmptySubsequences: false)
        guard value.count == 10, parts.count == 3, parts[0].count == 4, parts[1].count == 2, parts[2].count == 2,
              parts.allSatisfy({ $0.allSatisfy(\.isASCII) && $0.allSatisfy(\.isNumber) }) else { return "" }
        return value
    }

    public static func bounded(_ value: String, _ limit: Int) -> String {
        value.count > limit ? String(value.prefix(limit)) : value
    }

    public static func matches(_ query: String, _ fields: String...) -> Bool {
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        if needle.isEmpty { return true }
        return fields.contains { $0.lowercased().contains(needle) }
    }
}
