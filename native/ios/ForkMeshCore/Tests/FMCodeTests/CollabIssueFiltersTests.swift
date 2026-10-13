// The repo issue filters (taps become model changes), as in native/android
// RepoIssueFiltersTest.kt.

import FMApi
import XCTest
@testable import FMCode

/// One issue per filter dimension so a single selection keeps exactly the rows it should.
let filterFixtureIssues: [Issue] = [
    Issue(
        number: 4, title: "Signed inbox drops replies", status: "open", author: "alice", labels: ["mobile", "signed"],
        milestone: "v2 mobile", assignees: ["mona"]),
    Issue(
        number: 3, title: "World scene leaks textures", status: "open", author: "kai", labels: ["world"],
        milestone: "v2 mobile", assignees: ["preview-node"]),
    Issue(number: 2, title: "Mirror roster goes stale", status: "open", author: "alice", labels: ["mirrors"]),
    Issue(
        number: 1, title: "Relay retries forever", status: "closed", author: "kai", labels: ["mobile"], milestone: "v1",
        assignees: ["kai"]),
]

final class CollabIssueFiltersTests: XCTestCase {
    private let issues = filterFixtureIssues

    /// What the tab renders: normalize, then match, as `_IssuesTabState.build` does.
    private func visible(_ filters: IssueFilters, account: String = "") -> [String] {
        let normalized = normalizeIssueFilters(filters, issues, account: account)
        return issues.filter { issueMatchesFilters($0, normalized, account: account) }.map(\.title)
    }

    /// `_pickFilter`: the option with this label must be on offer.
    private func pick(
        _ filters: IssueFilters, _ field: IssueFilterField, _ label: String, account: String = "",
        file: StaticString = #filePath, line: UInt = #line
    ) -> IssueFilters {
        guard let option = issueFilterOptions(field, issues, account: account).last(where: { $0.label == label }) else {
            XCTFail("\(label) is not offered for \(field.rawValue)", file: file, line: line)
            return filters
        }
        return filters.selecting(field, option.value)
    }

    private let openTitles = ["Signed inbox drops replies", "World scene leaks textures", "Mirror roster goes stale"]

    func testAuthorFilterKeepsOnlyThatAuthorsIssues() {
        var filters = IssueFilters()
        XCTAssertEqual(visible(filters), openTitles)

        filters = pick(filters, .author, "kai")
        XCTAssertEqual(visible(filters), ["World scene leaks textures"])

        // Back to "Any author" restores the full open list.
        filters = pick(filters, .author, "Any author")
        XCTAssertEqual(visible(filters), openTitles)
    }

    func testLabelFilterMatchesAnyLabelOnAnIssue() {
        XCTAssertEqual(visible(pick(IssueFilters(), .label, "signed")), ["Signed inbox drops replies"])
    }

    func testMilestoneFilterOffersTheIssuesOwnMilestonesAndNone() {
        var filters = pick(IssueFilters(), .milestone, "v2 mobile")
        XCTAssertEqual(visible(filters), ["Signed inbox drops replies", "World scene leaks textures"])

        filters = pick(filters, .milestone, "No milestone")
        XCTAssertEqual(visible(filters), ["Mirror roster goes stale"])
        XCTAssertEqual(
            issueFilterOptions(.milestone, issues, account: "").map(\.label),
            ["Any milestone", "No milestone", "v1", "v2 mobile"])
    }

    func testAssigneeFilterCoversANamedAccountAndUnassigned() {
        var filters = pick(IssueFilters(), .assignee, "mona")
        XCTAssertEqual(visible(filters), ["Signed inbox drops replies"])

        filters = pick(filters, .assignee, "Unassigned")
        XCTAssertEqual(visible(filters), ["Mirror roster goes stale"])
    }

    func testAssignedToMeUsesTheSignedInAccount() {
        let filters = pick(IssueFilters(), .assignee, "Assigned to me", account: "preview-node")
        XCTAssertEqual(filters.valueOf(.assignee), issueFilterMe)
        XCTAssertEqual(visible(filters, account: "preview-node"), ["World scene leaks textures"])
    }

    func testAssignedToMeIsHiddenWhileSignedOut() {
        let labels = issueFilterOptions(.assignee, issues, account: "").map(\.label)
        XCTAssertTrue(labels.contains("Unassigned"))
        XCTAssertFalse(labels.contains("Assigned to me"))
        XCTAssertEqual(labels, ["Anyone", "Unassigned", "kai", "mona", "preview-node"])

        // A selection made while signed in is dropped once the account is gone.
        let signedIn = pick(IssueFilters(), .assignee, "Assigned to me", account: "preview-node")
        let signedOut = normalizeIssueFilters(signedIn, issues, account: "")
        XCTAssertEqual(signedOut.valueOf(.assignee), "")
        XCTAssertFalse(signedOut.hasMetadataFilter)
    }

    func testStatusSwitchMovesBetweenOpenClosedAndAll() {
        // Open is the default and hides the closed issue.
        XCTAssertFalse(visible(IssueFilters()).contains("Relay retries forever"))

        let closed = IssueFilters().withStatus(.closed)
        XCTAssertEqual(visible(closed), ["Relay retries forever"])

        let all = closed.withStatus(.all)
        XCTAssertTrue(visible(all).contains("Relay retries forever"))
        XCTAssertTrue(visible(all).contains("Signed inbox drops replies"))
        XCTAssertEqual(IssueStatusFilter.allCases.map(\.rawValue), ["open", "closed", "all"])
        XCTAssertEqual(IssueStatusFilter.allCases.map(\.label), ["Open", "Closed", "All"])
    }

    func testOpenMeansNotClosed() {
        XCTAssertTrue(IssueStatusFilter.open.matches(Issue(number: 1, title: "t", status: "reopened")))
        XCTAssertFalse(IssueStatusFilter.closed.matches(Issue(number: 1, title: "t", status: "reopened")))
    }

    func testFiltersCombineAndCanBeClearedInOneTap() {
        var filters = pick(IssueFilters(), .author, "alice")
        filters = pick(filters, .label, "mirrors")
        XCTAssertEqual(visible(filters), ["Mirror roster goes stale"])
        XCTAssertTrue(filters.hasMetadataFilter)

        filters = filters.clearingMetadata()
        XCTAssertEqual(visible(filters), openTitles)
        // Clearing the metadata pills leaves the open/closed switch alone.
        XCTAssertEqual(filters.status, .open)
    }

    func testAnEmptyResultKeepsTheClearControlAvailable() {
        var filters = pick(IssueFilters(), .author, "alice")
        filters = pick(filters, .label, "world")
        XCTAssertEqual(visible(filters), [])
        XCTAssertTrue(normalizeIssueFilters(filters, issues, account: "").hasMetadataFilter)
        XCTAssertTrue(visible(filters.clearingMetadata()).contains("World scene leaks textures"))
    }

    func testFilterSelectionsSurviveAsAValue() {
        var filters = pick(IssueFilters(), .author, "kai")
        filters = filters.withStatus(.all)
        let kept = filters
        XCTAssertEqual(visible(kept), ["World scene leaks textures", "Relay retries forever"])
        XCTAssertEqual(
            issueFilterSelection(issueFilterOptions(.author, issues, account: ""), kept.valueOf(.author)).label, "kai")
    }

    func testValuesAreDistinctFirstSpellingSortedCaseInsensitively() {
        let mixed = [
            Issue(number: 1, title: "a", labels: ["Zeta", " beta ", ""]),
            Issue(number: 2, title: "b", labels: ["BETA", "alpha"]),
        ]
        XCTAssertEqual(issueFilterValues(.label, mixed), ["alpha", "beta", "Zeta"])
        let selected = IssueFilters().selecting(.label, "beta")
        XCTAssertTrue(issueMatchesFilters(mixed[1], selected, account: ""), "matching ignores case")
    }

    func testAStaleSelectionFallsBackToTheAnyEntry() {
        let options = issueFilterOptions(.author, issues, account: "")
        XCTAssertEqual(issueFilterSelection(options, "nobody").label, "Any author")
        XCTAssertEqual(
            normalizeIssueFilters(IssueFilters().selecting(.author, "nobody"), issues, account: "").valueOf(.author), "")
    }

    func testFieldCaptionsAndKeys() {
        XCTAssertEqual(IssueFilterField.allCases.map(\.rawValue), ["author", "label", "milestone", "assignee"])
        XCTAssertEqual(IssueFilterField.allCases.map(\.title), ["Author", "Label", "Milestone", "Assignee"])
        XCTAssertEqual(
            IssueFilterField.allCases.map(\.anyLabel), ["Any author", "Any label", "Any milestone", "Anyone"])
        XCTAssertEqual(issueFilterNone, "__forkmesh_none__")
        XCTAssertEqual(issueFilterMe, "__forkmesh_me__")
    }
}
