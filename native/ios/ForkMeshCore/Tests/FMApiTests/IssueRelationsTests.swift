// Issue relations, as in native/android IssueRelationsTest.kt.

import XCTest
@testable import FMApi

final class IssueRelationsTests: XCTestCase {
    private func issue(_ number: Int, _ body: String, author: String = "", comments: [IssueEvent] = []) -> Issue {
        Issue(
            number: number, title: "Issue \(number)", body: body, author: author,
            events: [IssueEvent(type: "open", id: "open-\(number)", body: body, authorName: author, ts: number)] + comments)
    }

    func testRelationshipLinesNameParentsAndLeaveTheRenderedBody() {
        let body = "Intro #7\nParent issue: #12\nDepends on: #4, #12 and #0\n"
        XCTAssertEqual(issueRelationNumbers(body), [12, 4])
        XCTAssertEqual(issueBodyWithoutRelationLines(body), "Intro #7")
    }

    func testBulletedAndBoldRelationshipLinesBackfillParents() {
        let body = "## Delivery\n- Depends on: #481, #484\n* **Parent issue:** #7\n- Depends on: None\n"
        XCTAssertEqual(issueRelationNumbers(body), [481, 484, 7])
        XCTAssertEqual(issueBodyWithoutRelationLines(body), "## Delivery\n\n- Depends on: None")
    }

    func testRelationNumbersStopAtTwentyFiveAndIgnoreGluedDigits() {
        let many = "Depends on: " + (1...30).map { "#\($0)" }.joined(separator: ", ")
        XCTAssertEqual(issueRelationNumbers(many), Array(1...25))
        XCTAssertEqual(issueRelationNumbers("Depends on: #12a, #13"), [13])
        XCTAssertEqual(issueRelationNumbers("DEPENDS ON: #3"), [3])
    }

    func testReferencesSkipCodeGluedWordsAndEntities() {
        XCTAssertEqual(issueReferenceNumbers("#5 foo#6 &#7; `#8`\n```\n#9\n```\n#10"), [5, 10])
        XCTAssertEqual(issueReferenceNumbers("~~~\n#1\n~~~\n#2 a/#3 #2"), [2])
    }

    func testSubIssuesComeFromChildrenAndLinkedChecklistRows() {
        let parent = issue(20, "- [ ] Row (sub-issue #24)")
        let issues = [
            parent,
            issue(21, "Parent issue: #20"),
            issue(22, "Depends on: #23, #20"),
            issue(23, "Nothing"),
        ]
        XCTAssertEqual(subIssueNumbersOf(issues, parent), [21, 22, 24])
    }

    func testMentionsCreditTheAuthorAndSkipDeletedComments() {
        let issues = [
            issue(20, "Target"),
            issue(21, "Parent issue: #20", author: "bob"),
            issue(23, "`#20` is code", author: "dee", comments: [
                IssueEvent(type: "comment", id: "c1", body: "See #20", authorName: "eve", ts: 50),
                IssueEvent(type: "comment", id: "c2", body: "Also #20", authorName: "fay", ts: 60),
                IssueEvent(type: "delete", id: "d1", target: "c2", ts: 70),
            ]),
        ]
        let mentions = issueMentionsOf(issues, 20)
        XCTAssertEqual(mentions.map(\.sourceNumber), [21, 23])
        XCTAssertEqual(mentions.map(\.authorName), ["bob", "eve"])
        XCTAssertEqual(mentions.map(\.ts), [21, 50])
    }

    func testMentionsReadTheLatestSignedEdit() {
        let issues = [
            issue(1, "Target"),
            issue(2, "no reference", author: "ann", comments: [
                IssueEvent(type: "edit", target: "open-2", body: "now mentions #1", ts: 9),
            ]),
        ]
        XCTAssertEqual(issueMentionsOf(issues, 1).map(\.authorName), ["ann"])
    }
}
