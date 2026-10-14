import Foundation
import XCTest

@testable import FMChat

final class ViewOnceCaptionsTests: XCTestCase {
    private func spoken(_ text: String, from start: Double = 0, step: Double = 0.4, length: Double = 0.3) -> [CaptionWord] {
        text.split(separator: " ").enumerated().map { (entry: (offset: Int, element: Substring)) -> CaptionWord in
            CaptionWord(text: String(entry.element), start: start + Double(entry.offset) * step, duration: length)
        }
    }

    func testLinesHoldAtMostSixWords() {
        let lines = ViewOnceCaptions.lines(spoken("a b c d e f g h"))
        XCTAssertEqual(lines.map(\.text), ["a b c d e f", "g h"])
    }

    func testLinesBreakOnCharacterLimit() {
        let lines = ViewOnceCaptions.lines(spoken("wonderful afternoon everybody listening carefully"))
        XCTAssertEqual(lines.map(\.text), ["wonderful afternoon", "everybody listening", "carefully"])
        XCTAssertTrue(lines.allSatisfy { $0.text.count <= ViewOnceCaptions.maxCharacters })
    }

    func testLinesBreakOnPauseAndSentenceEnd() {
        var words = spoken("hi there")
        words += spoken("again", from: 2)
        words += spoken("Done. Next one", from: 3)
        let lines = ViewOnceCaptions.lines(words)
        XCTAssertEqual(lines.map(\.text), ["hi there", "again", "Done.", "Next one"])
    }

    func testLinesBreakOnClauseOnlyAfterThreeWords() {
        XCTAssertEqual(ViewOnceCaptions.lines(spoken("so, I think that")).map(\.text), ["so, I think that"])
        XCTAssertEqual(ViewOnceCaptions.lines(spoken("one two three, four")).map(\.text), ["one two three,", "four"])
    }

    func testLineShowsUntilNextLineOrHold() {
        var words = spoken("hello there")
        words += spoken("friend", from: 1.5)
        words += spoken("later", from: 10)
        let lines = ViewOnceCaptions.lines(words)
        XCTAssertEqual(lines.map(\.text), ["hello there", "friend", "later"])
        XCTAssertEqual(lines[0].start, 0)
        XCTAssertEqual(lines[0].end, 1.5, accuracy: 0.0001)
        XCTAssertEqual(lines[1].end, 1.8 + ViewOnceCaptions.hold, accuracy: 0.0001)
        XCTAssertEqual(lines[2].end, 10.3 + ViewOnceCaptions.hold, accuracy: 0.0001)
    }

    func testCleaningDropsBlanksSortsAndJoinsPunctuation() {
        let words = [
            CaptionWord(text: "world", start: 0.5, duration: 0.3),
            CaptionWord(text: " Hello ", start: 0.1, duration: 0.2),
            CaptionWord(text: "  ", start: 0.3, duration: 0.1),
            CaptionWord(text: "!", start: 0.8, duration: 0.05),
            CaptionWord(text: "now", start: 1.0, duration: 0),
        ]
        let cleaned = ViewOnceCaptions.cleaned(words)
        XCTAssertEqual(cleaned.map(\.text), ["Hello", "world!", "now"])
        XCTAssertEqual(cleaned[1].end, 0.85, accuracy: 0.0001)
        XCTAssertEqual(cleaned[2].duration, ViewOnceCaptions.minWordDuration)
        XCTAssertEqual(ViewOnceCaptions.lines(words).map(\.text), ["Hello world!", "now"])
    }

    func testCueFollowsTheSpokenWord() {
        let lines = ViewOnceCaptions.lines(spoken("one two three"))
        XCTAssertNil(ViewOnceCaptions.cue(at: -0.1, in: lines))
        XCTAssertEqual(ViewOnceCaptions.cue(at: 0, in: lines), CaptionCue(line: 0, word: 0))
        XCTAssertEqual(ViewOnceCaptions.cue(at: 0.35, in: lines), CaptionCue(line: 0, word: 0))
        XCTAssertEqual(ViewOnceCaptions.cue(at: 0.45, in: lines), CaptionCue(line: 0, word: 1))
        XCTAssertEqual(ViewOnceCaptions.cue(at: 0.9, in: lines), CaptionCue(line: 0, word: 2))
        XCTAssertEqual(ViewOnceCaptions.cue(at: 1.5, in: lines), CaptionCue(line: 0, word: 3))
        XCTAssertNil(ViewOnceCaptions.cue(at: 1.1 + ViewOnceCaptions.hold, in: lines))
        XCTAssertNil(ViewOnceCaptions.cue(at: .nan, in: lines))
        XCTAssertNil(ViewOnceCaptions.cue(at: 1, in: []))
    }

    func testCueMovesToTheNextLine() {
        let lines = ViewOnceCaptions.lines(spoken("a b c d e f g h"))
        XCTAssertEqual(ViewOnceCaptions.cue(at: 2.1, in: lines), CaptionCue(line: 0, word: 5))
        XCTAssertEqual(ViewOnceCaptions.cue(at: 2.41, in: lines), CaptionCue(line: 1, word: 0))
        XCTAssertEqual(ViewOnceCaptions.cue(at: 2.85, in: lines), CaptionCue(line: 1, word: 1))
    }

    func testCueStates() {
        let cue = CaptionCue(line: 0, word: 1)
        XCTAssertEqual((0..<3).map { cue.state(of: $0) }, [.spoken, .active, .upcoming])
        XCTAssertEqual(CaptionCue(line: 0, word: 3).state(of: 2), .spoken)
    }

    func testLayoutScalesWithTheVideo() {
        let portrait = CaptionLayout(width: 480, height: 640)
        XCTAssertEqual(portrait.fontSize, 27)
        XCTAssertEqual(portrait.bottomInset, 51)
        XCTAssertEqual(portrait.maxWidth, 432)
        let landscape = CaptionLayout(width: 1280, height: 720)
        XCTAssertEqual(landscape.fontSize, 30)
        XCTAssertEqual(landscape.bottomInset, 58)
        XCTAssertEqual(CaptionLayout(width: 100, height: 100).fontSize, CaptionLayout.minFontSize)
    }

    func testLayoutShrinksWideLines() {
        let layout = CaptionLayout(width: 480, height: 640)
        XCTAssertEqual(layout.fittedFontSize(textWidth: 300), layout.fontSize)
        let fitted = layout.fittedFontSize(textWidth: 600)
        XCTAssertLessThan(fitted, layout.fontSize)
        let width = 600 * fitted / layout.fontSize + 2 * layout.horizontalPadding(fitted)
        XCTAssertLessThanOrEqual(width, layout.maxWidth)
        XCTAssertEqual(layout.fittedFontSize(textWidth: 100_000), CaptionLayout.minFontSize)
    }

    func testBitRateStaysUnderTheSizeLimit() {
        let limit = ViewOnceKind.video.maxBytes
        let rate = ViewOnceCaptions.videoBitRate(
            durationSeconds: 60, sourceBitRate: 5_000_000, maxBytes: limit, audioBitRate: ViewOnceCaptions.audioBitRate)
        XCTAssertLessThanOrEqual(Double(rate + ViewOnceCaptions.audioBitRate) * 60 / 8, Double(limit))
        XCTAssertEqual(
            ViewOnceCaptions.videoBitRate(durationSeconds: 10, sourceBitRate: 1_000_000, maxBytes: limit, audioBitRate: 64_000),
            1_200_000)
        XCTAssertEqual(
            ViewOnceCaptions.videoBitRate(durationSeconds: 10, sourceBitRate: 0, maxBytes: limit, audioBitRate: 64_000),
            1_200_000)
        XCTAssertEqual(
            ViewOnceCaptions.videoBitRate(durationSeconds: 10, sourceBitRate: 200_000, maxBytes: limit, audioBitRate: 64_000),
            600_000)
    }

    func testOnlyMP4CanBeCaptioned() {
        XCTAssertTrue(ViewOnceCaptions.canCaption(mime: "video/mp4"))
        XCTAssertTrue(ViewOnceCaptions.canCaption(mime: "video/mp4; codecs=avc1"))
        XCTAssertTrue(ViewOnceCaptions.canCaption(mime: "video/quicktime"))
        XCTAssertFalse(ViewOnceCaptions.canCaption(mime: "video/webm"))
    }

    func testFailureMessages() {
        XCTAssertEqual(ViewOnceCaptionsFailure.noWords.message, "No words heard, so there are no captions.")
        for failure in [ViewOnceCaptionsFailure.denied, .unavailable, .unsupported, .noAudio, .tooLarge, .failed] {
            XCTAssertFalse(failure.message.isEmpty)
        }
    }
}
