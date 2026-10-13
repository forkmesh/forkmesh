import Foundation
import Observation

@MainActor
public protocol HeartRateBridge: AnyObject {
    var available: Bool { get }
    func authorizeHeartRate() async -> Bool
    func saveHeartRate(beatsPerMinute: Double, start: Date, end: Date) async throws
}

public enum HeartTap {
    public static let minTaps = 5
    public static let maxIntervals = 12
    public static let resetAfter: TimeInterval = 2.5
    public static let tolerance = 0.3
    public static let range: ClosedRange<Double> = 30...220

    public static func intervals(_ taps: [Date]) -> [TimeInterval] {
        Array(zip(taps.dropFirst(), taps).map { $0.timeIntervalSince($1) }.suffix(maxIntervals))
    }

    public static func median(_ values: [Double]) -> Double? {
        guard !values.isEmpty else { return nil }
        let sorted = values.sorted()
        let mid = sorted.count / 2
        return sorted.count % 2 == 1 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2
    }

    public static func bpm(_ taps: [Date]) -> Int? {
        guard taps.count >= minTaps else { return nil }
        let gaps = intervals(taps)
        guard let middle = median(gaps), middle > 0 else { return nil }
        let steady = gaps.filter { abs($0 - middle) <= middle * tolerance }
        guard steady.count >= minTaps - 1 else { return nil }
        let rate = 60 / (steady.reduce(0, +) / Double(steady.count))
        guard range.contains(rate) else { return nil }
        return Int(rate.rounded())
    }
}

@MainActor
@Observable
public final class HeartTapModel {
    public static let title = "Heart rate"
    public static let tapLabel = "Tap with each heartbeat"
    public static let saveLabel = "Save to Apple Health"
    public static let idleLine = "Find your pulse on your wrist or neck, then tap the heart with each beat."
    public static let unavailableMessage = "Apple Health is not available on this device."
    public static let deniedMessage =
        "ForkMesh was not allowed to save your heart rate. Allow it in Settings > Health > Data Access & Devices."
    public static let unsteadyLine = "Keep tapping in time with your pulse."

    @ObservationIgnored public let bridge: HeartRateBridge?
    @ObservationIgnored private let now: () -> Date

    public private(set) var taps: [Date] = []
    public private(set) var bpm: Int?
    public private(set) var saving = false
    public private(set) var savedBpm: Int?
    public var notice: ToolsNotice?

    public init(bridge: HeartRateBridge?, now: (() -> Date)? = nil) {
        self.bridge = bridge
        self.now = now ?? { Date() }
    }

    public var available: Bool { bridge?.available == true }
    public var canSave: Bool { bpm != nil && !saving && available }
    public var canReset: Bool { !taps.isEmpty && !saving }

    public var line: String {
        if let savedBpm, savedBpm == bpm {
            return "\(savedBpm) BPM saved to Apple Health."
        }
        if let bpm {
            return "\(bpm) BPM from \(taps.count) taps."
        }
        if taps.isEmpty {
            return Self.idleLine
        }
        if taps.count < HeartTap.minTaps {
            let left = HeartTap.minTaps - taps.count
            return "Keep going: \(left) more \(left == 1 ? "tap" : "taps")."
        }
        return Self.unsteadyLine
    }

    public func tap() {
        guard !saving else { return }
        let at = now()
        if let last = taps.last, at.timeIntervalSince(last) > HeartTap.resetAfter || at < last {
            taps = []
        }
        taps.append(at)
        let keep = HeartTap.maxIntervals + 1
        if taps.count > keep {
            taps.removeFirst(taps.count - keep)
        }
        bpm = HeartTap.bpm(taps)
        savedBpm = nil
    }

    public func reset() {
        guard !saving else { return }
        taps = []
        bpm = nil
        savedBpm = nil
    }

    public func save() async {
        guard let bpm, !saving, let first = taps.first, let last = taps.last else { return }
        guard let bridge, bridge.available else {
            notice = ToolsNotice(Self.unavailableMessage, isError: true)
            return
        }
        saving = true
        defer { saving = false }
        guard await bridge.authorizeHeartRate() else {
            notice = ToolsNotice(Self.deniedMessage, isError: true)
            return
        }
        do {
            try await bridge.saveHeartRate(beatsPerMinute: Double(bpm), start: first, end: last)
            savedBpm = bpm
            notice = ToolsNotice("Saved \(bpm) BPM to Apple Health.")
        } catch {
            notice = ToolsNotice("Could not save to Apple Health: \(error.localizedDescription)", isError: true)
        }
    }
}
