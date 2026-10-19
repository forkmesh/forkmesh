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
        XCTAssertEqual(quiet.scrollDepth, 100)
        XCTAssertEqual(CrmActivity(json: ["id": "e3"]).engagementLine, "")
    }
}
