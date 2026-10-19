import Foundation

public struct DjAutoDjCandidate: Equatable, Sendable, Identifiable {
    public var id: String
    public var title: String
    public var author: String
    public var seconds: Double
    public var views: Double
    public var likes: Double?
    public var rank: Int

    public init(id: String, title: String, author: String = "", seconds: Double = 0, views: Double = 0, likes: Double? = nil, rank: Int = 0) {
        self.id = id
        self.title = title
        self.author = author
        self.seconds = seconds
        self.views = views
        self.likes = likes
        self.rank = rank
    }

    public var result: DjSearchResult { DjSearchResult(id: id, title: title, author: author, seconds: seconds) }
}

public enum DjAutoDj {
    public static let shortest: Double = 60
    public static let longest: Double = 12 * 60
    public static let checked = 6
    public static let likeShare: Double = 0.01
    public static let sessionLimit = 200

    static let noise: Set<String> = [
        "official", "video", "audio", "music", "lyrics", "lyric", "visualizer", "visualiser", "hd", "hq", "4k", "mv", "m v",
        "remastered", "remaster", "explicit", "clean", "version", "full", "feat", "ft", "featuring", "topic",
    ]

    public static func candidates(_ data: Data) -> [DjAutoDjCandidate] {
        guard let object = try? JSONSerialization.jsonObject(with: data) else { return [] }
        let rows = (object as? [String: Any])?["recommendedVideos"] as? [[String: Any]] ?? object as? [[String: Any]] ?? []
        var seen = Set<String>()
        return rows.compactMap { row -> DjAutoDjCandidate? in
            if let type = row["type"] as? String, type != "video" { return nil }
            let link = (row["url"] as? String).map { $0.hasPrefix("/") ? "https://www.youtube.com" + $0 : $0 }
            let id = DjStudio.videoId((row["videoId"] as? String) ?? link ?? "")
            guard !id.isEmpty, seen.insert(id).inserted else { return nil }
            let title = (row["title"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? "Untitled video"
            let views = (row["viewCount"] as? NSNumber)?.doubleValue ?? count(row["viewCountText"] as? String ?? "")
            return DjAutoDjCandidate(
                id: id, title: title, author: row["author"] as? String ?? "", seconds: (row["lengthSeconds"] as? NSNumber)?.doubleValue ?? 0,
                views: views, rank: seen.count - 1)
        }
    }

    public static func count(_ text: String) -> Double {
        let lowered = text.lowercased().replacingOccurrences(of: ",", with: "")
        guard let match = lowered.range(of: #"[0-9]+(\.[0-9]+)?\s*[kmb]?"#, options: .regularExpression) else { return 0 }
        let part = lowered[match].replacingOccurrences(of: " ", with: "")
        let scale: Double = part.hasSuffix("k") ? 1e3 : part.hasSuffix("m") ? 1e6 : part.hasSuffix("b") ? 1e9 : 1
        return (Double(part.trimmingCharacters(in: CharacterSet(charactersIn: "kmb"))) ?? 0) * scale
    }

    public static func likesURL(_ instance: URL, id: String) -> URL? {
        guard DjStudio.isVideoId(id) else { return nil }
        var components = URLComponents(url: instance.appendingPathComponent("api/v1/videos/\(id)"), resolvingAgainstBaseURL: false)
        components?.queryItems = [URLQueryItem(name: "fields", value: "likeCount,viewCount")]
        return components?.url
    }

    public static func likes(_ data: Data) -> (likes: Double, views: Double)? {
        guard let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
            let likes = (object["likeCount"] as? NSNumber)?.doubleValue, likes >= 0
        else { return nil }
        return (likes, max(0, (object["viewCount"] as? NSNumber)?.doubleValue ?? 0))
    }

    public static func songKey(_ title: String) -> String {
        var name = title
        for (open, close) in [("(", ")"), ("[", "]"), ("{", "}")] {
            while let start = name.range(of: open), let end = name.range(of: close, range: start.upperBound..<name.endIndex) {
                name.removeSubrange(start.lowerBound..<end.upperBound)
            }
        }
        for dash in [" - ", " – ", " — ", " | "] {
            if let split = name.range(of: dash) {
                let tail = String(name[split.upperBound...])
                if !key(tail).isEmpty { name = tail }
                break
            }
        }
        return key(name)
    }

    static func key(_ text: String) -> String {
        DjSmart.text(text).split(separator: " ").map(String.init).filter { !noise.contains($0) }.joined(separator: " ")
    }

    public static func eligible(_ found: [DjAutoDjCandidate], seed: DjTrack, avoid: Set<String>, heard: [String]) -> [DjAutoDjCandidate] {
        var keys = Set(([seed.title] + heard).map(songKey).filter { !$0.isEmpty })
        return found.filter { candidate in
            guard candidate.id != seed.id, !avoid.contains(candidate.id) else { return false }
            guard candidate.seconds >= shortest, candidate.seconds <= longest else { return false }
            let key = songKey(candidate.title)
            return key.isEmpty || keys.insert(key).inserted
        }
    }

    public static func score(_ candidate: DjAutoDjCandidate, among count: Int) -> Double {
        let likes = candidate.likes ?? candidate.views * likeShare
        let related = count > 1 ? 1 - Double(candidate.rank) / Double(count - 1) : 1
        let ratio = candidate.likes != nil && candidate.views > 0 ? min(0.1, likes / candidate.views) * 10 : 0
        return log10(1 + max(0, likes)) + ratio + related * 2
    }

    public static func ranked(_ found: [DjAutoDjCandidate]) -> [DjAutoDjCandidate] {
        let span = (found.map(\.rank).max() ?? 0) + 1
        return found.enumerated()
            .sorted {
                let left = score($0.element, among: span)
                let right = score($1.element, among: span)
                return left != right ? left > right : $0.offset < $1.offset
            }
            .map(\.element)
    }

    public static func next(_ ranked: [DjAutoDjCandidate], loaded: Set<String>, skip: Set<String>, played: [String]) -> DjAutoDjCandidate? {
        ranked.first { !loaded.contains($0.id) && !skip.contains($0.id) && !played.contains($0.id) }
    }

    public static func remember(_ id: String, in played: [String]) -> [String] {
        Array(([id] + played.filter { $0 != id }).prefix(sessionLimit))
    }
}
