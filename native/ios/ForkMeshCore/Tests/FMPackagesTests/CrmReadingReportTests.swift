import FMBase
import XCTest

@testable import FMPackages

final class CrmReadingReportTests: XCTestCase {
    func testTheEmailRowCarriesOpensClicksReadTimeAndScroll() {
        let email = CrmActivity(json: [
            "id": "e1", "automated": true, "opens": 2, "clicks": 1, "readSeconds": 120, "scrollDepth": 100,
        ])
        XCTAssertTrue(email.automated)
        XCTAssertEqual(email.readLabel, "2 min+")
        XCTAssertEqual(email.scrollLabel, "to the end")
        XCTAssertEqual(email.engagementLine, "2 opens · 1 click · read 2 min+ · scrolled to the end")
    }

    func testPartialReadsAndUntrackedEmailsStayQuiet() {
        let partial = CrmActivity(json: ["opens": 1, "readSeconds": 30, "scrollDepth": 50])
        XCTAssertEqual(partial.engagementLine, "1 open · read 30s+ · scrolled 50%")
        let quiet = CrmActivity(json: ["id": "e2", "scrollDepth": 400])
        XCTAssertEqual(quiet.reachLabel, "scrolled to the end")
        XCTAssertEqual(quiet.scrollDepth, 100)
        XCTAssertEqual(CrmActivity(json: ["id": "e3"]).engagementLine, "")
    }

    func testADeckReaderShowsSlideReachAndMailApp() {
        let deck = CrmActivity(json: [
            "opens": 1, "readSeconds": 240, "scrollDepth": 64, "slideCount": 14, "slideReached": 9,
            "slideTitle": "Market", "mailClient": "iPhone · Apple Mail", "readingReported": "yes",
        ])
        XCTAssertEqual(deck.engagementLine, "1 open · read 4 min+ · reached slide 9/14 · Market · iPhone · Apple Mail")
        let whole = CrmActivity(json: ["opens": 1, "scrollDepth": 100, "slideCount": 14, "slideReached": 20])
        XCTAssertEqual(whole.slideReached, 14)
        XCTAssertEqual(whole.reachLabel, "read all 14 slides")
    }

    func testAMailAppThatCannotReportSaysSo() {
        let gmail = CrmActivity(json: ["opens": 2, "mailClient": "Gmail", "readingReported": "no"])
        XCTAssertTrue(gmail.silentClient)
        XCTAssertEqual(gmail.engagementLine, "2 opens · Gmail doesn't report read time")
        XCTAssertEqual(CrmActivity(json: ["mailClient": "Gmail", "readingReported": "no"]).engagementLine, "")
    }
}
