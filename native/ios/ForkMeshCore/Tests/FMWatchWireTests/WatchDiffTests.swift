import Foundation
import XCTest

@testable import FMWatchWire

final class WatchDiffTests: XCTestCase {
    private func sample(files: Int = 2, lines: Int = 4) -> WatchDiff {
        let made = (0..<files).map { index in
            WatchDiffFile(
                path: "src/file\(index).swift", status: "M", group: index == 0 ? .committed : .uncommitted,
                added: lines / 2, removed: lines / 2,
                lines: [WatchDiffLine(.hunk, "func run()", number: 3)]
                    + (0..<lines).map { WatchDiffLine($0 % 2 == 0 ? .added : .removed, "line \($0) " + String(repeating: "x", count: 40), number: $0 + 3) })
        }
        return WatchDiff(key: "desktop:a/b:1", branch: "fm/fix", base: "main", files: made)
    }

    func testDiffCommandRoundTrips() throws {
        let back = try WatchWire.open(.command, try WatchWire.envelope(.command, WatchCommand.diff(key: "k")), as: WatchCommand.self)
        XCTAssertEqual(back, .diff(key: "k"))
        XCTAssertEqual(back.key, "k")
        let missing = try JSONSerialization.data(withJSONObject: ["op": "diff"])
        XCTAssertThrowsError(try JSONDecoder().decode(WatchCommand.self, from: missing))
    }

    func testDiffRoundTripsThroughItsOwnEnvelope() throws {
        let diff = sample()
        let dictionary = try WatchWire.envelope(.diff, diff)
        XCTAssertEqual(WatchWire.kind(of: dictionary), .diff)
        XCTAssertEqual(try WatchWire.open(.diff, dictionary, as: WatchDiff.self), diff)
        XCTAssertThrowsError(try WatchWire.open(.reply, dictionary, as: WatchReply.self))
        let refused = WatchDiff.refused("k", "No desktop.")
        XCTAssertEqual(try WatchWire.open(.diff, try WatchWire.envelope(.diff, refused), as: WatchDiff.self), refused)
    }

    func testSummaryAndSections() {
        let diff = sample(files: 3, lines: 6)
        XCTAssertEqual(diff.added, 9)
        XCTAssertEqual(diff.removed, 9)
        XCTAssertEqual(diff.branchLine, "fm/fix \u{2192} main")
        XCTAssertEqual(diff.sections.map(\.title), ["Committed", "Uncommitted"])
        XCTAssertEqual(diff.sections[1].files.count, 2)
        XCTAssertEqual(diff.next(after: diff.files[0].id)?.path, "src/file1.swift")
        XCTAssertNil(diff.next(after: diff.files[2].id))
        XCTAssertEqual(diff.files[0].name, "file0.swift")
        XCTAssertEqual(diff.files[0].folder, "src")
        XCTAssertEqual(WatchDiffFile(path: "top.md").folder, "")
    }

    func testReviewMarkChangesWhenTheFileChanges() {
        var file = sample().files[0]
        let before = file.reviewMark
        file.added += 1
        XCTAssertNotEqual(file.reviewMark, before)
        XCTAssertNotEqual(WatchDiffFile(path: "a", group: .committed).reviewMark, WatchDiffFile(path: "a", group: .uncommitted).reviewMark)
    }

    func testNotes() {
        XCTAssertEqual(WatchDiffFile(path: "a.png", binary: true).note, "Binary file.")
        XCTAssertEqual(WatchDiffFile(path: "n.txt", status: "A", group: .untracked).note, "New file, not added to Git yet.")
        XCTAssertEqual(WatchDiffFile(path: "b", oldPath: "a", status: "R").note, "Renamed from a.")
        XCTAssertEqual(sample().files[0].note, "")
    }

    func testFittedShortensLongLinesAndStaysUnderTheCap() {
        var diff = sample(files: 1, lines: 2)
        diff.files[0].lines[1].text = String(repeating: "y", count: 500)
        let short = WatchWire.fitted(diff)
        XCTAssertEqual(short.files[0].lines[1].text.count, WatchDiff.maxLineChars)
        XCTAssertTrue(short.files[0].lines[1].text.hasSuffix("\u{2026}"))

        let big = sample(files: 40, lines: 200)
        XCTAssertGreaterThan(WatchWire.encodedSize(big), WatchWire.maxBodyBytes)
        let fitted = WatchWire.fitted(big)
        XCTAssertLessThanOrEqual(WatchWire.encodedSize(fitted), WatchWire.maxBodyBytes)
        XCTAssertEqual(fitted.files.count, 40)
        XCTAssertEqual(fitted.hiddenFiles, 0)
        for file in fitted.files {
            XCTAssertEqual(file.lines.count + file.hiddenLines, 201)
        }
        XCTAssertEqual(fitted.added, big.added)
    }

    func testFittedDropsTrailingFilesLast() {
        let many = sample(files: 900, lines: 0)
        let fitted = WatchWire.fitted(many, maxBytes: 20_000)
        XCTAssertLessThanOrEqual(WatchWire.encodedSize(fitted), 20_000)
        XCTAssertGreaterThan(fitted.hiddenFiles, 0)
        XCTAssertEqual(fitted.files.count + fitted.hiddenFiles, 900)
        XCTAssertEqual(fitted.files.first?.path, "src/file0.swift")
        XCTAssertTrue(fitted.files.allSatisfy { $0.lines.isEmpty })
    }

    func testOlderShapesDecode() throws {
        let json = #"{"key":"k","ok":true,"files":[{"path":"a","g":"stash","l":[{"k":"?","t":"x"}]}]}"#
        let back = try JSONDecoder().decode(WatchDiff.self, from: Data(json.utf8))
        XCTAssertEqual(back.files[0].group, .committed)
        XCTAssertEqual(back.files[0].lines[0].kind, .context)
        XCTAssertEqual(back.files[0].status, "M")
    }

    func testDiffControlIsItsOwnBit() {
        let controls: WireControls = [.stop, .diff]
        XCTAssertTrue(controls.contains(.diff))
        XCTAssertFalse(WireControls.prompt.contains(.diff))
    }
}
