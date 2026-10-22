import FMBase
import Foundation

public struct DjMotionSample: Equatable, Sendable {
    public var x: Double
    public var y: Double

    public init(x: Double, y: Double) {
        self.x = x
        self.y = y
    }

    public static let still = DjMotionSample(x: 0, y: 0)
}

public enum DjMotion {
    public static let limit: Double = 2
    public static let shared = 12
    public static let dancing: Double = 0.3

    public static func clamp(_ value: Double) -> Double {
        guard value.isFinite else { return 0 }
        return min(limit, max(-limit, value))
    }

    public static func downsample(_ samples: [DjMotionSample], count: Int = shared) -> [DjMotionSample] {
        guard count > 0, !samples.isEmpty else { return [] }
        guard samples.count > count else { return samples.map { DjMotionSample(x: clamp($0.x), y: clamp($0.y)) } }
        let size = Double(samples.count) / Double(count)
        return (0..<count).map { index in
            let start = Int((Double(index) * size).rounded(.down))
            let end = max(start + 1, min(samples.count, Int((Double(index + 1) * size).rounded(.down))))
            let slice = samples[start..<end]
            let peak = slice.max { abs($0.x) + abs($0.y) < abs($1.x) + abs($1.y) } ?? .still
            return DjMotionSample(x: (clamp(peak.x) * 100).rounded() / 100, y: (clamp(peak.y) * 100).rounded() / 100)
        }
    }

    public static func energy(_ samples: [DjMotionSample]) -> Double {
        guard !samples.isEmpty else { return 0 }
        let power = samples.reduce(0) { $0 + $1.x * $1.x + $1.y * $1.y } / Double(samples.count)
        let rms = power.squareRoot()
        guard rms.isFinite else { return 0 }
        return min(1, max(0, (rms - 0.04) / 0.6))
    }

    public static func replay(_ samples: [DjMotionSample], progress: Double) -> DjMotionSample {
        guard !samples.isEmpty, progress.isFinite else { return .still }
        guard samples.count > 1 else { return samples[0] }
        let wrapped = progress - progress.rounded(.down)
        let place = wrapped * Double(samples.count - 1)
        let low = min(samples.count - 1, Int(place))
        let high = min(samples.count - 1, low + 1)
        let mix = place - Double(low)
        return DjMotionSample(
            x: samples[low].x + (samples[high].x - samples[low].x) * mix,
            y: samples[low].y + (samples[high].y - samples[low].y) * mix)
    }

    public static func body(_ samples: [DjMotionSample]) -> JSONValue {
        .array(samples.map { .array([.double($0.x), .double($0.y)]) })
    }

    static func parse(_ value: JSONValue?) -> [DjMotionSample] {
        (value?.arrayValue ?? []).prefix(32).compactMap { item in
            guard let pair = item.arrayValue, pair.count == 2, let x = pair[0].doubleValue, let y = pair[1].doubleValue else { return nil }
            return DjMotionSample(x: clamp(x), y: clamp(y))
        }
    }
}

public enum DjPresenceState: String, Sendable {
    case studio, listening, idle
}

public enum DjPersonStatus: Equatable, Sendable {
    case invited, away, idle, listening, studio

    public var label: String {
        switch self {
        case .invited: return "Invited"
        case .away: return "Away"
        case .idle: return "Not listening"
        case .listening: return "Listening in the background"
        case .studio: return "In the studio"
        }
    }

    public var live: Bool { self == .listening || self == .studio }
}

public struct DjJamPresence: Equatable, Sendable, Identifiable {
    public var name: String
    public var state: String
    public var playing: Bool
    public var energy: Double
    public var motion: [DjMotionSample]
    public var ago: Int64

    public init(name: String, state: String = "studio", playing: Bool = false, energy: Double = 0, motion: [DjMotionSample] = [], ago: Int64 = 0) {
        self.name = name
        self.state = state
        self.playing = playing
        self.energy = energy
        self.motion = motion
        self.ago = ago
    }

    public var id: String { name }
    public var dancing: Bool { energy >= DjMotion.dancing }

    init?(_ object: JSONObject) {
        guard let name = object["name"]?.stringValue, !name.isEmpty else { return nil }
        self.init(
            name: String(name.prefix(64)),
            state: object["state"]?.stringValue ?? "studio",
            playing: object["playing"]?.boolValue ?? false,
            energy: min(1, max(0, object["energy"]?.doubleValue ?? 0)),
            motion: DjMotion.parse(object["motion"]),
            ago: max(0, object["ago"]?.int64Value ?? 0))
    }

    public static func body(state: DjPresenceState, playing: Bool, motion: [DjMotionSample]) -> JSONObject {
        let shared = DjMotion.downsample(motion)
        return [
            "state": .string(state.rawValue), "playing": .bool(playing),
            "energy": .double((DjMotion.energy(motion) * 1000).rounded() / 1000), "motion": DjMotion.body(shared),
        ]
    }
}

public struct DjJamReaction: Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var kind: String
    public var ago: Int64

    public init(id: String, name: String, kind: String, ago: Int64 = 0) {
        self.id = id
        self.name = name
        self.kind = kind
        self.ago = ago
    }

    init?(_ object: JSONObject) {
        guard let id = object["id"]?.stringValue, !id.isEmpty, let kind = object["kind"]?.stringValue, DjReaction.find(kind) != nil else { return nil }
        self.init(id: id, name: object["name"]?.stringValue ?? "", kind: kind, ago: max(0, object["ago"]?.int64Value ?? 0))
    }
}

public enum DjReactionStyle: String, CaseIterable, Sendable {
    case rise, rain, burst, zoom, spin, orbit, wave, confetti, fireworks, flash
}

public struct DjReaction: Identifiable, Equatable, Sendable {
    public let id: String
    public let emoji: String
    public let name: String
    public let style: DjReactionStyle
    public let hue: Double

    public init(_ id: String, _ emoji: String, _ name: String, _ style: DjReactionStyle, hue: Double) {
        self.id = id
        self.emoji = emoji
        self.name = name
        self.style = style
        self.hue = hue
    }

    public var seconds: Double {
        switch style {
        case .zoom, .flash: return 1.6
        case .spin, .orbit, .burst: return 2.2
        case .rise, .rain, .wave, .confetti, .fireworks: return 3
        }
    }

    public static let all: [DjReaction] = [
        DjReaction("hearts", "❤️", "Hearts", .rise, hue: 0.98),
        DjReaction("fire", "🔥", "Fire", .rise, hue: 0.06),
        DjReaction("clap", "👏", "Applause", .burst, hue: 0.12),
        DjReaction("confetti", "🎉", "Confetti", .confetti, hue: 0.9),
        DjReaction("fireworks", "🎆", "Fireworks", .fireworks, hue: 0.62),
        DjReaction("hundred", "💯", "100", .zoom, hue: 0.0),
        DjReaction("laugh", "😂", "LOL", .rise, hue: 0.14),
        DjReaction("starstruck", "🤩", "Star-struck", .burst, hue: 0.15),
        DjReaction("mindblown", "🤯", "Mind Blown", .zoom, hue: 0.05),
        DjReaction("rocket", "🚀", "Lift Off", .rise, hue: 0.58),
        DjReaction("sparkles", "✨", "Sparkles", .rain, hue: 0.15),
        DjReaction("notes", "🎵", "Music", .orbit, hue: 0.75),
        DjReaction("headphones", "🎧", "Headphones", .spin, hue: 0.55),
        DjReaction("dancer", "💃", "Dance", .wave, hue: 0.95),
        DjReaction("groove", "🕺", "Groove", .wave, hue: 0.8),
        DjReaction("hands", "🙌", "Hands Up", .burst, hue: 0.1),
        DjReaction("thumbs", "👍", "Thumbs Up", .zoom, hue: 0.12),
        DjReaction("cool", "😎", "Cool", .zoom, hue: 0.6),
        DjReaction("lightning", "⚡", "Lightning", .flash, hue: 0.15),
        DjReaction("disco", "🪩", "Disco", .spin, hue: 0.7),
        DjReaction("rainbow", "🌈", "Rainbow", .wave, hue: 0.33),
        DjReaction("balloons", "🎈", "Balloons", .rise, hue: 0.0),
        DjReaction("snow", "❄️", "Snow", .rain, hue: 0.55),
        DjReaction("money", "💸", "Make It Rain", .rain, hue: 0.33),
        DjReaction("tears", "🥹", "Feels", .rain, hue: 0.58),
        DjReaction("eyes", "👀", "Eyes", .zoom, hue: 0.1),
        DjReaction("skull", "💀", "Dead", .burst, hue: 0.0),
        DjReaction("bomb", "💣", "Drop", .flash, hue: 0.03),
        DjReaction("rock", "🤘", "Rock On", .burst, hue: 0.85),
        DjReaction("loveeyes", "😍", "Love It", .orbit, hue: 0.95),
    ]

    public static func find(_ id: String) -> DjReaction? { all.first { $0.id == id } }
}

public enum DjAutomixStep: Equatable, Sendable {
    case wait
    case preload(DjDeckSide)
    case start(DjDeckSide)
    case mix(from: DjDeckSide, to: DjDeckSide, seconds: Double)
}

public struct DjAutomixDeck: Equatable, Sendable {
    public var loaded: Bool
    public var playing: Bool
    public var position: Double
    public var duration: Double
    public var rate: Double
    public var repeats: Bool

    public init(loaded: Bool, playing: Bool = false, position: Double = 0, duration: Double = 0, rate: Double = 1, repeats: Bool = false) {
        self.loaded = loaded
        self.playing = playing
        self.position = position
        self.duration = duration
        self.rate = rate
        self.repeats = repeats
    }

    public var remaining: Double? {
        guard duration > 0, position.isFinite else { return nil }
        return max(0, duration - position) / max(0.25, rate)
    }

    public var fresh: Bool {
        loaded && !playing && (duration <= 0 || position < duration - DjStudio.automixSeconds - 2)
    }
}

public enum DjEndReport: Equatable, Sendable {
    case ended
    case doubted
    case failed
}

public enum DjEnd {
    public static let slack: Double = 5
    public static let patience: Double = 3

    public static func judge(position: Double, duration: Double, doubtedSince: Double?, intended: Bool, at now: Double) -> DjEndReport {
        guard duration > 0, position.isFinite, position < duration - slack else { return .ended }
        guard intended, let since = doubtedSince, now - since >= patience else { return .doubted }
        return .failed
    }
}

public enum DjAutomix {
    public static let lead: Double = 2
    public static let audible: Double = 3

    public static func step(a: DjAutomixDeck, b: DjAutomixDeck, favoured: DjDeckSide, queued: Bool) -> DjAutomixStep {
        let decks: [DjDeckSide: DjAutomixDeck] = [.a: a, .b: b]
        let playing = DjDeckSide.allCases.filter { decks[$0]?.playing == true }
        guard let live = playing.count > 1 ? favoured : playing.first else {
            for side in [favoured, favoured.other] where decks[side]?.fresh == true { return .start(side) }
            return queued ? .preload(favoured) : .wait
        }
        let other = live.other
        guard let incoming = decks[other], !incoming.playing, let outgoing = decks[live], !outgoing.repeats else { return .wait }
        if !incoming.fresh { return queued ? .preload(other) : .wait }
        guard let remaining = outgoing.remaining, remaining <= DjStudio.automixSeconds + lead else { return .wait }
        return .mix(from: live, to: other, seconds: max(2, min(DjStudio.automixSeconds, remaining)))
    }

    public static func next(_ library: [DjPlayedSong], loaded: Set<String>, playing: Set<String>, seed: UInt64) -> DjPlayedSong? {
        var pool = library.filter { !loaded.contains($0.id) && !playing.contains($0.id) }
        if pool.isEmpty { pool = library.filter { !playing.contains($0.id) } }
        guard !pool.isEmpty else { return nil }
        let oldest = pool.enumerated()
            .sorted { $0.element.last != $1.element.last ? $0.element.last < $1.element.last : $0.offset > $1.offset }
            .map(\.element)
        let choices = oldest.prefix(max(1, (oldest.count + 1) / 2))
        var random = DjRandom(seed)
        return choices[choices.startIndex + Int(random.next() % UInt64(choices.count))]
    }
}
