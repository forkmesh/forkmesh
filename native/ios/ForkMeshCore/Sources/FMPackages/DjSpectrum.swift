import Foundation

public enum DjSpectrum {
    public static let bandCount = 16
    public static let windowSize = 1024
    public static let lowest = 45.0
    public static let highest = 9_000.0
    public static let floorDb = -58.0
    public static let ceilingDb = -8.0
    public static let tiltPerOctave = 3.0

    private static let hann: [Double] = (0..<windowSize).map { 0.5 - 0.5 * cos(2 * Double.pi * Double($0) / Double(windowSize - 1)) }
    private static let twiddles: [(Double, Double)] = (0..<windowSize / 2).map {
        let angle = -2 * Double.pi * Double($0) / Double(windowSize)
        return (cos(angle), sin(angle))
    }
    private static let reversed: [Int] = {
        let bits = Int(log2(Double(windowSize)))
        return (0..<windowSize).map { index in
            var value = index
            var result = 0
            for _ in 0..<bits {
                result = (result << 1) | (value & 1)
                value >>= 1
            }
            return result
        }
    }()

    public static func edges(count: Int = bandCount, rate: Double) -> [Int] {
        guard count > 0, rate > 0 else { return [] }
        let width = rate / Double(windowSize)
        let top = min(highest, rate / 2 * 0.95)
        return (0...count).map { index in
            let frequency = lowest * pow(top / lowest, Double(index) / Double(count))
            return min(windowSize / 2, max(1, Int((frequency / width).rounded())))
        }
    }

    public static func bands(_ audio: DjScratchAudio, at time: Double, count: Int = bandCount) -> [Double] {
        let silent = [Double](repeating: 0, count: max(0, count))
        guard count > 0, audio.rate > 0, time.isFinite, audio.samples.count >= windowSize else { return silent }
        let centre = Int(time * audio.rate)
        guard centre >= 0, centre < audio.samples.count else { return silent }
        let start = min(audio.samples.count - windowSize, max(0, centre - windowSize / 2))
        var real = [Double](repeating: 0, count: windowSize)
        var imaginary = [Double](repeating: 0, count: windowSize)
        audio.samples.withUnsafeBufferPointer { pcm in
            for index in 0..<windowSize {
                real[reversed[index]] = Double(pcm[start + index]) / 32767 * hann[index]
            }
        }
        transform(&real, &imaginary)
        let edges = edges(count: count, rate: audio.rate)
        let width = audio.rate / Double(windowSize)
        let full = Double(windowSize) / 4
        return (0..<count).map { band in
            let low = edges[band]
            let high = max(low + 1, edges[band + 1])
            var peak = 0.0
            for bin in low..<min(windowSize / 2, high) {
                peak = max(peak, (real[bin] * real[bin] + imaginary[bin] * imaginary[bin]).squareRoot())
            }
            let frequency = Double(low + high) / 2 * width
            let db = 20 * log10(max(1e-9, peak / full)) + tiltPerOctave * log2(max(1, frequency) / 1000)
            return min(1, max(0, (db - floorDb) / (ceilingDb - floorDb)))
        }
    }

    public static func bands(_ bars: [DjWaveBar], time: Double, duration: Double, count: Int = bandCount) -> [Double] {
        guard count > 0 else { return [] }
        return (0..<count).map { band in
            let place = Double(band) / Double(max(1, count - 1))
            let bar = DjStudio.amplitude(bars, time: time - place * 0.18, duration: duration)
            let centre = 0.15 + bar.hue * 1.2
            let shape = 0.35 + 0.65 * exp(-pow(place - centre, 2) / 0.09)
            return min(1, max(0, bar.level * shape * (1 - 0.35 * place)))
        }
    }

    private static func transform(_ real: inout [Double], _ imaginary: inout [Double]) {
        var span = 2
        while span <= windowSize {
            let half = span / 2
            let step = windowSize / span
            var block = 0
            while block < windowSize {
                for offset in 0..<half {
                    let (cosine, sine) = twiddles[offset * step]
                    let top = block + offset
                    let bottom = top + half
                    let tr = real[bottom] * cosine - imaginary[bottom] * sine
                    let ti = real[bottom] * sine + imaginary[bottom] * cosine
                    real[bottom] = real[top] - tr
                    imaginary[bottom] = imaginary[top] - ti
                    real[top] += tr
                    imaginary[top] += ti
                }
                block += span
            }
            span *= 2
        }
    }
}

public struct DjSpectrumMeter: Sendable {
    public private(set) var values: [Double]
    public private(set) var peaks: [Double]
    public private(set) var bass: Double = 0
    public private(set) var energy: Double = 0
    public private(set) var kick: Double = 0
    public private(set) var phase: Double = 0
    private var lastBass: Double = 0
    private var stamp: Double?

    public init(count: Int = DjSpectrum.bandCount) {
        values = [Double](repeating: 0, count: max(0, count))
        peaks = values
    }

    public mutating func update(_ target: [Double], at now: Double) {
        guard target.count == values.count, now.isFinite else { return }
        let dt = stamp.map { min(0.25, max(0, now - $0)) } ?? 0
        stamp = now
        let rise = 1 - exp(-dt * 38)
        let fall = 1 - exp(-dt * 7)
        for index in values.indices {
            let goal = target[index].isFinite ? min(1, max(0, target[index])) : 0
            values[index] += (goal - values[index]) * (goal > values[index] ? rise : fall)
            peaks[index] = max(values[index], peaks[index] - dt * 0.7)
        }
        let low = Array(values.prefix(max(1, values.count / 4)))
        let nextBass = low.isEmpty ? 0 : low.reduce(0, +) / Double(low.count)
        if nextBass - lastBass > 0.06 * max(dt * 30, 0.5) { kick = min(1, kick + (nextBass - lastBass) * 4) }
        kick *= exp(-dt * 6)
        lastBass = nextBass
        bass = nextBass
        energy = values.isEmpty ? 0 : values.reduce(0, +) / Double(values.count)
        phase = (phase + dt * (0.25 + energy * 1.2 + kick * 3)).truncatingRemainder(dividingBy: 2 * Double.pi)
    }
}
