import FMApi
import XCTest

final class PublishedRecordsTests: XCTestCase {
    func testSplitLinesKeepsItsLineBreakRules() {
        XCTAssertEqual(PublishedRecords.splitLines(""), [])
        XCTAssertEqual(PublishedRecords.splitLines("a"), ["a"])
        XCTAssertEqual(PublishedRecords.splitLines("a\n"), ["a"])
        XCTAssertEqual(PublishedRecords.splitLines("a\n\n"), ["a", ""])
        XCTAssertEqual(PublishedRecords.splitLines("a\r\nb\rc\nd"), ["a", "b", "c", "d"])
        XCTAssertEqual(PublishedRecords.splitLines("\r\r\n"), ["", ""])
        XCTAssertEqual(PublishedRecords.splitLines("e\u{301}\r\n\u{1F600}"), ["e\u{301}", "\u{1F600}"])
    }

    func testEachLineStopsWhenAsked() {
        var seen: [String] = []
        PublishedRecords.eachLine("one\ntwo\nthree") { line in
            seen.append(line)
            return line != "two"
        }
        XCTAssertEqual(seen, ["one", "two"])
    }

    func testFrontMatterReadsOnlyTheLeadingBlock() {
        let md = "---\ntitle: Seven\nstatus: merged\n---\n# Heading\nstatus: not this\n"
        XCTAssertEqual(PublishedRecords.frontMatter(md), ["title": "Seven", "status": "merged"])
        XCTAssertEqual(PublishedRecords.frontMatter("title: x\n---\n"), [:])
        XCTAssertEqual(PublishedRecords.frontMatter(""), [:])
        XCTAssertEqual(PublishedRecords.firstHeading("---\nkey: v\n---\n\n## Fix it\nmore"), "Fix it")
        XCTAssertNil(PublishedRecords.firstHeading("---\nkey: v\n---\n"))
    }

    func testAPullHeadCarriesTheFrontMatterWithoutBodyOrPatch() {
        let patch = String(repeating: "+line\n", count: 5_000)
        let md = "---\ntitle: Seven\nstatus: merged\nhead: agent/7\nbase: main\nsig: abc\nauthor: k\nts: 12\n---\nBody.\n```diff\n\(patch)```\n"
        let head = PublishedRecords.pullHeadFromMarkdown("7", md)
        let full = PublishedRecords.pullFromMarkdown("7", md)
        XCTAssertEqual(head.number, 7)
        XCTAssertEqual(head.title, full.title)
        XCTAssertEqual(head.status, full.status)
        XCTAssertEqual(head.head, full.head)
        XCTAssertEqual(head.base, full.base)
        XCTAssertEqual(head.signed, full.signed)
        XCTAssertEqual(head.author, full.author)
        XCTAssertEqual(head.createdMs, full.createdMs)
        XCTAssertEqual(head.body, "")
        XCTAssertEqual(head.patch, "")
        XCTAssertFalse(full.patch.isEmpty)
        XCTAssertEqual(PublishedRecords.pullHeadFromMarkdown("8", "# Untitled pull\n").title, "Untitled pull")
        XCTAssertEqual(PublishedRecords.pullHeadFromMarkdown("9", "").title, "Pull request #9")
        XCTAssertEqual(PublishedRecords.pullHeadFromMarkdown("9", "").status, "open")
    }
}
