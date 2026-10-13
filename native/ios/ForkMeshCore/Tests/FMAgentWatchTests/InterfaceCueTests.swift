import XCTest

@testable import FMAgentWatch

final class InterfaceCueTests: XCTestCase {
    func testEveryCueSoundsAndHasTheDesktopSlug() {
        let slugs = InterfaceCue.allCases.map(\.rawValue)
        XCTAssertEqual(
            slugs,
            [
                "keystroke", "click", "toggle-on", "toggle-off", "select", "navigate", "open", "close", "send",
                "arrive", "success", "warn", "error", "delete",
            ])
        for cue in InterfaceCue.allCases {
            let notes = cueNotes(cue, start: 1)
            XCTAssertFalse(notes.isEmpty, cue.rawValue)
            XCTAssertEqual(notes.first?.start, 1, cue.rawValue)
            XCTAssertTrue(notes.allSatisfy { $0.instrument == .celesta && $0.pan == 0.5 }, cue.rawValue)
        }
    }

    func testCuesSitOnTheScaleTwoOctavesUp() {
        let click = cueNotes(.click, start: 0)
        XCTAssertEqual(click.count, 1)
        XCTAssertEqual(click[0].frequency, kTonicHz * 4, accuracy: 1e-9)
        XCTAssertEqual(click[0].gain, 0.085, accuracy: 1e-12)
        let on = cueNotes(.toggleOn, start: 0).map(\.frequency)
        let off = cueNotes(.toggleOff, start: 0).map(\.frequency)
        XCTAssertLessThan(on[0], on[1])
        XCTAssertEqual(on, off.reversed())
        let error = cueNotes(.error, start: 0)
        XCTAssertTrue(error.allSatisfy { $0.frequency < kTonicHz * 4 })
    }

    func testVolumeScalesAndZeroIsSilent() {
        XCTAssertTrue(cueNotes(.success, start: 0, volume: 0).isEmpty)
        XCTAssertEqual(cueNotes(.click, start: 0, volume: 0.5)[0].gain, 0.0425, accuracy: 1e-12)
        XCTAssertEqual(cueNotes(.click, start: 0, volume: 3)[0].gain, 0.085, accuracy: 1e-12)
        XCTAssertTrue(renderCue(.click, volume: 0).isEmpty)
    }

    func testWeightsRankOutcomesAboveGestures() {
        let ranked = InterfaceCue.allCases.sorted { $0.weight < $1.weight }
        XCTAssertEqual(ranked.first, .keystroke)
        XCTAssertEqual(ranked.last, .error)
        XCTAssertLessThan(InterfaceCue.close.weight, InterfaceCue.click.weight)
        XCTAssertLessThan(InterfaceCue.click.weight, InterfaceCue.open.weight)
        XCTAssertLessThan(InterfaceCue.send.weight, InterfaceCue.success.weight)
    }

    func testLabelsThatDestroyOrSendChooseTheirCue() {
        XCTAssertEqual(InterfaceCue.forLabel("Delete branch", fallback: .click), .delete)
        XCTAssertEqual(InterfaceCue.forLabel("&Remove", fallback: .click), .delete)
        XCTAssertEqual(InterfaceCue.forLabel("  Send ", fallback: .click), .send)
        XCTAssertEqual(InterfaceCue.forLabel("Publish", fallback: .click), .send)
        XCTAssertEqual(InterfaceCue.forLabel("Delete\u{2026}", fallback: .open), .open)
        XCTAssertEqual(InterfaceCue.forLabel("Delete...", fallback: .click), .click)
        XCTAssertEqual(InterfaceCue.forLabel("Open", fallback: .click), .click)
        XCTAssertEqual(InterfaceCue.forLabel("", fallback: .select), .select)
        XCTAssertEqual(InterfaceCue.toggle(true), .toggleOn)
        XCTAssertEqual(InterfaceCue.toggle(false), .toggleOff)
    }

    func testGateCollapsesARattleButLetsWeightierNewsThrough() {
        var gate = InterfaceCueGate()
        XCTAssertTrue(gate.admit(.click, atMs: 1000))
        XCTAssertFalse(gate.admit(.click, atMs: 1010))
        XCTAssertFalse(gate.admit(.select, atMs: 1020))
        XCTAssertTrue(gate.admit(.error, atMs: 1030))
        XCTAssertFalse(gate.admit(.success, atMs: 1040))
        XCTAssertTrue(gate.admit(.click, atMs: 1030 + kCueThrottleMs))
    }

    func testBatchKeepsTheWeightiestCueOfOneGesture() {
        var batch = InterfaceCueBatch()
        XCTAssertTrue(batch.hold(.click))
        XCTAssertFalse(batch.hold(.open))
        XCTAssertFalse(batch.hold(.close))
        XCTAssertEqual(batch.take(), .open)
        XCTAssertNil(batch.take())
        XCTAssertTrue(batch.hold(.select))
    }

    func testKeystrokesAreShadedApart() {
        let seeds = (1...kKeystrokeVariants).map(keystrokeSeed)
        XCTAssertEqual(Set(seeds).count, kKeystrokeVariants)
        XCTAssertTrue(seeds.allSatisfy { $0 != 0 && $0 & 1 == 1 })
        XCTAssertNotEqual(renderCue(.keystroke, seed: seeds[0]), renderCue(.keystroke, seed: seeds[1]))
    }

    func testRenderIsDryAndShort() {
        for cue in InterfaceCue.allCases {
            let pcm = renderCue(cue)
            let seconds = Double(pcm.count / 2) / Double(kWatchSampleRate)
            XCTAssertGreaterThan(seconds, 0.05, cue.rawValue)
            XCTAssertLessThan(seconds, 2.5, cue.rawValue)
            XCTAssertTrue(pcm.contains { $0 != 0 }, cue.rawValue)
        }
        XCTAssertLessThan(renderCue(.click).count, renderCue(.success).count)
    }

    func testRatchetBurstsSpreadTheirTeethAcrossOneBatch() {
        XCTAssertEqual(RatchetBurst.all.count, RatchetPawl.allCases.count * kRatchetMaxTeeth)
        XCTAssertEqual(Set(RatchetBurst.all).count, RatchetBurst.all.count)
        XCTAssertEqual(RatchetBurst(teeth: 0, pawl: .soft).teeth, 1)
        XCTAssertEqual(RatchetBurst(teeth: 500, pawl: .crisp).teeth, kRatchetMaxTeeth)
        let one = RatchetBurst(teeth: 1, pawl: .crisp).render()
        let six = RatchetBurst(teeth: 6, pawl: .crisp).render()
        XCTAssertEqual(one.count, Int((kRatchetToothSeconds * Double(kWatchSampleRate)).rounded(.up)) * 2)
        XCTAssertLessThan(six.count, Int(((kRatchetBatchSeconds + kRatchetToothSeconds) * Double(kWatchSampleRate)).rounded(.up)) * 2)
        XCTAssertGreaterThan(six.count, one.count * 2)
        XCTAssertLessThan(peak(RatchetBurst(teeth: 1, pawl: .soft).render()), peak(one))
        XCTAssertNotEqual(RatchetBurst(teeth: 2, pawl: .crisp).render(seed: keystrokeSeed(1)), RatchetBurst(teeth: 2, pawl: .crisp).render(seed: keystrokeSeed(2)))
        let frame = Int(RatchetBurst(teeth: 3, pawl: .crisp).spacing * Double(kWatchSampleRate))
        let three = RatchetBurst(teeth: 3, pawl: .crisp).render()
        XCTAssertGreaterThan(peak(Array(three[(frame * 2)..<(frame * 2 + 40)])), 1000)
        XCTAssertLessThan(peak(Array(three[(frame * 2 - 40)..<(frame * 2)])), peak(Array(three[(frame * 2)..<(frame * 2 + 40)])))
        let caught = renderRatchetCatch()
        XCTAssertGreaterThan(caught.count, one.count)
        XCTAssertGreaterThan(peak(caught), peak(one) / 2)
        XCTAssertLessThan(peak(caught), Int(Int16.max))
    }

    func testRatchetGateStrikesAtOnceThenOncePerBatch() {
        var gate = RatchetGate()
        XCTAssertNil(gate.pass(lines: 0, changed: 0, flagged: 0))
        XCTAssertFalse(gate.isOpen)
        XCTAssertEqual(gate.pass(lines: 1, changed: 0, flagged: 0), RatchetStroke(burst: RatchetBurst(teeth: 1, pawl: .soft), caught: false))
        XCTAssertTrue(gate.isOpen)
        XCTAssertNil(gate.pass(lines: 2, changed: 0, flagged: 0))
        XCTAssertNil(gate.pass(lines: 3, changed: 1, flagged: 0))
        XCTAssertEqual(gate.close(), RatchetStroke(burst: RatchetBurst(teeth: 5, pawl: .crisp), caught: false))
        XCTAssertTrue(gate.isOpen)
        XCTAssertNil(gate.pass(lines: 40, changed: 0, flagged: 1))
        XCTAssertEqual(gate.close(), RatchetStroke(burst: RatchetBurst(teeth: kRatchetMaxTeeth, pawl: .crisp), caught: true))
        XCTAssertNil(gate.close())
        XCTAssertFalse(gate.isOpen)
        XCTAssertEqual(gate.pass(lines: 2, changed: 2, flagged: 0)?.burst, RatchetBurst(teeth: 2, pawl: .crisp))
    }

    private func peak(_ pcm: [Int16]) -> Int {
        pcm.map { abs(Int($0)) }.max() ?? 0
    }

    func testWavHeaderDescribesThePcm() {
        let pcm: [Int16] = [1, -1, 300, -300]
        let wav = wavData(pcm, rate: 44100, channels: 2)
        XCTAssertEqual(wav.count, 44 + 8)
        XCTAssertEqual(String(decoding: wav[0..<4], as: UTF8.self), "RIFF")
        XCTAssertEqual(String(decoding: wav[8..<16], as: UTF8.self), "WAVEfmt ")
        XCTAssertEqual(String(decoding: wav[36..<40], as: UTF8.self), "data")
        func le32(_ at: Int) -> UInt32 { wav[at..<at + 4].reversed().reduce(0) { $0 << 8 | UInt32($1) } }
        XCTAssertEqual(le32(4), 44)
        XCTAssertEqual(le32(24), 44100)
        XCTAssertEqual(le32(28), 44100 * 4)
        XCTAssertEqual(le32(40), 8)
        XCTAssertEqual(wav[44], 1)
        XCTAssertEqual(wav[46], 0xFF)
        XCTAssertEqual(wav[47], 0xFF)
    }

    func testATouchFallsBackOnlyWhenNothingElseSounded() {
        var fallback = InterfaceCueFallback()
        XCTAssertTrue(fallback.silent(sinceMs: 0))
        fallback.heard(atMs: 100)
        XCTAssertFalse(fallback.silent(sinceMs: 90))
        XCTAssertFalse(fallback.silent(sinceMs: 100))
        XCTAssertTrue(fallback.silent(sinceMs: 101))
        XCTAssertGreaterThan(kCueFallbackDelayMs, kCueThrottleMs)
        XCTAssertGreaterThan(kCueSlideSettleMs, kCueFallbackDelayMs)
    }

    func testATouchSoundsWhatItDidToTheScreen() {
        let root = InterfaceScreenShape(presented: 0, pushed: 2)
        let sheet = InterfaceScreenShape(presented: 1, pushed: 2)
        let pushed = InterfaceScreenShape(presented: 0, pushed: 3)
        XCTAssertEqual(InterfaceCue.afterTouch(from: root, to: sheet, tapped: true), .open)
        XCTAssertEqual(InterfaceCue.afterTouch(from: sheet, to: root, tapped: false), .close)
        XCTAssertEqual(InterfaceCue.afterTouch(from: root, to: pushed, tapped: true), .navigate)
        XCTAssertEqual(InterfaceCue.afterTouch(from: pushed, to: root, tapped: false), .close)
        XCTAssertEqual(InterfaceCue.afterTouch(from: root, to: root, tapped: true), .click)
        XCTAssertNil(InterfaceCue.afterTouch(from: root, to: root, tapped: false))
    }
}
