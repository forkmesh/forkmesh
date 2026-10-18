import Foundation
import XCTest

@testable import FMPackages

final class DjBpmTests: XCTestCase {
    private let rate = 22_050.0

    private func groove(bpm: Double, seconds: Double = 40, hats: Bool = true, snare: Bool = true, seed: UInt64 = 7) -> DjScratchAudio {
        var random = seed
        func noise() -> Double {
            random = random &* 6364136223846793005 &+ 1442695040888963407
            return Double(Int64(bitPattern: random)) / Double(Int64.max)
        }
        let count = Int(seconds * rate)
        var signal = [Double](repeating: 0, count: count)
        let beat = 60 / bpm
        var index = 0
        var time = 0.31
        while time < seconds {
            let start = Int(time * rate)
            for offset in 0..<Int(0.12 * rate) where start + offset < count {
                let t = Double(offset) / rate
                signal[start + offset] += 0.8 * sin(2 * Double.pi * (55 + 90 * exp(-t * 30)) * t) * exp(-t * 22)
            }
            if snare, index % 2 == 1 {
                for offset in 0..<Int(0.09 * rate) where start + offset < count {
                    signal[start + offset] += 0.35 * noise() * exp(-Double(offset) / rate * 35)
                }
            }
            if hats {
                let hat = Int((time + beat / 2) * rate)
                for offset in 0..<Int(0.03 * rate) where hat + offset < count {
                    signal[hat + offset] += 0.2 * noise() * exp(-Double(offset) / rate * 120)
                }
            }
            index += 1
            time += beat
        }
        for sample in signal.indices { signal[sample] += 0.03 * noise() }
        return DjScratchAudio(samples: signal.map { DjScratchAudio.pcm(Float($0)) }, rate: rate)
    }

    func testFindsHouseTempo() throws {
        let found = try XCTUnwrap(DjBpm.detect(groove(bpm: 128)))
        XCTAssertEqual(found, 128, accuracy: 0.3)
    }

    func testFindsHipHopTempoWithoutDoubling() throws {
        let found = try XCTUnwrap(DjBpm.detect(groove(bpm: 90, seed: 11)))
        XCTAssertEqual(found, 90, accuracy: 0.3)
    }

    func testFindsFractionalTempo() throws {
        let found = try XCTUnwrap(DjBpm.detect(groove(bpm: 123.4, hats: false, seed: 3)))
        XCTAssertEqual(found, 123.4, accuracy: 0.3)
    }

    func testFindsTempoOfPlainKicks() throws {
        let found = try XCTUnwrap(DjBpm.detect(groove(bpm: 140, hats: false, snare: false, seed: 5)))
        XCTAssertEqual(found, 140, accuracy: 0.3)
    }

    func testNoBeatGivesNothing() {
        XCTAssertNil(DjBpm.detect(DjScratchAudio(samples: [Int16](repeating: 0, count: Int(rate * 30)), rate: rate)))
        var random: UInt64 = 1
        let hiss = (0..<Int(rate * 30)).map { _ -> Int16 in
            random = random &* 6364136223846793005 &+ 1442695040888963407
            return Int16(truncatingIfNeeded: Int64(bitPattern: random) >> 52)
        }
        XCTAssertNil(DjBpm.detect(DjScratchAudio(samples: hiss, rate: rate)))
        XCTAssertNil(DjBpm.detect(DjScratchAudio(samples: [1, 2, 3], rate: rate)))
        XCTAssertNil(DjBpm.detect(DjScratchAudio(samples: [], rate: 0)))
    }

    func testFindsEveryTempoInRangeWithoutOctaveOrThirdErrors() throws {
        for bpm in stride(from: 72.0, to: 180, by: 13.3) {
            for (hats, snare) in [(true, true), (false, false)] {
                let found = try XCTUnwrap(DjBpm.detect(groove(bpm: bpm, seconds: 30, hats: hats, snare: snare, seed: UInt64(bpm * 10))), "\(bpm)")
                XCTAssertEqual(found, bpm, accuracy: 0.3, "\(bpm) hats \(hats) snare \(snare)")
            }
        }
    }
}
