import XCTest
@testable import FMAgentWatch

final class GenieChimeTests: XCTestCase {
    func testEveryChimeSoundsBrieflyAndStaysInRange() {
        for chime in GenieChime.allCases {
            let notes = chime.notes
            XCTAssertFalse(notes.isEmpty, chime.rawValue)
            XCTAssertLessThan(notes.map { $0.start + $0.seconds }.max() ?? 0, 1.2, chime.rawValue)
            XCTAssertTrue(notes.allSatisfy { $0.gain > 0 && $0.gain <= 0.12 }, chime.rawValue)
            let pcm = chime.render()
            XCTAssertFalse(pcm.isEmpty, chime.rawValue)
            XCTAssertTrue(pcm.contains { $0 != 0 }, chime.rawValue)
        }
    }

    func testPitchFollowsThePentatonicScale() {
        XCTAssertEqual(GenieChime.pitch(0), GenieChime.tonicHz, accuracy: 0.001)
        XCTAssertEqual(GenieChime.pitch(5), GenieChime.tonicHz * 2, accuracy: 0.001)
        XCTAssertEqual(GenieChime.pitch(-5), GenieChime.tonicHz / 2, accuracy: 0.001)
        XCTAssertEqual(GenieChime.pitch(-1), GenieChime.tonicHz * pow(2, -3.0 / 12), accuracy: 0.001)
        XCTAssertEqual(GenieChime.pitch(0, octave: 1), GenieChime.tonicHz * 2, accuracy: 0.001)
    }

    func testAppearRisesAndVanishFalls() {
        let up = GenieChime.appear.notes.prefix(7).map(\.frequency)
        XCTAssertEqual(up, up.sorted())
        let down = GenieChime.vanish.notes.prefix(6).map(\.frequency)
        XCTAssertEqual(down, down.sorted(by: >))
    }

    func testThirtyDistinctMoveSounds() {
        let moves = GenieMoveKind.allCases.flatMap(\.chimes)
        XCTAssertEqual(moves.count, 30)
        XCTAssertEqual(Set(moves).count, 30)
        XCTAssertEqual(GenieChime.allCases.filter { $0.move != nil }.count, 30)
        for (index, chime) in moves.enumerated() {
            XCTAssertLessThan(chime.notes.map { $0.start + $0.seconds }.max() ?? 1, 0.4, chime.rawValue)
            XCTAssertTrue(chime.notes.allSatisfy { $0.gain <= 0.06 }, chime.rawValue)
            for other in moves.prefix(index) {
                XCTAssertNotEqual(chime.notes, other.notes, "\(chime.rawValue) \(other.rawValue)")
            }
            XCTAssertLessThanOrEqual(chime.render().count, kWatchSampleRate * 2, chime.rawValue)
        }
    }

    func testMoveKindFollowsTheDrag() {
        XCTAssertEqual(GenieMoveKind.read(dx: 60, dy: 0, seconds: 0.3, turn: 0), .creep)
        XCTAssertEqual(GenieMoveKind.read(dx: 60, dy: 10, seconds: 0.1, turn: 0), .scuttle)
        XCTAssertEqual(GenieMoveKind.read(dx: 100, dy: -100, seconds: 0.05, turn: 0), .dash)
        XCTAssertEqual(GenieMoveKind.read(dx: 5, dy: -60, seconds: 0.2, turn: 0), .rise)
        XCTAssertEqual(GenieMoveKind.read(dx: -5, dy: 60, seconds: 0.2, turn: 0), .fall)
        XCTAssertEqual(GenieMoveKind.read(dx: 60, dy: 0, seconds: 0.02, turn: 3), .swerve)
    }

    func testDragPlaysEveryMoveSoundInTurn() {
        var voice = GenieMoveVoice()
        voice.begin(x: 0, y: 0, at: 0)
        XCTAssertNil(voice.move(x: 20, y: 0, at: 0.5))
        XCTAssertNil(voice.move(x: 80, y: 0, at: 0.05))
        var heard: [GenieChime] = []
        var x = 0.0
        var t = 0.0
        for _ in 0..<5 {
            x += 60
            t += 0.3
            if let chime = voice.move(x: x, y: 0, at: t) { heard.append(chime) }
        }
        XCTAssertEqual(heard, GenieMoveKind.creep.chimes)
        var y = 0.0
        for _ in 0..<5 {
            y -= 60
            t += 0.3
            if let chime = voice.move(x: x, y: y, at: t) { heard.append(chime) }
        }
        XCTAssertEqual(Array(heard.suffix(5)), GenieMoveKind.rise.chimes)
        var zig = GenieMoveVoice()
        zig.begin(x: 0, y: 0, at: 0)
        var played: [GenieChime] = []
        for step in 1...11 {
            if let chime = zig.move(x: step % 2 == 1 ? 60 : 0, y: 0, at: Double(step) * 0.2) { played.append(chime) }
        }
        XCTAssertEqual(played.first, .tiptoe)
        XCTAssertEqual(Array(played.dropFirst()), GenieMoveKind.swerve.chimes + GenieMoveKind.swerve.chimes)
    }
}
