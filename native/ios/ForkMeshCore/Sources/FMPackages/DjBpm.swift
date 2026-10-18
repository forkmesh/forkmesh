import Foundation

public enum DjBpm {
    public static let slowest = 70.0
    public static let fastest = 180.0
    public static let favourite = 120.0
    public static let spread = 0.8
    public static let hop = 256
    public static let longest: Double = 120
    public static let multiples: [Double] = [1, 2, 3, 4, 5, 6, 7, 8]
    public static let confidence = 0.04
    public static let octave = 0.8

    public static func detect(_ audio: DjScratchAudio) -> Double? {
        detect(audio.samples, rate: audio.rate)
    }

    public static func detect(_ samples: [Int16], rate: Double) -> Double? {
        guard rate >= 2_000, rate.isFinite else { return nil }
        let fps = rate / Double(hop)
        let onsets = onsets(samples, rate: rate)
        let lags = Int((60 / slowest * fps * (multiples.last ?? 1)).rounded(.up)) + 2
        guard onsets.count > lags * 2 else { return nil }
        let acf = autocorrelation(onsets, lags: lags)
        var best: (bpm: Double, score: Double, raw: Double)?
        for step in Int((slowest * 10).rounded())...Int((fastest * 10).rounded()) {
            let bpm = Double(step) / 10
            let raw = score(acf, period: 60 / bpm * fps)
            let weighted = raw * prior(bpm)
            if weighted > best?.score ?? -.infinity { best = (bpm, weighted, raw) }
        }
        guard let best, best.raw >= confidence else { return nil }
        var chosen = best.bpm
        let doubled = [-0.1, 0, 0.1].map { best.bpm * 2 + $0 }.filter { $0 <= fastest }
        if let double = doubled.max(by: { score(acf, period: 60 / $0 * fps) < score(acf, period: 60 / $1 * fps) }),
            score(acf, period: 60 / double * fps) >= best.raw * octave
        {
            chosen = double
        }
        let period = refine(acf, period: 60 / chosen * fps)
        return min(fastest, max(slowest, (60 * fps / period * 10).rounded() / 10))
    }

    static func refine(_ acf: [Double], period: Double) -> Double {
        var multiple = 1.0
        while period * multiple * 2 + 3 < Double(acf.count) { multiple *= 2 }
        let centre = Int((period * multiple).rounded())
        guard centre >= 3, centre + 3 < acf.count else { return period }
        var peak = centre
        for lag in centre - 2...centre + 2 where acf[lag] > acf[peak] { peak = lag }
        let left = acf[peak - 1]
        let middle = acf[peak]
        let right = acf[peak + 1]
        let curve = left - 2 * middle + right
        let shift = curve < 0 ? min(0.5, max(-0.5, 0.5 * (left - right) / curve)) : 0
        let refined = (Double(peak) + shift) / multiple
        return abs(refined - period) < period * 0.02 ? refined : period
    }

    static func prior(_ bpm: Double) -> Double {
        let octaves = log2(bpm / favourite) / spread
        return exp(-0.5 * octaves * octaves)
    }

    static func score(_ acf: [Double], period: Double) -> Double {
        multiples.reduce(0) { $0 + value(acf, at: period * $1) } / Double(multiples.count)
    }

    static func value(_ acf: [Double], at lag: Double) -> Double {
        let base = Int(lag)
        guard lag >= 0, base + 1 < acf.count else { return 0 }
        let fraction = lag - Double(base)
        return acf[base] * (1 - fraction) + acf[base + 1] * fraction
    }

    static func onsets(_ samples: [Int16], rate: Double) -> [Double] {
        let span = min(samples.count, Int(longest * rate))
        let start = (samples.count - span) / 2
        let frames = span / hop
        guard frames > 2 else { return [] }
        let smoothing = 1 - exp(-2 * Double.pi * 180 / rate)
        var low = 0.0
        var full = [Double](repeating: 0, count: frames)
        var bass = [Double](repeating: 0, count: frames)
        samples.withUnsafeBufferPointer { pcm in
            for frame in 0..<frames {
                var fullEnergy = 0.0
                var bassEnergy = 0.0
                let offset = start + frame * hop
                for index in offset..<offset + hop {
                    let sample = Double(pcm[index]) / 32768
                    low += smoothing * (sample - low)
                    fullEnergy += sample * sample
                    bassEnergy += low * low
                }
                full[frame] = log(1 + 1_000 * fullEnergy / Double(hop))
                bass[frame] = log(1 + 1_000 * bassEnergy / Double(hop))
            }
        }
        var flux = [Double](repeating: 0, count: frames)
        for frame in 1..<frames {
            flux[frame] = max(0, full[frame] - full[frame - 1]) + max(0, bass[frame] - bass[frame - 1])
        }
        let reach = max(1, Int(0.2 * rate / Double(hop)))
        var running = [Double](repeating: 0, count: frames + 1)
        for frame in 0..<frames { running[frame + 1] = running[frame] + flux[frame] }
        var result = [Double](repeating: 0, count: frames)
        for frame in 0..<frames {
            let from = max(0, frame - reach)
            let to = min(frames, frame + reach + 1)
            result[frame] = max(0, flux[frame] - (running[to] - running[from]) / Double(to - from))
        }
        let mean = result.reduce(0, +) / Double(frames)
        return result.map { $0 - mean }
    }

    static func autocorrelation(_ values: [Double], lags: Int) -> [Double] {
        let count = values.count
        var acf = [Double](repeating: 0, count: min(lags, count))
        values.withUnsafeBufferPointer { x in
            for lag in acf.indices {
                var sum = 0.0
                for index in 0..<count - lag { sum += x[index] * x[index + lag] }
                acf[lag] = sum / Double(count - lag)
            }
        }
        guard let zero = acf.first, zero > 0 else { return acf.map { _ in 0 } }
        return acf.map { $0 / zero }
    }
}
