// The desktop's synthesiser, desktop/src/AgentSonifier.cpp `sampleNote` and
// `Mixer::render`, sample for sample: the same partials, the same shaping and
// the same ping-pong echo, at the same rate. Pure arithmetic, so it renders on
// the phone, the watch and Linux alike; the golden tests compare its PCM with
// recorded output.

import FMBase
import Foundation

/// The rate sounds are rendered at: the desktop's `kSampleRate`. The marimba's
/// ninth partial on a high voice reaches past 10 kHz, which a lower rate
/// would fold back down as a wrong note.
public let kWatchSampleRate: Int = 44100

private let tableSize = 4096

/// One sine table, linearly interpolated, as the desktop's `osc`. Stored as
/// 32-bit floats.
private let sineTable: [Float] = (0...tableSize).map { i in
    Float(sin(2 * Double.pi * Double(i) / Double(tableSize)))
}

/// `phase` in cycles: one whole number is one period.
@inline(__always)
private func osc(_ phase: Double) -> Double {
    let wrapped = phase - phase.rounded(.down)
    let x = wrapped * Double(tableSize)
    let i = Int(x)
    let f = x - Double(i)
    let a = Double(sineTable[i])
    return a + (Double(sineTable[i + 1]) - a) * f
}

/// How long a timbre rings past its held length (`releaseSeconds`).
public func releaseSeconds(_ timbre: Timbre) -> Double {
    switch timbre {
    case .bell: return 1.6
    case .pad: return 0.45
    case .glass: return 0.3
    case .wood: return 0.12
    case .chirp: return 0.3
    case .thud: return 0.1
    case .trill: return 0.25
    case .buzz: return 0.15
    case .swell: return 0.6
    case .ping: return 0.2
    case .twang: return 0.45
    case .mallet: return 0.25
    }
}

/// How long a note sounds in all: held, then released.
public func noteLife(_ note: Note) -> Double { note.seconds + releaseSeconds(note.timbre) }

private struct InstrumentSpec {
    /// (ratio, amplitude, decay per second) for each partial.
    let partials: [(Double, Double, Double)]
    let attackSeconds: Double
    var vibratoHz: Double = 0
    var vibratoDepth: Double = 0
}

private let instruments: [InstrumentSpec] = [
    // Marimba
    InstrumentSpec(partials: [(1, 1.0, 5.5), (3.9, 0.30, 13), (9.2, 0.10, 22)], attackSeconds: 0.003),
    // Celesta
    InstrumentSpec(partials: [(1, 1.0, 3.2), (3.0, 0.40, 5.5), (5.0, 0.15, 8)], attackSeconds: 0.002),
    // Pluck
    InstrumentSpec(
        partials: [(1, 1.0, 4.5), (2, 0.55, 7), (3, 0.30, 9), (4, 0.18, 12), (5, 0.10, 15)],
        attackSeconds: 0.0015),
    // Kalimba
    InstrumentSpec(partials: [(1, 1.0, 4.0), (2, 0.22, 7), (3.1, 0.12, 9)], attackSeconds: 0.004),
    // Chime
    InstrumentSpec(
        partials: [(1, 1.0, 1.7), (2.01, 0.50, 2.6), (2.76, 0.28, 4.0), (5.41, 0.14, 6.5)],
        attackSeconds: 0.002),
    // Reed
    InstrumentSpec(
        partials: [(1, 1.0, 1.0), (2, 0.28, 1.4), (3, 0.14, 1.8), (4, 0.07, 2.2)],
        attackSeconds: 0.050, vibratoHz: 5.2, vibratoDepth: 0.004),
]

/// How the work shapes whichever instrument plays it (`Shape`).
private struct Shape {
    var decayScale: Double
    var attackScale: Double
    var brightness: Double
    var bendDepth: Double = 0
    var bendRate: Double = 22
    var trillDepth: Double = 0
    var trillHz: Double = 0
    var tremoloDepth: Double = 0
    var tremoloHz: Double = 0
    var swellSeconds: Double = 0
}

private func shapeFor(_ timbre: Timbre) -> Shape {
    switch timbre {
    case .bell: return Shape(decayScale: 0.5, attackScale: 1.0, brightness: 1.0)
    case .glass: return Shape(decayScale: 1.1, attackScale: 0.7, brightness: 1.7)
    case .wood: return Shape(decayScale: 2.4, attackScale: 0.6, brightness: 0.8, bendDepth: 0.35, bendRate: 22)
    case .pad: return Shape(decayScale: 0.35, attackScale: 12.0, brightness: 0.6)
    case .chirp: return Shape(decayScale: 1.0, attackScale: 1.0, brightness: 1.3, bendDepth: -0.45, bendRate: 16)
    case .thud: return Shape(decayScale: 3.2, attackScale: 0.5, brightness: 0.45, bendDepth: 0.9, bendRate: 30)
    case .trill: return Shape(decayScale: 0.8, attackScale: 1.0, brightness: 1.1, trillDepth: 0.06, trillHz: 13.0)
    case .buzz:
        return Shape(
            decayScale: 1.4, attackScale: 0.6, brightness: 1.9, bendDepth: 0.08, bendRate: 10, tremoloDepth: 0.55,
            tremoloHz: 31.0)
    case .swell: return Shape(decayScale: 0.2, attackScale: 1.0, brightness: 0.9, swellSeconds: 0.35)
    case .ping: return Shape(decayScale: 1.9, attackScale: 0.5, brightness: 0.25)
    case .twang:
        return Shape(
            decayScale: 0.9, attackScale: 0.8, brightness: 1.5, bendDepth: 0.12, bendRate: 5, trillDepth: 0.012,
            trillHz: 5.5)
    case .mallet: return Shape(decayScale: 1.0, attackScale: 1.0, brightness: 1.0)
    }
}

/// One note, gain and pan applied, added into `left`/`right` from frame
/// `offset` (`sampleNote` summed as `Mixer::render` sums it).
///
/// The desktop works every exponential out per sample; here each decaying
/// term is stepped by a constant factor, which is the same curve to within
/// rounding and several times cheaper for a phone or a watch.
private func addNote(_ note: Note, _ rate: Int, _ left: inout [Double], _ right: inout [Double], _ offset: Int) {
    let spec = instruments[note.instrument.rawValue]
    var shape = shapeFor(note.timbre)
    let bloom = min(max(note.bloom, 0.0), 1.0)
    var stretch = 0.0
    if note.seed != 0 {
        shape.brightness *= 0.78 + 0.44 * seedUnit(note.seed, 0)
        shape.decayScale *= 0.8 + 0.4 * seedUnit(note.seed, 1)
        shape.bendDepth += (seedUnit(note.seed, 2) - 0.5) * 0.08
        stretch = (seedUnit(note.seed, 3) - 0.5) * 0.03
    }

    // Every term of the sum: a ratio of the phase, an amplitude and a decay.
    var ratios: [Double] = []
    var amps: [Double] = []
    var decays: [Double] = []
    var norm = 0.0
    for (i, partial) in spec.partials.enumerated() {
        let (ratio, amp, decay) = partial
        let a = amp * (i == 0 ? 1.0 : pow(shape.brightness, Double(i)))
        ratios.append(ratio * (1.0 + stretch * Double(i)))
        amps.append(a)
        decays.append(decay * shape.decayScale * (1.0 - 0.25 * bloom))
        norm += a
    }
    if bloom > 0 {
        // An octave and a twelfth that only a long-running agent has.
        let extra = 0.16 * bloom
        ratios.append(contentsOf: [2, 3])
        amps.append(contentsOf: [extra, extra * 0.5])
        decays.append(contentsOf: [3.0 * shape.decayScale, 4.0 * shape.decayScale])
        norm += extra * 1.5
    }
    // The low register carried by its octave and twelfth, so earbuds that
    // cannot reproduce 110-150 Hz still hear the note.
    let low = min(max((260.0 - note.frequency) / 130.0, 0.0), 1.0)
    if low > 0 {
        ratios.append(contentsOf: [2, 3])
        amps.append(contentsOf: [0.55 * low, 0.30 * low])
        decays.append(contentsOf: [2.6 * shape.decayScale, 3.4 * shape.decayScale])
        norm += 0.5 * low
    }
    let scale = 1 / max(1.0, 0.55 * norm) * 0.75 * note.gain
    let terms = ratios.count
    var level = amps
    var step = [Double](repeating: 0, count: terms)
    for k in 0..<terms {
        step[k] = exp(-decays[k] / Double(rate))
    }

    let angle = note.pan * Double.pi / 2
    let gl = cos(angle) * scale
    let gr = sin(angle) * scale
    let life = noteLife(note)
    let release = releaseSeconds(note.timbre)
    let attackSeconds = spec.attackSeconds * shape.attackScale
    let frames = min(Int((life * Double(rate)).rounded(.up)), left.count - offset)
    if frames <= 0 { return }
    let f = note.frequency
    let bend = shape.bendDepth
    let bendRate = shape.bendRate
    let bendStep = exp(-bendRate / Double(rate))
    var bendDecay = 1.0
    let trill = shape.trillDepth > 0 ? shape.trillDepth * f / (2.0 * Double.pi * shape.trillHz) : 0.0
    let rateD = Double(rate)
    for i in 0..<frames {
        let rel = Double(i) / rateD
        var phase = rel * f
        if bend != 0 {
            phase = f * (rel + bend * (1.0 - bendDecay) / bendRate)
            bendDecay *= bendStep
        }
        if spec.vibratoDepth > 0 {
            phase += spec.vibratoDepth * osc(rel * spec.vibratoHz)
        }
        if trill != 0 { phase += trill * osc(rel * shape.trillHz) }

        var v = 0.0
        for k in 0..<terms {
            v += level[k] * osc(phase * ratios[k])
            level[k] *= step[k]
        }

        var env = rel < attackSeconds ? rel / attackSeconds : 1.0
        if shape.swellSeconds > 0 {
            let rise = min(1.0, rel / shape.swellSeconds)
            env *= rise * rise
        }
        if shape.tremoloDepth > 0 {
            env *= 1.0 - shape.tremoloDepth * (0.5 + 0.5 * osc(rel * shape.tremoloHz))
        }
        if rel > note.seconds {
            env *= max(0.0, 1.0 - (rel - note.seconds) / release)
        }
        let s = v * env
        left[offset + i] += s * gl
        right[offset + i] += s * gr
    }
}

/// The echo the desktop mixes every note through (`Mixer::render`): a
/// dotted-note ping-pong, 0.45 s, fed back a third. What makes a handful of
/// events a room rather than a row of beeps.
private let echoSeconds = 0.45
private let echoFeedback = 0.34
private let echoWet = 0.26

/// How long the echo is let ring after the last note: four trips round, by
/// which it is under a thousandth of the note that fed it.
private let echoTail = echoSeconds * 4

@inline(__always)
private func softClip(_ x: Double) -> Double {
    if x > -0.7 && x < 0.7 { return x }
    let e = exp(2 * x)
    return (e - 1) / (e + 1)
}

/// Renders `notes` to interleaved 16-bit stereo PCM (left, right, left, ...),
/// starting at `from` on the notes' own clock (so a note due half a second
/// from now starts half a second into the buffer), through the desktop's echo.
/// Rests are skipped; with nothing to sound the buffer is empty.
///
/// This is arithmetic on every sample (a cadence rings for four seconds):
/// call it off the main thread.
public func renderNotes(_ notes: [Note], from: Double, rate: Int = kWatchSampleRate, echo: Bool = true) -> [Int16] {
    let sounding = notes.filter { !$0.isRest }
    if sounding.isEmpty { return [] }
    var end = from
    for note in sounding {
        end = max(end, note.start + noteLife(note))
    }
    if echo { end += echoTail }
    let frames = Int(((end - from) * Double(rate)).rounded(.up))
    var left = [Double](repeating: 0, count: frames)
    var right = [Double](repeating: 0, count: frames)
    for note in sounding {
        let offset = max(0, Int(((note.start - from) * Double(rate)).rounded()))
        if offset < frames { addNote(note, rate, &left, &right, offset) }
    }
    var out = [Int16](repeating: 0, count: frames * 2)
    let echoFrames = Int(echoSeconds * Double(rate))
    var echoLeft = echo ? [Double](repeating: 0, count: echoFrames) : []
    var echoRight = echo ? [Double](repeating: 0, count: echoFrames) : []
    var e = 0
    for i in 0..<frames {
        var l = left[i]
        var r = right[i]
        if echo {
            let wetL = echoLeft[e]
            let wetR = echoRight[e]
            echoLeft[e] = l + wetR * echoFeedback
            echoRight[e] = r + wetL * echoFeedback
            e = (e + 1) % echoFrames
            l += wetL * echoWet
            r += wetR * echoWet
        }
        out[i * 2] = Int16(truncatingIfNeeded: Int(softClip(l) * 30000))
        out[i * 2 + 1] = Int16(truncatingIfNeeded: Int(softClip(r) * 30000))
    }
    return out
}
