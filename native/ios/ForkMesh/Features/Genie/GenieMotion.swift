import CoreGraphics
import CoreMotion
import FMAssistant
import Foundation
import Observation
import UIKit

@MainActor
@Observable
final class GenieMotion {
    static let shared: GenieMotion = GenieMotion()
    nonisolated static let smoothing: Double = 0.18
    nonisolated static let settle: Double = 0.01
    nonisolated static let shakeLevel: Double = 1.5
    nonisolated static let bumpLevel: Double = 0.75
    nonisolated static let tiltSmoothing: Double = 0.35

    private(set) var lean: CGSize = .zero
    private(set) var gravity: CGSize = GenieGravity.down
    private(set) var flat: Bool = false

    @ObservationIgnored var onShake: (@MainActor () -> Void)?
    @ObservationIgnored var onBump: (@MainActor () -> Void)?
    @ObservationIgnored var onJolt: (@MainActor (CGSize) -> Void)?
    @ObservationIgnored private let motion: CMMotionManager = CMMotionManager()
    @ObservationIgnored private var users: Int = 0
    @ObservationIgnored private var restPitch: Double?
    @ObservationIgnored private var peaks: [Date] = []
    @ObservationIgnored private var quietUntil: Date = .distantPast
    @ObservationIgnored private var orientation: UIInterfaceOrientation = .portrait
    @ObservationIgnored private var frames: Int = 0
    @ObservationIgnored private var fast: Bool = false
    @ObservationIgnored private(set) var tilt: CGSize = GenieGravity.down
    @ObservationIgnored private var lyingSince: Date?

    var live: Bool { motion.isDeviceMotionActive }

    func begin() {
        users += 1
        guard users == 1, motion.isDeviceMotionAvailable, !motion.isDeviceMotionActive else { return }
        restPitch = nil
        fast = false
        motion.deviceMotionUpdateInterval = 1.0 / 30.0
        motion.startDeviceMotionUpdates(to: .main) { [weak self] (data: CMDeviceMotion?, _: Error?) in
            guard let data = data else { return }
            let gravity: CMAcceleration = data.gravity
            let push: CMAcceleration = data.userAcceleration
            MainActor.assumeIsolated { self?.feel(gravity, push) }
        }
    }

    func end() {
        users = max(0, users - 1)
        guard users == 0 else { return }
        if motion.isDeviceMotionActive { motion.stopDeviceMotionUpdates() }
        restPitch = nil
        lean = .zero
        gravity = GenieGravity.down
        tilt = GenieGravity.down
        lyingSince = nil
        flat = false
    }

    func quick(_ on: Bool) {
        guard on != fast else { return }
        fast = on
        motion.deviceMotionUpdateInterval = on ? 1.0 / 60.0 : 1.0 / 30.0
    }

    nonisolated static func plane(_ x: Double, _ y: Double, _ orientation: UIInterfaceOrientation) -> CGSize {
        switch orientation {
        case .portraitUpsideDown: return CGSize(width: -x, height: y)
        case .landscapeLeft: return CGSize(width: y, height: x)
        case .landscapeRight: return CGSize(width: -y, height: -x)
        default: return CGSize(width: x, height: -y)
        }
    }

    nonisolated static func screen(_ x: Double, _ y: Double, _ orientation: UIInterfaceOrientation) -> CGSize {
        let raw: CGSize = plane(x, y, orientation)
        let length: CGFloat = hypot(raw.width, raw.height)
        guard length > 0.3 else { return GenieGravity.down }
        return CGSize(width: raw.width / length, height: raw.height / length)
    }

    private func orient() {
        frames += 1
        guard frames % 30 == 1 else { return }
        let scenes: [UIWindowScene] = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        let scene: UIWindowScene? = scenes.first { $0.activationState == .foregroundActive } ?? scenes.first
        orientation = scene?.effectiveGeometry.interfaceOrientation ?? .portrait
    }

    private func feel(_ gravity: CMAcceleration, _ push: CMAcceleration) {
        var rest: Double = restPitch ?? gravity.y
        rest += (gravity.y - rest) * GenieMotion.settle
        restPitch = rest
        let x: Double = GenieMotion.clamp(-gravity.x * 1.6)
        let y: Double = GenieMotion.clamp((gravity.y - rest) / 0.35)
        let next: CGSize = CGSize(
            width: Double(lean.width) + (x - Double(lean.width)) * GenieMotion.smoothing,
            height: Double(lean.height) + (y - Double(lean.height)) * GenieMotion.smoothing)
        if abs(Double(next.width - lean.width)) + abs(Double(next.height - lean.height)) > 0.004 {
            lean = next
        }
        orient()
        let pull: CGSize = GenieMotion.screen(gravity.x, gravity.y, orientation)
        let blended: CGSize = GenieGravity.unit(CGSize(
            width: self.gravity.width + (pull.width - self.gravity.width) * 0.25,
            height: self.gravity.height + (pull.height - self.gravity.height) * 0.25))
        if abs(blended.width - self.gravity.width) + abs(blended.height - self.gravity.height) > 0.02 {
            self.gravity = blended
        }
        let plane: CGSize = GenieMotion.plane(gravity.x, gravity.y, orientation)
        let k: CGFloat = CGFloat(GenieMotion.tiltSmoothing)
        tilt = CGSize(width: tilt.width + (plane.width - tilt.width) * k, height: tilt.height + (plane.height - tilt.height) * k)
        lie(GenieGlass.flat(plane: tilt, z: gravity.z, was: flat))
        onJolt?(GenieMotion.plane(push.x, push.y, orientation))
        let force: Double = (push.x * push.x + push.y * push.y + push.z * push.z).squareRoot()
        let now: Date = Date()
        guard now >= quietUntil else { return }
        if force > GenieMotion.shakeLevel {
            peaks = peaks.filter { now.timeIntervalSince($0) < 0.9 } + [now]
            if peaks.count >= 3 {
                peaks = []
                quietUntil = now.addingTimeInterval(2.5)
                onShake?()
            }
        } else if force > GenieMotion.bumpLevel, peaks.isEmpty {
            quietUntil = now.addingTimeInterval(1.2)
            onBump?()
        }
    }

    private func lie(_ lying: Bool) {
        guard lying != flat else {
            lyingSince = nil
            return
        }
        let now: Date = Date()
        guard let since = lyingSince else {
            lyingSince = now
            return
        }
        guard now.timeIntervalSince(since) >= (lying ? GenieGlass.settleFor : GenieGlass.settleFor / 2) else { return }
        lyingSince = nil
        flat = lying
    }

    nonisolated static func clamp(_ value: Double) -> Double {
        min(max(value, -1), 1)
    }
}
