import Foundation

struct PokerNoise {
    private var state: UInt64

    init(seed: UInt64) {
        state = seed == 0 ? 0x9E37_79B9_7F4A_7C15 : seed
    }

    mutating func next() -> Float {
        state ^= state << 13
        state ^= state >> 7
        state ^= state << 17
        return Float(Double(state >> 11) / Double(1 << 53)) * 2 - 1
    }

    mutating func jitter(_ range: Double) -> Double {
        Double(next()) * range
    }
}

struct PokerBiquad {
    private var b0: Float = 1
    private var b1: Float = 0
    private var b2: Float = 0
    private var a1: Float = 0
    private var a2: Float = 0
    private var z1: Float = 0
    private var z2: Float = 0

    static func bandPass(_ frequency: Double, q: Double, rate: Double) -> PokerBiquad {
        var filter = PokerBiquad()
        filter.setBandPass(frequency, q: q, rate: rate)
        return filter
    }

    static func lowPass(_ frequency: Double, q: Double, rate: Double) -> PokerBiquad {
        let w0 = 2 * Double.pi * min(frequency, rate * 0.45) / rate
        let alpha = sin(w0) / (2 * q)
        let a0 = 1 + alpha
        var filter = PokerBiquad()
        filter.b0 = Float((1 - cos(w0)) / 2 / a0)
        filter.b1 = Float((1 - cos(w0)) / a0)
        filter.b2 = filter.b0
        filter.a1 = Float(-2 * cos(w0) / a0)
        filter.a2 = Float((1 - alpha) / a0)
        return filter
    }

    mutating func setBandPass(_ frequency: Double, q: Double, rate: Double) {
        let w0 = 2 * Double.pi * min(frequency, rate * 0.45) / rate
        let alpha = sin(w0) / (2 * q)
        let a0 = 1 + alpha
        b0 = Float(alpha / a0)
        b1 = 0
        b2 = Float(-alpha / a0)
        a1 = Float(-2 * cos(w0) / a0)
        a2 = Float((1 - alpha) / a0)
    }

    mutating func process(_ x: Float) -> Float {
        let y = b0 * x + z1
        z1 = b1 * x - a1 * y + z2
        z2 = b2 * x - a2 * y
        return y
    }
}

public enum PokerSynth {
    public static let sampleRate = 44100.0

    public static func peak(_ cue: PokerCue) -> Float {
        switch cue {
        case .shuffle: return 0.55
        case .deal: return 0.5
        case .chip: return 0.55
        case .check: return 0.7
        case .fold: return 0.4
        case .yourTurn: return 0.32
        case .clock: return 0.28
        case .allIn: return 0.6
        case .win: return 0.55
        }
    }

    public static func render(_ cue: PokerCue, rate: Double = sampleRate, seed: UInt64 = 0x51F1_5EED) -> [Float] {
        var noise = PokerNoise(seed: seed &+ UInt64(cue.rawValue.count) &* 0x9E37_79B9)
        var out: [Float]
        switch cue {
        case .shuffle: out = shuffle(rate, &noise)
        case .deal: out = deal(rate, &noise)
        case .chip: out = chips(3, spacing: 0.045, start: 0, rate, &noise)
        case .check: out = check(rate, &noise)
        case .fold: out = fold(rate, &noise)
        case .yourTurn: out = yourTurn(rate)
        case .clock: out = clock(rate)
        case .allIn: out = allIn(rate, &noise)
        case .win: out = win(rate, &noise)
        }
        normalize(&out, to: peak(cue))
        return out
    }

    static func buffer(_ seconds: Double, _ rate: Double) -> [Float] {
        [Float](repeating: 0, count: max(1, Int(seconds * rate)))
    }

    static func normalize(_ samples: inout [Float], to target: Float) {
        let top = samples.reduce(Float(0)) { max($0, abs($1)) }
        guard top > 0 else { return }
        let gain = target / top
        for index in samples.indices { samples[index] *= gain }
    }

    static func mix(_ source: [Float], into target: inout [Float], at start: Int, gain: Float = 1) {
        guard start < target.count else { return }
        for index in source.indices where start + index >= 0 && start + index < target.count {
            target[start + index] += source[index] * gain
        }
    }

    static func shuffle(_ rate: Double, _ noise: inout PokerNoise) -> [Float] {
        var out = buffer(0.36, rate)
        let tickLength = Int(0.012 * rate)
        for tick in 0..<10 {
            var filter = PokerBiquad.bandPass(3000, q: 1.4, rate: rate)
            let at = Double(tick) * 0.032 + (tick == 0 ? 0 : noise.jitter(0.006))
            let start = Int(max(0, at) * rate)
            let loudness = Float(0.35 + 0.65 * sin(Double.pi * (Double(tick) + 0.5) / 10))
            var burst = [Float](repeating: 0, count: tickLength)
            for index in 0..<tickLength {
                let t = Double(index) / rate
                let envelope = Float(min(1, t / 0.0008) * exp(-t / 0.0035))
                burst[index] = filter.process(noise.next()) * envelope * loudness
            }
            mix(burst, into: &out, at: start)
        }
        return out
    }

    static func deal(_ rate: Double, _ noise: inout PokerNoise) -> [Float] {
        var out = buffer(0.045, rate)
        var filter = PokerBiquad.bandPass(5000, q: 1.1, rate: rate)
        for index in out.indices {
            let t = Double(index) / rate
            if index % 16 == 0 {
                let sweep = 5000 * pow(1500.0 / 5000.0, min(1, t / 0.045))
                filter.setBandPass(sweep, q: 1.1, rate: rate)
            }
            let envelope = Float(min(1, t / 0.001) * exp(-t / 0.009))
            out[index] = filter.process(noise.next()) * envelope
        }
        return out
    }

    static func clack(_ rate: Double, _ noise: inout PokerNoise, gain: Float) -> [Float] {
        var out = buffer(0.03, rate)
        let tickLength = Int(0.005 * rate)
        for index in out.indices {
            let t = Double(index) / rate
            let envelope = exp(-t / 0.005)
            var value = Float((0.6 * sin(2 * Double.pi * 3200 * t) + 0.4 * sin(2 * Double.pi * 5100 * t)) * envelope)
            if index < tickLength {
                value += noise.next() * Float(1 - Double(index) / Double(tickLength)) * 0.45
            }
            out[index] = value * gain
        }
        return out
    }

    static func chips(_ count: Int, spacing: Double, start: Double, _ rate: Double, _ noise: inout PokerNoise, fade: Bool = false) -> [Float] {
        var out = buffer(start + Double(count - 1) * spacing + 0.035, rate)
        for index in 0..<count {
            let gain: Float = fade ? Float(1 - Double(index) / Double(count + 1)) : Float(0.85 + 0.15 * Double(noise.next()))
            let jitter = index == 0 ? 0 : noise.jitter(0.004)
            mix(clack(rate, &noise, gain: gain), into: &out, at: Int((start + Double(index) * spacing + jitter) * rate))
        }
        return out
    }

    static func check(_ rate: Double, _ noise: inout PokerNoise) -> [Float] {
        var out = buffer(0.19, rate)
        for knock in 0..<2 {
            var filter = PokerBiquad.lowPass(900, q: 0.7, rate: rate)
            var hit = buffer(0.07, rate)
            for index in hit.indices {
                let t = Double(index) / rate
                let envelope = min(1, t / 0.0015) * exp(-t / 0.012)
                let tone = sin(2 * Double.pi * 150 * t)
                hit[index] = Float(tone * envelope) + filter.process(noise.next()) * Float(envelope * 0.35)
            }
            mix(hit, into: &out, at: Int(Double(knock) * 0.12 * rate), gain: knock == 0 ? 1 : 0.9)
        }
        return out
    }

    static func fold(_ rate: Double, _ noise: inout PokerNoise) -> [Float] {
        var out = buffer(0.18, rate)
        var filter = PokerBiquad.lowPass(1200, q: 0.8, rate: rate)
        let length = Double(out.count)
        for index in out.indices {
            let envelope = Float(pow(sin(Double.pi * Double(index) / length), 1.6))
            out[index] = filter.process(noise.next()) * envelope
        }
        return out
    }

    static func tone(_ frequency: Double, seconds: Double, rate: Double, attack: Double = 0.01, release: Double = 0.05) -> [Float] {
        var out = buffer(seconds, rate)
        for index in out.indices {
            let t = Double(index) / rate
            let envelope = min(1, t / attack) * min(1, max(0, seconds - t) / release)
            out[index] = Float(sin(2 * Double.pi * frequency * t) * envelope)
        }
        return out
    }

    static func yourTurn(_ rate: Double) -> [Float] {
        var out = buffer(0.28, rate)
        mix(tone(880, seconds: 0.14, rate: rate), into: &out, at: 0)
        mix(tone(1320, seconds: 0.14, rate: rate), into: &out, at: Int(0.14 * rate), gain: 0.85)
        return out
    }

    static func clock(_ rate: Double) -> [Float] {
        var out = buffer(0.02, rate)
        for index in out.indices {
            let t = Double(index) / rate
            out[index] = Float(sin(2 * Double.pi * 1000 * t) * min(1, t / 0.0008) * exp(-t / 0.006))
        }
        return out
    }

    static func allIn(_ rate: Double, _ noise: inout PokerNoise) -> [Float] {
        var out = buffer(0.42, rate)
        var phase = 0.0
        let seconds = 0.4
        for index in 0..<min(out.count, Int(seconds * rate)) {
            let t = Double(index) / rate
            let frequency = 110 * pow(2, t / seconds)
            phase += 2 * Double.pi * frequency / rate
            let envelope = min(1, t / 0.02) * min(1, max(0, seconds - t) / 0.08)
            out[index] = Float(sin(phase) * envelope * 0.9)
        }
        mix(chips(7, spacing: 0.045, start: 0.05, rate, &noise, fade: true), into: &out, at: 0, gain: 0.45)
        return out
    }

    static func triangle(_ frequency: Double, seconds: Double, rate: Double) -> [Float] {
        var out = buffer(seconds, rate)
        for index in out.indices {
            let t = Double(index) / rate
            let cycle = (t * frequency).truncatingRemainder(dividingBy: 1)
            let wave = 4 * abs(cycle - 0.5) - 1
            let envelope = min(1, t / 0.006) * exp(-t / 0.09)
            out[index] = Float(wave * envelope)
        }
        return out
    }

    static func win(_ rate: Double, _ noise: inout PokerNoise) -> [Float] {
        var out = buffer(0.44 + 0.4 + 0.05, rate)
        let notes = [523.25, 659.25, 783.99, 1046.5]
        for (index, note) in notes.enumerated() {
            mix(triangle(note, seconds: 0.11 + (index == notes.count - 1 ? 0.12 : 0.02), rate: rate), into: &out, at: Int(Double(index) * 0.11 * rate))
        }
        mix(chips(9, spacing: 0.04, start: 0.44, rate, &noise, fade: true), into: &out, at: 0, gain: 0.5)
        return out
    }
}
