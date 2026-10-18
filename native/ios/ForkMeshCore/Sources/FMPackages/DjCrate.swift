import Foundation

public struct DjTrack: Codable, Equatable, Sendable, Identifiable {
    public var id: String
    public var title: String
    public var author: String
    public var tags: [String]

    public init(id: String, title: String = "", author: String = "", tags: [String] = []) {
        self.id = id
        self.title = DjCrate.line(title, 300)
        self.author = DjCrate.line(author, 200)
        self.tags = DjCrate.tagList(tags)
    }

    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        let id = try values.decode(String.self, forKey: .id)
        guard DjStudio.isVideoId(id) else {
            throw DecodingError.dataCorruptedError(forKey: .id, in: values, debugDescription: "Not a YouTube video id")
        }
        self.init(
            id: id,
            title: ((try? values.decodeIfPresent(String.self, forKey: .title)) ?? nil) ?? "",
            author: ((try? values.decodeIfPresent(String.self, forKey: .author)) ?? nil) ?? "",
            tags: ((try? values.decodeIfPresent([String].self, forKey: .tags)) ?? nil) ?? [])
    }

    public var thumbnail: URL { DjStudio.thumbnailURL(id, quality: "mqdefault") }
    public var shownTitle: String { title.isEmpty ? "YouTube video" : title }
}

public struct DjPlaylist: Codable, Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var songs: [String]

    public init(id: String, name: String, songs: [String] = []) {
        self.id = String(id.prefix(64))
        self.name = DjCrate.playlistName(name)
        var seen = Set<String>()
        self.songs = Array(songs.filter { DjStudio.isVideoId($0) && seen.insert($0).inserted }.prefix(DjCrate.playlistSongLimit))
    }

    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        let id = ((try? values.decodeIfPresent(String.self, forKey: .id)) ?? nil) ?? ""
        self.init(
            id: id.isEmpty ? UUID().uuidString : id,
            name: ((try? values.decodeIfPresent(String.self, forKey: .name)) ?? nil) ?? "",
            songs: ((try? values.decodeIfPresent([String].self, forKey: .songs)) ?? nil) ?? [])
    }
}

public enum DjFocus: Hashable, Sendable, Codable {
    case everything
    case playlist(String)
    case tag(String)

    public init(key: String) {
        if key.hasPrefix("playlist:") {
            self = .playlist(String(key.dropFirst(9)))
        } else if key.hasPrefix("tag:") {
            let name = DjCrate.tagName(String(key.dropFirst(4)))
            self = name.isEmpty ? .everything : .tag(name)
        } else {
            self = .everything
        }
    }

    public var key: String {
        switch self {
        case .everything: return ""
        case let .playlist(id): return "playlist:\(id)"
        case let .tag(name): return "tag:\(name)"
        }
    }

    public init(from decoder: Decoder) throws {
        self.init(key: (try? decoder.singleValueContainer().decode(String.self)) ?? "")
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(key)
    }
}

public struct DjTagCount: Equatable, Sendable, Identifiable {
    public let name: String
    public let count: Int
    public var id: String { name }

    public init(name: String, count: Int) {
        self.name = name
        self.count = count
    }
}

public struct DjImportedList: Equatable, Sendable {
    public var name: String
    public var songs: [String]

    public init(name: String, songs: [String]) {
        self.name = DjCrate.playlistName(name)
        self.songs = songs
    }
}

public struct DjCrateImport: Equatable, Sendable {
    public var tracks: [DjTrack]
    public var playlists: [DjImportedList]

    public init(tracks: [DjTrack], playlists: [DjImportedList] = []) {
        self.tracks = tracks
        self.playlists = playlists
    }
}

public struct DjMergeReport: Equatable, Sendable {
    public var songs = 0
    public var playlists = 0
    public var full = false

    public init(songs: Int = 0, playlists: Int = 0, full: Bool = false) {
        self.songs = songs
        self.playlists = playlists
        self.full = full
    }

    public var summary: String {
        let songs = songs == 1 ? "1 new song" : "\(songs) new songs"
        let lists = playlists == 0 ? "" : playlists == 1 ? " into 1 playlist" : " into \(playlists) playlists"
        return "Imported \(songs)\(lists)." + (full ? " Some were left out: the library is full." : "")
    }
}

public enum DjMove: Equatable, Sendable {
    case full
    case added
    case moved
    case removed
}

public struct DjCrate: Codable, Equatable, Sendable {
    public static let storageKey = "fm.videoMixDjStudio.crate.v1"
    public static let format = "forkmesh-dj-playlists"
    public static let trackLimit = 2000
    public static let playlistLimit = 100
    public static let playlistSongLimit = 1000
    public static let tagLimit = 20
    public static let tagLength = 32
    public static let nameLength = 80
    public static let importLimit = 4_000_000

    public var tracks: [DjTrack] = []
    public var playlists: [DjPlaylist] = []
    public var focus: DjFocus = .everything
    public var shuffle = true
    public var cursor = ""

    public init() {}

    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        let stored = ((try? values.decodeIfPresent([DjTrackEntry].self, forKey: .tracks)) ?? nil) ?? []
        var seen = Set<String>()
        tracks = Array(stored.compactMap(\.track).filter { seen.insert($0.id).inserted }.prefix(Self.trackLimit))
        let lists = ((try? values.decodeIfPresent([DjPlaylistEntry].self, forKey: .playlists)) ?? nil) ?? []
        var ids = Set<String>()
        playlists = Array(lists.compactMap(\.playlist).filter { ids.insert($0.id).inserted }.prefix(Self.playlistLimit))
        for index in playlists.indices { playlists[index].songs.removeAll { !seen.contains($0) } }
        shuffle = ((try? values.decodeIfPresent(Bool.self, forKey: .shuffle)) ?? nil) ?? true
        cursor = ((try? values.decodeIfPresent(String.self, forKey: .cursor)) ?? nil) ?? ""
        focus = ((try? values.decodeIfPresent(DjFocus.self, forKey: .focus)) ?? nil) ?? .everything
        if !knows(focus) { focus = .everything }
    }

    public static func decode(_ data: Data?) -> DjCrate {
        guard let data, let crate = try? JSONDecoder().decode(DjCrate.self, from: data) else { return DjCrate() }
        return crate
    }

    public func encoded() -> Data? { try? JSONEncoder().encode(self) }

    static func line(_ text: String, _ limit: Int) -> String {
        String(text.split(whereSeparator: \.isNewline).joined(separator: " ").prefix(limit))
    }

    public static func tagName(_ raw: String) -> String {
        let words = raw.lowercased().split { $0.isWhitespace || $0 == "," || $0 == "#" }
        return String(words.joined(separator: " ").prefix(tagLength)).trimmingCharacters(in: .whitespaces)
    }

    public static func tagNames(_ raw: String) -> [String] {
        tagList(raw.split(separator: ",").map(String.init))
    }

    public static func tagList(_ names: [String]) -> [String] {
        var seen = Set<String>()
        return Array(names.map(tagName).filter { !$0.isEmpty && seen.insert($0).inserted }.prefix(tagLimit))
    }

    public static func playlistName(_ raw: String) -> String {
        String(raw.split(whereSeparator: \.isWhitespace).joined(separator: " ").prefix(nameLength))
    }

    public func track(_ id: String) -> DjTrack? { tracks.first { $0.id == id } }

    public func playlist(_ id: String) -> DjPlaylist? { playlists.first { $0.id == id } }

    public func knows(_ focus: DjFocus) -> Bool {
        switch focus {
        case .everything: return true
        case let .playlist(id): return playlist(id) != nil
        case let .tag(name): return tracks.contains { $0.tags.contains(name) }
        }
    }

    public var tagCounts: [DjTagCount] {
        var counts: [String: Int] = [:]
        for track in tracks { for tag in track.tags { counts[tag, default: 0] += 1 } }
        return counts.map { DjTagCount(name: $0.key, count: $0.value) }.sorted { $0.name < $1.name }
    }

    public func label(_ focus: DjFocus) -> String {
        switch focus {
        case .everything: return "Everything"
        case let .playlist(id): return playlist(id)?.name ?? "Playlist"
        case let .tag(name): return "#\(name)"
        }
    }

    public func songs(_ focus: DjFocus) -> [DjTrack] {
        switch focus {
        case .everything:
            return tracks
        case let .playlist(id):
            let known = Dictionary(tracks.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
            return (playlist(id)?.songs ?? []).compactMap { known[$0] }
        case let .tag(name):
            return tracks.filter { $0.tags.contains(name) }
        }
    }

    @discardableResult
    public mutating func keep(_ id: String, title: String = "", author: String = "") -> Bool {
        guard DjStudio.isVideoId(id) else { return false }
        if let index = tracks.firstIndex(where: { $0.id == id }) {
            if !title.isEmpty { tracks[index].title = Self.line(title, 300) }
            if !author.isEmpty { tracks[index].author = Self.line(author, 200) }
            return true
        }
        guard tracks.count < Self.trackLimit else { return false }
        tracks.append(DjTrack(id: id, title: title, author: author))
        return true
    }

    @discardableResult
    public mutating func tag(_ id: String, _ raw: String, title: String = "", author: String = "") -> Bool {
        let names = Self.tagNames(raw)
        guard !names.isEmpty, keep(id, title: title, author: author), let index = tracks.firstIndex(where: { $0.id == id }) else { return false }
        tracks[index].tags = Self.tagList(tracks[index].tags + names)
        return true
    }

    public mutating func untag(_ id: String, _ name: String) {
        guard let index = tracks.firstIndex(where: { $0.id == id }) else { return }
        tracks[index].tags.removeAll { $0 == name }
        if focus == .tag(name), !knows(focus) { setFocus(.everything) }
    }

    @discardableResult
    public mutating func renameTag(_ old: String, to raw: String) -> Bool {
        let name = Self.tagName(raw)
        guard !name.isEmpty else { return false }
        guard name != old else { return true }
        for index in tracks.indices where tracks[index].tags.contains(old) {
            tracks[index].tags = Self.tagList(tracks[index].tags.map { $0 == old ? name : $0 })
        }
        if focus == .tag(old) { focus = .tag(name) }
        return true
    }

    public mutating func deleteTag(_ name: String) {
        for index in tracks.indices { tracks[index].tags.removeAll { $0 == name } }
        if focus == .tag(name) { setFocus(.everything) }
    }

    public func uniqueName(_ raw: String, except id: String? = nil) -> String {
        let base = Self.playlistName(raw).isEmpty ? "New Playlist" : Self.playlistName(raw)
        let taken = Set(playlists.filter { $0.id != id }.map { $0.name.lowercased() })
        guard taken.contains(base.lowercased()) else { return base }
        var number = 2
        while taken.contains("\(base) \(number)".lowercased()) { number += 1 }
        return "\(base) \(number)"
    }

    @discardableResult
    public mutating func createPlaylist(_ raw: String, id: String = UUID().uuidString) -> String? {
        guard playlists.count < Self.playlistLimit else { return nil }
        playlists.append(DjPlaylist(id: id, name: uniqueName(raw)))
        return id
    }

    public mutating func renamePlaylist(_ id: String, _ raw: String) {
        guard let index = playlists.firstIndex(where: { $0.id == id }), !Self.playlistName(raw).isEmpty else { return }
        playlists[index].name = uniqueName(raw, except: id)
    }

    public mutating func deletePlaylist(_ id: String) {
        playlists.removeAll { $0.id == id }
        if focus == .playlist(id) { setFocus(.everything) }
    }

    @discardableResult
    public mutating func add(_ songs: [DjTrack], to id: String) -> Int {
        guard let index = playlists.firstIndex(where: { $0.id == id }) else { return 0 }
        var added = 0
        for song in songs where playlists[index].songs.count < Self.playlistSongLimit && !playlists[index].songs.contains(song.id) {
            guard keep(song.id, title: song.title, author: song.author) else { continue }
            playlists[index].songs.append(song.id)
            added += 1
        }
        return added
    }

    public mutating func remove(_ song: String, from id: String) {
        guard let index = playlists.firstIndex(where: { $0.id == id }) else { return }
        playlists[index].songs.removeAll { $0 == song }
    }

    public mutating func removeSongs(at offsets: IndexSet, from id: String) {
        guard let index = playlists.firstIndex(where: { $0.id == id }) else { return }
        playlists[index].songs = playlists[index].songs.enumerated().filter { !offsets.contains($0.offset) }.map(\.element)
    }

    public mutating func moveSongs(in id: String, from offsets: IndexSet, to destination: Int) {
        guard let index = playlists.firstIndex(where: { $0.id == id }) else { return }
        let songs = playlists[index].songs
        let moving = offsets.filter { songs.indices.contains($0) }.map { songs[$0] }
        var rest = songs.enumerated().filter { !offsets.contains($0.offset) }.map(\.element)
        let place = destination - offsets.filter { $0 < destination }.count
        rest.insert(contentsOf: moving, at: min(rest.count, max(0, place)))
        playlists[index].songs = rest
    }

    public mutating func removeTrack(_ id: String) {
        tracks.removeAll { $0.id == id }
        for index in playlists.indices { playlists[index].songs.removeAll { $0 == id } }
        if cursor == id { cursor = "" }
        if !knows(focus) { setFocus(.everything) }
    }

    public mutating func move(_ id: String, title: String = "", author: String = "", to target: DjFocus) -> DjMove {
        switch target {
        case let .playlist(list):
            if playlist(list)?.songs.contains(id) != true, add([DjTrack(id: id, title: title, author: author)], to: list) == 0 { return .full }
        case let .tag(name):
            guard tag(id, name, title: title, author: author) else { return .full }
        case .everything:
            break
        }
        let from = focus
        guard from != target else { return .added }
        var left = false
        switch from {
        case let .playlist(list):
            left = playlist(list)?.songs.contains(id) == true
            remove(id, from: list)
        case let .tag(name):
            left = track(id)?.tags.contains(name) == true
            untag(id, name)
        case .everything:
            break
        }
        return target == .everything ? .removed : left ? .moved : .added
    }

    public mutating func setFocus(_ focus: DjFocus) {
        self.focus = knows(focus) ? focus : .everything
        cursor = ""
    }

    public func pool(_ library: [DjPlayedSong], skip: Set<String> = []) -> [DjPlayedSong] {
        let played = Dictionary(library.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        let songs: [DjPlayedSong]
        switch focus {
        case .everything:
            songs = library + tracks.filter { played[$0.id] == nil }.map { DjPlayedSong(id: $0.id, title: $0.title, author: $0.author, plays: 0) }
        default:
            songs = self.songs(focus).map { track in
                guard var song = played[track.id] else { return DjPlayedSong(id: track.id, title: track.title, author: track.author, plays: 0) }
                if song.title.isEmpty { song.title = track.title }
                if song.author.isEmpty { song.author = track.author }
                return song
            }
        }
        return songs.filter { !skip.contains($0.id) }
    }

    public mutating func next(_ library: [DjPlayedSong], loaded: Set<String>, playing: Set<String>, skip: Set<String> = [], seed: UInt64) -> DjPlayedSong? {
        let pool = pool(library, skip: skip)
        guard !pool.isEmpty else { return nil }
        let pick: DjPlayedSong?
        if focus == .everything || shuffle {
            pick = DjAutomix.next(pool, loaded: loaded, playing: playing, seed: seed)
        } else {
            let start = pool.firstIndex { $0.id == cursor }.map { $0 + 1 } ?? 0
            let order = pool.indices.map { pool[(start + $0) % pool.count] }
            pick = order.first { !loaded.contains($0.id) && !playing.contains($0.id) } ?? order.first { !playing.contains($0.id) }
        }
        if let pick { cursor = pick.id }
        return pick
    }

    public func exported(_ scope: DjFocus, at date: Date = Date()) -> Data {
        let lists: [DjPlaylist]
        switch scope {
        case .everything: lists = playlists
        case let .playlist(id): lists = playlist(id).map { [$0] } ?? []
        case .tag: lists = []
        }
        let file = DjCrateFile(
            format: Self.format, version: 1, exported: ISO8601DateFormatter().string(from: date),
            tracks: songs(scope), playlists: lists.map { DjCrateFile.List(name: $0.name, songs: $0.songs) })
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        return (try? encoder.encode(file)) ?? Data()
    }

    public func m3u(_ scope: DjFocus) -> String {
        (["#EXTM3U"] + songs(scope).flatMap { ["#EXTINF:-1,\($0.shownTitle)", DjStudio.watchURL($0.id).absoluteString] })
            .joined(separator: "\n") + "\n"
    }

    public func fileName(_ scope: DjFocus) -> String {
        let label: String
        switch scope {
        case .everything: label = "DJ Playlists"
        case let .playlist(id): label = playlist(id)?.name ?? "Playlist"
        case let .tag(name): label = "Tag \(name)"
        }
        let safe = String(label.unicodeScalars.map { CharacterSet.alphanumerics.contains($0) || " -_".unicodeScalars.contains($0) ? Character($0) : "-" })
        let trimmed = safe.trimmingCharacters(in: .whitespaces)
        return trimmed.isEmpty ? "DJ Playlist" : trimmed
    }

    public static func parse(_ data: Data) -> DjCrateImport? {
        guard !data.isEmpty, data.count <= importLimit else { return nil }
        if let json = try? JSONSerialization.jsonObject(with: data, options: [.fragmentsAllowed]), !(json is String) {
            return parse(json: json)
        }
        guard let text = String(data: data, encoding: .utf8) else { return nil }
        return parse(text: text)
    }

    public static func parse(text: String) -> DjCrateImport? {
        var tracks: [DjTrack] = []
        var title = ""
        for raw in text.split(whereSeparator: \.isNewline) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.uppercased().hasPrefix("#EXTINF") {
                title = line.firstIndex(of: ",").map { String(line[line.index(after: $0)...]).trimmingCharacters(in: .whitespaces) } ?? ""
                continue
            }
            if line.hasPrefix("#") { continue }
            let id = DjStudio.videoId(line)
            if !id.isEmpty { tracks.append(DjTrack(id: id, title: title)) }
            title = ""
        }
        return built(tracks, [])
    }

    static func parse(json: Any) -> DjCrateImport? {
        if let rows = json as? [Any] { return built(rows.compactMap(song), []) }
        guard let object = json as? [String: Any] else { return nil }
        var tracks = ((object["tracks"] ?? object["songs"] ?? object["videos"] ?? object["items"]) as? [Any] ?? []).compactMap(song)
        var lists: [DjImportedList] = []
        for case let list as [String: Any] in object["playlists"] as? [Any] ?? [] {
            let songs = ((list["songs"] ?? list["tracks"] ?? list["videos"] ?? list["items"]) as? [Any] ?? []).compactMap(song)
            tracks += songs
            lists.append(DjImportedList(name: (list["name"] ?? list["title"]) as? String ?? "", songs: songs.map(\.id)))
        }
        if object["playlists"] == nil, object["format"] as? String != format, let name = (object["name"] ?? object["title"]) as? String, !tracks.isEmpty {
            lists.append(DjImportedList(name: name, songs: tracks.map(\.id)))
        }
        return built(tracks, lists)
    }

    static func song(_ value: Any) -> DjTrack? {
        if let text = value as? String {
            let id = DjStudio.videoId(text)
            return id.isEmpty ? nil : DjTrack(id: id)
        }
        guard let object = value as? [String: Any] else { return nil }
        let link = ["videoId", "id", "url", "link", "href"].lazy.compactMap { object[$0] as? String }.map(DjStudio.videoId).first { !$0.isEmpty }
        guard let id = link else { return nil }
        let tags = (object["tags"] as? [Any])?.compactMap { $0 as? String } ?? (object["tags"] as? String).map(tagNames) ?? []
        return DjTrack(
            id: id, title: (object["title"] ?? object["name"]) as? String ?? "",
            author: (object["author"] ?? object["artist"] ?? object["channel"]) as? String ?? "", tags: tags)
    }

    static func built(_ found: [DjTrack], _ lists: [DjImportedList]) -> DjCrateImport? {
        var order: [String] = []
        var merged: [String: DjTrack] = [:]
        for track in found {
            guard var known = merged[track.id] else {
                order.append(track.id)
                merged[track.id] = track
                continue
            }
            if known.title.isEmpty { known.title = track.title }
            if known.author.isEmpty { known.author = track.author }
            known.tags = tagList(known.tags + track.tags)
            merged[track.id] = known
        }
        guard !order.isEmpty else { return nil }
        let tracks = order.prefix(trackLimit).compactMap { merged[$0] }
        return DjCrateImport(tracks: Array(tracks), playlists: Array(lists.prefix(playlistLimit)))
    }

    public mutating func merge(_ incoming: DjCrateImport, name fallback: String) -> DjMergeReport {
        var report = DjMergeReport()
        for track in incoming.tracks {
            if let index = tracks.firstIndex(where: { $0.id == track.id }) {
                if tracks[index].title.isEmpty { tracks[index].title = track.title }
                if tracks[index].author.isEmpty { tracks[index].author = track.author }
                tracks[index].tags = Self.tagList(tracks[index].tags + track.tags)
            } else if tracks.count < Self.trackLimit {
                tracks.append(track)
                report.songs += 1
            } else {
                report.full = true
            }
        }
        var lists = incoming.playlists
        if lists.isEmpty, !incoming.tracks.contains(where: { !$0.tags.isEmpty }) {
            lists = [DjImportedList(name: fallback, songs: incoming.tracks.map(\.id))]
        }
        for list in lists {
            let name = list.name.isEmpty ? Self.playlistName(fallback) : list.name
            let songs = list.songs.compactMap { track($0) }
            var id = playlists.first { $0.name.lowercased() == name.lowercased() }?.id
            if id == nil { id = createPlaylist(name) }
            guard let id else {
                report.full = true
                continue
            }
            add(songs, to: id)
            report.playlists += 1
        }
        return report
    }
}

struct DjCrateFile: Encodable {
    struct List: Encodable {
        let name: String
        let songs: [String]
    }

    let format: String
    let version: Int
    let exported: String
    let tracks: [DjTrack]
    let playlists: [List]
}

struct DjTrackEntry: Decodable {
    let track: DjTrack?

    init(from decoder: Decoder) throws {
        track = try? DjTrack(from: decoder)
    }
}

struct DjPlaylistEntry: Decodable {
    let playlist: DjPlaylist?

    init(from decoder: Decoder) throws {
        playlist = try? DjPlaylist(from: decoder)
    }
}
