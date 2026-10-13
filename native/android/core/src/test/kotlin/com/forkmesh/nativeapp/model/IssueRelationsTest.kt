package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.testing.assertEq

class IssueRelationsTest {
    private fun issue(
        number: Int,
        body: String,
        author: String = "",
        comments: List<IssueEvent> = emptyList(),
    ): Issue = Issue(
        number = number,
        title = "Issue $number",
        body = body,
        author = author,
        events = listOf(
            IssueEvent(type = "open", id = "open-$number", body = body, authorName = author, ts = number.toLong()),
        ) + comments,
    )

    fun testRelationshipLinesNameParentsAndLeaveTheRenderedBody() {
        val body = "Intro #7\nParent issue: #12\nDepends on: #4, #12 and #0\n"
        assertEq(listOf(12, 4), issueRelationNumbers(body))
        assertEq("Intro #7", issueBodyWithoutRelationLines(body))
    }

    fun testBulletedAndBoldRelationshipLinesBackfillParents() {
        val body = "## Delivery\n- Depends on: #481, #484\n* **Parent issue:** #7\n" +
            "- Depends on: None\n"
        assertEq(listOf(481, 484, 7), issueRelationNumbers(body))
        assertEq("## Delivery\n\n- Depends on: None", issueBodyWithoutRelationLines(body))
    }

    fun testReferencesSkipCodeGluedWordsAndEntities() {
        assertEq(listOf(5, 10), issueReferenceNumbers("#5 foo#6 &#7; `#8`\n```\n#9\n```\n#10"))
    }

    fun testSubIssuesComeFromChildrenAndLinkedChecklistRows() {
        val parent = issue(20, "- [ ] Row (sub-issue #24)")
        val issues = listOf(
            parent,
            issue(21, "Parent issue: #20"),
            issue(22, "Depends on: #23, #20"),
            issue(23, "Nothing"),
        )
        assertEq(listOf(21, 22, 24), subIssueNumbersOf(issues, parent))
    }

    fun testMentionsCreditTheAuthorAndSkipDeletedComments() {
        val issues = listOf(
            issue(20, "Target"),
            issue(21, "Parent issue: #20", author = "bob"),
            issue(
                23,
                "`#20` is code",
                author = "dee",
                comments = listOf(
                    IssueEvent(type = "comment", id = "c1", body = "See #20", authorName = "eve", ts = 50),
                    IssueEvent(type = "comment", id = "c2", body = "Also #20", authorName = "fay", ts = 60),
                    IssueEvent(type = "delete", id = "d1", target = "c2", ts = 70),
                ),
            ),
        )
        val mentions = issueMentionsOf(issues, 20)
        assertEq(
            listOf(listOf<Any>(21, "bob", 21L), listOf<Any>(23, "eve", 50L)),
            mentions.map { listOf<Any>(it.sourceNumber, it.authorName, it.ts) },
        )
    }

    fun testMentionsReadTheLatestSignedEdit() {
        val issues = listOf(
            issue(1, "Target"),
            issue(
                2,
                "no reference",
                author = "ann",
                comments = listOf(IssueEvent(type = "edit", target = "open-2", body = "now mentions #1", ts = 9)),
            ),
        )
        assertEq(listOf("ann"), issueMentionsOf(issues, 1).map { it.authorName })
    }
}
