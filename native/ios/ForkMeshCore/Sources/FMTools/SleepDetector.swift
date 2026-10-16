import FMBase
import Foundation

public enum SleepStage: String, Equatable, Sendable {
    case inBed
    case asleep
    case core
    case deep
    case rem
    case awake

    public var sleeping: Bool { self == .asleep || self == .core || self == .deep || self == .rem }
}

public struct SleepInterval: Equatable, Sendable {
    public var start: Date
    public var end: Date

    public init(start: Date, end: Date) {
        self.start = start
        self.end = end
    }

    public var seconds: TimeInterval { max(0, end.timeIntervalSince(start)) }

    public func overlap(_ other: SleepInterval) -> TimeInterval {
        max(0, min(end, other.end).timeIntervalSince(max(start, other.start)))
    }
}

public struct SleepRestSample: Equatable, Sendable {
    public var start: Date
    public var still: Bool
    public var confident: Bool

    public init(start: Date, still: Bool, confident: Bool) {
        self.start = start
        self.still = still
        self.confident = confident
    }
}

public struct HealthSleepSample: Equatable, Sendable {
    public let start: Date
    public let end: Date
    public let stage: SleepStage
    public let ours: Bool
    public let entryId: String
    public let version: String

    public init(start: Date, end: Date, stage: SleepStage, ours: Bool = false, entryId: String = "", version: String = "") {
        self.start = start
        self.end = end
        self.stage = stage
        self.ours = ours
        self.entryId = entryId
        self.version = version
    }
}

public struct HealthSleepWrite: Equatable, Sendable {
    public let entryId: String
    public let version: String
    public let start: Date
    public let end: Date

    public init(entryId: String, version: String, start: Date, end: Date) {
        self.entryId = entryId
        self.version = version
        self.start = start
        self.end = end
    }
}

public struct SleepNight: Equatable, Sendable {
    public var night: String
    public var bedAt: Date
    public var wakeAt: Date
    public var asleepMinutes: Int
    public var awakeMinutes: Int
    public var interruptions: Int
    public var confidence: String
    public var deepMinutes: Int
    public var remMinutes: Int
    public var coreMinutes: Int

    public init(
        night: String, bedAt: Date, wakeAt: Date, asleepMinutes: Int, awakeMinutes: Int = 0, interruptions: Int = 0,
        confidence: String = "", deepMinutes: Int = 0, remMinutes: Int = 0, coreMinutes: Int = 0
    ) {
        self.night = night
        self.bedAt = bedAt
        self.wakeAt = wakeAt
        self.asleepMinutes = asleepMinutes
        self.awakeMinutes = awakeMinutes
        self.interruptions = interruptions
        self.confidence = confidence
        self.deepMinutes = deepMinutes
        self.remMinutes = remMinutes
        self.coreMinutes = coreMinutes
    }

    public var body: JSONObject {
        var body: JSONObject = [
            "night": .string(night),
            "bedAt": .int(HealthDates.epochMs(bedAt)),
            "wakeAt": .int(HealthDates.epochMs(wakeAt)),
            "asleepMinutes": .int(Int64(max(0, asleepMinutes))),
            "awakeMinutes": .int(Int64(max(0, awakeMinutes))),
            "interruptions": .int(Int64(max(0, interruptions))),
        ]
        if !confidence.isEmpty { body["confidence"] = .string(confidence) }
        if deepMinutes > 0 { body["deepMinutes"] = .int(Int64(deepMinutes)) }
        if remMinutes > 0 { body["remMinutes"] = .int(Int64(remMinutes)) }
        if coreMinutes > 0 { body["coreMinutes"] = .int(Int64(coreMinutes)) }
        return body
    }

    public static func cleared(_ night: String) -> JSONObject {
        ["night": .string(night), "asleepMinutes": .int(0)]
    }
}

public struct SleepSchedule: Equatable, Sendable {
    public static let defaultBedtime = 23 * 60
    public static let defaultWake = 7 * 60

    public var bedtime: Int
    public var wake: Int

    public init(bedtime: Int = SleepSchedule.defaultBedtime, wake: Int = SleepSchedule.defaultWake) {
        self.bedtime = ((bedtime % 1440) + 1440) % 1440
        self.wake = ((wake % 1440) + 1440) % 1440
    }

    public init(bedtime: String, wakeTime: String) {
        self.init(
            bedtime: SleepSchedule.minutes(bedtime) ?? SleepSchedule.defaultBedtime,
            wake: SleepSchedule.minutes(wakeTime) ?? SleepSchedule.defaultWake)
    }

    public static func minutes(_ clock: String) -> Int? {
        let parts = clock.split(separator: ":", omittingEmptySubsequences: false)
        guard parts.count == 2, let hour = Int(parts[0]), let minute = Int(parts[1]),
            (0..<24).contains(hour), (0..<60).contains(minute)
        else { return nil }
        return hour * 60 + minute
    }

    public var lengthMinutes: Int {
        let length = ((wake - bedtime) % 1440 + 1440) % 1440
        return length == 0 ? 480 : length
    }

    public var nightBoundary: Int { (wake + SleepDetector.lateMinutes) % 1440 }

    public func nightKey(_ date: Date, timeZone: TimeZone) -> String {
        let shift = TimeInterval((1440 - nightBoundary) % 1440 * 60)
        return CaloriesText.isoDay(date.addingTimeInterval(shift), timeZone: timeZone)
    }

    public func planned(night: String, timeZone: TimeZone) -> SleepInterval? {
        guard let midnight = CaloriesText.tryParseDay(night, timeZone: timeZone) else { return nil }
        let calendar = CaloriesText.calendar(timeZone)
        guard let wakeAt = calendar.date(byAdding: .minute, value: wake, to: midnight) else { return nil }
        return SleepInterval(start: wakeAt.addingTimeInterval(-TimeInterval(lengthMinutes * 60)), end: wakeAt)
    }

    public func window(night: String, timeZone: TimeZone) -> SleepInterval? {
        guard let planned = planned(night: night, timeZone: timeZone) else { return nil }
        return SleepInterval(
            start: planned.start.addingTimeInterval(-TimeInterval(SleepDetector.earlyMinutes * 60)),
            end: planned.end.addingTimeInterval(TimeInterval(SleepDetector.lateMinutes * 60)))
    }
}

public enum SleepDetector {
    public static let earlyMinutes = 4 * 60
    public static let lateMinutes = 5 * 60
    public static let mergeGap: TimeInterval = 20 * 60
    public static let wakeGap: TimeInterval = 75 * 60
    public static let minimumOverlap: TimeInterval = 60 * 60
    public static let offScheduleSleep: TimeInterval = 5 * 3600
    public static let interruptionGap: TimeInterval = 2 * 60
    public static let minimumSleep: TimeInterval = 3 * 3600
    public static let settleMinutes = 10

    public static func stillIntervals(_ samples: [SleepRestSample], within window: SleepInterval) -> [SleepInterval] {
        let sorted = samples.sorted { $0.start < $1.start }
        var result: [SleepInterval] = []
        for (index, sample) in sorted.enumerated() where sample.still && sample.confident {
            let next = index + 1 < sorted.count ? sorted[index + 1].start : window.end
            let start = max(sample.start, window.start)
            let end = min(next, window.end)
            guard end > start else { continue }
            if let last = result.last, last.end >= start {
                result[result.count - 1].end = max(last.end, end)
            } else {
                result.append(SleepInterval(start: start, end: end))
            }
        }
        return result
    }

    public static func subtract(_ intervals: [SleepInterval], _ cuts: [SleepInterval]) -> [SleepInterval] {
        var pieces = intervals
        for cut in cuts.sorted(by: { $0.start < $1.start }) where cut.end > cut.start {
            var next: [SleepInterval] = []
            for piece in pieces {
                if cut.end <= piece.start || cut.start >= piece.end {
                    next.append(piece)
                    continue
                }
                if cut.start > piece.start { next.append(SleepInterval(start: piece.start, end: cut.start)) }
                if cut.end < piece.end { next.append(SleepInterval(start: cut.end, end: piece.end)) }
            }
            pieces = next
        }
        return pieces
    }

    struct Block {
        var interval: SleepInterval
        var restSeconds: TimeInterval
        var gapSeconds: TimeInterval
        var interruptions: Int
    }

    static func joins(_ gap: SleepInterval, planned: SleepInterval) -> Bool {
        if gap.seconds <= mergeGap { return true }
        return gap.seconds <= wakeGap && gap.start >= planned.start && gap.end <= planned.end
    }

    static func blocks(_ intervals: [SleepInterval], planned: SleepInterval) -> [Block] {
        var blocks: [Block] = []
        for interval in intervals.sorted(by: { $0.start < $1.start }) {
            if var last = blocks.last,
                joins(SleepInterval(start: last.interval.end, end: max(last.interval.end, interval.start)), planned: planned) {
                let gap = max(0, interval.start.timeIntervalSince(last.interval.end))
                last.interval.end = max(last.interval.end, interval.end)
                last.restSeconds += interval.seconds
                last.gapSeconds += gap
                if gap >= interruptionGap { last.interruptions += 1 }
                blocks[blocks.count - 1] = last
            } else {
                blocks.append(Block(interval: interval, restSeconds: interval.seconds, gapSeconds: 0, interruptions: 0))
            }
        }
        return blocks
    }

    public static func detect(
        night: String, rest: [SleepRestSample], usage: [SleepInterval], schedule: SleepSchedule,
        timeZone: TimeZone, now: Date
    ) -> SleepNight? {
        guard let planned = schedule.planned(night: night, timeZone: timeZone),
            var window = schedule.window(night: night, timeZone: timeZone)
        else { return nil }
        window.end = min(window.end, now)
        guard window.end > window.start else { return nil }
        let still = subtract(stillIntervals(rest, within: window), usage)
        let candidates = blocks(still, planned: planned).filter { block in
            block.interval.seconds >= minimumSleep
                && (block.interval.overlap(planned) >= minimumOverlap || block.interval.seconds >= offScheduleSleep)
        }
        guard let best = candidates.max(by: { left, right in
            let a = left.interval.overlap(planned), b = right.interval.overlap(planned)
            return a == b ? left.interval.seconds < right.interval.seconds : a < b
        }) else { return nil }
        let spanMinutes = Int(best.interval.seconds / 60)
        let gapMinutes = Int((best.gapSeconds / 60).rounded())
        let settle = min(settleMinutes, max(0, spanMinutes - gapMinutes))
        let asleep = max(0, spanMinutes - gapMinutes - settle)
        let overlapRatio = best.interval.overlap(planned) / max(60, min(best.interval.seconds, planned.seconds))
        let confidence: String
        if overlapRatio >= 0.6 && best.interruptions <= 4 && best.restSeconds / best.interval.seconds >= 0.85 {
            confidence = "high"
        } else if overlapRatio >= 0.3 {
            confidence = "medium"
        } else {
            confidence = "low"
        }
        return SleepNight(
            night: night, bedAt: best.interval.start, wakeAt: best.interval.end, asleepMinutes: asleep,
            awakeMinutes: gapMinutes + settle, interruptions: best.interruptions, confidence: confidence)
    }

    public static func detect(
        nights: [String], rest: [SleepRestSample], usage: [SleepInterval], schedule: SleepSchedule,
        timeZone: TimeZone, now: Date
    ) -> (bodies: [JSONObject], detected: [SleepNight]) {
        let sorted = rest.sorted { $0.start < $1.start }
        var bodies: [JSONObject] = []
        var detected: [SleepNight] = []
        for key in nights {
            if let night = detect(night: key, rest: sorted, usage: usage, schedule: schedule, timeZone: timeZone, now: now) {
                detected.append(night)
                bodies.append(night.body)
            } else {
                bodies.append(SleepNight.cleared(key))
            }
        }
        return (bodies, detected)
    }

    public static func union(_ intervals: [SleepInterval]) -> [SleepInterval] {
        var merged: [SleepInterval] = []
        for interval in intervals.sorted(by: { $0.start < $1.start }) where interval.end > interval.start {
            if let last = merged.last, interval.start <= last.end {
                merged[merged.count - 1].end = max(last.end, interval.end)
            } else {
                merged.append(interval)
            }
        }
        return merged
    }

    static func minutes(_ intervals: [SleepInterval]) -> Int {
        Int((union(intervals).reduce(0) { $0 + $1.seconds } / 60).rounded())
    }

    public static func healthNights(_ samples: [HealthSleepSample], schedule: SleepSchedule, timeZone: TimeZone) -> [SleepNight] {
        var grouped: [String: [HealthSleepSample]] = [:]
        for sample in samples where !sample.ours && sample.end > sample.start {
            grouped[schedule.nightKey(sample.end, timeZone: timeZone), default: []].append(sample)
        }
        var nights: [SleepNight] = []
        for (night, items) in grouped {
            let asleep = items.filter { $0.stage.sleeping }.map { SleepInterval(start: $0.start, end: $0.end) }
            let inBed = items.filter { $0.stage == .inBed }.map { SleepInterval(start: $0.start, end: $0.end) }
            let awake = items.filter { $0.stage == .awake }.map { SleepInterval(start: $0.start, end: $0.end) }
            let main = asleep.isEmpty ? subtract(union(inBed), union(awake)) : asleep
            guard let bedAt = items.map(\.start).min(), let wakeAt = items.map(\.end).max() else { continue }
            let asleepMinutes = minutes(main)
            guard asleepMinutes > 0 else { continue }
            let spanMinutes = Int(wakeAt.timeIntervalSince(bedAt) / 60)
            func stage(_ kind: SleepStage) -> Int {
                minutes(items.filter { $0.stage == kind }.map { SleepInterval(start: $0.start, end: $0.end) })
            }
            nights.append(SleepNight(
                night: night, bedAt: bedAt, wakeAt: wakeAt, asleepMinutes: asleepMinutes,
                awakeMinutes: max(0, spanMinutes - asleepMinutes),
                interruptions: union(awake).filter { $0.seconds >= interruptionGap }.count,
                deepMinutes: stage(.deep), remMinutes: stage(.rem), coreMinutes: stage(.core)))
        }
        return nights.sorted { $0.night < $1.night }
    }

    public static func healthWrites(_ nights: [SleepNight]) -> [HealthSleepWrite] {
        nights.compactMap { night in
            let start = night.bedAt.addingTimeInterval(TimeInterval(night.awakeMinutes * 60))
            guard night.asleepMinutes > 0, night.wakeAt > start else { return nil }
            return HealthSleepWrite(
                entryId: "sleep-" + night.night,
                version: "\(HealthDates.epochMs(night.bedAt))-\(HealthDates.epochMs(night.wakeAt))-\(night.asleepMinutes)",
                start: start, end: night.wakeAt)
        }
    }

    public static func reconcile(desired: [HealthSleepWrite], existing: [HealthSleepSample]) -> (save: [HealthSleepWrite], delete: [String]) {
        var versions: [String: Set<String>] = [:]
        for sample in existing where sample.ours && !sample.entryId.isEmpty {
            versions[sample.entryId, default: []].insert(sample.version)
        }
        var save: [HealthSleepWrite] = []
        var delete: [String] = []
        var wanted = Set<String>()
        for write in desired where wanted.insert(write.entryId).inserted {
            guard let stored = versions[write.entryId] else {
                save.append(write)
                continue
            }
            if stored != [write.version] {
                delete.append(write.entryId)
                save.append(write)
            }
        }
        return (save, delete)
    }
}

public enum SleepUsageLog {
    public static let keep: TimeInterval = 8 * 86400

    public static func begin(_ prefs: KeyValueStore, now: Date) {
        if prefs.double(forKey: HealthPrefs.sleepUsageOpen, or: 0) > 0 { end(prefs, now: now) }
        prefs.set(now.timeIntervalSince1970, forKey: HealthPrefs.sleepUsageOpen)
    }

    public static func end(_ prefs: KeyValueStore, now: Date) {
        let opened = prefs.double(forKey: HealthPrefs.sleepUsageOpen, or: 0)
        prefs.set(0.0, forKey: HealthPrefs.sleepUsageOpen)
        guard opened > 0, now.timeIntervalSince1970 > opened else { return }
        var entries = prefs.stringList(forKey: HealthPrefs.sleepUsage) ?? []
        entries.append("\(Int64(opened))-\(Int64(now.timeIntervalSince1970.rounded(.up)))")
        let cutoff = now.timeIntervalSince1970 - keep
        prefs.set(entries.filter { (parse($0)?.end.timeIntervalSince1970 ?? 0) >= cutoff }, forKey: HealthPrefs.sleepUsage)
    }

    public static func intervals(_ prefs: KeyValueStore, now: Date) -> [SleepInterval] {
        var result = (prefs.stringList(forKey: HealthPrefs.sleepUsage) ?? []).compactMap(parse)
        let opened = prefs.double(forKey: HealthPrefs.sleepUsageOpen, or: 0)
        if opened > 0 && opened < now.timeIntervalSince1970 {
            result.append(SleepInterval(start: Date(timeIntervalSince1970: opened), end: now))
        }
        return result
    }

    static func parse(_ entry: String) -> SleepInterval? {
        let parts = entry.split(separator: "-")
        guard parts.count == 2, let start = Double(parts[0]), let end = Double(parts[1]), end > start else { return nil }
        return SleepInterval(start: Date(timeIntervalSince1970: start), end: Date(timeIntervalSince1970: end))
    }
}
