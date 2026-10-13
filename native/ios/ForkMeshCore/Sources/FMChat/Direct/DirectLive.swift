import FMBase
import Foundation
import Observation

public struct DirectLivePoint: Equatable, Sendable {
    public let x: Double
    public let y: Double

    public init(x: Double, y: Double) {
        self.x = min(max(x, 0), 1)
        self.y = min(max(y, -DirectLiveFrames.maxY), DirectLiveFrames.maxY)
    }

    func distance(to other: DirectLivePoint) -> Double { hypot(x - other.x, y - other.y) }
}

public struct DirectLiveTap: Equatable, Sendable, Identifiable {
    public let id: Int
    public let point: DirectLivePoint
    public let at: Int64
    public let isSelf: Bool
}

public struct DirectLiveStroke: Equatable, Sendable, Identifiable {
    public let id: String
    public let color: UInt32
    public let width: Double
    public var points: [DirectLivePoint]
    public let isSelf: Bool
    public var ended: Bool
    public var touchedAt: Int64

    public func opacity(at now: Int64) -> Double {
        let age = now - touchedAt
        let fadeStart = DirectLiveFrames.inkLifeMs - DirectLiveFrames.inkFadeMs
        if age <= fadeStart { return 1 }
        return max(0, 1 - Double(age - fadeStart) / Double(DirectLiveFrames.inkFadeMs))
    }
}

public struct DirectLiveMotionSample: Equatable, Sendable {
    public let at: Int64
    public let gx: Double
    public let gy: Double
    public let gz: Double
    public let ax: Double
    public let ay: Double
    public let az: Double

    public init(at: Int64, gx: Double, gy: Double, gz: Double, ax: Double = 0, ay: Double = 0, az: Double = 0) {
        self.at = at
        self.gx = DirectLiveMotionSample.bounded(gx)
        self.gy = DirectLiveMotionSample.bounded(gy)
        self.gz = DirectLiveMotionSample.bounded(gz)
        self.ax = DirectLiveMotionSample.bounded(ax)
        self.ay = DirectLiveMotionSample.bounded(ay)
        self.az = DirectLiveMotionSample.bounded(az)
    }

    public var roll: Double { atan2(gx, -gy) }

    public var pitch: Double { atan2(-gz, hypot(gx, gy)) }

    public var steadyRoll: Double { roll * min(1, hypot(gx, gy) / 0.35) }

    public var jolt: Double { sqrt(ax * ax + ay * ay + az * az) }

    func shifted(to at: Int64) -> DirectLiveMotionSample {
        DirectLiveMotionSample(at: at, gx: gx, gy: gy, gz: gz, ax: ax, ay: ay, az: az)
    }

    func blended(_ other: DirectLiveMotionSample, _ t: Double, at: Int64) -> DirectLiveMotionSample {
        func mix(_ a: Double, _ b: Double) -> Double { a + (b - a) * t }
        return DirectLiveMotionSample(
            at: at, gx: mix(gx, other.gx), gy: mix(gy, other.gy), gz: mix(gz, other.gz), ax: mix(ax, other.ax),
            ay: mix(ay, other.ay), az: mix(az, other.az))
    }

    static func bounded(_ value: Double) -> Double {
        value.isFinite ? min(max(value, -DirectLiveFrames.maxMotion), DirectLiveFrames.maxMotion) : 0
    }
}

public struct DirectLiveMotionTrack: Equatable, Sendable {
    public var samples: [DirectLiveMotionSample] = []
    public var lastHeard: Int64 = 0
    public var stopped = false

    public func isSharing(at now: Int64) -> Bool {
        !stopped && lastHeard > 0 && now - lastHeard < DirectLiveFrames.motionQuietMs
    }

    public func sample(at now: Int64) -> DirectLiveMotionSample? {
        guard let first = samples.first else { return nil }
        if now <= first.at { return first.shifted(to: now) }
        for index in samples.indices.dropFirst() where samples[index].at >= now {
            let before = samples[index - 1]
            let after = samples[index]
            let span = Double(after.at - before.at)
            let t = span > 0 ? Double(now - before.at) / span : 1
            return before.blended(after, t, at: now)
        }
        return samples[samples.count - 1].shifted(to: now)
    }
}

public enum DirectLiveFrames {
    public static let type = "live"
    public static let budgetKind = "forkmesh-live"
    public static let maxEnvelopeBytes = 16 * 1024
    public static let minIntervalMs: Int64 = 80
    public static let fallbackIntervalMs: Int64 = 2_500
    public static let maxY = 2.5
    public static let maxMotion = 8.0
    public static let maxTapsPerFrame = 12
    public static let maxMotionPerFrame = 40
    public static let maxInkPointsPerFrame = 160
    public static let maxStrokesPerFrame = 8
    public static let maxPointsPerStroke = 1_500
    public static let maxStrokes = 80
    public static let minPointGap = 0.004
    public static let inkLifeMs: Int64 = 45_000
    public static let inkFadeMs: Int64 = 5_000
    public static let tapLifeMs: Int64 = 1_400
    public static let motionQuietMs: Int64 = 4_000
    public static let motionPlaybackMarginMs: Int64 = 120
    public static let motionKeepMs: Int64 = 1_500
    public static let maxStrokeWidth = 24.0
    public static let colors: [UInt32] = [0xFFFFFF, 0xFF453A, 0xFF9F0A, 0xFFD60A, 0x32D74B, 0x64D2FF, 0xBF5AF2, 0x000000]

    public static func isLiveFrame(_ plain: JSONObject) -> Bool { plain["type"]?.text == type }

    public static func interval(perWindow: Int, windowMs: Int) -> Int64 {
        guard perWindow > 0, windowMs > 0 else { return fallbackIntervalMs }
        return max(minIntervalMs, Int64(windowMs) * 4 / Int64(perWindow * 3))
    }

    public static func budget(_ envelope: JSONObject) -> (perWindow: Int, windowMs: Int)? {
        guard envelope["kind"]?.text == budgetKind, let per = envelope["perWindow"]?.intValue,
            let window = envelope["windowMs"]?.intValue, per > 0, window > 0
        else { return nil }
        return (per, window)
    }

    static func scaled(_ value: Double, _ factor: Double) -> JSONValue { .int(Int64((value * factor).rounded())) }

    static func number(_ value: JSONValue?, _ factor: Double) -> Double? {
        guard let raw = value?.doubleValue, raw.isFinite else { return nil }
        return raw / factor
    }

    static func encodePoints(_ points: ArraySlice<DirectLivePoint>) -> JSONValue {
        var out: [JSONValue] = []
        out.reserveCapacity(points.count * 2)
        for point in points {
            out.append(scaled(point.x, 1000))
            out.append(scaled(point.y, 1000))
        }
        return .array(out)
    }

    static func decodePoints(_ value: JSONValue?, limit: Int) -> [DirectLivePoint] {
        let flat = value?.arrayValue ?? []
        var out: [DirectLivePoint] = []
        var index = 0
        while index + 1 < flat.count && out.count < limit {
            if let x = number(flat[index], 1000), let y = number(flat[index + 1], 1000) {
                out.append(DirectLivePoint(x: x, y: y))
            }
            index += 2
        }
        return out
    }

    static func encodeMotion(_ sample: DirectLiveMotionSample, base: Int64) -> JSONValue {
        .array([
            .int(sample.at - base), scaled(sample.gx, 100), scaled(sample.gy, 100), scaled(sample.gz, 100),
            scaled(sample.ax, 100), scaled(sample.ay, 100), scaled(sample.az, 100),
        ])
    }

    static func decodeMotion(_ value: JSONValue, base: Int64) -> DirectLiveMotionSample? {
        guard let parts = value.arrayValue, parts.count >= 4, let dt = parts[0].int64Value, abs(dt) <= 60_000,
            let gx = number(parts[1], 100), let gy = number(parts[2], 100), let gz = number(parts[3], 100)
        else { return nil }
        let rest = parts.dropFirst(4).map { number($0, 100) ?? 0 } + [0, 0, 0]
        return DirectLiveMotionSample(at: base + dt, gx: gx, gy: gy, gz: gz, ax: rest[0], ay: rest[1], az: rest[2])
    }

    static func color(_ value: JSONValue?) -> UInt32 {
        guard let raw = value?.int64Value, raw >= 0, raw <= 0xFFFFFF else { return colors[0] }
        return UInt32(raw)
    }

    static func strokeId(_ value: JSONValue?) -> String {
        let raw = value?.text ?? ""
        guard !raw.isEmpty, raw.count <= 24, raw.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "-") })
        else { return "" }
        return raw
    }
}

struct DirectLiveOutgoing {
    var taps: [DirectLivePoint] = []
    var motion: [DirectLiveMotionSample] = []
    var motionOff = false
    var clear = false

    var isEmpty: Bool { taps.isEmpty && motion.isEmpty && !motionOff && !clear }
}

@MainActor
@Observable
public final class DirectLiveEngine {
    @ObservationIgnored let selfId: @MainActor () -> String
    @ObservationIgnored let send: @MainActor (String, JSONObject) async -> Bool
    @ObservationIgnored let interval: @MainActor (String) -> Int64
    @ObservationIgnored let clock: Clock
    @ObservationIgnored let sleeper: @Sendable (TimeInterval) async throws -> Void
    @ObservationIgnored public var onTap: (@MainActor (String, DirectLiveTap) -> Void)?
    @ObservationIgnored public var onStroke: (@MainActor (String, DirectLiveStroke) -> Void)?
    @ObservationIgnored private var pending: [String: DirectLiveOutgoing] = [:]
    @ObservationIgnored private var inkSent: [String: [String: Int]] = [:]
    @ObservationIgnored private var inkEndSent: [String: Set<String>] = [:]
    @ObservationIgnored private var flushing: [String: Task<Void, Never>] = [:]
    @ObservationIgnored private var lastSent: [String: Int64] = [:]
    @ObservationIgnored private var pruning: Task<Void, Never>?
    @ObservationIgnored private var tapSequence = 0
    @ObservationIgnored private var strokeSequence = 0

    public private(set) var taps: [String: [DirectLiveTap]] = [:]
    public private(set) var strokes: [String: [DirectLiveStroke]] = [:]
    public private(set) var motion: [String: DirectLiveMotionTrack] = [:]
    public private(set) var sharingMotion: Set<String> = []

    public init(
        selfId: @escaping @MainActor () -> String, send: @escaping @MainActor (String, JSONObject) async -> Bool,
        interval: @escaping @MainActor (String) -> Int64, clock: Clock = SystemClock(),
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        }
    ) {
        self.selfId = selfId
        self.send = send
        self.interval = interval
        self.clock = clock
        self.sleeper = sleep
    }

    public var now: Int64 { clock.nowMillis }

    public func strokes(in conversation: String) -> [DirectLiveStroke] { strokes[conversation] ?? [] }

    public func taps(in conversation: String) -> [DirectLiveTap] { taps[conversation] ?? [] }

    public func partnerMotion(_ conversation: String) -> DirectLiveMotionTrack? {
        guard let track = motion[conversation], track.isSharing(at: now) else { return nil }
        return track
    }

    public func isSharingMotion(_ conversation: String) -> Bool { sharingMotion.contains(conversation) }

    public func tap(_ conversation: String, at point: DirectLivePoint) {
        guard !conversation.isEmpty else { return }
        addTap(conversation, point: point, isSelf: true)
        var out = pending[conversation] ?? DirectLiveOutgoing()
        if out.taps.count < DirectLiveFrames.maxTapsPerFrame { out.taps.append(point) }
        pending[conversation] = out
        schedule(conversation)
    }

    @discardableResult
    public func beginStroke(_ conversation: String, at point: DirectLivePoint, color: UInt32, width: Double) -> String {
        guard !conversation.isEmpty else { return "" }
        strokeSequence += 1
        let id = "\(now % 1_000_000_000)-\(strokeSequence)"
        let stroke = DirectLiveStroke(
            id: id, color: color & 0xFFFFFF, width: min(max(width, 1), DirectLiveFrames.maxStrokeWidth), points: [point],
            isSelf: true, ended: false, touchedAt: now)
        insert(stroke, conversation)
        schedule(conversation)
        return id
    }

    public func extendStroke(_ conversation: String, id: String, to point: DirectLivePoint) {
        guard var list = strokes[conversation], let index = list.firstIndex(where: { $0.id == id && $0.isSelf }),
            !list[index].ended, list[index].points.count < DirectLiveFrames.maxPointsPerStroke
        else { return }
        if let last = list[index].points.last, last.distance(to: point) < DirectLiveFrames.minPointGap { return }
        list[index].points.append(point)
        list[index].touchedAt = now
        strokes[conversation] = list
        schedule(conversation)
    }

    public func endStroke(_ conversation: String, id: String) {
        guard var list = strokes[conversation], let index = list.firstIndex(where: { $0.id == id && $0.isSelf }) else { return }
        list[index].ended = true
        list[index].touchedAt = now
        strokes[conversation] = list
        schedule(conversation)
    }

    public func clearInk(_ conversation: String) {
        strokes.removeValue(forKey: conversation)
        inkSent.removeValue(forKey: conversation)
        inkEndSent.removeValue(forKey: conversation)
        var out = pending[conversation] ?? DirectLiveOutgoing()
        out.clear = true
        pending[conversation] = out
        schedule(conversation)
    }

    public func setSharingMotion(_ conversation: String, _ on: Bool) {
        guard !conversation.isEmpty, on != sharingMotion.contains(conversation) else { return }
        var out = pending[conversation] ?? DirectLiveOutgoing()
        if on {
            sharingMotion.insert(conversation)
            out.motionOff = false
        } else {
            sharingMotion.remove(conversation)
            out.motion = []
            out.motionOff = true
        }
        pending[conversation] = out
        if !on { schedule(conversation) }
    }

    public func stopSharingMotion() {
        for conversation in sharingMotion { setSharingMotion(conversation, false) }
    }

    public func addMotion(_ sample: DirectLiveMotionSample) {
        for conversation in sharingMotion {
            var out = pending[conversation] ?? DirectLiveOutgoing()
            out.motion.append(sample)
            if out.motion.count > DirectLiveFrames.maxMotionPerFrame {
                out.motion.removeFirst(out.motion.count - DirectLiveFrames.maxMotionPerFrame)
            }
            pending[conversation] = out
            schedule(conversation)
        }
    }

    public func receive(_ plain: JSONObject, conversation: String) {
        guard DirectLiveFrames.isLiveFrame(plain), plain["channel"]?.text == conversation else { return }
        let sender = String((plain["senderId"]?.text ?? "").prefix(64))
        let me = selfId()
        guard !sender.isEmpty, sender != me else { return }
        let received = now
        if plain["clear"]?.isTrue == true {
            strokes.removeValue(forKey: conversation)
            inkSent.removeValue(forKey: conversation)
            inkEndSent.removeValue(forKey: conversation)
        }
        for value in (plain["taps"]?.arrayValue ?? []).prefix(DirectLiveFrames.maxTapsPerFrame) {
            let points = DirectLiveFrames.decodePoints(value, limit: 1)
            guard let point = points.first else { continue }
            let tap = addTap(conversation, point: point, isSelf: false)
            onTap?(conversation, tap)
        }
        receiveInk(plain["ink"]?.objects ?? [], sender: sender, conversation: conversation, received: received)
        receiveMotion(plain, conversation: conversation, received: received)
        schedulePrune()
    }

    private func receiveInk(_ chunks: [JSONObject], sender: String, conversation: String, received: Int64) {
        var budget = DirectLiveFrames.maxInkPointsPerFrame
        for chunk in chunks.prefix(DirectLiveFrames.maxStrokesPerFrame) {
            let strokeId = DirectLiveFrames.strokeId(chunk["s"])
            if strokeId.isEmpty { continue }
            let points = DirectLiveFrames.decodePoints(chunk["p"], limit: budget)
            budget -= points.count
            let id = "\(sender)|\(strokeId)"
            var list = strokes[conversation] ?? []
            if let index = list.firstIndex(where: { $0.id == id }) {
                let room = DirectLiveFrames.maxPointsPerStroke - list[index].points.count
                list[index].points.append(contentsOf: points.prefix(max(0, room)))
                list[index].touchedAt = received
                if chunk["e"]?.isTrue == true { list[index].ended = true }
                strokes[conversation] = list
            } else if !points.isEmpty {
                let width = min(max(DirectLiveFrames.number(chunk["w"], 10) ?? 4, 1), DirectLiveFrames.maxStrokeWidth)
                let stroke = DirectLiveStroke(
                    id: id, color: DirectLiveFrames.color(chunk["c"]), width: width, points: points, isSelf: false,
                    ended: chunk["e"]?.isTrue == true, touchedAt: received)
                insert(stroke, conversation)
                onStroke?(conversation, stroke)
            }
        }
    }

    private func receiveMotion(_ plain: JSONObject, conversation: String, received: Int64) {
        var track = motion[conversation] ?? DirectLiveMotionTrack()
        if plain["motionOff"]?.isTrue == true {
            track.stopped = true
            track.samples = []
            motion[conversation] = track
            return
        }
        let base = plain["ts"]?.int64Value ?? 0
        let incoming = (plain["motion"]?.arrayValue ?? []).prefix(DirectLiveFrames.maxMotionPerFrame)
            .compactMap { DirectLiveFrames.decodeMotion($0, base: base) }.sorted { $0.at < $1.at }
        guard let first = incoming.first, let last = incoming.last else { return }
        let span = max(0, last.at - first.at)
        let shift = received + DirectLiveFrames.motionPlaybackMarginMs - first.at
        var samples = track.samples.filter { $0.at < first.at + shift }
        samples.append(contentsOf: incoming.map { $0.shifted(to: $0.at + shift) })
        let keepFrom = received - DirectLiveFrames.motionKeepMs
        if let keep = samples.lastIndex(where: { $0.at < keepFrom }), keep > 0 { samples.removeFirst(keep) }
        track.samples = samples
        track.lastHeard = received + span
        track.stopped = false
        motion[conversation] = track
    }

    @discardableResult
    private func addTap(_ conversation: String, point: DirectLivePoint, isSelf: Bool) -> DirectLiveTap {
        tapSequence += 1
        let tap = DirectLiveTap(id: tapSequence, point: point, at: now, isSelf: isSelf)
        var list = (taps[conversation] ?? []).filter { now - $0.at < DirectLiveFrames.tapLifeMs }
        list.append(tap)
        if list.count > DirectLiveFrames.maxTapsPerFrame * 2 { list.removeFirst(list.count - DirectLiveFrames.maxTapsPerFrame * 2) }
        taps[conversation] = list
        schedulePrune()
        return tap
    }

    private func insert(_ stroke: DirectLiveStroke, _ conversation: String) {
        var list = strokes[conversation] ?? []
        list.append(stroke)
        if list.count > DirectLiveFrames.maxStrokes { list.removeFirst(list.count - DirectLiveFrames.maxStrokes) }
        strokes[conversation] = list
        schedulePrune()
    }

    func body(_ conversation: String) -> JSONObject? {
        let out = pending.removeValue(forKey: conversation) ?? DirectLiveOutgoing()
        var fields: JSONObject = [:]
        let stamp = now
        if out.clear { fields["clear"] = true }
        if !out.taps.isEmpty {
            fields["taps"] = .array(out.taps.map { DirectLiveFrames.encodePoints([$0][...]) })
        }
        if out.motionOff {
            fields["motionOff"] = true
        } else if !out.motion.isEmpty {
            fields["motion"] = .array(out.motion.map { DirectLiveFrames.encodeMotion($0, base: stamp) })
        }
        let ink = inkChunks(conversation)
        if !ink.isEmpty { fields["ink"] = .array(ink.map { .object($0) }) }
        if fields.isEmpty { return nil }
        fields["ts"] = .int(stamp)
        return fields
    }

    private func inkChunks(_ conversation: String) -> [JSONObject] {
        var sent = inkSent[conversation] ?? [:]
        var ended = inkEndSent[conversation] ?? []
        var budget = DirectLiveFrames.maxInkPointsPerFrame
        var chunks: [JSONObject] = []
        for stroke in strokes[conversation] ?? [] where stroke.isSelf {
            if chunks.count >= DirectLiveFrames.maxStrokesPerFrame || budget <= 0 { break }
            let from = sent[stroke.id] ?? 0
            let unsent = stroke.points.count - from
            let finishing = stroke.ended && !ended.contains(stroke.id)
            if unsent <= 0 && !finishing { continue }
            let take = min(max(0, unsent), budget)
            var chunk: JSONObject = [
                "s": .string(stroke.id), "p": DirectLiveFrames.encodePoints(stroke.points[from..<(from + take)]),
            ]
            if from == 0 {
                chunk["c"] = .int(Int64(stroke.color))
                chunk["w"] = DirectLiveFrames.scaled(stroke.width, 10)
            }
            if stroke.ended && take == unsent {
                chunk["e"] = true
                ended.insert(stroke.id)
            }
            sent[stroke.id] = from + take
            budget -= take
            chunks.append(chunk)
        }
        inkSent[conversation] = sent
        inkEndSent[conversation] = ended
        return chunks
    }

    private func hasWork(_ conversation: String) -> Bool {
        if let out = pending[conversation], !out.isEmpty { return true }
        let sent = inkSent[conversation] ?? [:]
        let ended = inkEndSent[conversation] ?? []
        return (strokes[conversation] ?? []).contains {
            $0.isSelf && ($0.points.count > (sent[$0.id] ?? 0) || ($0.ended && !ended.contains($0.id)))
        }
    }

    private func schedule(_ conversation: String) {
        guard flushing[conversation] == nil else { return }
        let wait = max(0, (lastSent[conversation] ?? 0) + interval(conversation) - now)
        let sleeper = self.sleeper
        flushing[conversation] = Task { [weak self] in
            if wait > 0 { try? await sleeper(Double(wait) / 1000) }
            await self?.flush(conversation)
        }
    }

    private func flush(_ conversation: String) async {
        if let fields = body(conversation) {
            lastSent[conversation] = now
            _ = await send(conversation, fields)
        }
        flushing.removeValue(forKey: conversation)
        if hasWork(conversation) { schedule(conversation) }
    }

    private func schedulePrune() {
        guard pruning == nil else { return }
        let sleeper = self.sleeper
        pruning = Task { [weak self] in
            while true {
                try? await sleeper(1)
                guard let self, !Task.isCancelled else { return }
                if !self.prune() {
                    self.pruning = nil
                    return
                }
            }
        }
    }

    @discardableResult
    public func prune() -> Bool {
        let at = now
        var left = false
        for (conversation, list) in taps {
            let kept = list.filter { at - $0.at < DirectLiveFrames.tapLifeMs }
            if kept.count != list.count { taps[conversation] = kept.isEmpty ? nil : kept }
            left = left || !kept.isEmpty
        }
        for (conversation, list) in strokes {
            let kept = list.filter { at - $0.touchedAt < DirectLiveFrames.inkLifeMs || ($0.isSelf && !$0.ended) }
            if kept.count != list.count {
                strokes[conversation] = kept.isEmpty ? nil : kept
                let ids = Set(kept.map(\.id))
                inkSent[conversation] = inkSent[conversation]?.filter { ids.contains($0.key) }
                inkEndSent[conversation] = inkEndSent[conversation]?.filter { ids.contains($0) }
            }
            left = left || !kept.isEmpty
        }
        for (conversation, track) in motion where !track.isSharing(at: at) {
            motion.removeValue(forKey: conversation)
        }
        return left || !motion.isEmpty
    }

    public func clear() {
        for task in flushing.values { task.cancel() }
        flushing = [:]
        pruning?.cancel()
        pruning = nil
        pending = [:]
        inkSent = [:]
        inkEndSent = [:]
        lastSent = [:]
        taps = [:]
        strokes = [:]
        motion = [:]
        sharingMotion = []
    }
}
