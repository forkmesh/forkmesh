// A port of the desktop's desktop/src/AgentSonifier.{h,cpp}: the same scale,
// the same grid, the same instruments and the same note for the same kind of
// work, so an agent heard on the watch sounds as it does on the phone and the desktop.
//
// Hashes are 32-bit and wrap exactly as the desktop's quint32 does; the golden
// tests in FMAgentWatchTests hold the port to recorded output.

import FMBase
import Foundation

/// The grid every voice lands on: eighth notes at 100 BPM (`kSlotSeconds`).
public let kSlotSeconds: Double = 0.3

/// A note wanted while the grid is already this far ahead is dropped instead
/// of queued (`kMaxLeadSeconds`).
public let kMaxLeadSeconds: Double = 1.8

/// D3, the tonic every voice is tuned from (`kTonicHz`).
public let kTonicHz: Double = 146.832

/// D major pentatonic in semitones: no interval in it can clash with another,
/// which is what lets several agents play at once without anyone arranging it.
public let kScale: [Int] = [0, 2, 4, 7, 9]

/// The most text an event is weighed by (`kPayloadCap`).
public let kPayloadCap: Int = 20000

/// What an agent is doing shapes the note: how long it is held, how bright it
/// is, how hard it is struck. The same gesture whoever plays it.
public enum Timbre: Int, CaseIterable, Sendable {
    case mallet  // short wooden strike: the agent saying something
    case bell  // ringing partials, slow decay: arrivals and the done cadence
    case glass  // thin and bright: reading, searching
    case wood  // dry, low, quick: shell commands
    case pad  // soft and slow: thinking
    case chirp  // swoops up into the note: something launched, something new
    case thud  // a low drum, bent hard down: a build or an install grinding
    case trill  // two pitches shaken fast: tests running, a retry
    case buzz  // rough, bright, trembling: an error, a refusal
    case swell  // breathed in backwards, rising: waiting, compacting
    case ping  // near-pure, short and high: a listing, a seal
    case twang  // a string sagging under the finger: git, a skill, a hook
}

/// The instrument an agent plays: which agent it is, not what it is doing.
public enum Instrument: Int, CaseIterable, Sendable {
    case marimba, celesta, pluck, kalimba, chime, reed
}

/// Colour family per instrument (the desktop's `kInstrumentHues`), so an
/// agent is drawn in the colour of what it sounds like.
private let instrumentHues: [Double] = [0.08, 0.55, 0.92, 0.33, 0.72, 0.17]

/// 32-bit FNV-1a over UTF-16 code units. The desktop hashes titles with Qt's
/// per-process seeded qHash, so its voices change between launches; this one
/// does not.
public func fnv1a(_ text: String, _ start: UInt32 = 0x811c_9dc5) -> Int {
    var hash = start
    for unit in text.utf16 {
        hash ^= UInt32(unit)
        hash = hash &* 0x0100_0193
    }
    return Int(hash)
}

/// A 32-bit multiply, wrapped as C++'s quint32 would.
@inline(__always)
func mul32(_ a: UInt32, _ b: UInt32) -> UInt32 { a &* b }

/// How one agent sounds and where it sits (`Voice`). Derived from its number
/// and title, so the same agent sounds the same every time it is watched.
public struct Voice: Equatable, Sendable {
    public var index: Int
    public var instrument: Instrument

    /// Registers above the tonic, 0..2.
    public var octave: Int

    /// The scale degree this voice calls home.
    public var degreeOffset: Int

    /// Stereo position, 0 hard left .. 1 hard right. Mutable: a newcomer is
    /// seated on the emptier side of the room.
    public var pan: Double

    /// 0 on the beat, 0.5 between them.
    public var slotOffset: Double

    /// A few cents off centre, so unisons shimmer.
    public var detune: Double

    /// Scale degrees added by the turns taken.
    public var drift: Int = 0

    /// 0..1 from the tokens burned: fuller partials, a longer ring.
    public var bloom: Double = 0

    public init(
        index: Int = 0,
        instrument: Instrument = .marimba,
        octave: Int = 0,
        degreeOffset: Int = 0,
        pan: Double = 0.5,
        slotOffset: Double = 0,
        detune: Double = 1.0
    ) {
        self.index = index
        self.instrument = instrument
        self.octave = octave
        self.degreeOffset = degreeOffset
        self.pan = pan
        self.slotOffset = slotOffset
        self.detune = detune
    }

    /// `voiceForSession`: `salt` re-rolls it, which is how a clash between two
    /// agents heard together is settled.
    public static func forAgent(_ id: Int, _ title: String, salt: Int = 0) -> Voice {
        var hash = UInt32(fnv1a(dartTrim(title).lowercased()))
        hash ^= mul32(UInt32(truncatingIfNeeded: id), 2_654_435_761)
        hash ^= mul32(UInt32(truncatingIfNeeded: salt), 0x9e37_79b9)
        hash ^= hash >> 15
        hash = mul32(hash, 0x2545_f491)
        hash ^= hash >> 13
        // Each trait from its own stir of the hash, so consecutive session
        // numbers do not come out alike.
        func draw(_ sides: UInt32) -> Int {
            hash ^= hash << 13
            hash ^= hash >> 17
            hash ^= hash << 5
            return Int(hash % sides)
        }
        let pans: [Double] = [0.5, 0.26, 0.74, 0.14, 0.86]
        let cents: [Double] = [0, 5, -5, 9, -9]
        let instrument = Instrument(rawValue: draw(6))!
        let octave = draw(3)
        let pan = pans[draw(5)]
        let degree = draw(5)
        let slotOffset = draw(2) != 0 ? 0.5 : 0.0
        let detune = pow(2.0, cents[draw(5)] / 1200.0)
        return Voice(
            index: id, instrument: instrument, octave: octave, degreeOffset: degree, pan: pan,
            slotOffset: slotOffset, detune: detune)
    }

    public var home: Int { degreeOffset + drift }

    /// The agent's colour, 0..1 around the wheel (`hueForVoice`).
    public var hue: Double {
        let h = instrumentHues[instrument.rawValue] + Double(degreeOffset - 2) * 0.018
        return h - h.rounded(.down)
    }

    /// How crisp its haptics are: the bright instruments tap, the soft ones
    /// thud. 0..1, as Core Haptics' sharpness.
    public var sharpness: Double {
        switch instrument {
        case .celesta: return 0.85
        case .chime: return 0.7
        case .pluck: return 0.6
        case .marimba: return 0.45
        case .kalimba: return 0.3
        case .reed: return 0.2
        }
    }

    /// `applyUsage`: every six turns the line steps up the scale, capped at an
    /// octave; tokens open the instrument out, log scaled.
    public mutating func applyUsage(turns: Int, tokens: Int) {
        drift = min(max(max(0, turns) / 6, 0), 5)
        let burned = Double(max(0, tokens))
        bloom = min(max(dartLog1p(burned) / dartLog1p(400_000.0), 0.0), 1.0)
    }

    /// "celesta, middle register, hard left" (`voiceDescription`).
    public var description: String {
        let names = ["marimba", "celesta", "plucked string", "kalimba", "chime", "reed"]
        let where_: String
        if pan < 0.2 {
            where_ = "hard left"
        } else if pan < 0.45 {
            where_ = "left"
        } else if pan > 0.8 {
            where_ = "hard right"
        } else if pan > 0.55 {
            where_ = "right"
        } else {
            where_ = "centre"
        }
        let register = octave == 0 ? "low" : octave == 1 ? "middle" : "high"
        return "\(names[instrument.rawValue]), \(register) register, \(where_)"
    }

    /// Scale degree (may be negative, and wraps octaves every five steps) to
    /// Hz (`degreeFrequency`).
    public func frequency(_ degree: Int) -> Double {
        let d = degree + home
        // Floored, so the scale keeps walking below its own tonic.
        let octaves = Int((Double(d) / 5).rounded(.down))
        let step = d - octaves * 5
        let semitones = 12 * (octave + octaves) + kScale[step]
        return kTonicHz * pow(2.0, Double(semitones) / 12.0) * detune
    }
}

/// The one voice for what belongs to no agent (`desktopVoice`): a low chime
/// dead centre, under the agents' registers.
public func desktopVoice() -> Voice {
    Voice(index: -2, instrument: .chime, octave: 1, pan: 0.5, slotOffset: 0.25)
}

/// `log(1 + x)`, not the libm `log1p`.
@inline(__always)
func dartLog1p(_ x: Double) -> Double { log(1 + x) }

/// One sounded note, placed on the watcher's clock (`Note`).
public struct Note: Equatable, Sendable {
    /// On the watcher's clock, in seconds.
    public var start: Double
    public var frequency: Double

    /// How long it is held before its release.
    public var seconds: Double
    public var gain: Double
    public var pan: Double
    public var timbre: Timbre
    public var instrument: Instrument
    public var bloom: Double

    /// What the event was, hashed: it shades the note. 0 is the plain
    /// instrument.
    public var seed: Int

    public init(
        start: Double,
        frequency: Double,
        seconds: Double = 0.3,
        gain: Double = 0.2,
        pan: Double = 0.5,
        timbre: Timbre = .mallet,
        instrument: Instrument = .marimba,
        bloom: Double = 0,
        seed: Int = 0
    ) {
        self.start = start
        self.frequency = frequency
        self.seconds = seconds
        self.gain = gain
        self.pan = pan
        self.timbre = timbre
        self.instrument = instrument
        self.bloom = bloom
        self.seed = seed
    }

    public var isRest: Bool { frequency <= 0 || seconds <= 0 || gain <= 0 }

    static let rest = Note(start: 0, frequency: 0)

    func with(seed: Int) -> Note {
        var copy = self
        copy.seed = seed
        return copy
    }
}

/// 0..1 from byte `index` (0-3) of a seed: one independent knob per use.
public func seedUnit(_ seed: Int, _ index: Int) -> Double {
    Double((seed >> (8 * (index & 3))) & 0xff) / 255.0
}

/// A voice's running position in the scale, carried between events
/// (`Melody`).
public struct Melody: Equatable, Sendable {
    public var degree = 0
    public var lastSlot: Double = -1
    public var lastGrace: Double = -1
    public var count = 0
    public var graceCount = 0
    public var graceRun = 0

    public init() {}
}

/// A stable little number for one note of one voice (`stir`).
public func stir(_ count: Int, _ bytes: Int) -> Int {
    var h = mul32(UInt32(truncatingIfNeeded: count), 2_654_435_761)
        ^ mul32(UInt32(truncatingIfNeeded: bytes), 0x9e37_79b9)
    h ^= h >> 15
    h = mul32(h, 0x2545_f491)
    h ^= h >> 13
    return Int(h)
}

/// How far the melody moves, and which way, for one kind of work
/// (`stepForMotion`).
public func stepForMotion(_ motion: AgentMotion, _ bytes: Int, _ count: Int = 0) -> Int {
    let h = stir(count, bytes)
    func pick(_ steps: [Int]) -> Int { steps[h % steps.count] }
    switch motion {
    case .speak: return pick([1, -1, 2, -1, 1, -2, 3, -2])
    case .think: return pick([-1, 1, -2, -1, 2, -1])
    case .read: return pick([1, 2, 1, 3])
    case .search: return pick([2, 3, 2, 4])
    case .web: return pick([4, 3, 5, 3])
    case .plan: return pick([2, -1, 1, -1])
    case .write: return pick([2, 1, 3, 2])
    case .run: return pick([1, 1, 2, 3])
    case .delegate: return pick([3, 4, 2])
    case .arrive: return (bytes > 8000 ? -3 : bytes > 800 ? -2 : -1) - h % 2
    case .notice: return pick([0, 1, -1])
    case .scan: return pick([1, 2, 1, 2])
    case .create: return pick([3, 2, 4, 2])
    case .test: return pick([1, -1, 2, -1])
    case .build: return pick([-1, 1, -2, 1])
    case .git: return pick([2, -2, 3, -1])
    case .install: return pick([1, 1, 1, 2])
    case .spawn: return pick([4, 5, 3])
    case .wait: return pick([0, 1, 0, -1])
    case .ask: return pick([3, 4])
    case .skill: return pick([3, 2, 4])
    case .mcp: return pick([2, 3, 5])
    case .todo: return pick([1, -1, 2, -1])
    case .fumble: return pick([-2, -3])
    case .denied: return pick([-3, -4])
    case .image: return pick([-1, -2, 2])
    case .compact: return -4 - h % 2
    case .retry: return pick([0, 1, 0])
    default: return 0
    }
}

/// `timbreForMotion`.
public func timbreForMotion(_ motion: AgentMotion) -> Timbre {
    switch motion {
    case .think:
        return .pad
    case .read, .search, .web, .notice, .compose, .tick, .network, .mcp, .image, .logSync, .logInfo:
        return .glass
    case .run, .frame, .close, .task, .logTask:
        return .wood
    case .usage, .plan:
        return .mallet
    case .system, .write, .delegate, .prompt, .start, .finish, .ask, .logAccount:
        return .bell
    case .scan, .seal, .logCloud:
        return .ping
    case .create, .spawn, .connect:
        return .chirp
    case .test, .retry:
        return .trill
    case .build, .install:
        return .thud
    case .git, .skill, .hook, .logGit:
        return .twang
    case .wait, .compact, .logStall:
        return .swell
    case .fumble, .denied, .fail, .logError:
        return .buzz
    case .todo, .speak, .arrive:
        return .mallet
    }
}

/// `baseGainForMotion`.
public func baseGainForMotion(_ motion: AgentMotion) -> Double {
    switch motion {
    case .think: return 0.10
    case .arrive: return 0.15
    case .read, .search: return 0.16
    case .web: return 0.17
    case .plan: return 0.20
    case .speak: return 0.19
    case .run: return 0.18
    case .notice: return 0.20
    case .write, .delegate: return 0.23
    case .start: return 0.26
    case .finish: return 0.27
    case .prompt: return 0.28
    case .wait: return 0.12
    case .scan: return 0.15
    case .install, .todo, .image: return 0.18
    case .mcp, .retry: return 0.18
    case .test, .spawn, .fumble: return 0.20
    case .build, .git, .skill: return 0.21
    case .denied, .compact: return 0.22
    case .create: return 0.23
    case .fail: return 0.25
    case .ask: return 0.27
    case .frame, .compose, .usage, .system, .tick, .close, .seal, .hook, .network: return 0.05
    case .connect, .task, .logError, .logStall, .logSync, .logGit, .logCloud, .logTask, .logAccount,
        .logInfo:
        return 0.06
    }
}

/// `secondsForTimbre`.
public func secondsForTimbre(_ timbre: Timbre) -> Double {
    switch timbre {
    case .bell: return 0.9
    case .pad: return 0.8
    case .glass: return 0.4
    case .wood: return 0.18
    case .chirp: return 0.3
    case .thud: return 0.16
    case .trill: return 0.45
    case .buzz: return 0.3
    case .swell: return 0.7
    case .ping: return 0.12
    case .twang: return 0.5
    case .mallet: return 0.34
    }
}

@inline(__always)
private func clamp01(_ x: Double) -> Double { min(max(x, 0.0), 1.0) }

/// A grace note: the quiet one between the beats (`graceNote`). Bookkeeping
/// sounds as one, and so does work the melody has no slot left for.
public func graceNote(
    _ motion: AgentMotion, _ bytes: Int, _ voice: Voice, _ melody: inout Melody, _ now: Double
) -> Note {
    let graceSeconds = kSlotSeconds / 4
    var grace = (now / graceSeconds - 1e-9).rounded(.up)
    if grace <= melody.lastGrace { grace = melody.lastGrace + 1 }
    let start = grace * graceSeconds
    if start - now > kMaxLeadSeconds { return .rest }
    melody.graceRun = grace <= melody.lastGrace + 1.5 ? melody.graceRun + 1 : 0
    melody.lastGrace = grace
    melody.graceCount += 1
    let h = stir(melody.graceCount, bytes)

    let tick = motion.isFaint
    var seconds = tick ? 0.04 + 0.03 * Double(h % 3) / 2.0 : 0.1
    let shimmer = [5, 7, 4, 8, 6, 9, 3]
    var offset = tick ? shimmer[h % 7] : stepForMotion(motion, bytes, melody.count)
    var gainScale = 1.0
    switch motion {
    case .frame:
        offset = 7
        seconds = 0.03
        gainScale = 0.8
    case .close:
        offset = 5
        seconds = 0.03
        gainScale = 0.75
    case .seal:
        offset = 9 + h % 2
        seconds = 0.025
        gainScale = 0.9
    case .hook:
        offset = 2 + h % 3
        seconds = 0.1
    case .compose:
        offset = 3 + melody.graceRun % 5
        seconds = 0.035
    case .usage:
        offset = -3 - h % 2
        seconds = 0.07
        gainScale = 1.1
    case .system:
        offset = 4 + (h % 2) * 5
        seconds = 0.12
    default:
        break
    }
    let weight = dartLog1p(Double(bytes)) / dartLog1p(Double(kPayloadCap))
    let thinning = max(0.3, 1.0 / (1.0 + 0.3 * Double(melody.graceRun)))
    let gain = tick
        ? (0.035 + 0.02 * weight) * gainScale
        : baseGainForMotion(motion) * (0.3 + 0.12 * weight)
    return Note(
        start: start,
        frequency: voice.frequency(melody.degree + offset),
        seconds: seconds,
        gain: gain * thinning,
        pan: clamp01(voice.pan + (Double(h % 5) - 2.0) * 0.03),
        timbre: timbreForMotion(motion),
        instrument: voice.instrument,
        bloom: voice.bloom)
}

/// The next note for this voice, quantised onto the shared grid
/// (`noteForEvent`). Work takes the next slot of the melody; bookkeeping, and
/// work arriving faster than the melody can take it, becomes a grace note.
public func noteForBeat(
    _ motion: AgentMotion, _ voice: Voice, _ melody: inout Melody, _ now: Double, bytes: Int = 0, seed: Int = 0
) -> Note {
    if motion.isBookkeeping {
        return graceNote(motion, bytes, voice, &melody, now).with(seed: seed)
    }
    var slot = (now / kSlotSeconds - 1e-9).rounded(.up)
    let minSlot = melody.lastSlot + 1
    if slot < minSlot { slot = minSlot }
    let start = (slot + voice.slotOffset) * kSlotSeconds
    if start - now > kMaxLeadSeconds {
        return graceNote(motion, bytes, voice, &melody, now).with(seed: seed)
    }

    melody.count += 1
    let h = stir(melody.count, bytes)
    var degree = melody.degree
    let root = motion == .start || motion == .prompt || motion == .finish || motion == .fail
    if root {
        degree = motion == .fail ? -2 : 0
    } else {
        degree += stepForMotion(motion, bytes, melody.count + seed % 11)
    }
    if degree > 9 {
        degree -= 5
    } else if degree < -2 {
        degree += 5
    }
    // The same pitch twice running is where a line starts to sound like an
    // alarm: step off it.
    if !root && degree == melody.degree && melody.lastSlot >= 0 {
        degree += degree >= 9 ? -1 : degree <= -2 ? 1 : (h & 1) != 0 ? 1 : -1
    }
    let timbre = timbreForMotion(motion)
    var frequency = voice.frequency(degree)
    if timbre == .thud { frequency *= 0.5 }
    let weight = dartLog1p(Double(bytes)) / dartLog1p(Double(kPayloadCap))
    let touch = 0.88 + 0.24 * Double(h % 101) / 100.0
    melody.degree = degree
    melody.lastSlot = slot
    return Note(
        start: start,
        frequency: frequency,
        seconds: secondsForTimbre(timbre) * (0.85 + 0.3 * seedUnit(seed, 1)),
        gain: baseGainForMotion(motion) * (0.72 + 0.38 * weight) * touch,
        pan: voice.pan,
        timbre: timbre,
        instrument: voice.instrument,
        bloom: voice.bloom,
        seed: seed)
}

/// The figure that says a run is over (`doneCadence`): a rising arpeggio
/// resolving onto the root for a clean finish, a falling one sinking onto it
/// for a failure or a stop.
public func doneCadence(_ voice: Voice, _ success: Bool, _ start: Double) -> [Note] {
    let step = success ? 0.18 : 0.22
    let rising = [0, 2, 3, 5]
    let falling = [4, 3, 1, 0]
    let chordUp = [5, 3, 0]
    let chordDown = [0, -2]
    var notes: [Note] = []
    for i in 0..<4 {
        notes.append(Note(
            start: start + Double(i) * step,
            frequency: voice.frequency(success ? rising[i] : falling[i]),
            seconds: 0.5,
            gain: success ? 0.30 : 0.24,
            pan: voice.pan,
            timbre: .bell,
            instrument: voice.instrument,
            bloom: voice.bloom))
    }
    for degree in success ? chordUp : chordDown {
        notes.append(Note(
            start: start + 4 * step,
            frequency: voice.frequency(degree),
            seconds: 1.8,
            gain: success ? 0.26 : 0.22,
            pan: voice.pan,
            timbre: .bell,
            instrument: voice.instrument,
            bloom: voice.bloom))
    }
    return notes
}

/// The two notes a voice announces its seat with when it is first heard
/// (`AgentSonifier::listen`): the root, then a step up.
public func announce(_ voice: Voice, _ melody: inout Melody, _ now: Double) -> [Note] {
    let slot = (now / kSlotSeconds).rounded(.up) + 1
    melody.lastSlot = slot + 1
    return (0..<2).map { i in
        Note(
            start: (slot + Double(i) + voice.slotOffset) * kSlotSeconds,
            frequency: voice.frequency(i == 0 ? 0 : 3),
            seconds: 0.45,
            gain: 0.22,
            pan: voice.pan,
            timbre: .bell,
            instrument: voice.instrument)
    }
}
