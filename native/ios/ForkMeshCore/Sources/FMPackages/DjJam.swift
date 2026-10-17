import FMBase
import Foundation

public struct DjQueueItem: Codable, Equatable, Sendable, Identifiable {
    public var id: String
    public var videoId: String
    public var title: String
    public var author: String
    public var addedBy: String

    public init(id: String, videoId: String, title: String, author: String, addedBy: String = "") {
        self.id = String(id.prefix(64))
        self.videoId = videoId
        self.title = String(title.prefix(300))
        self.author = String(author.prefix(200))
        self.addedBy = String(addedBy.prefix(64))
    }

    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        let videoId = try values.decode(String.self, forKey: .videoId)
        guard DjStudio.isVideoId(videoId) else {
            throw DecodingError.dataCorruptedError(forKey: .videoId, in: values, debugDescription: "Not a YouTube video id")
        }
        self.init(
            id: ((try? values.decodeIfPresent(String.self, forKey: .id)) ?? nil) ?? UUID().uuidString,
            videoId: videoId,
            title: ((try? values.decodeIfPresent(String.self, forKey: .title)) ?? nil) ?? "",
            author: ((try? values.decodeIfPresent(String.self, forKey: .author)) ?? nil) ?? "",
            addedBy: ((try? values.decodeIfPresent(String.self, forKey: .addedBy)) ?? nil) ?? "")
    }

    public var thumbnail: URL { DjStudio.thumbnailURL(videoId, quality: "mqdefault") }

    init?(_ object: JSONObject) {
        let videoId = object["videoId"]?.stringValue ?? ""
        let id = object["id"]?.stringValue ?? ""
        guard DjStudio.isVideoId(videoId), !id.isEmpty else { return nil }
        self.init(
            id: id, videoId: videoId, title: object["title"]?.stringValue ?? "",
            author: object["author"]?.stringValue ?? "", addedBy: object["addedBy"]?.stringValue ?? "")
    }
}

public struct DjQueueEntry: Decodable {
    public let item: DjQueueItem?

    public init(from decoder: Decoder) throws {
        item = try? DjQueueItem(from: decoder)
    }
}

public struct DjJamMember: Equatable, Sendable, Identifiable {
    public var name: String
    public var status: String

    public init(name: String, status: String) {
        self.name = name
        self.status = status
    }

    public var id: String { name }
    public var joined: Bool { status == "owner" || status == "joined" }
}

public struct DjJamDeck: Equatable, Sendable {
    public var videoId: String
    public var title: String
    public var author: String
    public var playing: Bool
    public var position: Double
    public var rate: Double
    public var at: Int64
    public var by: String

    public init(
        videoId: String = "", title: String = "", author: String = "", playing: Bool = false,
        position: Double = 0, rate: Double = 1, at: Int64 = 0, by: String = ""
    ) {
        self.videoId = videoId
        self.title = title
        self.author = author
        self.playing = playing
        self.position = position
        self.rate = rate
        self.at = at
        self.by = by
    }

    init(_ object: JSONObject) {
        let id = object["videoId"]?.stringValue ?? ""
        self.init(
            videoId: DjStudio.isVideoId(id) ? id : "",
            title: String((object["title"]?.stringValue ?? "").prefix(300)),
            author: String((object["author"]?.stringValue ?? "").prefix(200)),
            playing: object["playing"]?.boolValue ?? false,
            position: max(0, object["position"]?.doubleValue ?? 0),
            rate: DjStudio.clampRate(object["rate"]?.doubleValue ?? 1),
            at: object["at"]?.int64Value ?? 0,
            by: object["by"]?.stringValue ?? "")
    }

    public func expectedPosition(serverNow: Int64) -> Double {
        guard playing, at > 0, serverNow > at else { return position }
        return position + Double(serverNow - at) / 1000 * rate
    }

    public var body: JSONObject {
        [
            "videoId": .string(videoId), "title": .string(title), "author": .string(author),
            "playing": .bool(playing), "position": .double((position * 1000).rounded() / 1000), "rate": .double(rate),
        ]
    }
}

public struct DjJamSummary: Equatable, Sendable, Identifiable {
    public var id: String
    public var title: String
    public var owner: String
    public var myStatus: String
    public var members: Int

    public init(id: String, title: String, owner: String, myStatus: String, members: Int) {
        self.id = id
        self.title = title
        self.owner = owner
        self.myStatus = myStatus
        self.members = members
    }

    public var invited: Bool { myStatus == "invited" }

    public static func list(_ object: JSONObject) -> [DjJamSummary] {
        (object["jams"]?.arrayValue ?? []).compactMap { value in
            guard let row = value.objectValue, let id = row["id"]?.stringValue, !id.isEmpty else { return nil }
            return DjJamSummary(
                id: id, title: row["title"]?.stringValue ?? "", owner: row["owner"]?.stringValue ?? "",
                myStatus: row["myStatus"]?.stringValue ?? "", members: row["members"]?.intValue ?? 0)
        }
    }
}

public struct DjJam: Equatable, Sendable, Identifiable {
    public var id: String
    public var title: String
    public var owner: String
    public var me: String
    public var myStatus: String
    public var now: Int64
    public var members: [DjJamMember]
    public var queue: [DjQueueItem]
    public var decks: [DjDeckSide: DjJamDeck]
    public var presence: [DjJamPresence]
    public var reactions: [DjJamReaction]

    public init(
        id: String, title: String = "", owner: String = "", me: String = "", myStatus: String = "",
        now: Int64 = 0, members: [DjJamMember] = [], queue: [DjQueueItem] = [], decks: [DjDeckSide: DjJamDeck] = [:],
        presence: [DjJamPresence] = [], reactions: [DjJamReaction] = []
    ) {
        self.id = id
        self.title = title
        self.owner = owner
        self.me = me
        self.myStatus = myStatus
        self.now = now
        self.members = members
        self.queue = queue
        self.decks = decks
        self.presence = presence
        self.reactions = reactions
    }

    public static let awayAfter: Int64 = 12_000

    public func presence(of name: String) -> DjJamPresence? { presence.first { $0.name == name } }

    public func status(of member: DjJamMember) -> DjPersonStatus { Self.status(of: member, seen: presence(of: member.name)) }

    public static func status(of member: DjJamMember, seen: DjJamPresence?) -> DjPersonStatus {
        guard member.joined else { return .invited }
        guard let seen, seen.ago <= awayAfter else { return .away }
        if seen.state == DjPresenceState.studio.rawValue { return .studio }
        return seen.playing ? .listening : .idle
    }

    public var shared: DjJam {
        var copy = self
        copy.now = 0
        copy.presence = []
        copy.reactions = []
        return copy
    }

    public var isOwner: Bool { myStatus == "owner" }
    public var joined: Bool { myStatus == "owner" || myStatus == "joined" }
    public var listeners: [DjJamMember] { members.filter(\.joined) }

    public subscript(side: DjDeckSide) -> DjJamDeck { decks[side] ?? DjJamDeck() }

    public static func parse(_ object: JSONObject) -> DjJam? {
        guard let jam = object["jam"]?.objectValue, let id = jam["id"]?.stringValue, !id.isEmpty else { return nil }
        var decks: [DjDeckSide: DjJamDeck] = [:]
        if let rows = jam["decks"]?.objectValue {
            for side in DjDeckSide.allCases {
                if let row = rows[side.rawValue]?.objectValue { decks[side] = DjJamDeck(row) }
            }
        }
        return DjJam(
            id: id,
            title: jam["title"]?.stringValue ?? "",
            owner: jam["owner"]?.stringValue ?? "",
            me: jam["me"]?.stringValue ?? "",
            myStatus: jam["myStatus"]?.stringValue ?? "",
            now: jam["now"]?.int64Value ?? 0,
            members: (jam["members"]?.arrayValue ?? []).compactMap { value in
                guard let row = value.objectValue, let name = row["name"]?.stringValue, !name.isEmpty else { return nil }
                return DjJamMember(name: name, status: row["status"]?.stringValue ?? "")
            },
            queue: (jam["queue"]?.arrayValue ?? []).compactMap { $0.objectValue.flatMap(DjQueueItem.init) },
            decks: decks,
            presence: (jam["presence"]?.arrayValue ?? []).compactMap { $0.objectValue.flatMap(DjJamPresence.init) },
            reactions: (jam["reactions"]?.arrayValue ?? []).compactMap { $0.objectValue.flatMap(DjJamReaction.init) })
    }

    public static func userNames(_ object: JSONObject, excluding: Set<String> = []) -> [String] {
        var names: [String] = []
        for value in object["users"]?.arrayValue ?? [] {
            let name = (value.objectValue?["name"]?.stringValue ?? "").trimmingCharacters(in: .whitespaces).lowercased()
            if !name.isEmpty, !excluding.contains(name), !names.contains(name) { names.append(name) }
        }
        return names
    }

    public static func needsSeek(local: Double, remote: Double, playing: Bool) -> Bool {
        guard local.isFinite, remote.isFinite else { return false }
        return abs(local - remote) > (playing ? 1.5 : 0.5)
    }
}
