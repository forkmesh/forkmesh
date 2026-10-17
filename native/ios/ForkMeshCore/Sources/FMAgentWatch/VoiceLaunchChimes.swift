import Foundation

public enum VoiceLaunchChime: String, CaseIterable, Sendable {
    case summon
    case launch
    case fizzle

    public static let summonSeconds: Double = 0.42

    public var notes: [Note] {
        func note(_ at: Double, _ degree: Int, _ seconds: Double, _ gain: Double, _ timbre: Timbre, pan: Double = 0.5,
                  instrument: Instrument = .celesta, octave: Int = 0) -> Note {
            Note(start: at, frequency: GenieChime.pitch(degree, octave: octave), seconds: seconds, gain: gain, pan: pan,
                 timbre: timbre, instrument: instrument)
        }
        switch self {
        case .summon:
            var out: [Note] = [note(0, -5, 0.4, 0.05, .swell, instrument: .reed)]
            out += (0..<6).map { (step: Int) -> Note in
                note(0.04 + Double(step) * 0.04, step, 0.1, 0.05 + Double(step) * 0.006, .glass,
                     pan: 0.85 - Double(step) * 0.07)
            }
            out.append(note(0.28, 7, 0.55, 0.09, .bell, instrument: .chime))
            out.append(note(0.28, 2, 0.55, 0.05, .bell, instrument: .chime))
            out.append(note(0.3, 5, 0.6, 0.04, .pad))
            return out
        case .launch:
            var out: [Note] = [note(0, -7, 0.32, 0.06, .swell, instrument: .reed)]
            out += (0..<8).map { (step: Int) -> Note in
                note(0.1 + Double(step) * 0.028, step * 2 - 3, 0.07, 0.05 + Double(step) * 0.004, .chirp,
                     pan: 0.15 + Double(step) * 0.1)
            }
            out.append(note(0.36, 12, 0.7, 0.1, .bell))
            out.append(note(0.36, 7, 0.7, 0.06, .bell, instrument: .chime))
            out.append(note(0.42, 15, 0.5, 0.05, .ping, pan: 0.8))
            out.append(note(0.5, 17, 0.4, 0.04, .ping, pan: 0.9))
            return out
        case .fizzle:
            var out: [Note] = (0..<4).map { (step: Int) -> Note in
                note(Double(step) * 0.06, 4 - step * 2, 0.1, 0.06 - Double(step) * 0.008, .glass, pan: 0.7 - Double(step) * 0.1)
            }
            out.append(note(0.24, -3, 0.35, 0.05, .pad))
            return out
        }
    }

    public func render(rate: Int = kWatchSampleRate) -> [Int16] {
        renderNotes(notes, from: 0, rate: rate, echo: self != .summon)
    }
}
