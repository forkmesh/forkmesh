package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.api.DartException
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.model.PullEvent
import com.forkmesh.nativeapp.model.RemoteFork
import com.forkmesh.nativeapp.model.RepoComparison
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class PullComposeTest {
    private val fork = RemoteFork("https://other.example/bob/tool", "https://other.example", "other.example", "bob", "tool")

    fun testRemoteForksParseOnlySafeOriginsAndNames() {
        val forks = RemoteFork.list(
            mapOf(
                "forks" to listOf(
                    mapOf("origin" to "HTTPS://Other.Example", "owner" to "bob", "name" to "tool", "label" to "Bob"),
                    mapOf("origin" to "http://plain.example", "owner" to "bob", "name" to "tool"),
                    mapOf("origin" to "https://ok.example:8443", "owner" to "../x", "name" to "tool"),
                    mapOf("origin" to "https://ok.example:8443", "owner" to "carol", "name" to "tool", "host" to "ok.example:8443"),
                ),
            ),
        )
        assertEq(listOf("bob/tool on other.example", "carol/tool on ok.example:8443"), forks.map { it.displayName })
        assertEq("https://other.example/bob/tool", forks[0].url)
        assertEq("https://other.example/api/repo/bob/tool/pulls", forks[0].pullsUrl)
        assertEq(emptyList<RemoteFork>(), RemoteFork.list(null))
        assertEq("app.forkmesh.com", RemoteFork.instanceHost("wss://app.forkmesh.com/ws"))
        assertEq("box.example:8443", RemoteFork.instanceHost("https://box.example:8443"))
        assertEq("box.example/alice/tool:fix/a", RemoteFork.signedHead("box.example", "alice", "tool", "fix/a"))
    }

    fun testCompareFormChecksBranchesAndFillsTheTitle() {
        assertEq(PullCompose.PICK_BRANCHES, PullCompose.compareProblem("main", " "))
        assertEq(PullCompose.SAME_BRANCH, PullCompose.compareProblem("main", "main"))
        assertEq(null, PullCompose.compareProblem("main", "fix/a"))
        assertEq(listOf("dev", "fix/a"), PullCompose.headChoices(listOf("main", "dev", "fix/a"), "main"))
        val empty = PullCompose.compared(RepoComparison(patch = " "), "fix/a", "")
        assertEq(null, empty.comparison)
        assertEq(PullCompose.NO_CHANGES, empty.error)
        val found = PullCompose.compared(RepoComparison(commitCount = 2, patch = "diff --git a/x b/x"), "agent/fix-login_flow", "")
        assertEq("Fix login flow", found.title)
        assertEq("2 commits to merge.", PullCompose.ableToMerge(found.comparison!!))
        assertEq("Kept", PullCompose.compared(RepoComparison(patch = "diff"), "fix/a", "Kept").title)
        assertEq("1 commit to merge.", PullCompose.ableToMerge(RepoComparison(commitCount = 1)))
        assertEq("Ready to send.", PullCompose.ableToMerge(RepoComparison()))
        assertEq("Body", PullCompose.description("Body\n\r\n"))
    }

    fun testSubmitRulesAndRemoteMessages() {
        assertEq(PullCompose.TITLE_REQUIRED, PullCompose.submitProblem(" ", "diff", false, null, "h"))
        assertEq(PullCompose.PICK_BRANCHES, PullCompose.submitProblem("T", "", false, null, "h"))
        assertEq(PullCompose.PICK_BRANCHES, PullCompose.submitProblem("T", "diff", true, fork, "h"))
        assertEq(null, PullCompose.submitProblem("T", "diff", false, fork, "h"))
        expect(!PullCompose.canSubmit("T", "diff", true, fork))
        expect(PullCompose.canSubmit("T", "diff", true, null))
        assertEq("Create pull request", PullCompose.confirmLabel(null))
        assertEq("Send pull request", PullCompose.confirmLabel(fork))
        assertEq(PullCompose.THIS_REPOSITORY, PullCompose.targetLabel(null))
        assertEq("Sent to bob/tool on other.example as pull request #4.", PullCompose.sentRemote(fork, 4))
        assertEq("Sent to bob/tool on other.example. It is reviewed there.", PullCompose.sentRemote(fork, 0))
        expect(PullCompose.remoteError(DartException("remote_unreachable"), fork).startsWith("Nothing was delivered: other.example"))
        assertEq("bob/tool was not found on other.example.", PullCompose.remoteError(DartException("not_found"), fork))
        assertEq("Could not send the pull request to bob/tool on other.example (boom).", PullCompose.remoteError(DartException("boom"), fork))
    }

    fun testPullStatePillSummaryAndDoneCopy() {
        val now = 1_800_000_000_000L
        val open = PublishedPull(3, "Fix", base = "main", head = "fix/a", author = "abcdefghijklmnop", createdMs = now - 3_600_000)
        assertEq(PullState.Open, PullState.of(open))
        assertEq("abcdefgh… wants to merge 3 commits into main from fix/a", PullState.summary(open, 3, now))
        assertEq("abcdefgh… opened this pull request · 1h ago", PullDetail.openedLine(open, now))
        val merged = open.copy(status = "merged", authorName = "Ann", mergedMs = (now - 120_000) / 1000)
        assertEq(PullState.Merged, PullState.of(merged))
        assertEq("Ann's changes merged into main from fix/a 2m ago", PullState.summary(merged, 0, now))
        assertEq("fix/a is in main. Nothing more can be merged from this pull request; comments are still welcome.", PullState.mergedCopy(merged))
        assertEq(PullState.Merged, PullState.of(open.copy(status = "closed-merged")))
        val closed = open.copy(status = "closed", authorName = "Ann")
        assertEq(PullState.Closed, PullState.of(closed))
        assertEq("Ann proposed 1 commit into main from fix/a · closed without merging", PullState.summary(closed, 1, now))
        expect(PullState.reviewable(open) && !PullState.reviewable(merged) && !PullState.reviewable(closed))
        assertEq("These changes are in the base branch. Nothing more can be merged from this pull request; comments are still welcome.", PullState.mergedCopy(PublishedPull(1, "x")))
    }

    fun testMergedEntryIsPlacedByTimeAmongTheEvents() {
        val events = listOf(
            PullEvent("comment", body = "first", ts = 1_000L),
            PullEvent("review", state = "approve", ts = 3_000_000L),
        )
        val pull = PublishedPull(5, "Fix", status = "merged", base = "main", head = "fix/a", events = events, author = "k", authorName = "Ann", mergedBy = "k", mergedMs = 2_000_000L)
        val merged = PullTimeline.mergedEvent(pull)!!
        assertEq("Merged", merged.displayTitle)
        assertEq("Merged fix/a into main.", merged.body)
        assertEq("Ann", PullTimeline.author(merged))
        assertEq(listOf("Comment", "Merged", "Approved"), PullTimeline.events(pull).map { it.displayTitle })
        assertEq("Merged", PullTimeline.events(pull.copy(mergedMs = 0)).last().displayTitle)
        assertEq("", PullTimeline.mergedEvent(pull.copy(mergedBy = ""))!!.author)
        assertEq(null, PullTimeline.mergedEvent(pull.copy(status = "open")))
        assertEq(events, PullTimeline.events(pull.copy(status = "open")))
        assertEq(CollabTone.AccentEdge, PullTimeline.tone(merged))
        assertEq("call_merge", PullTimeline.icon(merged))
        assertEq("Closed", PullEvent("status", state = "closed").displayTitle)
        assertEq("Reopened", PullEvent("status", state = "open").displayTitle)
        assertEq(CollabTone.Danger, PullTimeline.tone(PullEvent("status", state = "closed")))
        assertEq(5_000L, PullTimeline.epochMs(5L))
        assertEq(1_800_000_000_000L, PullTimeline.epochMs(1_800_000_000_000L))
    }
}
