import Foundation
import XCTest

@testable import FMPackages

final class DjScratchTests: XCTestCase {
    private let sourceRate = 22_050.0
    private let outputRate = 44_100.0

    private func tone(seconds: Double, frequency: Double) -> [Int16] {
        (0..<Int(seconds * sourceRate)).map { DjScratchAudio.pcm(Float(0.5 * sin(2 * Double.pi * frequency * Double($0) / sourceRate))) }
    }

    private func run(_ head: inout DjScratchHead, _ pcm: [Int16], from start: Double, speed: Double, seconds: Double, gain: Float = 1) -> [Float] {
        let block = 256
        var goal = start
        var out: [Float] = []
        let blocks = Int(seconds * outputRate) / block
        pcm.withUnsafeBufferPointer { source in
            for _ in 0..<blocks {
                goal += speed * Double(block) / outputRate
                var chunk = [Float](repeating: 0, count: block)
                chunk.withUnsafeMutableBufferPointer { head.render(source, sourceRate: sourceRate, outputRate: outputRate, goal: goal, gain: gain, into: $0) }
                out += chunk
            }
        }
        return out
    }

    private func crossings(_ samples: ArraySlice<Float>) -> Int {
        zip(samples, samples.dropFirst()).filter { ($0 < 0) != ($1 < 0) }.count
    }

    func testPcmConversionClamps() {
        XCTAssertEqual(DjScratchAudio.pcm(2), 32767)
        XCTAssertEqual(DjScratchAudio.pcm(-2), -32767)
        XCTAssertEqual(DjScratchAudio.pcm(.nan), 0)
        XCTAssertEqual(DjScratchAudio(samples: [Int16](repeating: 0, count: 44_100), rate: 22_050).duration, 2)
        XCTAssertTrue(DjScratchAudio.fits(seconds: 240))
        XCTAssertFalse(DjScratchAudio.fits(seconds: 3600))
        XCTAssertFalse(DjScratchAudio.fits(seconds: .nan))
    }

    func testForwardStrokePlaysTheSongAtTheHandsSpeed() {
        let pcm = tone(seconds: 4, frequency: 441)
        var head = DjScratchHead()
        head.place(1)
        let out = run(&head, pcm, from: 1, speed: 1, seconds: 1)
        let tail = out.suffix(Int(outputRate / 2))
        XCTAssertEqual(Double(crossings(tail)) / 2 / 0.5, 441, accuracy: 12)
        XCTAssertGreaterThan(tail.map(abs).max() ?? 0, 0.3)
        XCTAssertEqual(head.position, 2, accuracy: 0.03)
    }

    func testBackwardStrokePlaysTheSongInReverse() {
        let pcm = tone(seconds: 4, frequency: 441)
        var head = DjScratchHead()
        head.place(3)
        let out = run(&head, pcm, from: 3, speed: -2, seconds: 0.5)
        let tail = out.suffix(Int(outputRate / 4))
        XCTAssertEqual(Double(crossings(tail)) / 2 / 0.25, 882, accuracy: 25)
        XCTAssertLessThan(head.velocity, -1.5)
        XCTAssertEqual(head.position, 2, accuracy: 0.03)
    }

    func testHoldingTheRecordStillIsSilent() {
        let pcm = tone(seconds: 4, frequency: 441)
        var head = DjScratchHead()
        head.place(1)
        _ = run(&head, pcm, from: 1, speed: 1, seconds: 0.3)
        let held = run(&head, pcm, from: head.position, speed: 0, seconds: 0.3)
        XCTAssertLessThan(held.suffix(4410).map(abs).max() ?? 1, 0.001)
        XCTAssertFalse(head.audible)
    }

    func testZeroGainFadesOutAndTheHeadStaysInTheSong() {
        let pcm = tone(seconds: 2, frequency: 441)
        var head = DjScratchHead()
        head.place(1.5)
        let out = run(&head, pcm, from: 1.5, speed: 3, seconds: 0.5, gain: 0)
        XCTAssertEqual(out.map(abs).max() ?? 1, 0, accuracy: 1e-6)
        XCTAssertLessThanOrEqual(head.position, Double(pcm.count - 1) / sourceRate)
        XCTAssertGreaterThanOrEqual(head.position, 0)
    }
}
