import Foundation

public enum DjDeckSide: String, Codable, CaseIterable, Sendable, Identifiable {
    case a, b

    public var id: String { rawValue }
    public var other: DjDeckSide { self == .a ? .b : .a }
    public var label: String { rawValue.uppercased() }
}

public struct DjWaveBar: Equatable, Sendable {
    public let level: Double
    public let hue: Double

    public init(level: Double, hue: Double) {
        self.level = level
        self.hue = hue
    }
}

public struct DjSearchResult: Identifiable, Equatable, Sendable {
    public let id: String
    public let title: String
    public let author: String
    public let seconds: Double

    public init(id: String, title: String, author: String, seconds: Double = 0) {
        self.id = id
        self.title = title
        self.author = author
        self.seconds = seconds
    }

    public var thumbnail: URL { DjStudio.thumbnailURL(id, quality: "mqdefault") }
}

public enum DjDrumPad: String, CaseIterable, Codable, Sendable, Identifiable {
    case kick, snare, hat, clap, tom, perc, riser, stab

    public var id: String { rawValue }
    public var label: String { rawValue.capitalized }
}

public enum DjStudio {
    public static let secondsPerTurn: Double = 1.8
    public static let pitchRange: Double = 0.16
    public static let bendAmount: Double = 0.04
    public static let loopSeconds: Double = 8
    public static let sectionSeconds: Double = 8
    public static let sectionCount = 4
    public static let automixSeconds: Double = 8
    public static let rateBounds: ClosedRange<Double> = 0.25...2
    public static let storageKey = "fm.videoMixDjStudio.v1"
    public static let historyLimit = 40

    public static func isVideoId(_ value: String) -> Bool {
        value.utf8.count == 11 && value.unicodeScalars.allSatisfy { scalar in
            CharacterSet.alphanumerics.contains(scalar) && scalar.isASCII || scalar == "_" || scalar == "-"
        }
    }

    public static func videoId(_ value: String) -> String {
        var raw = value.trimmingCharacters(in: .whitespacesAndNewlines)
        if raw.hasPrefix("<") { raw.removeFirst() }
        if raw.hasSuffix(">") { raw.removeLast() }
        if isVideoId(raw) { return raw }
        var text = raw
        if text.range(of: #"^(?:www\.|m\.|music\.)?(?:youtube(?:-nocookie)?\.com|youtu\.be)/"#, options: [.regularExpression, .caseInsensitive]) != nil {
            text = "https://" + text
        }
        if let components = URLComponents(string: text), let found = components.host?.lowercased() {
            let host = found.replacingOccurrences(of: #"^(?:www\.|m\.|music\.)"#, with: "", options: .regularExpression)
            let items = components.queryItems ?? []
            let item = { (name: String) -> String? in items.first { $0.name == name }?.value }
            let segments = components.path.split(separator: "/").map(String.init)
            if host == "youtu.be" {
                let id = segments.first ?? item("v") ?? ""
                return isVideoId(id) ? id : ""
            }
            if host == "youtube.com" || host == "youtube-nocookie.com" {
                var candidates = [item("v"), item("vi"), item("video_id")]
                if components.path == "/attribution_link", let inner = item("u") {
                    let path = inner.hasPrefix("/") ? inner : "/" + inner
                    candidates.append(URLComponents(string: "https://www.youtube.com" + path)?.queryItems?.first { $0.name == "v" }?.value)
                }
                if let index = segments.firstIndex(where: { ["embed", "shorts", "live", "v", "e", "watch"].contains($0) }), index + 1 < segments.count {
                    candidates.append(segments[index + 1])
                }
                if let fragment = components.fragment, let range = fragment.range(of: #"[?&]v=[A-Za-z0-9_-]{11}"#, options: .regularExpression) {
                    candidates.append(String(fragment[range].suffix(11)))
                }
                return candidates.compactMap { $0 }.first(where: isVideoId) ?? ""
            }
        }
        let pattern = #"(?:youtu\.be/|youtube(?:-nocookie)?\.com/(?:watch\?(?:[^#\s]*&)?v=|(?:embed|shorts|live|v|e)/))([A-Za-z0-9_-]{11})"#
        guard let expression = try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive]),
            let match = expression.firstMatch(in: raw, range: NSRange(raw.startIndex..., in: raw)),
            let range = Range(match.range(at: 1), in: raw)
        else { return "" }
        return String(raw[range])
    }

    public static func watchURL(_ id: String) -> URL {
        var components = URLComponents(string: "https://www.youtube.com/watch")!
        components.queryItems = [URLQueryItem(name: "v", value: id)]
        return components.url!
    }

    public static func thumbnailURL(_ id: String, quality: String = "hqdefault") -> URL {
        URL(string: "https://i.ytimg.com/vi/\(id)/\(quality).jpg")!
    }

    public static func gains(crossfader: Double) -> (a: Double, b: Double) {
        let position = (min(1, max(-1, crossfader)) + 1) / 2
        let a = cos(position * Double.pi / 2)
        let b = sin(position * Double.pi / 2)
        return (abs(a) < 1e-9 ? 0 : a, abs(b) < 1e-9 ? 0 : b)
    }

    public static func gain(_ side: DjDeckSide, crossfader: Double) -> Double {
        let both = gains(crossfader: crossfader)
        return side == .a ? both.a : both.b
    }

    public static func clock(_ seconds: Double) -> String {
        let total = seconds.isFinite ? max(0, Int(seconds)) : 0
        let hours = total / 3600
        let minutes = total % 3600 / 60
        let secs = total % 60
        if hours > 0 { return String(format: "%d:%02d:%02d", hours, minutes, secs) }
        return String(format: "%02d:%02d", minutes, secs)
    }

    public static func clampRate(_ rate: Double) -> Double {
        guard rate.isFinite else { return 1 }
        return min(1 + pitchRange, max(1 - pitchRange, rate))
    }

    public static func syncedRate(ownBpm: Double, otherBpm: Double, otherRate: Double) -> Double {
        if ownBpm > 0, otherBpm > 0 { return clampRate(otherBpm * otherRate / ownBpm) }
        return clampRate(otherRate)
    }

    public static func scratchSeconds(fromRadians change: Double) -> Double {
        change / (2 * Double.pi) * secondsPerTurn
    }

    public static func scratchSpeed(previous: Double, seconds: Double, elapsed: Double) -> Double {
        guard seconds.isFinite, elapsed.isFinite, elapsed > 0 else { return previous }
        let instant = min(8, max(-8, seconds / max(0.008, elapsed)))
        return previous * 0.4 + instant * 0.6
    }

    public static func scratchLevel(_ speed: Double) -> Double {
        guard speed.isFinite else { return 0 }
        return min(1, abs(speed) / 2.5) * (speed < 0 ? 1 : 0.5)
    }

    public static func trackedPosition(predicted: Double, reported: Double, continuous: Bool) -> Double {
        guard reported.isFinite else { return predicted }
        guard continuous, predicted.isFinite else { return reported }
        let error = reported - predicted
        return abs(error) > 0.3 ? reported : predicted + error * 0.35
    }

    public static func miniBars(_ bars: [DjWaveBar], time: Double, duration: Double, count: Int = 7, spacing: Double = 0.18) -> [DjWaveBar] {
        guard count > 0, !bars.isEmpty, duration > 0, time.isFinite else {
            return Array(repeating: DjWaveBar(level: 0.08, hue: 0), count: max(0, count))
        }
        return (0..<count).map { index in
            let moment = time + (Double(index) - Double(count - 1) / 2) * spacing
            guard moment >= 0, moment <= duration else { return DjWaveBar(level: 0.08, hue: 0) }
            return bars[min(bars.count - 1, max(0, Int(moment / duration * Double(bars.count))))]
        }
    }

    public static func remember(_ history: [DjHistoryItem], id: String, title: String, author: String, limit: Int = historyLimit) -> [DjHistoryItem] {
        guard isVideoId(id), limit > 0 else { return history }
        let known = history.first { $0.id == id }
        let item = DjHistoryItem(id: id, title: title.isEmpty ? known?.title ?? "" : title, author: author.isEmpty ? known?.author ?? "" : author)
        return Array(([item] + history.filter { $0.id != id }).prefix(limit))
    }

    public static func turnDegrees(_ seconds: Double) -> Double {
        (seconds / secondsPerTurn * 360).truncatingRemainder(dividingBy: 360)
    }

    public static func waveform(_ id: String, count: Int = 360) -> [DjWaveBar] {
        guard count > 0, !id.isEmpty else { return [] }
        var seed: UInt64 = 0xcbf2_9ce4_8422_2325
        for byte in id.utf8 { seed = (seed ^ UInt64(byte)) &* 0x0000_0100_0000_01b3 }
        var random = DjRandom(seed)
        let sectionCount = 5 + Int(random.next() % 4)
        let bounds = ((1..<sectionCount).map { _ in Int(random.unit() * Double(count)) } + [count]).sorted()
        var levels: [Double] = []
        var hues: [Double] = []
        levels.reserveCapacity(count)
        hues.reserveCapacity(count)
        var start = 0
        for (index, end) in bounds.enumerated() {
            let edge = index == 0 || index == bounds.count - 1
            let energy = (0.35 + random.unit() * 0.65) * (edge ? 0.55 : 1)
            let hue = random.unit()
            for bar in start..<max(start, end) {
                let pulse = bar % 4 == 0 ? 1 : 0.5 + 0.3 * random.unit()
                levels.append(min(1, max(0.06, energy * pulse * (0.55 + 0.45 * random.unit()))))
                hues.append((hue + 0.08 * sin(Double(bar) * 0.23) + 1).truncatingRemainder(dividingBy: 1))
            }
            start = max(start, end)
        }
        return (0..<count).map { index in
            let previous = levels[max(0, index - 1)]
            let next = levels[min(count - 1, index + 1)]
            return DjWaveBar(level: levels[index] * 0.6 + (previous + next) * 0.2, hue: hues[index])
        }
    }

    public static func searchInstances(_ data: Data, limit: Int = 3) -> [URL] {
        guard let rows = try? JSONSerialization.jsonObject(with: data) as? [Any] else { return [] }
        let found: [URL] = rows.compactMap { row in
            guard let pair = row as? [Any], pair.count > 1, let details = pair[1] as? [String: Any],
                details["api"] as? Bool == true, let uri = details["uri"] as? String, uri.hasPrefix("https://")
            else { return nil }
            return URL(string: uri.hasSuffix("/") ? String(uri.dropLast()) : uri)
        }
        return Array(found.prefix(limit))
    }

    public static func searchURL(_ instance: URL, query: String) -> URL? {
        var components = URLComponents(url: instance.appendingPathComponent("api/v1/search"), resolvingAgainstBaseURL: false)
        components?.queryItems = [URLQueryItem(name: "q", value: query), URLQueryItem(name: "type", value: "video")]
        return components?.url
    }

    public static func searchResults(_ data: Data) -> [DjSearchResult] {
        guard let rows = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else { return [] }
        var seen = Set<String>()
        return rows.compactMap { row in
            let link = (row["url"] as? String).map { $0.hasPrefix("/") ? "https://www.youtube.com" + $0 : $0 }
            let id = videoId((row["videoId"] as? String) ?? link ?? "")
            guard !id.isEmpty, seen.insert(id).inserted else { return nil }
            let title = (row["title"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? "Untitled video"
            let seconds = (row["lengthSeconds"] as? NSNumber)?.doubleValue ?? 0
            return DjSearchResult(id: id, title: title, author: row["author"] as? String ?? "", seconds: seconds)
        }
    }
}

public struct DjTapTempo: Sendable {
    public private(set) var taps: [Double] = []

    public init() {}

    public mutating func tap(at time: Double) -> Double? {
        if let last = taps.last, time - last > 2 || time <= last { taps.removeAll() }
        taps.append(time)
        if taps.count > 8 { taps.removeFirst(taps.count - 8) }
        guard taps.count >= 3, let first = taps.first, let last = taps.last else { return nil }
        let bpm = 60 / ((last - first) / Double(taps.count - 1))
        guard bpm.isFinite, (40...250).contains(bpm) else { return nil }
        return (bpm * 10).rounded() / 10
    }
}

struct DjRandom {
    private var state: UInt64

    init(_ seed: UInt64) { state = seed }

    mutating func next() -> UInt64 {
        state &+= 0x9E37_79B9_7F4A_7C15
        var value = state
        value = (value ^ (value >> 30)) &* 0xBF58_476D_1CE4_E5B9
        value = (value ^ (value >> 27)) &* 0x94D0_49BB_1331_11EB
        return value ^ (value >> 31)
    }

    mutating func unit() -> Double { Double(next() >> 11) / Double(1 << 53) }
}

public struct DjSavedDeck: Codable, Equatable, Sendable {
    public var videoId: String = ""
    public var title: String = ""
    public var author: String = ""
    public var cue: Double = 0
    public var level: Double = 1
    public var rate: Double = 1
    public var bpm: Double = 0
    public var loop: Bool = false

    public init() {}

    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        let id = (try? values.decodeIfPresent(String.self, forKey: .videoId)) ?? nil
        videoId = DjStudio.isVideoId(id ?? "") ? id ?? "" : ""
        title = String(((try? values.decodeIfPresent(String.self, forKey: .title)) ?? nil)?.prefix(300) ?? "")
        author = String(((try? values.decodeIfPresent(String.self, forKey: .author)) ?? nil)?.prefix(200) ?? "")
        cue = DjSaved.clamp((try? values.decodeIfPresent(Double.self, forKey: .cue)) ?? nil, 0, 0...864_000)
        level = DjSaved.clamp((try? values.decodeIfPresent(Double.self, forKey: .level)) ?? nil, 1, 0...1)
        rate = DjStudio.clampRate(DjSaved.clamp((try? values.decodeIfPresent(Double.self, forKey: .rate)) ?? nil, 1, DjStudio.rateBounds))
        bpm = DjSaved.clamp((try? values.decodeIfPresent(Double.self, forKey: .bpm)) ?? nil, 0, 0...250)
        loop = ((try? values.decodeIfPresent(Bool.self, forKey: .loop)) ?? nil) ?? false
    }
}

public struct DjHistoryItem: Codable, Equatable, Sendable, Identifiable {
    public var id: String
    public var title: String
    public var author: String

    public init(id: String, title: String, author: String) {
        self.id = id
        self.title = String(title.prefix(300))
        self.author = String(author.prefix(200))
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
            author: ((try? values.decodeIfPresent(String.self, forKey: .author)) ?? nil) ?? "")
    }
}

public struct DjSection: Codable, Equatable, Sendable {
    public var deck: DjDeckSide
    public var start: Double
    public var duration: Double

    public init(deck: DjDeckSide, start: Double, duration: Double) {
        self.deck = deck
        self.start = max(0, start)
        self.duration = min(30, max(1, duration))
    }

    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        self.init(
            deck: try values.decode(DjDeckSide.self, forKey: .deck),
            start: DjSaved.clamp(try values.decode(Double.self, forKey: .start), 0, 0...864_000),
            duration: DjSaved.clamp(try values.decode(Double.self, forKey: .duration), DjStudio.sectionSeconds, 1...30))
    }
}

public struct DjSaved: Codable, Equatable, Sendable {
    public var a = DjSavedDeck()
    public var b = DjSavedDeck()
    public var crossfader: Double = 0
    public var samplerLevel: Double = 0.8
    public var sections: [DjSection?] = Array(repeating: nil, count: DjStudio.sectionCount)
    public var zoomed = false
    public var history: [DjHistoryItem] = []

    public init() {}

    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        a = ((try? values.decodeIfPresent(DjSavedDeck.self, forKey: .a)) ?? nil) ?? DjSavedDeck()
        b = ((try? values.decodeIfPresent(DjSavedDeck.self, forKey: .b)) ?? nil) ?? DjSavedDeck()
        crossfader = DjSaved.clamp((try? values.decodeIfPresent(Double.self, forKey: .crossfader)) ?? nil, 0, -1...1)
        samplerLevel = DjSaved.clamp((try? values.decodeIfPresent(Double.self, forKey: .samplerLevel)) ?? nil, 0.8, 0...1)
        let stored = ((try? values.decodeIfPresent([DjSection?].self, forKey: .sections)) ?? nil) ?? []
        sections = (0..<DjStudio.sectionCount).map { $0 < stored.count ? stored[$0] : nil }
        zoomed = ((try? values.decodeIfPresent(Bool.self, forKey: .zoomed)) ?? nil) ?? false
        let entries = ((try? values.decodeIfPresent([DjHistoryEntry].self, forKey: .history)) ?? nil) ?? []
        var seen = Set<String>()
        history = Array(entries.compactMap(\.item).filter { seen.insert($0.id).inserted }.prefix(DjStudio.historyLimit))
    }

    public subscript(side: DjDeckSide) -> DjSavedDeck {
        get { side == .a ? a : b }
        set { if side == .a { a = newValue } else { b = newValue } }
    }

    public static func decode(_ data: Data?) -> DjSaved {
        guard let data, let saved = try? JSONDecoder().decode(DjSaved.self, from: data) else { return DjSaved() }
        return saved
    }

    public func encoded() -> Data? { try? JSONEncoder().encode(self) }

    static func clamp(_ value: Double?, _ fallback: Double, _ range: ClosedRange<Double>) -> Double {
        guard let value, value.isFinite else { return fallback }
        return min(range.upperBound, max(range.lowerBound, value))
    }
}

struct DjHistoryEntry: Decodable {
    let item: DjHistoryItem?

    init(from decoder: Decoder) throws {
        item = try? DjHistoryItem(from: decoder)
    }
}

public enum DjDrumSynth {
    public static func samples(_ pad: DjDrumPad, sampleRate: Double = 44_100) -> [Float] {
        var out = [Float](repeating: 0, count: Int(sampleRate * length(pad)))
        var random = DjRandom(0x5eed_0000 &+ UInt64(DjDrumPad.allCases.firstIndex(of: pad) ?? 0))
        switch pad {
        case .kick: tone(&out, sampleRate, .sine, 155, 43, 0.32, 0.8)
        case .tom: tone(&out, sampleRate, .sine, 190, 80, 0.36, 0.65)
        case .perc: tone(&out, sampleRate, .triangle, 740, 160, 0.16, 0.36)
        case .riser: tone(&out, sampleRate, .saw, 160, 1300, 0.75, 0.25)
        case .stab: for frequency in [196.0, 247, 294] { tone(&out, sampleRate, .saw, frequency, frequency * 0.98, 0.22, 0.13) }
        case .snare: noise(&out, sampleRate, &random, 0, 0.18, 1800, 0.5)
        case .hat: noise(&out, sampleRate, &random, 0, 0.07, 6000, 0.22)
        case .clap:
            noise(&out, sampleRate, &random, 0, 0.3, 1000, 0.5)
            for delay in [0.045, 0.09] { noise(&out, sampleRate, &random, delay, 0.12, 1000, 0.25) }
        }
        return out.map { min(1, max(-1, $0)) }
    }

    static func length(_ pad: DjDrumPad) -> Double {
        switch pad {
        case .kick: return 0.35
        case .tom: return 0.39
        case .perc: return 0.19
        case .riser: return 0.78
        case .stab: return 0.25
        case .snare: return 0.21
        case .hat: return 0.1
        case .clap: return 0.33
        }
    }

    enum Wave { case sine, triangle, saw }

    static func envelope(_ time: Double, _ duration: Double, _ peak: Double) -> Double {
        let attack = 0.004
        let floor = 0.0001
        if time < attack { return floor * pow(peak / floor, time / attack) }
        if time >= duration { return 0 }
        return peak * pow(floor / peak, (time - attack) / (duration - attack))
    }

    static func tone(_ out: inout [Float], _ rate: Double, _ wave: Wave, _ start: Double, _ end: Double, _ duration: Double, _ peak: Double) {
        var phase = 0.0
        for index in 0..<min(out.count, Int(duration * rate)) {
            let time = Double(index) / rate
            phase += start * pow(end / start, time / duration) / rate
            let cycle = phase - phase.rounded(.down)
            let value: Double
            switch wave {
            case .sine: value = sin(2 * Double.pi * cycle)
            case .triangle: value = 1 - 4 * abs(cycle - 0.5)
            case .saw: value = 2 * cycle - 1
            }
            out[index] += Float(value * envelope(time, duration, peak))
        }
    }

    static func noise(_ out: inout [Float], _ rate: Double, _ random: inout DjRandom, _ delay: Double, _ duration: Double, _ cutoff: Double, _ peak: Double) {
        let resistance = 1 / (2 * Double.pi * cutoff)
        let alpha = resistance / (resistance + 1 / rate)
        var previousIn = 0.0
        var previousOut = 0.0
        let first = Int(delay * rate)
        for index in 0..<Int(duration * rate) where first + index < out.count {
            let input = random.unit() * 2 - 1
            previousOut = alpha * (previousOut + input - previousIn)
            previousIn = input
            out[first + index] += Float(previousOut * envelope(Double(index) / rate, duration, peak))
        }
    }
}
