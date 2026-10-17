import XCTest
@testable import FMAgentWatch

final class VoiceLaunchChimeTests: XCTestCase {
    func testEveryChimeSoundsBrieflyAndStaysInRange() {
        for chime in VoiceLaunchChime.allCases {
            let notes = chime.notes
            XCTAssertFalse(notes.isEmpty, chime.rawValue)
            XCTAssertLessThan(notes.map { $0.start + $0.seconds }.max() ?? 0, 1.2, chime.rawValue)
            XCTAssertTrue(notes.allSatisfy { $0.gain > 0 && $0.gain <= 0.12 }, chime.rawValue)
            let pcm = chime.render()
            XCTAssertFalse(pcm.isEmpty, chime.rawValue)
            XCTAssertTrue(pcm.contains { $0 != 0 }, chime.rawValue)
        }
    }

    func testSummonRisesAndFizzleFalls() {
        let up = VoiceLaunchChime.summon.notes.dropFirst().prefix(6).map(\.frequency)
        XCTAssertEqual(up, up.sorted())
        let down = VoiceLaunchChime.fizzle.notes.prefix(4).map(\.frequency)
        XCTAssertEqual(down, down.sorted(by: >))
    }

    func testLaunchClimbsIntoItsBell() {
        let run = VoiceLaunchChime.launch.notes.dropFirst().prefix(8).map(\.frequency)
        XCTAssertEqual(run, run.sorted())
        let bell = VoiceLaunchChime.launch.notes[9]
        XCTAssertGreaterThan(bell.frequency, run.last ?? 0)
    }

    func testSummonBellRingsBeforeTheMicrophoneOpens() {
        let bell = VoiceLaunchChime.summon.notes.filter { $0.timbre == .bell }.map(\.start).min() ?? 1
        XCTAssertLessThan(bell, VoiceLaunchChime.summonSeconds)
    }

    func testChimesAreDistinct() {
        let all = VoiceLaunchChime.allCases
        for (index, chime) in all.enumerated() {
            for other in all.prefix(index) {
                XCTAssertNotEqual(chime.notes, other.notes, "\(chime.rawValue) \(other.rawValue)")
            }
        }
    }
}
