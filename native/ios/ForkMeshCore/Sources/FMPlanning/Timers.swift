import FMBase
import Foundation

public enum PlanningTimerKind: String, Codable, Hashable, Sendable, CaseIterable {
    case stopwatch
    case countdown
    case alarm

    public var label: String {
        switch self {
        case .stopwatch: return "Timer"
        case .countdown: return "Countdown"
        case .alarm: return "Alarm"
        }
    }

    public var systemImage: String {
        switch self {
        case .stopwatch: return "stopwatch"
        case .countdown: return "timer"
        case .alarm: return "alarm"
        }
    }
}

public struct PlanningTimer: Codable, Hashable, Sendable, Identifiable {
    public var id: String
    public var kind: PlanningTimerKind
    public var label: String
    public var createdAtMs: Int64
    public var startedAtMs: Int64
    public var accumulatedMs: Int64
    public var durationMs: Int64
    public var alarmAtMs: Int64
    public var finishedAtMs: Int64

    public init(
        id: String, kind: PlanningTimerKind, label: String, createdAtMs: Int64, startedAtMs: Int64 = 0,
        accumulatedMs: Int64 = 0, durationMs: Int64 = 0, alarmAtMs: Int64 = 0, finishedAtMs: Int64 = 0
    ) {
        self.id = id
        self.kind = kind
        self.label = label
        self.createdAtMs = createdAtMs
        self.startedAtMs = startedAtMs
        self.accumulatedMs = accumulatedMs
        self.durationMs = durationMs
        self.alarmAtMs = alarmAtMs
        self.finishedAtMs = finishedAtMs
    }

    public var isRunning: Bool { startedAtMs > 0 && finishedAtMs == 0 }
    public var isPaused: Bool { startedAtMs == 0 && finishedAtMs == 0 && kind != .alarm }
    public var isFinished: Bool { finishedAtMs > 0 }

    public func elapsedMs(now: Int64) -> Int64 {
        let current = isRunning ? max(0, now - startedAtMs) : 0
        return max(0, accumulatedMs + current)
    }

    public func remainingMs(now: Int64) -> Int64? {
        switch kind {
        case .stopwatch: return nil
        case .countdown: return max(0, durationMs - elapsedMs(now: now))
        case .alarm: return max(0, alarmAtMs - now)
        }
    }

    public var fireAtMs: Int64? {
        if isFinished { return nil }
        switch kind {
        case .stopwatch: return nil
        case .countdown: return isRunning ? startedAtMs + max(0, durationMs - accumulatedMs) : nil
        case .alarm: return alarmAtMs > 0 ? alarmAtMs : nil
        }
    }

    public func isDue(now: Int64) -> Bool {
        guard let fire = fireAtMs else { return false }
        return now >= fire
    }

    public func display(now: Int64) -> String {
        if isFinished && kind != .stopwatch { return "Done" }
        if let remaining = remainingMs(now: now) { return PlanningTimer.clock(remaining, roundUp: true) }
        return PlanningTimer.clock(elapsedMs(now: now), roundUp: false)
    }

    public static func clock(_ ms: Int64, roundUp: Bool) -> String {
        let clamped = max(0, ms)
        let seconds = roundUp ? (clamped + 999) / 1_000 : clamped / 1_000
        let h = seconds / 3_600
        let m = (seconds % 3_600) / 60
        let s = seconds % 60
        func two(_ n: Int64) -> String { n < 10 ? "0\(n)" : "\(n)" }
        return h > 0 ? "\(h):\(two(m)):\(two(s))" : "\(m):\(two(s))"
    }
}

public struct PlanningTimerNotification: Hashable, Sendable {
    public var id: String
    public var fireAtMs: Int64
    public var title: String
    public var body: String
}

public struct TimerActivitySnapshot: Codable, Hashable, Sendable {
    public var timerId: String
    public var kind: PlanningTimerKind
    public var label: String
    public var endsAtMs: Int64
    public var countsFromMs: Int64
    public var paused: Bool
    public var pausedText: String

    public init(timerId: String, kind: PlanningTimerKind, label: String, endsAtMs: Int64, countsFromMs: Int64,
                paused: Bool, pausedText: String) {
        self.timerId = timerId
        self.kind = kind
        self.label = label
        self.endsAtMs = endsAtMs
        self.countsFromMs = countsFromMs
        self.paused = paused
        self.pausedText = pausedText
    }
}

@MainActor
public final class PlanningTimerStore {
    nonisolated public static let storageKey = "planning/timers.v1"
    nonisolated public static let maxTimers = 50
    nonisolated public static let maxLabelLength = 80

    private let store: KeyValueStore
    private let clock: Clock
    private let makeId: () -> String
    public private(set) var timers: [PlanningTimer]
    public var onChange: (@MainActor ([PlanningTimer]) -> Void)?

    public init(store: KeyValueStore, clock: Clock = SystemClock(), makeId: @escaping () -> String = { UUID().uuidString }) {
        self.store = store
        self.clock = clock
        self.makeId = makeId
        timers = PlanningTimerStore.decode(store.string(forKey: PlanningTimerStore.storageKey))
    }

    nonisolated static func decode(_ text: String?) -> [PlanningTimer] {
        guard let text, let data = text.data(using: .utf8),
              let list = try? JSONDecoder().decode([PlanningTimer].self, from: data) else { return [] }
        return Array(list.prefix(maxTimers))
    }

    private func save() {
        if let data = try? JSONEncoder().encode(timers), let text = String(data: data, encoding: .utf8) {
            store.set(text, forKey: PlanningTimerStore.storageKey)
        }
        onChange?(timers)
    }

    private var now: Int64 { clock.nowMillis }

    private func cleanLabel(_ label: String, _ kind: PlanningTimerKind) -> String {
        let trimmed = label.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? kind.label : String(trimmed.prefix(PlanningTimerStore.maxLabelLength))
    }

    public enum StoreError: Error, Equatable, CustomStringConvertible {
        case full
        case invalidDuration
        case alarmInPast

        public var description: String {
            switch self {
            case .full: return "Remove a timer first: at most \(PlanningTimerStore.maxTimers) are kept."
            case .invalidDuration: return "Choose a duration between one second and 24 hours."
            case .alarmInPast: return "Choose a time in the future."
            }
        }
    }

    private func insert(_ timer: PlanningTimer) throws -> PlanningTimer {
        if timers.count >= PlanningTimerStore.maxTimers { throw StoreError.full }
        timers.insert(timer, at: 0)
        save()
        return timer
    }

    @discardableResult
    public func startStopwatch(label: String) throws -> PlanningTimer {
        try insert(PlanningTimer(id: makeId(), kind: .stopwatch, label: cleanLabel(label, .stopwatch), createdAtMs: now, startedAtMs: now))
    }

    @discardableResult
    public func startCountdown(label: String, seconds: Int) throws -> PlanningTimer {
        guard seconds >= 1 && seconds <= 24 * 3_600 else { throw StoreError.invalidDuration }
        return try insert(PlanningTimer(
            id: makeId(), kind: .countdown, label: cleanLabel(label, .countdown), createdAtMs: now, startedAtMs: now,
            durationMs: Int64(seconds) * 1_000))
    }

    @discardableResult
    public func setAlarm(label: String, atMs: Int64) throws -> PlanningTimer {
        guard atMs > now else { throw StoreError.alarmInPast }
        return try insert(PlanningTimer(id: makeId(), kind: .alarm, label: cleanLabel(label, .alarm), createdAtMs: now, alarmAtMs: atMs))
    }

    private func update(_ id: String, _ change: (inout PlanningTimer) -> Void) {
        guard let index = timers.firstIndex(where: { $0.id == id }) else { return }
        change(&timers[index])
        save()
    }

    public func pause(_ id: String) {
        let now = self.now
        update(id) { timer in
            guard timer.isRunning, timer.kind != .alarm else { return }
            timer.accumulatedMs = timer.elapsedMs(now: now)
            timer.startedAtMs = 0
        }
    }

    public func resume(_ id: String) {
        let now = self.now
        update(id) { timer in
            guard timer.isPaused else { return }
            timer.startedAtMs = now
        }
    }

    public func restart(_ id: String) {
        let now = self.now
        update(id) { timer in
            guard timer.kind != .alarm else { return }
            timer.accumulatedMs = 0
            timer.finishedAtMs = 0
            timer.startedAtMs = now
        }
    }

    public func stop(_ id: String) {
        let now = self.now
        update(id) { timer in
            timer.accumulatedMs = timer.elapsedMs(now: now)
            timer.startedAtMs = 0
            timer.finishedAtMs = now
        }
    }

    public func remove(_ id: String) {
        timers.removeAll { $0.id == id }
        save()
    }

    public func removeFinished() {
        timers.removeAll { $0.isFinished }
        save()
    }

    @discardableResult
    public func settle() -> [PlanningTimer] {
        let now = self.now
        var rung: [PlanningTimer] = []
        for index in timers.indices where timers[index].isDue(now: now) {
            let fire = timers[index].fireAtMs ?? now
            if timers[index].kind == .countdown {
                timers[index].accumulatedMs = timers[index].durationMs
            }
            timers[index].startedAtMs = 0
            timers[index].finishedAtMs = fire
            rung.append(timers[index])
        }
        if !rung.isEmpty { save() }
        return rung
    }

    public func notificationPlan() -> [PlanningTimerNotification] {
        timers.compactMap { timer in
            guard let fire = timer.fireAtMs else { return nil }
            let body = timer.kind == .alarm ? "Alarm" : "Countdown finished"
            return PlanningTimerNotification(id: "timer-\(timer.id)", fireAtMs: fire, title: timer.label, body: body)
        }
    }

    public func activityTimer() -> PlanningTimer? {
        let live = timers.filter { !$0.isFinished }
        if let countdown = live.filter({ $0.kind == .countdown && $0.isRunning })
            .min(by: { ($0.fireAtMs ?? .max) < ($1.fireAtMs ?? .max) }) { return countdown }
        if let alarm = live.filter({ $0.kind == .alarm }).min(by: { $0.alarmAtMs < $1.alarmAtMs }) { return alarm }
        return live.first { $0.kind == .stopwatch && $0.isRunning }
    }

    public func activitySnapshot(_ timer: PlanningTimer) -> TimerActivitySnapshot {
        let now = self.now
        let paused = !timer.isRunning && timer.kind != .alarm
        return TimerActivitySnapshot(
            timerId: timer.id, kind: timer.kind, label: timer.label,
            endsAtMs: timer.fireAtMs ?? 0,
            countsFromMs: timer.kind == .stopwatch && timer.isRunning ? timer.startedAtMs - timer.accumulatedMs : 0,
            paused: paused, pausedText: timer.display(now: now))
    }
}
