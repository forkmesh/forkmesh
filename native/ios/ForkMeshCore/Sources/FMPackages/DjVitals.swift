import Foundation

public enum DjWork: String, CaseIterable, Sendable {
    case tick = "player tick"
    case event = "player event"
    case wave = "waveform draw"
    case spin = "record step"
    case fade = "crossfade step"
    case automix = "automix step"
    case scratch = "scratch move"
    case jam = "jam reply"
    case save = "state save"
}

public struct DjVitalsAlert: Equatable, Sendable {
    public static let none = DjVitalsAlert(reason: "")

    public var reason: String

    public init(reason: String) {
        self.reason = reason
    }

    public var detected: Bool { !reason.isEmpty }
}

public struct DjVitalsLive: Equatable, Sendable {
    public var fps: Double
    public var hitchRatio: Double
    public var worstMs: Double
    public var dropped: Int
    public var probeHz: Double

    public init(fps: Double = 0, hitchRatio: Double = 0, worstMs: Double = 0, dropped: Int = 0, probeHz: Double = 0) {
        self.fps = fps
        self.hitchRatio = hitchRatio
        self.worstMs = worstMs
        self.dropped = dropped
        self.probeHz = probeHz
    }

    public var smooth: Bool { hitchRatio < DjVitals.hitchRatioWarn && worstMs < DjVitals.longFrameMs }
}

public struct DjVitals: Sendable {
    public static let playbook = "docs/marketplace/video-mix-dj-studio-performance.md"
    public static let repository = "forkmesh/forkmesh"
    public static let secondsKept = 120
    public static let liveSeconds = 5
    public static let alertSeconds = 30
    public static let dropFactor = 1.5
    public static let hitchRatioWarn = 5.0
    public static let hitchRatioBad = 10.0
    public static let longFrameMs = 50.0
    public static let hangMs = 250.0
    public static let gapSeconds = 1.0
    public static let workBudgetMs = 4.0
    public static let tickGap = 0.6
    public static let snapSeconds = 0.3
    public static let marksKept = 40
    public static let longFramesKept = 12
    public static let workKept = 96
    public static let workShownMs = 1.0

    public struct Second: Equatable, Sendable {
        public var at: Int
        public var frames = 0
        public var dropped = 0
        public var hitchMs: Double = 0
        public var worstMs: Double = 0
        public var span: Double = 0
    }

    public struct Mark: Equatable, Sendable {
        public var at: Double
        public var text: String
    }

    public struct Work: Equatable, Sendable {
        public var at: Double
        public var kind: DjWork
        public var ms: Double
    }

    public struct LongFrame: Equatable, Sendable {
        public var at: Double
        public var ms: Double
        public var work: [Work]
        public var marks: [Mark]
    }

    public struct Meter: Equatable, Sendable {
        public var count = 0
        public var totalMs: Double = 0
        public var maxMs: Double = 0
        public var over = 0

        mutating func add(_ ms: Double, budget: Double) {
            count += 1
            totalMs += ms
            maxMs = max(maxMs, ms)
            if ms > budget { over += 1 }
        }

        public var averageMs: Double { count > 0 ? totalMs / Double(count) : 0 }
    }

    public struct Ticks: Equatable, Sendable {
        public var count = 0
        public var lastAt: Double?
        public var maxGap: Double = 0
        public var late = 0
        public var lastLateAt: Double?
        public var lastLateGap: Double = 0
        public var maxDrift: Double = 0
        public var snaps = 0
    }

    public private(set) var seconds: [Second] = []
    public private(set) var frames = 0
    public private(set) var dropped = 0
    public private(set) var hitchMs: Double = 0
    public private(set) var worstMs: Double = 0
    public private(set) var measured: Double = 0
    public private(set) var period: Double = 0
    public private(set) var work: [DjWork: Meter] = [:]
    public private(set) var recentWork: [Work] = []
    public private(set) var marks: [Mark] = []
    public private(set) var longFrames: [LongFrame] = []
    public private(set) var ticks: [DjDeckSide: Ticks] = [:]
    public private(set) var scripts = 0
    public private(set) var scriptTrips = Meter()
    public private(set) var opened: Double?
    private var lastFrame: Double?

    public init() {}

    public mutating func begin(at now: Double) {
        if opened == nil { opened = now }
        lastFrame = nil
        for side in DjDeckSide.allCases { ticks[side]?.lastAt = nil }
        mark("studio on screen", at: now)
    }

    public mutating func end(at now: Double) {
        lastFrame = nil
        mark("studio closed", at: now)
    }

    public mutating func frame(at now: Double, period: Double) {
        defer { lastFrame = now }
        guard period > 0, let last = lastFrame else { return }
        let interval = now - last
        guard interval > 0, interval < Self.gapSeconds else { return }
        self.period = period
        let ms = interval * 1000
        let lost = max(0, Int((interval / period).rounded()) - 1)
        let late = interval > period * Self.dropFactor ? (interval - period) * 1000 : 0
        frames += 1
        dropped += lost
        hitchMs += late
        worstMs = max(worstMs, ms)
        measured += interval
        let key = Int(now.rounded(.down))
        if seconds.last?.at != key {
            seconds.append(Second(at: key))
            if seconds.count > Self.secondsKept { seconds.removeFirst(seconds.count - Self.secondsKept) }
        }
        seconds[seconds.count - 1].frames += 1
        seconds[seconds.count - 1].dropped += lost
        seconds[seconds.count - 1].hitchMs += late
        seconds[seconds.count - 1].worstMs = max(seconds[seconds.count - 1].worstMs, ms)
        seconds[seconds.count - 1].span += interval
        guard ms >= Self.longFrameMs else { return }
        let inside = recentWork.filter { $0.at > last - 0.001 && $0.at <= now }
        let before = marks.filter { $0.at >= last - 0.5 && $0.at <= now }
        longFrames.append(LongFrame(at: now, ms: ms, work: inside, marks: before))
        if longFrames.count > Self.longFramesKept { longFrames.removeFirst(longFrames.count - Self.longFramesKept) }
    }

    public mutating func work(_ kind: DjWork, ms: Double, at now: Double) {
        guard ms.isFinite, ms >= 0 else { return }
        work[kind, default: Meter()].add(ms, budget: Self.workBudgetMs)
        guard ms >= Self.workShownMs else { return }
        recentWork.append(Work(at: now, kind: kind, ms: ms))
        if recentWork.count > Self.workKept { recentWork.removeFirst(recentWork.count - Self.workKept) }
    }

    public mutating func mark(_ text: String, at now: Double) {
        marks.append(Mark(at: now, text: text))
        if marks.count > Self.marksKept { marks.removeFirst(marks.count - Self.marksKept) }
    }

    public mutating func tick(_ side: DjDeckSide, at now: Double, playing: Bool, drift: Double?) {
        var state = ticks[side] ?? Ticks()
        state.count += 1
        if playing, let last = state.lastAt {
            let gap = now - last
            state.maxGap = max(state.maxGap, gap)
            if gap > Self.tickGap {
                state.late += 1
                state.lastLateAt = now
                state.lastLateGap = gap
            }
        }
        state.lastAt = playing ? now : nil
        if let drift, drift.isFinite {
            state.maxDrift = max(state.maxDrift, abs(drift))
            if abs(drift) > Self.snapSeconds { state.snaps += 1 }
        }
        ticks[side] = state
    }

    public mutating func scriptSent() {
        scripts += 1
    }

    public mutating func scriptReturned(ms: Double) {
        guard ms.isFinite, ms >= 0 else { return }
        scriptTrips.add(ms, budget: Self.workBudgetMs * 4)
    }

    public func window(_ count: Int, now: Double) -> (frames: Int, dropped: Int, hitchMs: Double, worstMs: Double, span: Double) {
        let from = Int(now.rounded(.down)) - count + 1
        var out: (frames: Int, dropped: Int, hitchMs: Double, worstMs: Double, span: Double) = (0, 0, 0, 0, 0)
        for second in seconds where second.at >= from {
            out.frames += second.frames
            out.dropped += second.dropped
            out.hitchMs += second.hitchMs
            out.worstMs = max(out.worstMs, second.worstMs)
            out.span += second.span
        }
        return out
    }

    public func live(now: Double) -> DjVitalsLive {
        let recent = window(Self.liveSeconds, now: now)
        guard recent.span > 0 else { return DjVitalsLive(probeHz: period > 0 ? 1 / period : 0) }
        return DjVitalsLive(
            fps: Double(recent.frames) / recent.span, hitchRatio: recent.hitchMs / recent.span,
            worstMs: recent.worstMs, dropped: recent.dropped, probeHz: period > 0 ? 1 / period : 0)
    }

    public func alert(now: Double) -> DjVitalsAlert {
        let since = now - Double(Self.alertSeconds)
        if let hang = longFrames.last(where: { $0.at >= since && $0.ms >= Self.hangMs }) {
            return DjVitalsAlert(reason: "the studio froze for \(Self.ms(hang.ms)) ms")
        }
        let recent = window(Self.alertSeconds, now: now)
        if recent.span > 1, recent.hitchMs / recent.span >= Self.hitchRatioBad {
            return DjVitalsAlert(
                reason: "the studio dropped \(recent.dropped) frames in \(Self.alertSeconds) s (\(Self.one(recent.hitchMs / recent.span)) ms of hitches a second)")
        }
        let long = longFrames.filter { $0.at >= since }
        if long.count >= 3 {
            return DjVitalsAlert(reason: "\(long.count) frames over \(Int(Self.longFrameMs)) ms in \(Self.alertSeconds) s, the worst \(Self.ms(long.map(\.ms).max() ?? 0)) ms")
        }
        for side in DjDeckSide.allCases {
            guard let state = ticks[side], let at = state.lastLateAt, at >= since else { continue }
            return DjVitalsAlert(reason: "Deck \(side.label)'s player went \(Self.one(state.lastLateGap)) s without reporting its position")
        }
        return .none
    }

    public func report(now: Double, alert: DjVitalsAlert, context: [(String, String)]) -> String {
        var lines: [String] = ["# DJ studio vitals"]
        let open = opened.map { now - $0 } ?? 0
        lines.append("measured \(Self.duration(measured)) of frames over \(Self.duration(open)) since the studio first opened, probe \(Self.hz(period))")
        if alert.detected { lines.append("alert: \(alert.reason)") }
        lines.append("")
        lines.append("## Frames (last 10 s | last 60 s | all)")
        let ten = window(10, now: now)
        let sixty = window(60, now: now)
        let all = (frames: frames, dropped: dropped, hitchMs: hitchMs, worstMs: worstMs, span: measured)
        let columns = [ten, sixty, all]
        lines.append("fps " + columns.map { $0.span > 0 ? Self.one(Double($0.frames) / $0.span) : "-" }.joined(separator: " | "))
        lines.append("dropped " + columns.map { String($0.dropped) }.joined(separator: " | "))
        lines.append("hitch ms/s " + columns.map { $0.span > 0 ? Self.one($0.hitchMs / $0.span) : "-" }.joined(separator: " | "))
        lines.append("worst ms " + columns.map { Self.ms($0.worstMs) }.joined(separator: " | "))
        let from = Int(now.rounded(.down)) - 29
        let series = (from...Int(now.rounded(.down))).map { key in seconds.first { $0.at == key }.map { Self.ms($0.worstMs) } ?? "." }
        lines.append("worst frame each second, last 30 s, oldest first: " + series.joined(separator: " "))
        lines.append("")
        lines.append("## Long frames (\(Int(Self.longFrameMs)) ms or more), newest first")
        if longFrames.isEmpty { lines.append("none") }
        for long in longFrames.reversed() {
            var line = "\(Self.ago(long.at, now)) \(Self.ms(long.ms)) ms"
            if !long.work.isEmpty {
                line += "; inside: " + long.work.map { "\($0.kind.rawValue) \(Self.one($0.ms)) ms" }.joined(separator: ", ")
            }
            if !long.marks.isEmpty { line += "; around: " + long.marks.map(\.text).joined(separator: ", ") }
            lines.append(line)
        }
        lines.append("")
        lines.append("## Main-thread work (count, avg ms, max ms, over \(Int(Self.workBudgetMs)) ms)")
        let busy = DjWork.allCases.filter { (work[$0]?.count ?? 0) > 0 }
        if busy.isEmpty { lines.append("none") }
        for kind in busy {
            guard let meter = work[kind] else { continue }
            lines.append("\(kind.rawValue): \(meter.count), \(Self.two(meter.averageMs)), \(Self.one(meter.maxMs)), \(meter.over)")
        }
        lines.append("")
        lines.append("## Player ticks (expected every 0.25 s while playing)")
        for side in DjDeckSide.allCases {
            let state = ticks[side] ?? Ticks()
            lines.append(
                "Deck \(side.label): \(state.count) ticks, longest gap \(Self.two(state.maxGap)) s, late \(state.late), largest drift \(Self.two(state.maxDrift)) s, snaps \(state.snaps)")
        }
        lines.append("")
        lines.append("## Web view scripts")
        lines.append(
            "sent \(scripts) since launch, round trip avg \(Self.one(scriptTrips.averageMs)) ms, max \(Self.one(scriptTrips.maxMs)) ms, over \(Int(Self.workBudgetMs * 4)) ms \(scriptTrips.over) of \(scriptTrips.count) sampled")
        lines.append("")
        lines.append("## Recent moments, newest first")
        if marks.isEmpty { lines.append("none") }
        for mark in marks.suffix(16).reversed() {
            lines.append("\(Self.ago(mark.at, now)) \(mark.text)")
        }
        if !context.isEmpty {
            lines.append("")
            lines.append("## Studio and device")
            for (key, value) in context { lines.append("\(key): \(value)") }
        }
        return lines.joined(separator: "\n")
    }

    public static func lead(reason: String) -> String {
        let why = reason.isEmpty ? "The user asked for a DJ studio performance check" : "The DJ studio detected a performance issue: \(reason)"
        return """
            \(why). Make Video Mix DJ Studio on iOS run in real time: every frame on time, no lag between a touch and the sound, \
            no stutter in the records, the waveforms or the audio. Its code is native/ios/ForkMesh/Features/Packages/VideoMixDjStudio*.swift \
            and native/ios/ForkMeshCore/Sources/FMPackages/Dj*.swift in \(repository). \
            First read \(playbook): the budgets, how to read each section of the bundle below, and what earlier checks fixed and ruled out. \
            Find the cause from the evidence, fix it on a branch with a test in ForkMeshCore that fails without the fix, and explain what you found. \
            Before committing, add this check to that page (evidence, cause, fix, ruled out) and correct what it got wrong; \
            if the bundle lacked evidence you needed, make the studio collect it (DjVitals) in the same change, so the next check starts further ahead.
            """
    }

    static func ms(_ value: Double) -> String { String(Int(value.rounded())) }
    static func one(_ value: Double) -> String { String(format: "%.1f", value) }
    static func two(_ value: Double) -> String { String(format: "%.2f", value) }

    static func hz(_ period: Double) -> String {
        period > 0 ? "\(Int((1 / period).rounded())) Hz" : "not running"
    }

    static func ago(_ at: Double, _ now: Double) -> String {
        "-" + one(max(0, now - at)) + " s"
    }

    static func duration(_ seconds: Double) -> String {
        let whole = Int(max(0, seconds).rounded())
        return whole >= 60 ? "\(whole / 60)m\(whole % 60)s" : "\(whole)s"
    }
}

public final class DjVitalsRecorder: @unchecked Sendable {
    public static let shared = DjVitalsRecorder()

    private let lock = NSLock()
    private var vitals = DjVitals()

    public init() {}

    public var snapshot: DjVitals {
        lock.lock()
        defer { lock.unlock() }
        return vitals
    }

    public func update(_ change: (inout DjVitals) -> Void) {
        lock.lock()
        defer { lock.unlock() }
        change(&vitals)
    }

    public func work(_ kind: DjWork, from start: Double, to end: Double) {
        update { $0.work(kind, ms: (end - start) * 1000, at: end) }
    }

    public func mark(_ text: String, at now: Double) {
        update { $0.mark(text, at: now) }
    }

    public func reset() {
        update { $0 = DjVitals() }
    }
}
