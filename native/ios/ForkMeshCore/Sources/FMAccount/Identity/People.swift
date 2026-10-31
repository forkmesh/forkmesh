import FMBase
import Foundation
import Observation

public enum PeopleRing: String, CaseIterable, Sendable {
    case blazing, glowing, kindling, warm, embers

    public var title: String {
        switch self {
        case .blazing: return "Blazing"
        case .glowing: return "Glowing"
        case .kindling: return "Kindling"
        case .warm: return "Warm"
        case .embers: return "Embers"
        }
    }

    public var blurb: String {
        switch self {
        case .blazing: return "Active today and deep in the work"
        case .glowing: return "Active in the last five days"
        case .kindling: return "Joined in the last two weeks"
        case .warm: return "Active this month, or has contributed"
        case .embers: return "Quiet for a while"
        }
    }

    public var order: Int { PeopleRing.allCases.firstIndex(of: self) ?? 0 }
}

public struct PeopleMember: Equatable, Sendable, Identifiable {
    public let name: String
    public let avatar: String
    public let createdAt: Int64
    public let activityBucket: String
    public let totalActiveMs: Int64
    public let totalSteps: Int64
    public let officeVisits: Int
    public let contributions: Int
    public let nodeCount: Int
    public let emailVerified: Bool
    public let hasPayoutAddress: Bool
    public let statusText: String
    public let statusEmoji: String

    public var id: String { name.lowercased() }

    public init(
        name: String, avatar: String = "", createdAt: Int64 = 0, activityBucket: String = "",
        totalActiveMs: Int64 = 0, totalSteps: Int64 = 0, officeVisits: Int = 0, contributions: Int = 0,
        nodeCount: Int = 0, emailVerified: Bool = false, hasPayoutAddress: Bool = false,
        statusText: String = "", statusEmoji: String = ""
    ) {
        self.name = name
        self.avatar = avatar
        self.createdAt = createdAt
        self.activityBucket = activityBucket
        self.totalActiveMs = totalActiveMs
        self.totalSteps = totalSteps
        self.officeVisits = officeVisits
        self.contributions = contributions
        self.nodeCount = nodeCount
        self.emailVerified = emailVerified
        self.hasPayoutAddress = hasPayoutAddress
        self.statusText = statusText
        self.statusEmoji = statusEmoji
    }

    public static func parse(_ map: JSONObject) -> PeopleMember? {
        let name = map.str("name").trimmingCharacters(in: .whitespaces)
        guard !name.isEmpty, map.str("kind", "user") == "user" else { return nil }
        let contributions = ["issues", "pulls", "commits", "discussions"].reduce(0) { $0 + max(0, map.int($1)) }
        let status = map["userStatus"]?.objectValue ?? JSONObject()
        return PeopleMember(
            name: name,
            avatar: PeopleAvatar.base64(map.str("avatarThumb")),
            createdAt: max(0, map.int64("createdAt")),
            activityBucket: map.str("activityBucket"),
            totalActiveMs: max(0, map.int64("totalActiveMs")),
            totalSteps: max(0, map.int64("totalSteps")),
            officeVisits: max(0, map.int("officeVisits")),
            contributions: contributions,
            nodeCount: map["nodes"]?.arrayOrEmpty.count ?? 0,
            emailVerified: map.bool("emailVerified"),
            hasPayoutAddress: !map.str("solana").trimmingCharacters(in: .whitespaces).isEmpty,
            statusText: String(status.str("text").prefix(80)),
            statusEmoji: String(status.str("emoji").prefix(8)))
    }
}

public enum PeopleAvatar {
    public static func base64(_ value: String) -> String {
        guard value.hasPrefix("data:image/"), let comma = value.firstIndex(of: ",") else { return "" }
        let header = value[..<comma]
        guard header.hasSuffix(";base64") else { return "" }
        return String(value[value.index(after: comma)...])
    }
}

public enum PeopleRings {
    public static let newWindowMs: Int64 = 14 * 24 * 60 * 60 * 1000
    public static let blazingScore = 70

    static let recency: [String: Int] = [
        "hour": 48, "5h": 46, "24h": 42, "3d": 36, "5d": 32, "10d": 25, "30d": 18, "60d": 11, "120d": 5,
    ]

    static func tier(_ value: Int64, _ tiers: [(Int64, Int)]) -> Int {
        for (minimum, points) in tiers where value >= minimum { return points }
        return 0
    }

    public static func score(_ member: PeopleMember) -> Int {
        let hour: Int64 = 60 * 60 * 1000
        let recencyPoints = recency[member.activityBucket] ?? 0
        let depth = tier(member.totalActiveMs, [(40 * hour, 22), (12 * hour, 17), (4 * hour, 12), (hour, 7), (10 * 60 * 1000, 3)])
            + tier(member.totalSteps, [(100_000, 13), (25_000, 10), (5_000, 7), (500, 4), (1, 1)])
            + tier(Int64(member.officeVisits), [(20, 6), (5, 4), (1, 2)])
            + min(14, Int((log2(Double(member.contributions + 1)) * 3.5).rounded(.down)))
        let readiness = (member.emailVerified ? 4 : 0) + (member.hasPayoutAddress ? 4 : 0) + min(7, member.nodeCount * 3)
        return min(100, max(0, recencyPoints + depth + readiness))
    }

    public static func ring(_ member: PeopleMember, now: Int64) -> PeopleRing {
        let bucket = member.activityBucket
        let today = ["hour", "5h", "24h"].contains(bucket)
        if today && score(member) >= blazingScore { return .blazing }
        if today || ["3d", "5d"].contains(bucket) { return .glowing }
        if member.createdAt > 0 && now - member.createdAt < newWindowMs { return .kindling }
        if ["10d", "30d"].contains(bucket) || member.contributions > 0 || member.totalSteps > 0 || member.officeVisits > 0 {
            return .warm
        }
        return .embers
    }

    public static func lastActive(_ bucket: String) -> String {
        switch bucket {
        case "hour": return "Active in the last hour"
        case "5h": return "Active in the last few hours"
        case "24h": return "Active today"
        case "3d": return "Active in the last 3 days"
        case "5d": return "Active in the last 5 days"
        case "10d": return "Active in the last 10 days"
        case "30d": return "Active this month"
        case "60d": return "Active in the last 2 months"
        case "120d": return "Active in the last 4 months"
        default: return "Not seen recently"
        }
    }
}

public struct PeopleRanked: Equatable, Sendable, Identifiable {
    public let member: PeopleMember
    public let ring: PeopleRing
    public let score: Int
    public var id: String { member.id }
}

public struct PeopleSection: Equatable, Sendable, Identifiable {
    public let ring: PeopleRing
    public let people: [PeopleRanked]
    public var id: String { ring.rawValue }
}

public enum PeopleDirectory {
    public static func sections(_ members: [PeopleMember], now: Int64) -> [PeopleSection] {
        let ranked = members.map { PeopleRanked(member: $0, ring: PeopleRings.ring($0, now: now), score: PeopleRings.score($0)) }
        return PeopleRing.allCases.map { ring in
            PeopleSection(ring: ring, people: ranked.filter { $0.ring == ring }.sorted(by: before))
        }
    }

    static func before(_ left: PeopleRanked, _ right: PeopleRanked) -> Bool {
        if left.score != right.score { return left.score > right.score }
        if left.member.totalActiveMs != right.member.totalActiveMs { return left.member.totalActiveMs > right.member.totalActiveMs }
        return left.member.name.lowercased() < right.member.name.lowercased()
    }

    public static func filter(_ sections: [PeopleSection], query: String) -> [PeopleSection] {
        let needle = query.trimmingCharacters(in: .whitespaces).lowercased().trimmingCharacters(in: CharacterSet(charactersIn: "@"))
        guard !needle.isEmpty else { return sections }
        return sections.map { PeopleSection(ring: $0.ring, people: $0.people.filter { $0.member.id.contains(needle) }) }
    }
}

public struct PeopleSeat: Equatable, Sendable {
    public let ring: PeopleRing
    public let x: Double
    public let y: Double
    public let person: PeopleRanked?
    public let overflow: Int
}

public enum PeopleRingLayout {
    public static func radius(ring: Int, size: Double) -> Double {
        let half = size / 2
        let inner = half * 0.26
        let step = (half * 0.9 - inner) / Double(max(1, PeopleRing.allCases.count - 1))
        return inner + step * Double(ring)
    }

    public static func labelGap(ring: Int, size: Double, seat: Double, label: Double) -> Double {
        let r = radius(ring: ring, size: size)
        return asin(min(1, (label / 2 + seat / 2 + 2) / r))
    }

    public static func capacity(ring: Int, size: Double, seat: Double, label: Double) -> Int {
        let arc = 2 * Double.pi - 2 * labelGap(ring: ring, size: size, seat: seat, label: label)
        return max(2, Int(arc * radius(ring: ring, size: size) / (seat * 1.15)))
    }

    public static func seats(_ sections: [PeopleSection], size: Double, seat: Double, label: Double) -> [PeopleSeat] {
        var seats: [PeopleSeat] = []
        for section in sections where !section.people.isEmpty {
            let index = section.ring.order
            let capacity = capacity(ring: index, size: size, seat: seat, label: label)
            let overflow = section.people.count > capacity ? section.people.count - (capacity - 1) : 0
            let shown = overflow > 0 ? Array(section.people.prefix(capacity - 1)) : section.people
            let count = shown.count + (overflow > 0 ? 1 : 0)
            let r = radius(ring: index, size: size)
            let gap = labelGap(ring: index, size: size, seat: seat, label: label)
            let arc = 2 * Double.pi - 2 * gap
            let spread = min(arc, Double(count) * seat * 1.15 / r)
            let start = Double.pi / 2 - spread / 2
            for slot in 0..<count {
                let angle = start + spread * (Double(slot) + 0.5) / Double(count)
                let person = slot < shown.count ? shown[slot] : nil
                seats.append(PeopleSeat(
                    ring: section.ring, x: size / 2 + r * cos(angle), y: size / 2 + r * sin(angle),
                    person: person, overflow: person == nil ? overflow : 0))
            }
        }
        return seats
    }
}

public struct PeopleProfile: Equatable, Sendable {
    public let name: String
    public let exists: Bool
    public let isPrivate: Bool
    public let avatar: String
    public let bio: String
    public let about: String
    public let location: String
    public let createdAt: Int64
    public let followers: Int
    public let following: Int
    public let isFollowing: Bool
    public let statusText: String
    public let statusEmoji: String
    public var tipAddress: String = ""

    public static func parse(_ map: JSONObject, requested: String) -> PeopleProfile {
        let social = map["social"]?.objectValue ?? JSONObject()
        let status = map["userStatus"]?.objectValue ?? JSONObject()
        let pick: (String) -> Int = { key in max(0, social[key] != nil ? social.int(key) : map.int(key)) }
        return PeopleProfile(
            name: map.str("name").isEmpty ? requested : map.str("name"),
            exists: map["exists"] != .bool(false),
            isPrivate: map.bool("profilePrivate"),
            avatar: map.str("avatarPng"),
            bio: String(map.str("profileBio").prefix(500)),
            about: String(map.str("profileAbout").prefix(2000)),
            location: String(map.str("profileLocation").prefix(120)),
            createdAt: map.int64("createdAt"),
            followers: pick("followers"),
            following: pick("following"),
            isFollowing: social["isFollowing"] != nil ? social.bool("isFollowing") : map.bool("isFollowing"),
            statusText: String(status.str("text").prefix(80)),
            statusEmoji: String(status.str("emoji").prefix(8)),
            tipAddress: SolanaAddress.valid(map.str("solana")))
    }

    public func following(_ value: Bool, followers count: Int) -> PeopleProfile {
        PeopleProfile(
            name: name, exists: exists, isPrivate: isPrivate, avatar: avatar, bio: bio, about: about,
            location: location, createdAt: createdAt, followers: max(0, count), following: following,
            isFollowing: value, statusText: statusText, statusEmoji: statusEmoji, tipAddress: tipAddress)
    }
}

@MainActor
@Observable
public final class PeopleModel {
    public static let directoryPath = "/api/accounts/users?view=world"

    public private(set) var members: [PeopleMember] = []
    public private(set) var loading = false
    public private(set) var loaded = false
    public private(set) var error = ""
    public var query = ""
    public var focus: PeopleRing?

    @ObservationIgnored private let client: AccountEndpointCalling
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored private let viewer: @MainActor () -> String

    public init(client: AccountEndpointCalling, viewer: @escaping @MainActor () -> String, clock: Clock = SystemClock()) {
        self.client = client
        self.viewer = viewer
        self.clock = clock
    }

    public var viewerName: String { viewer().lowercased() }

    public var sections: [PeopleSection] { PeopleDirectory.sections(members, now: clock.nowMillis) }

    public var visibleSections: [PeopleSection] {
        let filtered = PeopleDirectory.filter(sections, query: query)
        guard let focus else { return filtered }
        return filtered.filter { $0.ring == focus }
    }

    public func load() async {
        loading = true
        defer { loading = false }
        do {
            let map = try await client.accountCall("GET", Self.directoryPath, nil)
            var seen = Set<String>()
            members = map["users"]?.objects.compactMap(PeopleMember.parse).filter { seen.insert($0.id).inserted } ?? []
            loaded = true
            error = ""
        } catch {
            self.error = AccountErrorText.text(error, fallback: "Could not load people.")
        }
    }

    public func toggleFocus(_ ring: PeopleRing) {
        focus = focus == ring ? nil : ring
    }
}

@MainActor
@Observable
public final class PeopleProfileModel {
    public let name: String
    public private(set) var profile: PeopleProfile?
    public private(set) var loading = false
    public private(set) var busy = false
    public private(set) var error = ""
    public private(set) var actionError = ""

    @ObservationIgnored private let client: AccountEndpointCalling
    @ObservationIgnored private let viewer: String

    public init(name: String, viewer: String, client: AccountEndpointCalling) {
        self.name = name.lowercased()
        self.viewer = viewer.lowercased()
        self.client = client
    }

    public var isSelf: Bool { !viewer.isEmpty && viewer == name }
    public var signedIn: Bool { !viewer.isEmpty }

    public var path: String {
        let base = "/api/accounts/\(name)"
        guard signedIn, let encoded = viewer.addingPercentEncoding(withAllowedCharacters: .alphanumerics) else { return base }
        return "\(base)?viewer=\(encoded)"
    }

    public func load() async {
        guard NodeLink.isNodeName(name) else {
            error = "That is not a ForkMesh name."
            return
        }
        loading = true
        defer { loading = false }
        do {
            profile = PeopleProfile.parse(try await client.accountCall("GET", path, nil), requested: name)
            error = ""
        } catch {
            self.error = AccountErrorText.text(error, fallback: "Could not load @\(name).")
        }
    }

    public func toggleFollow() async {
        guard let current = profile, signedIn, !isSelf, !busy else { return }
        busy = true
        defer { busy = false }
        let follow = !current.isFollowing
        do {
            let map = try await client.accountCall(follow ? "POST" : "DELETE", "/api/accounts/\(name)/follow", JSONObject())
            let count = map["followers"] != nil ? map.int("followers") : current.followers + (follow ? 1 : -1)
            let state = map["isFollowing"] != nil ? map.bool("isFollowing") : follow
            profile = current.following(state, followers: count)
            actionError = ""
        } catch {
            actionError = PeopleProfileModel.followError(error, follow: follow)
        }
    }

    static func followError(_ error: Error, follow: Bool) -> String {
        if let failure = error as? AccountEndpointError {
            if failure.code == "cannot_follow_self" { return "You cannot follow yourself." }
            if failure.status == 403 || ["email_not_verified", "account_pending_verification"].contains(failure.code) { return "Verify your email to follow people." }
        }
        return AccountErrorText.text(error, fallback: follow ? "Could not follow." : "Could not unfollow.")
    }
}
