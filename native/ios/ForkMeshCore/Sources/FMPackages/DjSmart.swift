import Foundation

public enum DjSmartKind: String, CaseIterable, Sendable {
    case listening
    case genre
    case style
    case decade
    case artist

    public var title: String {
        switch self {
        case .listening: return "Listening"
        case .genre: return "Genres"
        case .style: return "Styles"
        case .decade: return "Decades"
        case .artist: return "Artists"
        }
    }
}

public struct DjSmartList: Equatable, Sendable, Identifiable {
    public let id: String
    public let name: String
    public let symbol: String
    public let kind: DjSmartKind
    public let songs: [DjTrack]

    public init(id: String, name: String, symbol: String, kind: DjSmartKind, songs: [DjTrack]) {
        self.id = id
        self.name = name
        self.symbol = symbol
        self.kind = kind
        self.songs = songs
    }
}

public struct DjSimilarSong: Equatable, Sendable, Identifiable {
    public let track: DjTrack
    public let score: Int
    public let reasons: [String]
    public var id: String { track.id }

    public init(track: DjTrack, score: Int, reasons: [String]) {
        self.track = track
        self.score = score
        self.reasons = reasons
    }
}

public enum DjSmart {
    public static let minimumSongs = 2
    public static let listeningLimit = 50
    public static let artistLimit = 30
    public static let rediscoverAfter: Double = 30 * 24 * 60 * 60

    struct Rule {
        let id: String
        let name: String
        let symbol: String
        let words: [String]
    }

    static let genres: [Rule] = [
        Rule(id: "house", name: "House", symbol: "house", words: ["house", "deep house", "tech house", "afro house", "progressive house"]),
        Rule(id: "techno", name: "Techno", symbol: "waveform.path", words: ["techno", "hard techno", "minimal"]),
        Rule(id: "trance", name: "Trance", symbol: "sparkles", words: ["trance", "psytrance", "uplifting"]),
        Rule(id: "dnb", name: "Drum & Bass", symbol: "metronome", words: ["drum & bass", "drum and bass", "drum n bass", "dnb", "d&b", "jungle"]),
        Rule(id: "dubstep", name: "Dubstep & Bass", symbol: "speaker.wave.3", words: ["dubstep", "riddim", "bass music", "brostep", "future bass"]),
        Rule(id: "edm", name: "EDM & Electro", symbol: "bolt", words: ["edm", "big room", "electro", "electro house", "festival", "hardstyle"]),
        Rule(id: "garage", name: "UK Garage & Breaks", symbol: "square.stack.3d.up", words: ["garage", "ukg", "2 step", "breakbeat", "breaks"]),
        Rule(id: "hiphop", name: "Hip-Hop & Rap", symbol: "mic", words: ["hip hop", "hiphop", "rap", "trap", "drill", "boom bap", "grime"]),
        Rule(id: "rnb", name: "R&B & Soul", symbol: "heart", words: ["r&b", "rnb", "soul", "neo soul", "motown"]),
        Rule(id: "pop", name: "Pop", symbol: "star", words: ["pop", "synthpop", "dance pop", "europop"]),
        Rule(id: "kpop", name: "K-Pop", symbol: "star.circle", words: ["k pop", "kpop"]),
        Rule(id: "rock", name: "Rock & Metal", symbol: "guitars", words: ["rock", "metal", "punk", "grunge", "emo", "alternative"]),
        Rule(id: "chill", name: "Chill & Lo-fi", symbol: "moon.stars", words: ["lofi", "lo fi", "chill", "chillhop", "chillout", "ambient", "relax", "relaxing", "study", "sleep"]),
        Rule(id: "latin", name: "Latin", symbol: "sun.max", words: ["reggaeton", "latin", "bachata", "salsa", "cumbia", "dembow", "merengue", "latino"]),
        Rule(id: "afro", name: "Afrobeats & Amapiano", symbol: "globe.europe.africa", words: ["afrobeats", "afrobeat", "amapiano", "afro"]),
        Rule(id: "reggae", name: "Reggae & Dancehall", symbol: "leaf", words: ["reggae", "dancehall", "ska", "dub"]),
        Rule(id: "disco", name: "Disco & Funk", symbol: "circle.hexagongrid", words: ["disco", "nu disco", "funk", "funky", "boogie"]),
        Rule(id: "jazz", name: "Jazz & Blues", symbol: "music.quarternote.3", words: ["jazz", "blues", "swing", "bossa nova"]),
        Rule(id: "classical", name: "Classical", symbol: "pianokeys", words: ["classical", "symphony", "sonata", "concerto", "orchestra", "beethoven", "mozart", "bach", "chopin"]),
        Rule(id: "country", name: "Country & Folk", symbol: "music.mic", words: ["country", "folk", "bluegrass", "americana"]),
    ]

    static let styles: [Rule] = [
        Rule(id: "remix", name: "Remixes & Edits", symbol: "slider.horizontal.3", words: ["remix", "edit", "bootleg", "flip", "vip", "rework", "mashup", "extended mix", "club mix"]),
        Rule(id: "sets", name: "DJ Sets & Mixes", symbol: "record.circle", words: ["dj set", "live set", "boiler room", "full set", "mixtape", "megamix", "essential mix", "b2b"]),
        Rule(id: "live", name: "Live Performances", symbol: "music.mic.circle", words: ["live", "concert", "tiny desk", "session", "unplugged"]),
        Rule(id: "acoustic", name: "Acoustic & Covers", symbol: "guitars.fill", words: ["acoustic", "cover", "piano version"]),
        Rule(id: "speed", name: "Sped Up & Slowed", symbol: "gauge.with.dots.needle.33percent", words: ["sped up", "slowed", "nightcore", "reverb"]),
        Rule(id: "videos", name: "Official Videos", symbol: "play.rectangle", words: ["official video", "official music video", "music video"]),
        Rule(id: "lyrics", name: "Lyric Videos", symbol: "text.quote", words: ["lyrics", "lyric video"]),
    ]

    public static func text(_ raw: String) -> String {
        let spaced = raw.lowercased().unicodeScalars.map { scalar -> Character in
            CharacterSet.alphanumerics.contains(scalar) || scalar == "&" ? Character(scalar) : " "
        }
        return " " + String(spaced).split(separator: " ").joined(separator: " ") + " "
    }

    static func haystack(_ track: DjTrack) -> String {
        text(([track.title] + track.tags).joined(separator: " "))
    }

    static func matches(_ rule: Rule, _ haystack: String) -> Bool {
        rule.words.contains { haystack.contains(" \($0) ") }
    }

    public static func artist(_ track: DjTrack) -> String {
        var name = track.author.trimmingCharacters(in: .whitespaces)
        for suffix in [" - Topic", "VEVO", " Official", " Music"] where name.count > suffix.count && name.lowercased().hasSuffix(suffix.lowercased()) {
            name = String(name.dropLast(suffix.count)).trimmingCharacters(in: .whitespaces)
        }
        if name.isEmpty, let dash = track.title.range(of: " - ") {
            let lead = track.title[..<dash.lowerBound].trimmingCharacters(in: .whitespaces)
            if lead.count <= 40 { name = lead }
        }
        return name
    }

    public static func artistKey(_ track: DjTrack) -> String {
        text(artist(track)).trimmingCharacters(in: .whitespaces)
    }

    public static func decade(_ track: DjTrack) -> Int? {
        for word in haystack(track).split(separator: " ") {
            if word.count == 4, let year = Int(word), (1950...2029).contains(year) { return year / 10 * 10 }
            switch word {
            case "50s": return 1950
            case "60s": return 1960
            case "70s": return 1970
            case "80s": return 1980
            case "90s": return 1990
            case "00s", "2000s": return 2000
            case "2010s": return 2010
            case "2020s": return 2020
            default: continue
            }
        }
        return nil
    }

    public static func categories(_ track: DjTrack) -> [String] {
        let words = haystack(track)
        var found = genres.filter { matches($0, words) }.map { "genre:\($0.id)" }
        found += styles.filter { matches($0, words) }.map { "style:\($0.id)" }
        if let decade = decade(track) { found.append("decade:\(decade)") }
        return found
    }

    public static func lists(_ songs: [DjTrack], library: [DjPlayedSong], now: Double) -> [DjSmartList] {
        var seen = Set<String>()
        let songs = songs.filter { DjStudio.isVideoId($0.id) && seen.insert($0.id).inserted }
        let known = Dictionary(songs.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        let played = library.filter { known[$0.id] != nil }
        let plays = Dictionary(played.map { ($0.id, $0.plays) }, uniquingKeysWith: max)
        var result: [DjSmartList] = []

        func add(_ id: String, _ name: String, _ symbol: String, _ kind: DjSmartKind, _ tracks: [DjTrack], minimum: Int = minimumSongs) {
            guard tracks.count >= minimum else { return }
            result.append(DjSmartList(id: id, name: name, symbol: symbol, kind: kind, songs: tracks))
        }

        let most = played.enumerated().filter { $0.element.plays > 1 }
            .sorted { $0.element.plays != $1.element.plays ? $0.element.plays > $1.element.plays : $0.offset < $1.offset }
        add("listen:most", "Most Played", "flame", .listening, most.prefix(listeningLimit).compactMap { known[$0.element.id] })
        let recent = played.enumerated().filter { $0.element.plays > 0 }
            .sorted { $0.element.last != $1.element.last ? $0.element.last > $1.element.last : $0.offset < $1.offset }
        add("listen:recent", "Recently Played", "clock", .listening, recent.prefix(listeningLimit).compactMap { known[$0.element.id] })
        add("listen:new", "Never Played", "sparkle", .listening, songs.filter { (plays[$0.id] ?? 0) == 0 })
        add("listen:once", "Played Once", "1.circle", .listening, songs.filter { plays[$0.id] == 1 })
        let rediscover = played.filter { $0.plays > 0 && $0.last > 0 && now - $0.last > rediscoverAfter }
        add("listen:rediscover", "Rediscover", "arrow.counterclockwise", .listening, rediscover.compactMap { known[$0.id] })

        let tagged = songs.map { ($0, Set(categories($0))) }
        for rule in genres {
            add("genre:\(rule.id)", rule.name, rule.symbol, .genre, tagged.filter { $0.1.contains("genre:\(rule.id)") }.map(\.0))
        }
        for rule in styles {
            add("style:\(rule.id)", rule.name, rule.symbol, .style, tagged.filter { $0.1.contains("style:\(rule.id)") }.map(\.0))
        }
        let decades = Set(tagged.flatMap { $0.1.filter { $0.hasPrefix("decade:") } }).compactMap { Int($0.dropFirst(7)) }.sorted()
        for decade in decades {
            add("decade:\(decade)", "\(decade)s", "calendar", .decade, tagged.filter { $0.1.contains("decade:\(decade)") }.map(\.0))
        }

        var artists: [String: [DjTrack]] = [:]
        var names: [String: [String: Int]] = [:]
        var order: [String] = []
        for song in songs {
            let key = artistKey(song)
            guard !key.isEmpty else { continue }
            if artists[key] == nil { order.append(key) }
            artists[key, default: []].append(song)
            names[key, default: [:]][artist(song), default: 0] += 1
        }
        let ranked = order.enumerated()
            .filter { (artists[$0.element]?.count ?? 0) >= minimumSongs }
            .sorted { (artists[$0.element]?.count ?? 0) != (artists[$1.element]?.count ?? 0) ? (artists[$0.element]?.count ?? 0) > (artists[$1.element]?.count ?? 0) : $0.offset < $1.offset }
            .prefix(artistLimit)
        for (_, key) in ranked {
            let spelled = names[key]?.max { $0.value != $1.value ? $0.value < $1.value : $0.key > $1.key }?.key ?? key
            add("artist:\(key)", spelled, "person", .artist, artists[key] ?? [])
        }
        return result
    }

    public static func similar(to seed: DjTrack, among songs: [DjTrack], playlists: [DjPlaylist] = [], limit: Int = 25) -> [DjSimilarSong] {
        let artist = artistKey(seed)
        let tags = Set(seed.tags)
        let lists = playlists.filter { $0.songs.contains(seed.id) }
        let kinds = Set(categories(seed))
        let labels = Dictionary((genres.map { ("genre:\($0.id)", $0.name) } + styles.map { ("style:\($0.id)", $0.name) }), uniquingKeysWith: { first, _ in first })
        var seen: Set<String> = [seed.id]
        var found: [(offset: Int, song: DjSimilarSong)] = []
        for (offset, song) in songs.enumerated() where seen.insert(song.id).inserted {
            var score = 0
            var reasons: [String] = []
            if !artist.isEmpty, artistKey(song) == artist {
                score += 5
                reasons.append("Same artist")
            }
            for tag in song.tags.filter(tags.contains) {
                score += 3
                reasons.append("#\(tag)")
            }
            for list in lists where list.songs.contains(song.id) {
                score += 2
                reasons.append(list.name)
            }
            for kind in categories(song) where kinds.contains(kind) {
                if kind.hasPrefix("genre:") {
                    score += 2
                    reasons.append(labels[kind] ?? kind)
                } else if kind.hasPrefix("style:") {
                    score += 1
                    reasons.append(labels[kind] ?? kind)
                } else if kind.hasPrefix("decade:") {
                    score += 1
                    reasons.append("\(kind.dropFirst(7))s")
                }
            }
            guard score > 0 else { continue }
            found.append((offset, DjSimilarSong(track: song, score: score, reasons: reasons)))
        }
        return Array(found.sorted { $0.song.score != $1.song.score ? $0.song.score > $1.song.score : $0.offset < $1.offset }.prefix(limit).map(\.song))
    }

    public static func searchQuery(_ seed: DjTrack) -> String {
        let artist = artist(seed)
        let genre = genres.first { matches($0, haystack(seed)) }?.name
        if !artist.isEmpty { return [artist, genre ?? "mix"].joined(separator: " ") }
        var title = seed.title
        for (open, close) in [("(", ")"), ("[", "]")] {
            while let start = title.range(of: open), let end = title.range(of: close, range: start.upperBound..<title.endIndex) {
                title.removeSubrange(start.lowerBound..<end.upperBound)
            }
        }
        return title.split(whereSeparator: \.isWhitespace).prefix(6).joined(separator: " ")
    }
}
