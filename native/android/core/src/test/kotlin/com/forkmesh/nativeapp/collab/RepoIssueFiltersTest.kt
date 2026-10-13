package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.model.Issue
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class RepoIssueFiltersTest {
    private val issues = listOf(
        Issue(
            number = 4,
            title = "Signed inbox drops replies",
            status = "open",
            author = "alice",
            labels = listOf("mobile", "signed"),
            milestone = "v2 mobile",
            assignees = listOf("mona"),
        ),
        Issue(
            number = 3,
            title = "World scene leaks textures",
            status = "open",
            author = "kai",
            labels = listOf("world"),
            milestone = "v2 mobile",
            assignees = listOf("preview-node"),
        ),
        Issue(number = 2, title = "Mirror roster goes stale", status = "open", author = "alice", labels = listOf("mirrors")),
        Issue(
            number = 1,
            title = "Relay retries forever",
            status = "closed",
            author = "kai",
            labels = listOf("mobile"),
            milestone = "v1",
            assignees = listOf("kai"),
        ),
    )

    private fun visible(filters: IssueFilters, account: String = ""): List<String> {
        val normalized = normalizeIssueFilters(filters, issues, account)
        return issues.filter { issueMatchesFilters(it, normalized, account) }.map { it.title }
    }

    private fun pick(filters: IssueFilters, field: IssueFilterField, label: String, account: String = ""): IssueFilters {
        val option = issueFilterOptions(field, issues, account).lastOrNull { it.label == label }
            ?: throw AssertionError("$label is not offered for ${field.key}")
        return filters.select(field, option.value)
    }

    fun testAuthorFilterKeepsOnlyThatAuthorsIssues() {
        var filters = IssueFilters()
        assertEq(listOf("Signed inbox drops replies", "World scene leaks textures", "Mirror roster goes stale"), visible(filters))

        filters = pick(filters, IssueFilterField.Author, "kai")
        assertEq(listOf("World scene leaks textures"), visible(filters))

        filters = pick(filters, IssueFilterField.Author, "Any author")
        assertEq(listOf("Signed inbox drops replies", "World scene leaks textures", "Mirror roster goes stale"), visible(filters))
    }

    fun testLabelFilterMatchesAnyLabelOnAnIssue() {
        assertEq(listOf("Signed inbox drops replies"), visible(pick(IssueFilters(), IssueFilterField.Label, "signed")))
    }

    fun testMilestoneFilterOffersTheIssuesOwnMilestonesAndNone() {
        var filters = pick(IssueFilters(), IssueFilterField.Milestone, "v2 mobile")
        assertEq(listOf("Signed inbox drops replies", "World scene leaks textures"), visible(filters))

        filters = pick(filters, IssueFilterField.Milestone, "No milestone")
        assertEq(listOf("Mirror roster goes stale"), visible(filters))
        assertEq(
            listOf("Any milestone", "No milestone", "v1", "v2 mobile"),
            issueFilterOptions(IssueFilterField.Milestone, issues, "").map { it.label },
        )
    }

    fun testAssigneeFilterCoversANamedAccountAndUnassigned() {
        var filters = pick(IssueFilters(), IssueFilterField.Assignee, "mona")
        assertEq(listOf("Signed inbox drops replies"), visible(filters))

        filters = pick(filters, IssueFilterField.Assignee, "Unassigned")
        assertEq(listOf("Mirror roster goes stale"), visible(filters))
    }

    fun testAssignedToMeUsesTheSignedInAccount() {
        val filters = pick(IssueFilters(), IssueFilterField.Assignee, "Assigned to me", account = "preview-node")
        assertEq(ISSUE_FILTER_ME, filters.valueOf(IssueFilterField.Assignee))
        assertEq(listOf("World scene leaks textures"), visible(filters, account = "preview-node"))
    }

    fun testAssignedToMeIsHiddenWhileSignedOut() {
        val labels = issueFilterOptions(IssueFilterField.Assignee, issues, "").map { it.label }
        expect("Unassigned" in labels)
        expect("Assigned to me" !in labels)
        assertEq(listOf("Anyone", "Unassigned", "kai", "mona", "preview-node"), labels)

        val signedIn = pick(IssueFilters(), IssueFilterField.Assignee, "Assigned to me", account = "preview-node")
        val signedOut = normalizeIssueFilters(signedIn, issues, "")
        assertEq("", signedOut.valueOf(IssueFilterField.Assignee))
        expect(!signedOut.hasMetadataFilter)
    }

    fun testStatusSwitchMovesBetweenOpenClosedAndAll() {
        expect("Relay retries forever" !in visible(IssueFilters()))

        val closed = IssueFilters().withStatus(IssueStatusFilter.Closed)
        assertEq(listOf("Relay retries forever"), visible(closed))

        val all = closed.withStatus(IssueStatusFilter.All)
        expect("Relay retries forever" in visible(all))
        expect("Signed inbox drops replies" in visible(all))
        assertEq(listOf("open", "closed", "all"), IssueStatusFilter.entries.map { it.key })
    }

    fun testOpenMeansNotClosed() {
        expect(IssueStatusFilter.Open.matches(Issue(number = 1, title = "t", status = "reopened")))
        expect(!IssueStatusFilter.Closed.matches(Issue(number = 1, title = "t", status = "reopened")))
    }

    fun testFiltersCombineAndCanBeClearedInOneTap() {
        var filters = pick(IssueFilters(), IssueFilterField.Author, "alice")
        filters = pick(filters, IssueFilterField.Label, "mirrors")
        assertEq(listOf("Mirror roster goes stale"), visible(filters))
        expect(filters.hasMetadataFilter)

        filters = filters.clearMetadata()
        assertEq(listOf("Signed inbox drops replies", "World scene leaks textures", "Mirror roster goes stale"), visible(filters))
        assertEq(IssueStatusFilter.Open, filters.status)
    }

    fun testAnEmptyResultKeepsTheClearControlAvailable() {
        var filters = pick(IssueFilters(), IssueFilterField.Author, "alice")
        filters = pick(filters, IssueFilterField.Label, "world")
        assertEq(emptyList<String>(), visible(filters))
        expect(normalizeIssueFilters(filters, issues, "").hasMetadataFilter)
        assertEq(listOf("World scene leaks textures"), visible(filters.clearMetadata()).filter { it.startsWith("World") })
    }

    fun testFilterSelectionsSurviveAsAValue() {
        var filters = pick(IssueFilters(), IssueFilterField.Author, "kai")
        filters = filters.withStatus(IssueStatusFilter.All)
        val kept = filters.copy()
        assertEq(listOf("World scene leaks textures", "Relay retries forever"), visible(kept))
        assertEq("kai", issueFilterSelection(issueFilterOptions(IssueFilterField.Author, issues, ""), kept.valueOf(IssueFilterField.Author)).label)
    }

    fun testValuesAreDistinctFirstSpellingSortedCaseInsensitively() {
        val mixed = listOf(
            Issue(number = 1, title = "a", labels = listOf("Zeta", " beta ", "")),
            Issue(number = 2, title = "b", labels = listOf("BETA", "alpha")),
        )
        assertEq(listOf("alpha", "beta", "Zeta"), issueFilterValues(IssueFilterField.Label, mixed))
        val selected = IssueFilters().select(IssueFilterField.Label, "beta")
        expect(issueMatchesFilters(mixed[1], selected, ""), "matching ignores case")
    }

    fun testAStaleSelectionFallsBackToTheAnyEntry() {
        val options = issueFilterOptions(IssueFilterField.Author, issues, "")
        assertEq("Any author", issueFilterSelection(options, "nobody").label)
        assertEq("", normalizeIssueFilters(IssueFilters().select(IssueFilterField.Author, "nobody"), issues, "").valueOf(IssueFilterField.Author))
    }
}
