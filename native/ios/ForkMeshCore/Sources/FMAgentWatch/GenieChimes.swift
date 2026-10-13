import Foundation

public enum GenieChime: String, CaseIterable, Sendable {
    case appear
    case vanish
    case listen
    case spotted
    case whoosh
    case reply
    case giggle
    case dizzy
    case wakeOn = "wake-on"
    case wakeOff = "wake-off"
    case boing
    case tiptoe, patter, tick, creak, hush
    case scuttle, skitter, trot, clatter, scamper
    case zip, swish, zoom, streak, rush
    case climb, lift, soar, float, hop
    case slide, tumble, drop, plunk, swoop
    case yip, swerve, wobble, twirl, flip

    public static let tonicHz: Double = 523.25

    public static func pitch(_ degree: Int, octave: Int = 0) -> Double {
        let count = kScale.count
        let steps = degree >= 0 ? degree : degree - (count - 1)
        let shift = steps / count + octave
        let index = ((degree % count) + count) % count
        return tonicHz * pow(2, Double(12 * shift + kScale[index]) / 12)
    }

    public var notes: [Note] {
        func note(_ at: Double, _ degree: Int, _ seconds: Double, _ gain: Double, _ timbre: Timbre, pan: Double = 0.5,
                  instrument: Instrument = .celesta, octave: Int = 0) -> Note {
            Note(start: at, frequency: GenieChime.pitch(degree, octave: octave), seconds: seconds, gain: gain, pan: pan,
                 timbre: timbre, instrument: instrument)
        }
        switch self {
        case .appear:
            var out = (0..<7).map { (step: Int) -> Note in
                note(Double(step) * 0.045, step - 2, 0.12, 0.06 + Double(step) * 0.006, .glass, pan: 0.2 + Double(step) * 0.1)
            }
            out.append(note(0.33, 5, 0.6, 0.1, .bell, octave: 0))
            out.append(note(0.33, 10, 0.6, 0.05, .bell))
            return out
        case .vanish:
            var out = (0..<6).map { (step: Int) -> Note in
                note(Double(step) * 0.05, 6 - step, 0.1, 0.075 - Double(step) * 0.006, .glass, pan: 0.8 - Double(step) * 0.1)
            }
            out.append(note(0.3, -3, 0.35, 0.06, .pad))
            return out
        case .listen:
            return [note(0, 0, 0.07, 0.07, .chirp), note(0.07, 3, 0.12, 0.08, .chirp)]
        case .spotted:
            return [
                note(0, 7, 0.06, 0.06, .ping), note(0.05, 9, 0.06, 0.065, .ping), note(0.1, 11, 0.08, 0.07, .ping),
                note(0.16, 12, 0.5, 0.09, .bell, instrument: .chime), note(0.16, 7, 0.5, 0.05, .bell, instrument: .chime),
            ]
        case .whoosh:
            var out = [note(0, -5, 0.35, 0.05, .swell, instrument: .reed)]
            out += (0..<5).map { (step: Int) -> Note in
                note(0.18 + Double(step) * 0.03, step * 2, 0.08, 0.06, .chirp, pan: 0.3 + Double(step) * 0.12)
            }
            out.append(note(0.36, 10, 0.4, 0.08, .bell))
            return out
        case .reply:
            return [note(0, 5, 0.04, 0.07, .ping), note(0.05, 2, 0.14, 0.07, .mallet, instrument: .kalimba)]
        case .giggle:
            return (0..<6).map { (step: Int) -> Note in
                note(Double(step) * 0.075, step % 2 == 0 ? 4 : 2, 0.06, 0.07 - Double(step) * 0.006, .mallet,
                     pan: step % 2 == 0 ? 0.35 : 0.65, instrument: .kalimba)
            }
        case .dizzy:
            return (0..<8).map { (step: Int) -> Note in
                let degree = [3, 1, 4, 0, 3, -1, 2, -2][step]
                return note(Double(step) * 0.07, degree, 0.1, 0.06, .twang, pan: 0.5 + 0.4 * sin(Double(step) * 1.4),
                            instrument: .pluck)
            }
        case .wakeOn:
            return [note(0, 0, 0.08, 0.07, .glass), note(0.08, 2, 0.08, 0.075, .glass), note(0.16, 4, 0.4, 0.09, .bell)]
        case .wakeOff:
            return [note(0, 4, 0.08, 0.07, .glass), note(0.08, 2, 0.08, 0.065, .glass), note(0.16, 0, 0.3, 0.06, .mallet)]
        case .boing:
            return [note(0, -3, 0.18, 0.08, .twang, instrument: .pluck), note(0.12, 2, 0.12, 0.05, .chirp)]
        case .tiptoe:
            return [note(0, 0, 0.05, 0.045, .mallet, pan: 0.4, instrument: .kalimba), note(0.07, 2, 0.05, 0.04, .mallet, pan: 0.6, instrument: .kalimba)]
        case .patter:
            return (0..<3).map { (step: Int) -> Note in
                note(Double(step) * 0.05, [1, 0, 1][step], 0.04, 0.04, .wood, pan: 0.35 + Double(step) * 0.15, instrument: .marimba)
            }
        case .tick:
            return [note(0, 7, 0.03, 0.035, .ping), note(0.06, 6, 0.03, 0.03, .ping)]
        case .creak:
            return [note(0, -3, 0.18, 0.04, .twang, instrument: .pluck), note(0.09, -2, 0.12, 0.03, .twang, instrument: .pluck)]
        case .hush:
            return [note(0, 2, 0.22, 0.035, .pad, instrument: .reed), note(0.05, 4, 0.18, 0.025, .glass)]
        case .scuttle:
            return (0..<4).map { (step: Int) -> Note in
                note(Double(step) * 0.035, step % 2 == 0 ? 2 : 4, 0.035, 0.05, .wood, pan: step % 2 == 0 ? 0.4 : 0.6, instrument: .marimba)
            }
        case .skitter:
            return (0..<5).map { (step: Int) -> Note in
                note(Double(step) * 0.028, 5 + step % 2, 0.03, 0.045, .ping, pan: 0.3 + Double(step) * 0.1)
            }
        case .trot:
            return [
                note(0, 0, 0.06, 0.055, .mallet, instrument: .marimba), note(0.08, 4, 0.06, 0.05, .mallet, instrument: .marimba),
                note(0.16, 2, 0.08, 0.045, .mallet, instrument: .marimba),
            ]
        case .clatter:
            return (0..<4).map { (step: Int) -> Note in
                note(Double(step) * 0.04, [3, 1, 3, 0][step], 0.04, 0.05, .wood, pan: 0.5, instrument: .kalimba)
            }
        case .scamper:
            return (0..<4).map { (step: Int) -> Note in
                note(Double(step) * 0.03, step + 1, 0.04, 0.05, .chirp, pan: 0.65 - Double(step) * 0.1, instrument: .pluck)
            }
        case .zip:
            return [note(0, 3, 0.12, 0.05, .chirp, pan: 0.2, instrument: .reed), note(0.06, 8, 0.1, 0.05, .chirp, pan: 0.8)]
        case .swish:
            return [note(0, -2, 0.22, 0.045, .swell, instrument: .reed), note(0.12, 5, 0.12, 0.04, .glass)]
        case .zoom:
            return (0..<6).map { (step: Int) -> Note in
                note(Double(step) * 0.022, step * 2 - 1, 0.04, 0.04, .chirp, pan: 0.1 + Double(step) * 0.16)
            }
        case .streak:
            return [
                note(0, 9, 0.05, 0.045, .ping, pan: 0.8), note(0.04, 7, 0.05, 0.045, .ping, pan: 0.6),
                note(0.08, 4, 0.18, 0.05, .mallet, pan: 0.4, instrument: .chime),
            ]
        case .rush:
            return [note(0, -5, 0.28, 0.05, .swell, instrument: .reed), note(0.16, 0, 0.12, 0.045, .twang, instrument: .pluck)]
        case .climb:
            return (0..<4).map { (step: Int) -> Note in
                note(Double(step) * 0.05, step, 0.05, 0.045 + Double(step) * 0.004, .mallet, instrument: .kalimba)
            }
        case .lift:
            return [note(0, 2, 0.1, 0.045, .chirp), note(0.08, 5, 0.16, 0.05, .glass, instrument: .chime)]
        case .soar:
            return [note(0, 0, 0.3, 0.04, .swell, instrument: .reed), note(0.15, 7, 0.18, 0.045, .glass)]
        case .float:
            return [note(0, 4, 0.25, 0.035, .pad), note(0.1, 6, 0.2, 0.035, .pad, instrument: .chime)]
        case .hop:
            return [note(0, 1, 0.05, 0.05, .twang, instrument: .pluck), note(0.07, 6, 0.08, 0.05, .chirp, instrument: .kalimba)]
        case .slide:
            return (0..<3).map { (step: Int) -> Note in
                note(Double(step) * 0.06, 6 - step * 3, step == 2 ? 0.14 : 0.08, 0.045, .glass, pan: 0.7 - Double(step) * 0.2)
            }
        case .tumble:
            return (0..<5).map { (step: Int) -> Note in
                note(Double(step) * 0.04, 4 - step, 0.05, 0.05, .wood, pan: step % 2 == 0 ? 0.3 : 0.7, instrument: .marimba)
            }
        case .drop:
            return [note(0, 3, 0.04, 0.04, .ping), note(0.05, -4, 0.16, 0.055, .thud, instrument: .marimba)]
        case .plunk:
            return [note(0, -1, 0.12, 0.055, .twang, instrument: .pluck), note(0.1, -3, 0.12, 0.04, .mallet, instrument: .kalimba)]
        case .swoop:
            return [note(0, 8, 0.12, 0.04, .swell, instrument: .reed), note(0.12, 2, 0.12, 0.045, .mallet)]
        case .yip:
            return [note(0, 5, 0.04, 0.05, .chirp, instrument: .kalimba), note(0.05, 7, 0.05, 0.05, .ping)]
        case .swerve:
            return [note(0, 2, 0.1, 0.045, .twang, pan: 0.2, instrument: .pluck), note(0.08, 4, 0.1, 0.045, .twang, pan: 0.8, instrument: .pluck)]
        case .wobble:
            return (0..<4).map { (step: Int) -> Note in
                note(Double(step) * 0.045, [2, 3, 1, 2][step], 0.05, 0.045, .trill)
            }
        case .twirl:
            return (0..<5).map { (step: Int) -> Note in
                note(Double(step) * 0.03, [3, 5, 7, 5, 3][step], 0.04, 0.045, .glass, pan: 0.5 + 0.4 * sin(Double(step) * 1.3))
            }
        case .flip:
            return [note(0, -2, 0.06, 0.05, .mallet, instrument: .marimba), note(0.06, 5, 0.1, 0.05, .ping, instrument: .chime)]
        }
    }

    public var move: GenieMoveKind? {
        GenieMoveKind.allCases.first { $0.chimes.contains(self) }
    }

    public func render(rate: Int = kWatchSampleRate) -> [Int16] {
        renderNotes(notes, from: 0, rate: rate, echo: move == nil)
    }
}

public enum GenieMoveKind: String, CaseIterable, Sendable {
    case creep, scuttle, dash, rise, fall, swerve

    public var chimes: [GenieChime] {
        switch self {
        case .creep: return [.tiptoe, .patter, .tick, .creak, .hush]
        case .scuttle: return [.scuttle, .skitter, .trot, .clatter, .scamper]
        case .dash: return [.zip, .swish, .zoom, .streak, .rush]
        case .rise: return [.climb, .lift, .soar, .float, .hop]
        case .fall: return [.slide, .tumble, .drop, .plunk, .swoop]
        case .swerve: return [.yip, .swerve, .wobble, .twirl, .flip]
        }
    }

    public static let dashSpeed: Double = 1400
    public static let creepSpeed: Double = 380
    public static let steep: Double = 0.7
    public static let sharpTurn: Double = Double.pi * 0.6

    public static func read(dx: Double, dy: Double, seconds: Double, turn: Double) -> GenieMoveKind {
        let distance = hypot(dx, dy)
        let speed = distance / max(seconds, 0.001)
        if abs(turn) >= sharpTurn { return .swerve }
        if speed >= dashSpeed { return .dash }
        if -dy >= distance * steep { return .rise }
        if dy >= distance * steep { return .fall }
        return speed < creepSpeed ? .creep : .scuttle
    }
}

public struct GenieMoveVoice: Sendable {
    public static let stride: Double = 56
    public static let gap: Double = 0.14

    private var mark: (x: Double, y: Double, t: Double)?
    private var heading: Double?
    private var steps: [GenieMoveKind: Int] = [:]

    public init() {}

    public mutating func begin(x: Double, y: Double, at t: Double) {
        mark = (x, y, t)
        heading = nil
    }

    public mutating func move(x: Double, y: Double, at t: Double) -> GenieChime? {
        guard let from = mark else {
            begin(x: x, y: y, at: t)
            return nil
        }
        let dx = x - from.x
        let dy = y - from.y
        guard hypot(dx, dy) >= GenieMoveVoice.stride, t - from.t >= GenieMoveVoice.gap else { return nil }
        let now = atan2(dy, dx)
        var turn = 0.0
        if let before = heading {
            turn = now - before
            while turn > Double.pi { turn -= 2 * Double.pi }
            while turn < -Double.pi { turn += 2 * Double.pi }
        }
        heading = now
        mark = (x, y, t)
        let kind = GenieMoveKind.read(dx: dx, dy: dy, seconds: t - from.t, turn: turn)
        let step = steps[kind] ?? 0
        steps[kind] = step + 1
        let options = kind.chimes
        return options[step % options.count]
    }
}
