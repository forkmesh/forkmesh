// The new issue page as its model, the issue metadata case and native/android
// NewIssueDraftTest.kt.

import FMApi
import XCTest
@testable import FMCode

final class CollabNewIssueTests: XCTestCase {
    @MainActor func testTheComposerReturnsADraft() async {
        let draft = NewIssueComposer.draft(
            title: "Crash on launch", body: "", labels: "mobile, bug", priority: "2", milestone: "", assignees: "")
        XCTAssertEqual(draft?.title, "Crash on launch")
        XCTAssertEqual(draft?.labels, ["mobile", "bug"])
        XCTAssertEqual(draft?.priority, 2)
        XCTAssertEqual(NewIssueComposer.title(0), "New issue")
    }

    @MainActor func testSubmittingWithoutATitleKeepsThePageOpenWithAnError() async {
        XCTAssertNil(NewIssueComposer.draft(
            title: "  ", body: "body", labels: "", priority: "", milestone: "", assignees: ""))
        XCTAssertEqual(NewIssueComposer.titleRequired, "Give the issue a title.")

        let model = NewIssueModel(repo: Repository(owner: "owner", name: "repo"))
        XCTAssertEqual(model.pageTitle, "New issue")
        XCTAssertNil(model.submit())
        XCTAssertEqual(model.titleError, "Give the issue a title.")
        // Typing a title clears the error at once.
        model.title = "Crash on launch"
        XCTAssertNil(model.titleError)
        model.labels = "mobile, bug"
        model.priority = "2"
        XCTAssertEqual(model.submit(), NewIssueDraft(title: "Crash on launch", labels: ["mobile", "bug"], priority: 2))
    }

    @MainActor func testNewIssueSubmitsCollaborationMetadata() async {
        let draft = NewIssueComposer.draft(
            title: "Polish mobile issues", body: "Add the same metadata controls mobile maintainers expect.\n",
            labels: "mobile, signed", priority: "4", milestone: " v2 mobile ", assignees: "mona, kai")
        XCTAssertEqual(draft, NewIssueDraft(
            title: "Polish mobile issues", body: "Add the same metadata controls mobile maintainers expect.",
            labels: ["mobile", "signed"], milestone: "v2 mobile", priority: 4, assignees: ["mona", "kai"]))
    }

    @MainActor func testListsDropEmptyPartsAndPriorityParsesLikeIntTryParse() async {
        XCTAssertEqual(NewIssueComposer.split(" a,, ,b ,"), ["a", "b"])
        XCTAssertEqual(NewIssueComposer.split(""), [])
        func priority(_ text: String) -> Int? {
            NewIssueComposer.draft(title: "t", body: "", labels: "", priority: text, milestone: "", assignees: "")?.priority
        }
        XCTAssertEqual(priority("high"), 0)
        XCTAssertEqual(priority(""), 0)
        XCTAssertEqual(priority(" +3 "), 3)
        XCTAssertEqual(priority("-1"), -1)
        XCTAssertEqual(priority("0x10"), 16)
    }

    @MainActor func testASubIssueStartsWithItsParentLine() async {
        XCTAssertEqual(NewIssueComposer.title(12), "New sub-issue of #12")
        XCTAssertEqual(NewIssueComposer.initialBody(12), "Parent issue: #12\n\n")
        XCTAssertEqual(NewIssueComposer.initialBody(0), "")
        let draft = NewIssueComposer.draft(
            title: "Child", body: NewIssueComposer.initialBody(12), labels: "", priority: "", milestone: "", assignees: "")
        XCTAssertEqual(draft?.body, "Parent issue: #12")

        let model = NewIssueModel(repo: Repository(owner: "owner", name: "repo"), parentNumber: 12)
        XCTAssertEqual(model.pageTitle, "New sub-issue of #12")
        XCTAssertEqual(model.body, "Parent issue: #12\n\n")
    }
}
