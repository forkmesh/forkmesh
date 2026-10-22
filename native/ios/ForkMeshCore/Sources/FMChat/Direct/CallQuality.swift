import FMBase
import Foundation

public enum CallQualityGrade: String, Equatable, Sendable {
    case good
    case fair
    case poor
    case unknown

    public init(bars: Int) {
        switch bars {
        case 4: self = .good
        case 1: self = .poor
        case 2, 3: self = .fair
        default: self = .unknown
        }
    }

    public var label: String {
        switch self {
        case .good: return "Good connection"
        case .fair: return "Fair connection"
        case .poor: return "Poor connection"
        case .unknown: return "Measuring connection"
        }
    }
}

public struct CallVoiceLevel: Equatable, Sendable {
    public var mic: Double
    public var peer: Double?

    public init(mic: Double, peer: Double? = nil) {
        self.mic = mic
        self.peer = peer
    }
}

public struct CallQualitySample: Equatable, Sendable {
    public var at: Double
    public var rttMs: Double?
    public var lossPct: Double?
    public var jitterMs: Double?
    public var sendKbps: Double?
    public var recvKbps: Double?
    public var inWidth: Int?
    public var inHeight: Int?
    public var inFps: Double?
    public var outWidth: Int?
    public var outHeight: Int?
    public var outFps: Double?
    public var limit: String
    public var micLevel: Double?
    public var peerLevel: Double?

    public init(
        at: Double = 0, rttMs: Double? = nil, lossPct: Double? = nil, jitterMs: Double? = nil, sendKbps: Double? = nil,
        recvKbps: Double? = nil, inWidth: Int? = nil, inHeight: Int? = nil, inFps: Double? = nil, outWidth: Int? = nil,
        outHeight: Int? = nil, outFps: Double? = nil, limit: String = "", micLevel: Double? = nil, peerLevel: Double? = nil
    ) {
        self.at = at
        self.rttMs = rttMs
        self.lossPct = lossPct
        self.jitterMs = jitterMs
        self.sendKbps = sendKbps
        self.recvKbps = recvKbps
        self.inWidth = inWidth
        self.inHeight = inHeight
        self.inFps = inFps
        self.outWidth = outWidth
        self.outHeight = outHeight
        self.outFps = outFps
        self.limit = limit
        self.micLevel = micLevel
        self.peerLevel = peerLevel
    }
}

public enum CallQualityDirection: Sendable {
    case incoming
    case outgoing
}

public enum CallQuality {
    public static let levelEvery: Double = 0.1
    public static let statsEvery: Double = 1
    public static let levelSamples = 50
    public static let statsSamples = 60
    public static let pillSamples = 20
    public static let openKey = "fm.call.qualityOpen"
    public static let rttSteps: [Double] = [150, 300, 500]
    public static let lossSteps: [Double] = [1, 3, 8]
    public static let jitterSteps: [Double] = [30, 60, 100]

    static func step(_ value: Double?, _ steps: [Double]) -> Int {
        guard let value else { return 0 }
        return steps.filter { value >= $0 }.count
    }

    public static func bars(_ sample: CallQualitySample?) -> Int {
        guard let sample, sample.rttMs != nil || sample.lossPct != nil else { return 0 }
        let penalty = max(
            step(sample.rttMs, rttSteps), step(sample.lossPct, lossSteps), step(sample.jitterMs, jitterSteps))
        return max(1, 4 - penalty)
    }

    public static func grade(_ sample: CallQualitySample?) -> CallQualityGrade {
        CallQualityGrade(bars: bars(sample))
    }

    public static func level(rms: Double) -> Double {
        guard rms.isFinite, rms > 0 else { return 0 }
        return level(decibels: 20 * log10(rms))
    }

    public static func level(decibels: Double) -> Double {
        guard decibels.isFinite else { return 0 }
        return min(1, max(0, (decibels + 60) / 60))
    }

    public static func level(samples: UnsafeBufferPointer<Float>) -> Double {
        guard !samples.isEmpty else { return 0 }
        var sum: Double = 0
        for value in samples { sum += Double(value) * Double(value) }
        return level(rms: (sum / Double(samples.count)).squareRoot())
    }

    public static func level(samples: [Float]) -> Double {
        samples.withUnsafeBufferPointer { level(samples: $0) }
    }

    public static func lossMark(_ lossPct: Double?) -> CallQualityGrade? {
        guard let lossPct, lossPct >= lossSteps[0] else { return nil }
        return lossPct >= lossSteps[2] ? .poor : .fair
    }

    public static func formatKbps(_ kbps: Double?) -> String {
        guard let kbps, kbps.isFinite else { return "" }
        if kbps >= 1000 { return String(format: "%.1f Mb/s", kbps / 1000) }
        return "\(Int(kbps.rounded())) kb/s"
    }

    public static func formatLoss(_ lossPct: Double) -> String {
        String(format: lossPct < 10 ? "%.1f" : "%.0f", lossPct) + "% loss"
    }

    public static func describeNetwork(_ sample: CallQualitySample?) -> String {
        guard let sample, let rtt = sample.rttMs else { return "Measuring…" }
        var parts = ["\(Int(rtt.rounded())) ms"]
        if let loss = sample.lossPct { parts.append(formatLoss(loss)) }
        if let jitter = sample.jitterMs { parts.append("\(Int(jitter.rounded())) ms jitter") }
        return parts.joined(separator: " · ")
    }

    public static func limitLabel(_ limit: String) -> String {
        switch limit {
        case "": return ""
        case "bandwidth": return "limited by bandwidth"
        case "cpu": return "limited by CPU"
        default: return "limited"
        }
    }

    public static func describeVideo(_ sample: CallQualitySample?, _ direction: CallQualityDirection) -> String {
        guard let sample else { return "" }
        let incoming = direction == .incoming
        let width = incoming ? sample.inWidth : sample.outWidth
        let height = incoming ? sample.inHeight : sample.outHeight
        let fps = incoming ? sample.inFps : sample.outFps
        let kbps = incoming ? sample.recvKbps : sample.sendKbps
        var parts: [String] = []
        if let width, let height, width > 0, height > 0 { parts.append("\(width)×\(height)") }
        if let fps, fps.isFinite { parts.append("\(Int(fps.rounded())) fps") }
        let speed = formatKbps(kbps)
        if !speed.isEmpty { parts.append(speed) }
        if !incoming, !sample.limit.isEmpty { parts.append(limitLabel(sample.limit)) }
        return parts.joined(separator: " · ")
    }

    public static func pillText(_ sample: CallQualitySample?, live: Bool) -> String {
        guard live, let rtt = sample?.rttMs else { return "" }
        return "\(Int(rtt.rounded())) ms"
    }

    public static func summary(_ sample: CallQualitySample?, live: Bool) -> String {
        guard live else { return "Microphone level" }
        let label = grade(sample).label
        guard let sample else { return label }
        return label + " · " + describeNetwork(sample)
    }

    public static func accessibilityLabel(_ sample: CallQualitySample?, live: Bool) -> String {
        "Call quality: " + summary(sample, live: live)
    }

    public static func voiceValue(hasPeer: Bool, peerLabel: String = "Them") -> String {
        hasPeer ? "You ▲ · \(peerLabel) ▼" : "You"
    }
}

public enum CallStatsValue: Equatable, Sendable {
    case number(Double)
    case text(String)
    case flag(Bool)
}

public struct CallStatsEntry: Equatable, Sendable {
    public let id: String
    public let type: String
    public let values: [String: CallStatsValue]

    public init(id: String, type: String, values: [String: CallStatsValue]) {
        self.id = id
        self.type = type
        self.values = values
    }

    public func number(_ key: String) -> Double? {
        switch values[key] {
        case let .number(value): return value.isFinite ? value : nil
        case let .flag(value): return value ? 1 : 0
        case let .text(value): return Double(value).flatMap { $0.isFinite ? $0 : nil }
        default: return nil
        }
    }

    public func text(_ key: String) -> String {
        switch values[key] {
        case let .text(value): return value
        case let .number(value): return String(value)
        case let .flag(value): return value ? "true" : "false"
        default: return ""
        }
    }

    public func flag(_ key: String) -> Bool {
        switch values[key] {
        case let .flag(value): return value
        case let .number(value): return value != 0
        case let .text(value): return value == "true"
        default: return false
        }
    }

    public var kind: String {
        let kind = text("kind")
        return kind.isEmpty ? text("mediaType") : kind
    }
}

public struct CallStatsCounters: Equatable, Sendable {
    public var at: Double
    public var sent: Double = 0
    public var received: Double = 0
    public var lost: Double = 0
    public var packets: Double = 0
    public var frames: [String: Double] = [:]

    public init(at: Double) {
        self.at = at
    }
}

public enum CallStatsSummary {
    static func maxOf(_ current: Double?, _ value: Double?) -> Double? {
        guard let value else { return current }
        guard let current else { return value }
        return max(current, value)
    }

    static func rate(_ bytes: Double, _ previous: Double, _ seconds: Double) -> Double? {
        guard seconds > 0 else { return nil }
        return max(0, (bytes - previous) * 8 / 1000 / seconds)
    }

    public static func summarize(
        _ reports: [[CallStatsEntry]], previous: CallStatsCounters?, at: Double
    ) -> (sample: CallQualitySample, counters: CallStatsCounters) {
        var sample = CallQualitySample(at: at)
        var counters = CallStatsCounters(at: at)
        let seconds = previous.map { at - $0.at } ?? 0
        var sentSeen = false
        var receivedSeen = false
        var inArea = 0
        var outArea = 0
        var remoteLoss: Double?
        for (index, entries) in reports.enumerated() {
            var byId: [String: CallStatsEntry] = [:]
            for entry in entries { byId[entry.id] = entry }
            var pair: CallStatsEntry?
            for entry in entries where entry.type == "transport" {
                let selected = entry.text("selectedCandidatePairId")
                if !selected.isEmpty, let found = byId[selected] { pair = found }
            }
            if pair == nil {
                pair = entries.first { entry in
                    entry.type == "candidate-pair" && entry.text("state") == "succeeded"
                        && (entry.flag("selected") || entry.flag("nominated"))
                }
            }
            let pairRtt = pair?.number("currentRoundTripTime")
            if let pairRtt { sample.rttMs = maxOf(sample.rttMs, pairRtt * 1000) }
            for entry in entries {
                let kind = entry.kind
                switch entry.type {
                case "outbound-rtp":
                    if let bytes = entry.number("bytesSent") {
                        counters.sent += bytes
                        sentSeen = true
                    }
                    if kind == "video" {
                        let width = Int(entry.number("frameWidth") ?? 0)
                        let height = Int(entry.number("frameHeight") ?? 0)
                        if width * height >= outArea, width * height > 0 {
                            outArea = width * height
                            sample.outWidth = width
                            sample.outHeight = height
                            sample.outFps = entry.number("framesPerSecond")
                            let reason = entry.text("qualityLimitationReason")
                            sample.limit = reason == "none" ? "" : reason
                        }
                    }
                case "inbound-rtp":
                    if let bytes = entry.number("bytesReceived") {
                        counters.received += bytes
                        receivedSeen = true
                    }
                    if let lost = entry.number("packetsLost") { counters.lost += max(0, lost) }
                    if let packets = entry.number("packetsReceived") { counters.packets += packets }
                    if kind == "audio" {
                        if let jitter = entry.number("jitter") { sample.jitterMs = maxOf(sample.jitterMs, jitter * 1000) }
                        sample.peerLevel = maxOf(sample.peerLevel, entry.number("audioLevel"))
                    } else if kind == "video" {
                        let width = Int(entry.number("frameWidth") ?? 0)
                        let height = Int(entry.number("frameHeight") ?? 0)
                        let key = "\(index):\(entry.id)"
                        let decoded = entry.number("framesDecoded")
                        if let decoded { counters.frames[key] = decoded }
                        var fps = entry.number("framesPerSecond")
                        if fps == nil, let decoded, let before = previous?.frames[key], seconds > 0 {
                            fps = max(0, (decoded - before) / seconds)
                        }
                        if width * height >= inArea, width * height > 0 {
                            inArea = width * height
                            sample.inWidth = width
                            sample.inHeight = height
                            sample.inFps = fps
                        }
                    }
                case "remote-inbound-rtp":
                    if let fraction = entry.number("fractionLost") { remoteLoss = maxOf(remoteLoss, fraction * 100) }
                    if let rtt = entry.number("roundTripTime"), pairRtt == nil {
                        sample.rttMs = maxOf(sample.rttMs, rtt * 1000)
                    }
                case "media-source" where kind == "audio":
                    sample.micLevel = maxOf(sample.micLevel, entry.number("audioLevel"))
                default:
                    break
                }
            }
        }
        if let previous {
            sample.sendKbps = sentSeen ? rate(counters.sent, previous.sent, seconds) : nil
            sample.recvKbps = receivedSeen ? rate(counters.received, previous.received, seconds) : nil
            let lost = counters.lost - previous.lost
            let packets = counters.packets - previous.packets
            if lost >= 0, packets >= 0, lost + packets > 0 {
                sample.lossPct = lost * 100 / (lost + packets)
            } else if packets == 0, lost == 0, receivedSeen {
                sample.lossPct = 0
            }
        }
        if let remoteLoss { sample.lossPct = maxOf(sample.lossPct, remoteLoss) }
        return (sample, counters)
    }

    public static func levels(_ reports: [[CallStatsEntry]], muted: Bool = false) -> CallVoiceLevel {
        var mic: Double?
        var peer: Double?
        for entries in reports {
            for entry in entries {
                if entry.type == "media-source", entry.kind == "audio" {
                    mic = maxOf(mic, entry.number("audioLevel"))
                } else if entry.type == "inbound-rtp", entry.kind == "audio" {
                    peer = maxOf(peer, entry.number("audioLevel"))
                }
            }
        }
        return CallVoiceLevel(
            mic: muted ? 0 : CallQuality.level(rms: mic ?? 0), peer: peer.map { CallQuality.level(rms: $0) })
    }
}

public struct CallQualityEvent: Equatable, Sendable {
    public var live: Bool
    public var sample: CallQualitySample
    public var levels: [CallVoiceLevel]

    public init(live: Bool, sample: CallQualitySample, levels: [CallVoiceLevel]) {
        self.live = live
        self.sample = sample
        self.levels = levels
    }

    public init?(_ message: JSONObject, at: Double = Date().timeIntervalSince1970) {
        guard message["event"]?.text == "quality" else { return nil }
        func number(_ key: String) -> Double? {
            guard let value = message[key]?.doubleValue, value.isFinite else { return nil }
            return value
        }
        func whole(_ key: String) -> Int? { number(key).map { Int($0.rounded()) } }
        live = message["live"]?.boolValue ?? false
        sample = CallQualitySample(
            at: at, rttMs: number("rttMs"), lossPct: number("lossPct"), jitterMs: number("jitterMs"),
            sendKbps: number("sendKbps"), recvKbps: number("recvKbps"), inWidth: whole("inWidth"),
            inHeight: whole("inHeight"), inFps: number("inFps"), outWidth: whole("outWidth"),
            outHeight: whole("outHeight"), outFps: number("outFps"), limit: message["limit"]?.stringValue ?? "")
        let mic = message["mic"]?.arrayValue ?? []
        let peer = message["peer"]?.arrayValue ?? []
        levels = mic.enumerated().map { (index: Int, value: JSONValue) in
            let other = index < peer.count ? peer[index].doubleValue : nil
            return CallVoiceLevel(
                mic: min(1, max(0, value.doubleValue ?? 0)), peer: other.map { min(1, max(0, $0)) })
        }
        sample.micLevel = levels.last?.mic
        sample.peerLevel = levels.last?.peer
    }
}

public struct CallQualityHistory: Equatable, Sendable {
    public static let backlog = 20

    public private(set) var levels: [CallVoiceLevel] = []
    public private(set) var samples: [CallQualitySample] = []
    public private(set) var pending: [CallVoiceLevel] = []
    public var live = false

    public init(live: Bool = false) {
        self.live = live
    }

    public var latest: CallQualitySample? { samples.last }
    public var bars: Int { live ? CallQuality.bars(latest) : 0 }
    public var grade: CallQualityGrade { live ? CallQuality.grade(latest) : .unknown }
    public var hasPeer: Bool { levels.contains { $0.peer != nil } }
    public var heading: String { live ? grade.label : "Microphone" }
    public var pillText: String { CallQuality.pillText(latest, live: live) }
    public var accessibilityLabel: String { CallQuality.accessibilityLabel(latest, live: live) }

    public mutating func push(level: CallVoiceLevel) {
        levels.append(level)
        if levels.count > CallQuality.levelSamples { levels.removeFirst(levels.count - CallQuality.levelSamples) }
    }

    public mutating func push(sample: CallQualitySample) {
        live = true
        samples.append(sample)
        if samples.count > CallQuality.statsSamples { samples.removeFirst(samples.count - CallQuality.statsSamples) }
    }

    public mutating func apply(_ event: CallQualityEvent) {
        if event.live {
            push(sample: event.sample)
        } else if live {
            live = false
            samples.removeAll()
        }
        pending.append(contentsOf: event.levels)
        if pending.count > Self.backlog { pending.removeFirst(pending.count - Self.backlog / 2) }
    }

    @discardableResult
    public mutating func advance() -> Bool {
        guard !pending.isEmpty else { return false }
        push(level: pending.removeFirst())
        return true
    }

    public mutating func reset() {
        self = CallQualityHistory()
    }
}
