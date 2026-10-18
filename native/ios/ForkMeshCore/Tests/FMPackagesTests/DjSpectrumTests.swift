import Foundation
import XCTest

@testable import FMPackages

final class DjSpectrumTests: XCTestCase {
    private let rate = 22_050.0

    private func tone(_ frequency: Double, seconds: Double = 1, amplitude: Double = 0.5) -> DjScratchAudio {
        DjScratchAudio(samples: (0..<Int(seconds * rate)).map { DjScratchAudio.pcm(Float(amplitude * sin(2 * Double.pi * frequency * Double($0) / rate))) }, rate: rate)
    }

    private func loudest(_ bands: [Double]) -> Int {
        bands.indices.max { bands[$0] < bands[$1] } ?? -1
    }

    func testEdgesRiseAndStayInRange() {
        let edges = DjSpectrum.edges(rate: rate)
        XCTAssertEqual(edges.count, DjSpectrum.bandCount + 1)
        XCTAssertEqual(edges, edges.sorted())
        XCTAssertGreaterThanOrEqual(edges.first ?? 0, 1)
        XCTAssertLessThanOrEqual(edges.last ?? .max, DjSpectrum.windowSize / 2)
    }

    func testToneLightsItsOwnBand() {
        let low = DjSpectrum.bands(tone(80), at: 0.5)
        let high = DjSpectrum.bands(tone(5_000), at: 0.5)
        XCTAssertEqual(low.count, DjSpectrum.bandCount)
        XCTAssertLessThan(loudest(low), 4)
        XCTAssertGreaterThan(loudest(high), DjSpectrum.bandCount - 5)
        XCTAssertGreaterThan(low[loudest(low)], 0.7)
        XCTAssertLessThan(low[DjSpectrum.bandCount - 1], 0.2)
    }

    func testSilenceAndBadTimesAreQuiet() {
        let silence = DjScratchAudio(samples: [Int16](repeating: 0, count: 22_050), rate: rate)
        XCTAssertEqual(DjSpectrum.bands(silence, at: 0.5), [Double](repeating: 0, count: DjSpectrum.bandCount))
        XCTAssertEqual(DjSpectrum.bands(tone(440), at: .nan), [Double](repeating: 0, count: DjSpectrum.bandCount))
        XCTAssertEqual(DjSpectrum.bands(tone(440), at: 5), [Double](repeating: 0, count: DjSpectrum.bandCount))
        XCTAssertEqual(DjSpectrum.bands(DjScratchAudio(samples: [1, 2, 3], rate: rate), at: 0), [Double](repeating: 0, count: DjSpectrum.bandCount))
    }

    func testWaveformFallbackFollowsLevel() {
        let loud = [DjWaveBar](repeating: DjWaveBar(level: 1, hue: 0.2), count: 100)
        let quiet = [DjWaveBar](repeating: DjWaveBar(level: 0.1, hue: 0.2), count: 100)
        let a = DjSpectrum.bands(loud, time: 5, duration: 10)
        let b = DjSpectrum.bands(quiet, time: 5, duration: 10)
        XCTAssertEqual(a.count, DjSpectrum.bandCount)
        XCTAssertGreaterThan(a.reduce(0, +), b.reduce(0, +) * 5)
        XCTAssertTrue(a.allSatisfy { (0...1).contains($0) })
        XCTAssertEqual(DjSpectrum.bands([], time: 5, duration: 10), [Double](repeating: 0, count: DjSpectrum.bandCount))
    }

    func testMeterRisesFastFallsSlowlyAndKicks() {
        var meter = DjSpectrumMeter(count: 4)
        meter.update([0, 0, 0, 0], at: 0)
        meter.update([1, 1, 1, 1], at: 1.0 / 30)
        XCTAssertGreaterThan(meter.values[0], 0.6)
        XCTAssertGreaterThan(meter.kick, 0.5)
        XCTAssertGreaterThan(meter.bass, 0.6)
        let high = meter.values[0]
        meter.update([0, 0, 0, 0], at: 2.0 / 30)
        XCTAssertGreaterThan(meter.values[0], high * 0.6)
        XCTAssertGreaterThanOrEqual(meter.peaks[0], meter.values[0])
        for frame in 3..<120 { meter.update([0, 0, 0, 0], at: Double(frame) / 30) }
        XCTAssertLessThan(meter.values[0], 0.01)
        XCTAssertLessThan(meter.kick, 0.01)
        XCTAssertEqual(meter.peaks[0], meter.values[0], accuracy: 0.01)
        meter.update([1, 1], at: 5)
        XCTAssertLessThan(meter.values[0], 0.01)
    }
}
