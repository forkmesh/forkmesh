import Foundation

public enum InterfaceCue: String, CaseIterable, Sendable {
    case keystroke
    case click
    case toggleOn = "toggle-on"
    case toggleOff = "toggle-off"
    case select
    case navigate
    case open
    case close
    case send
    case arrive
    case success
    case warn
    case error
    case delete

    public var weight: Int {
        switch self {
        case .keystroke: return 0
        case .select: return 1
        case .close: return 2
        case .click: return 3
        case .toggleOn, .toggleOff: return 4
        case .navigate: return 5
        case .open: return 6
        case .arrive: return 7
        case .send: return 8
        case .warn: return 9
        case .success: return 10
        case .delete: return 11
        case .error: return 12
        }
    }

    public static func toggle(_ on: Bool) -> InterfaceCue { on ? .toggleOn : .toggleOff }

    public static func forLabel(_ label: String, fallback: InterfaceCue) -> InterfaceCue {
        let text = label.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.hasSuffix("\u{2026}") || text.hasSuffix("...") { return fallback }
        let word = text.replacingOccurrences(of: "&", with: "")
            .split(separator: " ", omittingEmptySubsequences: true).first.map { $0.lowercased() } ?? ""
        if ["delete", "remove", "discard", "uninstall", "trash", "erase", "purge"].contains(word) { return .delete }
        if ["send", "post", "submit", "publish", "push"].contains(word) { return .send }
        return fallback
    }
}

public let kCueThrottleMs: Int64 = 45
public let kKeystrokeVariants = 6

public func interfaceVoice() -> Voice {
    Voice(index: -3, instrument: .celesta, octave: 2, degreeOffset: 0, pan: 0.5, slotOffset: 0, detune: 1.0)
}

private struct CueStep {
    let degree: Int
    let delay: Double
    let seconds: Double
    let gain: Double
    let timbre: Timbre
}

private func cueSteps(_ cue: InterfaceCue) -> [CueStep] {
    switch cue {
    case .keystroke:
        return [CueStep(degree: 1, delay: 0, seconds: 0.022, gain: 0.04, timbre: .wood)]
    case .click:
        return [CueStep(degree: 0, delay: 0, seconds: 0.05, gain: 0.085, timbre: .mallet)]
    case .toggleOn:
        return [
            CueStep(degree: 0, delay: 0, seconds: 0.05, gain: 0.075, timbre: .mallet),
            CueStep(degree: 2, delay: 0.045, seconds: 0.07, gain: 0.085, timbre: .glass),
        ]
    case .toggleOff:
        return [
            CueStep(degree: 2, delay: 0, seconds: 0.05, gain: 0.075, timbre: .glass),
            CueStep(degree: 0, delay: 0.045, seconds: 0.07, gain: 0.08, timbre: .mallet),
        ]
    case .select:
        return [CueStep(degree: -1, delay: 0, seconds: 0.035, gain: 0.05, timbre: .wood)]
    case .navigate:
        return [CueStep(degree: 3, delay: 0, seconds: 0.07, gain: 0.09, timbre: .glass)]
    case .open:
        return [
            CueStep(degree: 0, delay: 0, seconds: 0.06, gain: 0.07, timbre: .glass),
            CueStep(degree: 2, delay: 0.04, seconds: 0.06, gain: 0.075, timbre: .glass),
            CueStep(degree: 4, delay: 0.08, seconds: 0.13, gain: 0.085, timbre: .bell),
        ]
    case .close:
        return [
            CueStep(degree: 4, delay: 0, seconds: 0.05, gain: 0.075, timbre: .glass),
            CueStep(degree: 2, delay: 0.04, seconds: 0.05, gain: 0.07, timbre: .glass),
            CueStep(degree: 0, delay: 0.08, seconds: 0.11, gain: 0.07, timbre: .mallet),
        ]
    case .send:
        return [
            CueStep(degree: 0, delay: 0, seconds: 0.05, gain: 0.085, timbre: .mallet),
            CueStep(degree: 3, delay: 0.05, seconds: 0.05, gain: 0.09, timbre: .glass),
            CueStep(degree: 5, delay: 0.1, seconds: 0.16, gain: 0.095, timbre: .bell),
        ]
    case .arrive:
        return [
            CueStep(degree: 5, delay: 0, seconds: 0.06, gain: 0.085, timbre: .bell),
            CueStep(degree: 2, delay: 0.06, seconds: 0.18, gain: 0.09, timbre: .bell),
        ]
    case .success:
        return [
            CueStep(degree: 0, delay: 0, seconds: 0.09, gain: 0.1, timbre: .bell),
            CueStep(degree: 3, delay: 0.09, seconds: 0.09, gain: 0.105, timbre: .bell),
            CueStep(degree: 5, delay: 0.18, seconds: 0.45, gain: 0.115, timbre: .bell),
            CueStep(degree: 10, delay: 0.18, seconds: 0.45, gain: 0.06, timbre: .bell),
        ]
    case .warn:
        return [
            CueStep(degree: -2, delay: 0, seconds: 0.3, gain: 0.1, timbre: .pad),
            CueStep(degree: -2, delay: 0.16, seconds: 0.26, gain: 0.075, timbre: .pad),
        ]
    case .error:
        return [
            CueStep(degree: -1, delay: 0, seconds: 0.13, gain: 0.13, timbre: .wood),
            CueStep(degree: -4, delay: 0.11, seconds: 0.42, gain: 0.13, timbre: .pad),
            CueStep(degree: -6, delay: 0.11, seconds: 0.42, gain: 0.08, timbre: .pad),
        ]
    case .delete:
        return [
            CueStep(degree: 2, delay: 0, seconds: 0.05, gain: 0.09, timbre: .wood),
            CueStep(degree: -3, delay: 0.055, seconds: 0.2, gain: 0.1, timbre: .wood),
        ]
    }
}

public func cueNotes(_ cue: InterfaceCue, start: Double, volume: Double = 1, seed: Int = 0) -> [Note] {
    let voice = interfaceVoice()
    let level = min(max(volume, 0), 1)
    return cueSteps(cue).compactMap { (step: CueStep) -> Note? in
        let note = Note(
            start: start + step.delay, frequency: voice.frequency(step.degree), seconds: step.seconds,
            gain: step.gain * level, pan: voice.pan, timbre: step.timbre, instrument: voice.instrument,
            bloom: 0, seed: seed)
        return note.isRest ? nil : note
    }
}

public func keystrokeSeed(_ strike: Int) -> Int {
    Int(UInt32(truncatingIfNeeded: strike) &* 2_654_435_761 | 1)
}

public func renderCue(_ cue: InterfaceCue, volume: Double = 1, seed: Int = 0, rate: Int = kWatchSampleRate) -> [Int16] {
    renderNotes(cueNotes(cue, start: 0, volume: volume, seed: seed), from: 0, rate: rate, echo: false)
}

public func wavData(_ pcm: [Int16], rate: Int = kWatchSampleRate, channels: Int = 2) -> Data {
    var data = Data(capacity: 44 + pcm.count * 2)
    func word(_ value: UInt32) { withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) } }
    func half(_ value: UInt16) { withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) } }
    let bytes = UInt32(pcm.count * 2)
    data.append(contentsOf: Array("RIFF".utf8))
    word(36 + bytes)
    data.append(contentsOf: Array("WAVEfmt ".utf8))
    word(16)
    half(1)
    half(UInt16(channels))
    word(UInt32(rate))
    word(UInt32(rate * channels * 2))
    half(UInt16(channels * 2))
    half(16)
    data.append(contentsOf: Array("data".utf8))
    word(bytes)
    for sample in pcm { half(UInt16(bitPattern: sample)) }
    return data
}

public let kRatchetBatchSeconds = 1.0 / 60
public let kRatchetMaxTeeth = 8
public let kRatchetVariants = 2
public let kRatchetToothSeconds = 0.008
public let kRatchetCatchSeconds = 0.06

public enum RatchetPawl: Int, CaseIterable, Sendable {
    case soft
    case crisp

    var partials: [(hz: Double, gain: Double)] {
        switch self {
        case .soft: return [(1180, 0.07), (2710, 0.026)]
        case .crisp: return [(1640, 0.085), (3290, 0.038), (5150, 0.012)]
        }
    }

    var snap: Double { self == .soft ? 0.03 : 0.05 }
    var ring: Double { self == .soft ? 0.0021 : 0.0018 }
}

public struct RatchetBurst: Hashable, Sendable {
    public let teeth: Int
    public let pawl: RatchetPawl

    public init(teeth: Int, pawl: RatchetPawl) {
        self.teeth = min(max(teeth, 1), kRatchetMaxTeeth)
        self.pawl = pawl
    }

    public static var all: [RatchetBurst] {
        RatchetPawl.allCases.flatMap { (pawl: RatchetPawl) in
            (1...kRatchetMaxTeeth).map { RatchetBurst(teeth: $0, pawl: pawl) }
        }
    }

    public var spacing: Double { kRatchetBatchSeconds / Double(teeth) }

    public func render(seed: Int = 0, rate: Int = kWatchSampleRate) -> [Int16] {
        let seconds = Double(teeth - 1) * spacing + kRatchetToothSeconds
        var mono = [Double](repeating: 0, count: Int((seconds * Double(rate)).rounded(.up)))
        for tooth in 0..<teeth {
            let start = Int((Double(tooth) * spacing * Double(rate)).rounded())
            ratchetTooth(&mono, at: start, pawl: pawl, seed: seed &+ tooth &* 7919, rate: rate)
        }
        return ratchetStereo(mono)
    }
}

public func renderRatchetCatch(seed: Int = 0, rate: Int = kWatchSampleRate) -> [Int16] {
    var mono = [Double](repeating: 0, count: Int((kRatchetCatchSeconds * Double(rate)).rounded(.up)))
    for i in mono.indices {
        let t = Double(i) / Double(rate)
        mono[i] = 0.12 * sin(2 * .pi * 380 * t) * exp(-t / 0.012) + 0.05 * sin(2 * .pi * 1140 * t) * exp(-t / 0.005)
    }
    ratchetTooth(&mono, at: 0, pawl: .crisp, seed: seed, rate: rate)
    return ratchetStereo(mono)
}

public struct RatchetStroke: Equatable, Sendable {
    public let burst: RatchetBurst
    public let caught: Bool

    public init(burst: RatchetBurst, caught: Bool) {
        self.burst = burst
        self.caught = caught
    }
}

public struct RatchetGate: Sendable {
    private var open = false
    private var lines = 0
    private var changed = 0
    private var flagged = 0

    public init() {}

    public var isOpen: Bool { open }

    public mutating func pass(lines: Int, changed: Int, flagged: Int) -> RatchetStroke? {
        guard lines > 0 else { return nil }
        if open {
            self.lines += lines
            self.changed += changed
            self.flagged += flagged
            return nil
        }
        open = true
        return RatchetGate.stroke(lines, changed, flagged)
    }

    public mutating func close() -> RatchetStroke? {
        let stroke = RatchetGate.stroke(lines, changed, flagged)
        lines = 0
        changed = 0
        flagged = 0
        open = stroke != nil
        return stroke
    }

    private static func stroke(_ lines: Int, _ changed: Int, _ flagged: Int) -> RatchetStroke? {
        guard lines > 0 else { return nil }
        let pawl: RatchetPawl = changed > 0 || flagged > 0 ? .crisp : .soft
        return RatchetStroke(burst: RatchetBurst(teeth: lines, pawl: pawl), caught: flagged > 0)
    }
}

private struct RatchetNoise {
    private var state: UInt32

    init(seed: Int) {
        let mixed = UInt32(truncatingIfNeeded: seed) ^ 0x9E37_79B9
        state = mixed == 0 ? 1 : mixed
    }

    mutating func next() -> Double {
        state ^= state << 13
        state ^= state >> 17
        state ^= state << 5
        return Double(state) / Double(UInt32.max)
    }
}

private func ratchetTooth(_ mono: inout [Double], at start: Int, pawl: RatchetPawl, seed: Int, rate: Int) {
    var noise = RatchetNoise(seed: seed)
    let detune = 1 + (noise.next() - 0.5) * 0.06
    let level = 0.9 + noise.next() * 0.2
    let length = min(mono.count - start, Int(kRatchetToothSeconds * Double(rate)))
    guard start >= 0, length > 0 else { return }
    let full = Int(kRatchetToothSeconds * Double(rate))
    let release = max(full / 4, 1)
    var phases = [Double](repeating: 0, count: pawl.partials.count)
    var hiss = 0.0
    for i in 0..<length {
        let t = Double(i) / Double(rate)
        let glide = 1 + 0.12 * exp(-t / 0.0007)
        hiss += ((noise.next() * 2 - 1) - hiss) * 0.35
        var x = pawl.snap * hiss * exp(-t / 0.0004)
        for (k, partial) in pawl.partials.enumerated() {
            phases[k] += 2 * .pi * partial.hz * detune * glide / Double(rate)
            x += partial.gain * sin(phases[k]) * exp(-t / pawl.ring)
        }
        let attack = min(1, t / 0.00018)
        let fade = min(1, Double(full - i) / Double(release))
        mono[start + i] += x * level * attack * fade
    }
}

private func ratchetStereo(_ mono: [Double]) -> [Int16] {
    var out = [Int16](repeating: 0, count: mono.count * 2)
    for (i, x) in mono.enumerated() {
        let sample = Int16(clamping: Int((min(max(x, -1.5), 1.5) * 30000).rounded()))
        out[i * 2] = sample
        out[i * 2 + 1] = sample
    }
    return out
}

public struct InterfaceCueGate: Sendable {
    private var lastMs: Int64?
    private var last: InterfaceCue = .select

    public init() {}

    public mutating func admit(_ cue: InterfaceCue, atMs: Int64) -> Bool {
        if let lastMs, atMs - lastMs < kCueThrottleMs, cue.weight <= last.weight { return false }
        lastMs = atMs
        last = cue
        return true
    }
}

public struct InterfaceCueBatch: Sendable {
    public private(set) var held: InterfaceCue?

    public init() {}

    public mutating func hold(_ cue: InterfaceCue) -> Bool {
        guard let current = held else {
            held = cue
            return true
        }
        if cue.weight > current.weight { held = cue }
        return false
    }

    public mutating func take() -> InterfaceCue? {
        defer { held = nil }
        return held
    }
}

public let kCueFallbackDelayMs: Int64 = 80
public let kCueSlideSettleMs: Int64 = 450

public struct InterfaceScreenShape: Equatable, Sendable {
    public var presented: Int
    public var pushed: Int

    public init(presented: Int, pushed: Int) {
        self.presented = presented
        self.pushed = pushed
    }
}

public struct InterfaceCueFallback: Sendable {
    private var lastMs: Int64 = .min

    public init() {}

    public mutating func heard(atMs: Int64) {
        lastMs = atMs
    }

    public func silent(sinceMs: Int64) -> Bool { lastMs < sinceMs }
}

extension InterfaceCue {
    public static func afterTouch(from before: InterfaceScreenShape, to after: InterfaceScreenShape, tapped: Bool) -> InterfaceCue? {
        if after.presented > before.presented { return .open }
        if after.presented < before.presented { return .close }
        if after.pushed > before.pushed { return .navigate }
        if after.pushed < before.pushed { return .close }
        return tapped ? .click : nil
    }
}
