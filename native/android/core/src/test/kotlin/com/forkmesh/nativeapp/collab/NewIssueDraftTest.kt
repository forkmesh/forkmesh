package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class NewIssueDraftTest {
    fun testTheComposerReturnsADraft() {
        val draft = NewIssueComposer.draft(
            title = "Crash on launch",
            body = "",
            labels = "mobile, bug",
            priority = "2",
            milestone = "",
            assignees = "",
        )
        expect(draft != null)
        assertEq("Crash on launch", draft!!.title)
        assertEq(listOf("mobile", "bug"), draft.labels)
        assertEq(2, draft.priority)
        assertEq("New issue", NewIssueComposer.title(0))
    }

    fun testSubmittingWithoutATitleKeepsThePageOpenWithAnError() {
        assertEq(null, NewIssueComposer.draft(title = "  ", body = "body", labels = "", priority = "", milestone = "", assignees = ""))
        assertEq("Give the issue a title.", NewIssueComposer.TITLE_REQUIRED)
    }

    fun testNewIssueSubmitsCollaborationMetadata() {
        val draft = NewIssueComposer.draft(
            title = "Polish mobile issues",
            body = "Add the same metadata controls mobile maintainers expect.\n",
            labels = "mobile, signed",
            priority = "4",
            milestone = " v2 mobile ",
            assignees = "mona, kai",
        )!!
        assertEq(
            NewIssueDraft(
                title = "Polish mobile issues",
                body = "Add the same metadata controls mobile maintainers expect.",
                labels = listOf("mobile", "signed"),
                milestone = "v2 mobile",
                priority = 4,
                assignees = listOf("mona", "kai"),
            ),
            draft,
        )
    }

    fun testListsDropEmptyPartsAndPriorityParsesLikeIntTryParse() {
        assertEq(listOf("a", "b"), NewIssueComposer.split(" a,, ,b ,"))
        assertEq(emptyList<String>(), NewIssueComposer.split(""))
        fun priority(text: String) = NewIssueComposer.draft("t", "", "", text, "", "")!!.priority
        assertEq(0, priority("high"))
        assertEq(0, priority(""))
        assertEq(3, priority(" +3 "))
        assertEq(-1, priority("-1"))
        assertEq(16, priority("0x10"))
    }

    fun testASubIssueStartsWithItsParentLine() {
        assertEq("New sub-issue of #12", NewIssueComposer.title(12))
        assertEq("Parent issue: #12\n\n", NewIssueComposer.initialBody(12))
        assertEq("", NewIssueComposer.initialBody(0))
        val draft = NewIssueComposer.draft("Child", NewIssueComposer.initialBody(12), "", "", "", "")!!
        assertEq("Parent issue: #12", draft.body)
    }
}
