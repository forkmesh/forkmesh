import CoreHaptics
import Foundation
import UIKit

@MainActor
final class FmRumble {
    static let ramp: TimeInterval = 2.8
    static let length: TimeInterval = 12

    private var engine: CHHapticEngine?
    private var player: CHHapticPatternPlayer?

    func start() {
        stop()
        guard FmHaptics.enabled, !UIAccessibility.isReduceMotionEnabled,
              CHHapticEngine.capabilitiesForHardware().supportsHaptics else { return }
        do {
            let engine = try self.engine ?? CHHapticEngine()
            engine.isAutoShutdownEnabled = true
            engine.resetHandler = { [weak engine] in
                try? engine?.start()
            }
            try engine.start()
            self.engine = engine
            let player = try engine.makePlayer(with: Self.pattern())
            try player.start(atTime: CHHapticTimeImmediate)
            self.player = player
        } catch {
            player = nil
        }
    }

    func stop() {
        try? player?.stop(atTime: CHHapticTimeImmediate)
        player = nil
        engine?.stop(completionHandler: nil)
    }

    nonisolated static func level(_ elapsed: TimeInterval) -> Double {
        let t = min(max(elapsed / ramp, 0), 1)
        return t * t
    }

    nonisolated static func pattern() throws -> CHHapticPattern {
        var events = [
            CHHapticEvent(
                eventType: .hapticContinuous,
                parameters: [
                    CHHapticEventParameter(parameterID: .hapticIntensity, value: 1),
                    CHHapticEventParameter(parameterID: .hapticSharpness, value: 0.25),
                ],
                relativeTime: 0, duration: length)
        ]
        var time: TimeInterval = 0
        while time < length {
            let level = Self.level(time)
            events.append(CHHapticEvent(
                eventType: .hapticTransient,
                parameters: [
                    CHHapticEventParameter(parameterID: .hapticIntensity, value: Float(0.55 + 0.45 * level)),
                    CHHapticEventParameter(parameterID: .hapticSharpness, value: Float(0.3 + 0.6 * level)),
                ],
                relativeTime: time))
            time += 0.24 - 0.185 * level
        }
        let steps = 10
        let intensity = (0...steps).map { step -> CHHapticParameterCurve.ControlPoint in
            let at = Self.ramp * Double(step) / Double(steps)
            return .init(relativeTime: at, value: Float(0.1 + 0.9 * Self.level(at)))
        }
        let sharpness = (0...steps).map { step -> CHHapticParameterCurve.ControlPoint in
            let at = Self.ramp * Double(step) / Double(steps)
            return .init(relativeTime: at, value: Float(-0.3 + 0.7 * Self.level(at)))
        }
        return try CHHapticPattern(
            events: events,
            parameterCurves: [
                CHHapticParameterCurve(parameterID: .hapticIntensityControl, controlPoints: intensity, relativeTime: 0),
                CHHapticParameterCurve(parameterID: .hapticSharpnessControl, controlPoints: sharpness, relativeTime: 0),
            ])
    }
}
