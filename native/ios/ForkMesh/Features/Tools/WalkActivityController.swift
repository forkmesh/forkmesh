import ActivityKit
import FMBase
import FMSurfaces
import FMTools
import Foundation
import Observation
import UIKit

@MainActor
@Observable
final class WalkActivityController {
    static let shared: WalkActivityController = WalkActivityController()

    static let staleAfter: TimeInterval = 5 * 60
    static let dismissAfter: TimeInterval = 10 * 60

    private(set) var walking: Bool = false
    private(set) var cadence: Double = 0
    private(set) var running: Bool = false

    @ObservationIgnored private weak var health: HealthKitBridge?
    @ObservationIgnored private var detector: WalkDetector = WalkDetector()
    @ObservationIgnored private var walk: WalkProgress?
    @ObservationIgnored private var watching: Bool = false
    @ObservationIgnored private var generation: Int = 0
    @ObservationIgnored private var check: Task<Void, Never>?
    @ObservationIgnored private var pace: WalkPace = WalkPace()
    @ObservationIgnored private var pacing: Bool = false
    @ObservationIgnored private var paceExpiry: Task<Void, Never>?

    var stepping: Bool { cadence > 0 }
    var moving: Bool { stepping || walking }
    var gaitText: String { StepsText.gait(stepsPerMinute) }
    var stepsPerMinute: Int { Int((cadence * 60).rounded()) }

    func start(_ health: HealthKitBridge) {
        self.health = health
        startPace()
        guard !watching else { return }
        endActivities(final: nil)
        watching = health.startWalkDetection { [weak self] (sample: WalkMotionSample) in
            self?.observe(sample)
        }
    }

    func stop() {
        stopPace()
        check?.cancel()
        check = nil
        if watching {
            watching = false
            health?.stopWalkDetection()
        }
        generation += 1
        health?.stopWalkSteps()
        walk = nil
        walking = false
        detector = WalkDetector()
        endActivities(final: nil)
    }

    func appResumed() {
        startPace()
        guard watching, let health else { return }
        check?.cancel()
        check = nil
        detector.forgetPendingStart()
        watching = health.startWalkDetection { [weak self] (sample: WalkMotionSample) in
            self?.observe(sample)
        }
        scheduleCheck()
        if walk != nil { showActivity() }
    }

    func appLeaving() {
        guard watching, pacing, stepping else { return }
        apply(detector.begin(Date(), after: WalkDetector.leaveStartAfter))
        scheduleCheck()
    }

    func appPaused() {
        stopPace()
    }

    private func startPace() {
        guard !pacing, let health, UIApplication.shared.applicationState != .background else { return }
        pace = WalkPace()
        pacing = health.startPaceSteps { [weak self] (steps: Int, cadence: Double) in
            guard let self, self.pacing else { return }
            self.pace.record(steps: steps, cadence: cadence, at: Date())
            self.publishPace()
        }
    }

    private func stopPace() {
        if pacing {
            pacing = false
            health?.stopPaceSteps()
        }
        pace = WalkPace()
        publishPace()
    }

    private func publishPace() {
        if cadence != pace.cadence { cadence = pace.cadence }
        if running != pace.running { running = pace.running }
        if watching {
            apply(detector.paced(pace.moving, at: Date()))
            scheduleCheck()
        }
        paceExpiry?.cancel()
        paceExpiry = nil
        guard let expires = pace.expiresAt else { return }
        let delay: TimeInterval = max(0.5, expires.timeIntervalSinceNow)
        paceExpiry = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
            guard !Task.isCancelled, let self else { return }
            self.pace.expire(Date())
            self.publishPace()
        }
    }

    func catchUp() async {
        guard watching, let health, let started = walk?.startedAt,
            UIApplication.shared.applicationState != .active
        else { return }
        let now: Date = Date()
        let samples: [WalkMotionSample] = await health.walkHistory(from: started, to: now)
        guard walk?.startedAt == started else { return }
        var caught: WalkDetector = detector
        var end: Date?
        if case let .ended(at) = caught.replay(samples, until: now) { end = at }
        let reading: (steps: Int, distanceM: Double)? = await health.walkReading(from: started, to: end ?? now)
        guard walk?.startedAt == started else { return }
        if let reading { walk?.record(steps: reading.steps, distanceM: reading.distanceM) }
        if let end {
            detector = WalkDetector()
            finishWalk(at: end)
        } else {
            detector = caught
            publish()
            scheduleCheck()
        }
    }

    private func observe(_ sample: WalkMotionSample) {
        guard watching else { return }
        apply(detector.observe(sample, now: Date()))
        scheduleCheck()
    }

    private func evaluate(_ now: Date) {
        apply(detector.tick(now))
        scheduleCheck()
    }

    private func apply(_ event: WalkEvent?) {
        switch event {
        case let .started(start): walkStarted(start)
        case let .ended(end): finishWalk(at: end)
        case nil: break
        }
    }

    private func scheduleCheck() {
        check?.cancel()
        check = nil
        guard let next = detector.nextCheck else { return }
        let delay: TimeInterval = max(1, next.timeIntervalSinceNow)
        check = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
            guard !Task.isCancelled else { return }
            self?.evaluate(Date())
        }
    }

    private func walkStarted(_ start: Date) {
        guard let health else { return }
        generation += 1
        let current: Int = generation
        walk = WalkProgress(startedAt: start)
        walking = true
        _ = health.startWalkSteps(from: start) { [weak self] (steps: Int, distance: Double) in
            guard let self, self.generation == current else { return }
            self.walk?.record(steps: steps, distanceM: distance)
            self.publish()
        }
        Task { [weak self] in
            let baseline: Int = await health.stepsToday(until: start)
            guard let self, self.generation == current else { return }
            self.walk?.baseline = baseline
            self.publish()
        }
        showActivity()
    }

    private func finishWalk(at end: Date) {
        generation += 1
        health?.stopWalkSteps()
        let final: ActivityContent<WalkActivityAttributes.ContentState>? = walk.map { content($0, endedAt: end) }
        walk = nil
        walking = false
        endActivities(final: final)
    }

    private var activities: [Activity<WalkActivityAttributes>] {
        Activity<WalkActivityAttributes>.activities.filter {
            $0.activityState == .active || $0.activityState == .stale
        }
    }

    private func showActivity() {
        guard let walk, ActivityAuthorizationInfo().areActivitiesEnabled else { return }
        let attributes: WalkActivityAttributes = WalkActivityAttributes(startedAt: walk.startedAt)
        if activities.contains(where: { $0.attributes == attributes }) { return }
        guard UIApplication.shared.applicationState != .background else { return }
        do {
            _ = try Activity.request(attributes: attributes, content: content(walk, endedAt: nil), pushType: nil)
        } catch {
            Log.warning("steps", "Walk Live Activity not started: \(error)")
        }
    }

    private func publish() {
        guard let walk else { return }
        let attributes: WalkActivityAttributes = WalkActivityAttributes(startedAt: walk.startedAt)
        let next: ActivityContent<WalkActivityAttributes.ContentState> = content(walk, endedAt: nil)
        for activity in activities where activity.attributes == attributes && activity.content.state != next.state {
            Task { await activity.update(next) }
        }
    }

    private func endActivities(final: ActivityContent<WalkActivityAttributes.ContentState>?) {
        let policy: ActivityUIDismissalPolicy =
            final == nil ? .immediate : .after(Date().addingTimeInterval(Self.dismissAfter))
        for activity in activities {
            Task { await activity.end(final, dismissalPolicy: policy) }
        }
    }

    private func content(_ walk: WalkProgress, endedAt: Date?) -> ActivityContent<WalkActivityAttributes.ContentState> {
        let prefs: KeyValueStore = HealthFeature.prefs
        let imperial: Bool = prefs.bool(forKey: HealthPrefs.stepsImperial) ?? (Locale.current.measurementSystem == .us)
        let state: WalkActivityAttributes.ContentState = WalkActivityAttributes.ContentState(
            steps: walk.steps, todaySteps: walk.todaySteps, distance: StepsText.distance(walk.distanceM, imperial: imperial),
            endedAtMs: endedAt.map { Int64(($0.timeIntervalSince1970 * 1_000).rounded()) } ?? 0)
        return ActivityContent(state: state, staleDate: endedAt == nil ? Date().addingTimeInterval(Self.staleAfter) : nil)
    }
}
