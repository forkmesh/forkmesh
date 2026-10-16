package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.api.DartException
import com.forkmesh.nativeapp.model.Issue
import com.forkmesh.nativeapp.model.IssueBounty
import com.forkmesh.nativeapp.model.IssueEvent
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.model.PullEvent
import com.forkmesh.nativeapp.model.RepoDiscussion
import com.forkmesh.nativeapp.model.DiscussionEvent
import com.forkmesh.nativeapp.model.solanaExplorerAddressUrl
import com.forkmesh.nativeapp.model.solanaExplorerSignatureUrl
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class RepoCollaborationTest {
    private val issue = Issue(
        number = 12,
        title = "Stabilize signed inbox",
        status = "open",
        body = "Mobile should make signed collaboration clear.",
        author = "alice",
        labels = listOf("mobile", "signed"),
        milestone = "v2 mobile",
        priority = 3,
        assignees = listOf("mona", "kai"),
        votes = 2,
        bountyUsd = 150.0,
        bountyAddress = "escrow-fixture",
        bountyStatus = "funded",
        bountyRequiredLamports = 1500000000,
        bountyReceivedLamports = 750000000,
        bountyAmountSol = 1.5,
        bountyPayUri = "solana:escrow-fixture?amount=1.5",
        bountyPayee = "maintainer-node",
        bountyPayoutSig = "payout-fixture",
        events = listOf(IssueEvent(type = "comment", body = "I can reproduce this on mobile.", authorName = "Mona")),
    )

    private val pull = PublishedPull(
        number = 7,
        title = "Add signed review flow",
        status = "open",
        body = "Adds mobile review controls.",
        base = "main",
        head = "review/mobile",
        patch = "diff --git a/lib/a.dart b/lib/a.dart\n+review",
        signed = true,
        events = listOf(
            PullEvent(type = "comment", body = "Review changes before merge.", authorName = "Kai"),
            PullEvent(type = "review", state = "approve", body = "LGTM from mobile.", authorName = "Mona"),
            PullEvent(type = "review", state = "changes_requested", body = "Please add a regression test.", authorName = "Lin"),
        ),
    )

    fun testIssueDetailSurfacesSignedCollaborationState() {
        val labels = IssueDetail.chips(issue, "owner/repo").map { it.label }
        for (expected in listOf("mobile", "signed", "2 votes", "$150 bounty", "priority 3", "v2 mobile", "mona", "kai")) {
            assertEq(1, labels.count { it == expected }, expected)
        }
        assertEq("open", labels.first())
        assertEq("owner/repo", labels.last())
        assertEq("1 vote", IssueDetail.chips(issue.copy(votes = 1), "o/r").first { it.icon == "how_to_vote_outlined" }.label)
        expect(PENDING_NOTE.contains("pending the repo owner applying"))
    }

    fun testIssueDetailShowsMigrationOnlyBountyState() {
        assertEq("$150 pledged", IssueDetail.pledgeText(issue))
        assertEq(null, IssueDetail.pledgeText(issue.copy(bountyUsd = 0.0)))
        assertEq("funded", issue.bountyStatusLabel)
        assertEq("750000000 / 1500000000 lamports", issue.bountyProgressLabel)
        expect(IssueDetail.NON_CUSTODIAL_COPY.contains("live Worker creation, funding, signing, and payout"))
    }

    fun testIssueBountyCardShowsCopyableTransactionLinks() {
        assertEq("https://explorer.solana.com/address/escrow-fixture", solanaExplorerAddressUrl(issue.bountyAddress))
        assertEq("https://explorer.solana.com/tx/payout-fixture", solanaExplorerSignatureUrl(issue.bountyPayoutSig))
    }

    fun testBountyRefreshWithoutALegacyRecordKeepsTheIssue() {
        val (kept, message) = IssueDetail.bountyRefreshed(issue, IssueBounty())
        assertEq(issue, kept)
        assertEq("No historical bounty migration record was found.", message)
        expect(!IssueDetail.bountyMessageIsError(message))

        val refreshed = IssueDetail.bountyRefreshed(
            Issue(number = 1, title = "t"),
            IssueBounty(address = "escrow-fixture", status = "funded", amountUsd = 150.0),
        )
        assertEq("escrow-fixture", refreshed.first.bountyAddress)
        assertEq("Historical migration status refreshed.", refreshed.second)

        val failed = IssueDetail.bountyRefreshFailed(DartException("HTTP 500"))
        assertEq("Refresh failed: Exception: HTTP 500", failed)
        expect(IssueDetail.bountyMessageIsError(failed))
    }

    fun testOnlyTheDesktopRequestsThatFitThePullsStateAreOffered() {
        assertEq(
            listOf("pull.fix_agent", "pull.close", "pull.delete"),
            PullShipCommands.offered(pull).map { it.command },
        )
        assertEq(
            listOf("Fix with agent", "Close", "Delete"),
            PullShipCommands.offered(pull).map { it.chipLabel },
        )
        assertEq(
            listOf("pull.fix_agent", "pull.close", "pull.delete"),
            PullShipCommands.offered(pull.copy(head = "")).map { it.command },
        )
        assertEq(listOf("pull.reopen", "pull.delete"), PullShipCommands.offered(pull.copy(status = "closed")).map { it.command })
        assertEq(listOf("pull.delete"), PullShipCommands.offered(pull.copy(status = "merged")).map { it.command })
    }

    fun testPullDetailQueuesSignedDesktopMergeRequests() {
        assertEq("owner", DesktopCommand.ownerAccount("Owner ", "key", "key"))
        expect(PullDetail.canRequestMerge(pull))
        expect(PullDetail.shipControlsCopy(pull).contains("desktop node will verify mergeability"))

        val commands = PullShipCommands.all(pull)
        assertEq(
            listOf("pull.merge", "pull.merge_delete", "pull.fix_agent", "pull.close", "pull.reopen", "pull.delete"),
            commands.map { it.command },
        )
        assertEq(
            listOf(
                "Request merge on desktop",
                "Request merge + delete branch",
                "Request fix with agent",
                "Request close on desktop",
                "Request reopen on desktop",
                "Request delete on desktop",
            ),
            commands.map { it.label },
        )
        assertEq("pull:7", PullDetail.commandTarget(pull))
        assertEq(mapOf("pullNumber" to 7, "base" to "main", "head" to "review/mobile"), PullDetail.commandPayload(pull))
        assertEq(mapOf<String, Any?>("pullNumber" to 7), PullDetail.commandPayload(pull.copy(base = "", head = "")))
        assertEq(
            "forkmesh-desktop-command-v1\nowner\nrepo\npull.merge\npull:7\n1700000000000",
            DesktopCommand.canonical("owner", "repo", "pull.merge", "pull:7", "1700000000000"),
        )
        assertEq(
            "Merge queued. Approve it in the ForkMesh desktop app to merge pull request #7.",
            commands.first().successMessage,
        )
    }

    fun testDesktopRequestsNeedTheOwnersPairedKey() {
        fun message(block: () -> Unit) = assertFails<DartException>(block).toString()
        assertEq(
            "Exception: Sign in with the repo owner account to request desktop control.",
            message { DesktopCommand.ownerAccount(null, null, "key") },
        )
        assertEq(
            "Exception: Sign in with the repo owner account to request desktop control.",
            message { DesktopCommand.ownerAccount("owner", "key", null) },
        )
        assertEq("mona", DesktopCommand.ownerAccount("Mona", "key", "key"))
        assertEq(
            "Exception: Pair this mobile device with the owner key before requesting desktop control.",
            message { DesktopCommand.ownerAccount("owner", "other", "key") },
        )
        val closed = pull.copy(status = "merged")
        expect(!PullDetail.canRequestMerge(closed))
        assertEq(
            "This pull request is merged; merge/review-changing requests may be rejected by the desktop node if the local state does not support them.",
            PullDetail.shipControlsCopy(closed),
        )
    }

    fun testThreadAlertsSignWithThePairedAccountNode() {
        assertEq("preview-node", ThreadSubscription.signedAccountNode("Preview-Node", "key", "key"))
        assertEq("", ThreadSubscription.signedAccountNode(null, null, "key"))
        assertEq("", ThreadSubscription.signedAccountNode("preview-node", "key", null))
        assertEq("", ThreadSubscription.signedAccountNode("  ", "key", "key"))
        assertEq("", ThreadSubscription.signedAccountNode("preview-node", "other", "key"))

        assertEq("Subscribe thread", ThreadSubscription.label(guest = false, canSign = true, subscribed = false))
        assertEq("Unsubscribe thread", ThreadSubscription.label(guest = false, canSign = true, subscribed = true))
        assertEq("Thread alerts unavailable", ThreadSubscription.label(guest = false, canSign = false, subscribed = false))
        assertEq("Sign in for thread alerts", ThreadSubscription.label(guest = true, canSign = true, subscribed = true))
        assertEq("lock_outline", ThreadSubscription.icon(guest = false, canSign = false, subscribed = false))
        assertEq("notifications_off_outlined", ThreadSubscription.icon(guest = false, canSign = true, subscribed = true))
        assertEq("notifications_active_outlined", ThreadSubscription.icon(guest = false, canSign = true, subscribed = false))
        assertEq("Subscribed to thread notifications.", ThreadSubscription.toggled(true))
        assertEq("Unsubscribed from thread notifications.", ThreadSubscription.toggled(false))
    }

    fun testNewDiscussionCategoriesAndDiscussionDetail() {
        assertEq(listOf("general", "ideas", "help", "show-and-tell"), DISCUSSION_CATEGORY_LABELS.keys.toList())
        assertEq("ideas", DISCUSSION_CATEGORY_LABELS.entries.first { it.value == "Ideas" }.key)

        val discussion = RepoDiscussion(
            number = 3,
            title = "Mobile maintainer workflow",
            body = "How should maintainers review from phones?",
            author = "alice",
            category = "ideas",
            events = listOf(DiscussionEvent(type = "comment", body = "Keep desktop node as the canonical apply surface.", authorName = "Mona")),
        )
        assertEq(listOf("discussion", "ideas", "alice", "owner/repo"), DiscussionDetail.chips(discussion, "owner/repo").map { it.label })
        assertEq("#3 · ideas · alice", DiscussionCard.subtitle(discussion))
        assertEq("No description provided.", DiscussionDetail.body(discussion.copy(body = "")))
    }

    fun testReviewComposersTitleAndValidate() {
        assertEq("Approve PR #7", PullReview.composerTitle("approve", 7))
        expect(PullReview.composerHelper("approve").startsWith("Approval is signed and sent to the pull inbox"))
        assertEq(null, PullReview.validationError("approve", ""))

        assertEq("Request changes on PR #7", PullReview.composerTitle("request-changes", 7))
        assertEq("A note is required to request changes.", PullReview.validationError("request-changes", "  "))
        assertEq(null, PullReview.validationError("request-changes", "Please cover the empty state before owner apply."))

        assertEq("Review comment on PR #7", PullReview.composerTitle("comment", 7))
        assertEq("review/mobile → main · open", PullReview.subtitle(pull))
    }

    fun testPullDetailRendersSignedCommentAndReviewTimeline() {
        assertEq(listOf("Comment", "Approved", "Requested changes"), pull.events.map { it.displayTitle })
        assertEq(
            listOf(CollabTone.Tertiary, CollabTone.Success, CollabTone.Danger),
            pull.events.map { PullTimeline.tone(it) },
        )
        assertEq(
            listOf("rate_review_outlined", "check_circle_outline", "cancel_outlined"),
            pull.events.map { PullTimeline.icon(it) },
        )
        assertEq("#7 · review/mobile → main", PullCard.subtitle(pull))
        assertEq("signed", PullCard.badge(pull))
        assertEq("merged", PullCard.badge(pull.copy(signed = false, status = "merged")))
        assertEq(CollabTone.AccentEdge, PullCard.tone(pull.copy(status = "merged")))
        assertEq(CollabTone.Danger, PullCard.tone(pull.copy(status = "closed")))
        assertEq(CollabTone.Success, PullCard.tone(pull))
        assertEq("#7", PullCard.subtitle(pull.copy(head = "", base = "")))
    }

    fun testRowBadgesTakeTheirToneFromTheirWords() {
        assertEq(CollabTone.Success, badgeTone("open"))
        assertEq(CollabTone.Success, badgeTone("signed"))
        assertEq(CollabTone.Danger, badgeTone("closed"))
        assertEq(CollabTone.Accent, badgeTone("merged"))
        assertEq(CollabTone.Secondary, badgeTone("ideas"))
    }

    fun testFailureNoticesReadLikeDart() {
        assertEq("Failed: Exception: bad_signature", runFailureText(DartException("bad_signature")))
        assertEq(
            "Failed: Invalid argument (node): expected account node name: \"\"",
            runFailureText(IllegalArgumentException("Invalid argument (node): expected account node name: \"\"")),
        )
        assertEq("Failed: Unsupported operation: retired", runFailureText(UnsupportedOperationException("retired")))
    }
}
