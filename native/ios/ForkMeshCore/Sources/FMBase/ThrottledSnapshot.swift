import Foundation
import Observation

@MainActor
@Observable
public final class ThrottledSnapshot<Value: Equatable> {
    public private(set) var value: Value
    @ObservationIgnored public private(set) var evaluations = 0
    @ObservationIgnored private let interval: TimeInterval
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored private let sleeper: @Sendable (TimeInterval) async throws -> Void
    @ObservationIgnored private let compute: @MainActor () -> Value
    @ObservationIgnored private var pending: Task<Void, Never>?
    @ObservationIgnored private var lastEvaluated: Date = .distantPast

    public init(
        interval: TimeInterval, clock: Clock = SystemClock(),
        sleeper: @escaping @Sendable (TimeInterval) async throws -> Void = { (seconds: TimeInterval) in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        },
        compute: @escaping @MainActor () -> Value
    ) {
        self.interval = interval
        self.clock = clock
        self.sleeper = sleeper
        self.compute = compute
        value = compute()
        evaluate()
    }

    public func refreshNow() {
        pending?.cancel()
        evaluate()
    }

    private func evaluate() {
        pending = nil
        lastEvaluated = clock.now
        evaluations += 1
        let next: Value = withObservationTracking {
            compute()
        } onChange: { [weak self] in
            Task { @MainActor [weak self] in self?.changed() }
        }
        if next != value { value = next }
    }

    private func changed() {
        guard pending == nil else { return }
        let wait: TimeInterval = max(0, interval - clock.now.timeIntervalSince(lastEvaluated))
        let sleeper = self.sleeper
        pending = Task { @MainActor [weak self] in
            if wait > 0 {
                do { try await sleeper(wait) } catch { return }
            }
            guard let self, !Task.isCancelled else { return }
            self.evaluate()
        }
    }
}
