import Foundation

public struct DjWaveAccumulator: Sendable {
    public static let barCount = 1200
    public static let barsPerSecond = 20.0
    public static let mostBars = 12_000

    public static func bars(seconds: Double) -> Int {
        guard seconds.isFinite, seconds > 0 else { return barCount }
        return min(mostBars, max(barCount, Int(seconds * barsPerSecond)))
    }

    private let lowAlpha: Float
    private let highAlpha: Float
    private let samplesPerBar: Int
    private var low: Float = 0
    private var lower: Float = 0
    private var smooth: Float = 0
    private var smoother: Float = 0
    private var filled = 0
    private var lowEnergy: Double = 0
    private var midEnergy: Double = 0
    private var highEnergy: Double = 0
    private var rows: [(rms: Double, low: Double, mid: Double, high: Double)] = []

    public init(sampleRate: Double, totalSamples: Int, bars: Int = barCount) {
        let rate = max(1000, sampleRate.isFinite ? sampleRate : 22_050)
        lowAlpha = Float(1 - exp(-2 * Double.pi * 250 / rate))
        highAlpha = Float(1 - exp(-2 * Double.pi * 2500 / rate))
        samplesPerBar = max(1, Int((Double(max(1, totalSamples)) / Double(max(1, bars))).rounded(.up)))
        rows.reserveCapacity(bars + 1)
    }

    public mutating func add(_ samples: UnsafeBufferPointer<Float>) {
        for sample in samples {
            let value = sample.isFinite ? sample : 0
            low += lowAlpha * (value - low)
            lower += lowAlpha * (low - lower)
            smooth += highAlpha * (value - smooth)
            smoother += highAlpha * (smooth - smoother)
            let high = value - smoother
            let mid = smoother - lower
            lowEnergy += Double(lower * lower)
            midEnergy += Double(mid * mid)
            highEnergy += Double(high * high)
            filled += 1
            if filled >= samplesPerBar { close() }
        }
    }

    public mutating func add(_ samples: [Float]) {
        samples.withUnsafeBufferPointer { add($0) }
    }

    public mutating func finish(bars count: Int = barCount) -> [DjWaveBar] {
        if filled > 0 { close() }
        guard !rows.isEmpty, count > 0 else { return [] }
        let peak = rows.map(\.rms).max() ?? 0
        guard peak > 1e-5 else { return [] }
        return (0..<count).map { index in
            let row = rows[min(rows.count - 1, index * rows.count / count)]
            let level = min(1, max(0.04, pow(row.rms / peak, 0.8)))
            let weight = row.low + row.mid + row.high
            let hue = weight > 0 ? (row.mid * 0.33 + row.high * 0.6) / weight : 0
            return DjWaveBar(level: level, hue: min(0.999, max(0, hue)))
        }
    }

    private mutating func close() {
        let count = Double(max(1, filled))
        let total = (lowEnergy + midEnergy + highEnergy) / count
        rows.append((total.squareRoot(), (lowEnergy / count).squareRoot(), (midEnergy / count).squareRoot(), (highEnergy / count).squareRoot()))
        filled = 0
        lowEnergy = 0
        midEnergy = 0
        highEnergy = 0
    }
}

public enum DjWaveRanges {
    public static let chunk: Int64 = 2 * 1024 * 1024

    public static func declaredLength(_ url: URL) -> Int64? {
        guard let value = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems?.first(where: { $0.name == "clen" })?.value,
            let length = Int64(value), length > 0
        else { return nil }
        return length
    }

    public static func total(contentRange: String?) -> Int64? {
        guard let contentRange, let slash = contentRange.lastIndex(of: "/"),
            let total = Int64(contentRange[contentRange.index(after: slash)...].trimmingCharacters(in: .whitespaces)), total > 0
        else { return nil }
        return total
    }

    public static func spans(total: Int64, from start: Int64 = 0, size: Int64 = chunk) -> [ClosedRange<Int64>] {
        guard start >= 0, total > start, size > 0 else { return [] }
        return stride(from: start, to: total, by: Int(size)).map { $0...(min(total, $0 + size) - 1) }
    }

    public static func header(_ span: ClosedRange<Int64>) -> String {
        "bytes=\(span.lowerBound)-\(span.upperBound)"
    }
}

public enum DjWaveCache {
    public static func encode(_ bars: [DjWaveBar]) -> Data {
        var bytes: [UInt8] = []
        bytes.reserveCapacity(bars.count * 2)
        for bar in bars {
            bytes.append(UInt8(min(255, max(0, (bar.level * 255).rounded()))))
            bytes.append(UInt8(min(255, max(0, (bar.hue * 255).rounded()))))
        }
        return Data(bytes)
    }

    public static func decode(_ data: Data) -> [DjWaveBar] {
        guard !data.isEmpty, data.count % 2 == 0 else { return [] }
        let bytes = [UInt8](data)
        return stride(from: 0, to: bytes.count, by: 2).map { index in
            DjWaveBar(level: max(0.04, Double(bytes[index]) / 255), hue: min(0.999, Double(bytes[index + 1]) / 255))
        }
    }
}
