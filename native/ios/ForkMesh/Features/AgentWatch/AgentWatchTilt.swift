import CoreGraphics
import CoreMotion
import Observation

struct WatchMotionReading {
    let roll: Double
    let pitch: Double
    let facing: Double
    let shake: Double
}

@MainActor
@Observable
final class WatchTilt {
    private(set) var look: CGPoint = .zero

    @ObservationIgnored var onReading: (@MainActor (WatchMotionReading) -> Void)?
    @ObservationIgnored private let motion = CMMotionManager()
    @ObservationIgnored private var rest: CMAcceleration?

    static let span = 0.35
    static let settle = 0.004
    static let smoothing = 0.22

    func start() {
        guard motion.isDeviceMotionAvailable, !motion.isDeviceMotionActive else { return }
        rest = nil
        motion.deviceMotionUpdateInterval = 1.0 / 30.0
        motion.startDeviceMotionUpdates(to: .main) { [weak self] (data: CMDeviceMotion?, _: Error?) in
            guard let data else { return }
            let gravity = data.gravity
            let push = data.userAcceleration
            MainActor.assumeIsolated { self?.feel(gravity, push) }
        }
    }

    func stop() {
        if motion.isDeviceMotionActive { motion.stopDeviceMotionUpdates() }
        rest = nil
        look = .zero
    }

    private func feel(_ gravity: CMAcceleration, _ push: CMAcceleration) {
        onReading?(WatchMotionReading(
            roll: Self.clamp(gravity.x), pitch: Self.clamp(gravity.y), facing: Self.clamp(gravity.z),
            shake: (push.x * push.x + push.y * push.y + push.z * push.z).squareRoot()))
        var rest = self.rest ?? gravity
        rest.x += (gravity.x - rest.x) * Self.settle
        rest.y += (gravity.y - rest.y) * Self.settle
        self.rest = rest
        let x = Self.clamp((gravity.x - rest.x) / Self.span)
        let y = Self.clamp((gravity.y - rest.y) / Self.span)
        let next = CGPoint(
            x: Double(look.x) + (x - Double(look.x)) * Self.smoothing,
            y: Double(look.y) + (y - Double(look.y)) * Self.smoothing)
        if abs(Double(next.x - look.x)) + abs(Double(next.y - look.y)) > 0.002 {
            look = next
        }
    }

    private static func clamp(_ value: Double) -> Double {
        min(max(value, -1), 1)
    }
}
