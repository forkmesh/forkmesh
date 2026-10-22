import Foundation

public struct CallVoice: Equatable, Sendable {
    public let id: String
    public let label: String
    public let pitchHz: Double
    public let tract: Double
    public let brightness: Double
    public let breath: Double
    public let range: Double
    public let jitter: Double
    public let tremor: Double

    public init(id: String, label: String, pitchHz: Double, tract: Double, brightness: Double, breath: Double,
                range: Double = 1, jitter: Double = 0, tremor: Double = 0) {
        self.id = id
        self.label = label
        self.pitchHz = pitchHz
        self.tract = tract
        self.brightness = brightness
        self.breath = breath
        self.range = range
        self.jitter = jitter
        self.tremor = tremor
    }
}

public enum CallVoiceCatalog {
    public static let match = "face"
    public static let off = "off"
    public static let ownLabel = "Your own"

    public static let ratioLimits: ClosedRange<Double> = 0.45...2.6
    public static let formantLimits: ClosedRange<Double> = 0.72...1.42
    public static let callerHzLimits: ClosedRange<Double> = 70...330
    public static let tractLimits: ClosedRange<Double> = 0.94...1.26
    public static let rangeLimits: ClosedRange<Double> = 0...1.5
    public static let jitterLimits: ClosedRange<Double> = 0...0.04
    public static let tremorLimits: ClosedRange<Double> = 0...0.05

    public static let voices: [CallVoice] = [
        CallVoice(id: "maya", label: "Maya", pitchHz: 196, tract: 1.14, brightness: 0, breath: 0.04),
        CallVoice(id: "theo", label: "Theo", pitchHz: 124, tract: 1.03, brightness: 0.1, breath: 0),
        CallVoice(id: "kofi", label: "Kofi", pitchHz: 96, tract: 0.93, brightness: -0.15, breath: 0),
        CallVoice(id: "ingrid", label: "Ingrid", pitchHz: 228, tract: 1.21, brightness: 0.12, breath: 0.08),
        CallVoice(id: "ren", label: "Ren", pitchHz: 210, tract: 1.17, brightness: 0.05, breath: 0.02),
        CallVoice(id: "deep", label: "Deep man", pitchHz: 84, tract: 0.9, brightness: -0.25, breath: 0, range: 0.85),
        CallVoice(id: "older-man", label: "Older man", pitchHz: 128, tract: 0.97, brightness: -0.1, breath: 0.1,
                  range: 0.8, jitter: 0.012, tremor: 0.012),
        CallVoice(id: "young-man", label: "Young man", pitchHz: 138, tract: 1.06, brightness: 0.12, breath: 0.02, range: 1.15),
        CallVoice(id: "warm", label: "Warm woman", pitchHz: 188, tract: 1.12, brightness: -0.1, breath: 0.06),
        CallVoice(id: "low", label: "Low woman", pitchHz: 165, tract: 1.1, brightness: -0.05, breath: 0.03, range: 0.9),
        CallVoice(id: "older-woman", label: "Older woman", pitchHz: 176, tract: 1.12, brightness: -0.08, breath: 0.1,
                  range: 0.85, jitter: 0.012, tremor: 0.015),
        CallVoice(id: "teen", label: "Teen", pitchHz: 252, tract: 1.25, brightness: 0.15, breath: 0.04, range: 1.2),
        CallVoice(id: "neutral", label: "Neutral", pitchHz: 158, tract: 1.08, brightness: 0, breath: 0.03, range: 0.9),
        CallVoice(id: "robot", label: "Robot", pitchHz: 118, tract: 1, brightness: 0.1, breath: 0, range: 0),
    ]

    public static func voice(_ id: String?) -> CallVoice? {
        guard let id else { return nil }
        return voices.first { $0.id == id }
    }

    public static func normalizeChoice(_ choice: String?) -> String {
        let value = (choice ?? "").trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        if value == off { return off }
        if voice(value) != nil { return value }
        return match
    }

    public static func resolve(_ choice: String?, faceId: String?) -> String {
        let value = normalizeChoice(choice)
        if value == off { return "" }
        if value == match { return voice(faceId ?? "")?.id ?? "" }
        return value
    }

    public static func label(_ id: String?) -> String {
        voice(id)?.label ?? ownLabel
    }

    public static func choices() -> [(id: String, label: String)] {
        [(match, "Match face"), (off, "Your own voice")] + voices.map { ($0.id, $0.label) }
    }
}
