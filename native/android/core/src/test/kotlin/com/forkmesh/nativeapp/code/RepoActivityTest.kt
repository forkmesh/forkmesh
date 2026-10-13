package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.api.DartException
import com.forkmesh.nativeapp.api.TestIdentity
import com.forkmesh.nativeapp.model.ActionRun
import com.forkmesh.nativeapp.model.ActionWorkflow
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.model.RepoActions
import com.forkmesh.nativeapp.model.RepoMirror
import com.forkmesh.nativeapp.model.RepoRelease
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class RepoActivityTest {
    private fun session(nodeName: String, pubkey: String) = AuthSession(
        nodeName = nodeName,
        email = "owner@example.com",
        status = "active",
        pubkey = pubkey,
        emailVerified = true,
        isAdmin = false,
        solana = "",
        hasPayoutAddress = false,
        avatarPng = "",
        avatarUpdatedAt = 0,
        createdAt = 1,
        savedAt = 1,
    )

    fun testActionsReadProofIsSignedByThePairedOwnerKey() {
        val identity = TestIdentity()
        val proof = OwnerProofs.actionsRead(session(" Owner ", identity.publicKeyB64url), identity, "owner", "repo", nowMillis = 1234)
        assertEq("owner", proof.ownerAccount)
        assertEq("1234", proof.ts)
        val canonical = "forkmesh-actions-read-v1\nowner\nrepo\n1234"
        expect(TestIdentity.verify(canonical.toByteArray(), proof.sig, identity.publicKeyB64url))
    }

    fun testActionsReadRefusesWithReadableReasons() {
        val identity = TestIdentity()
        val signedOut = assertFails<DartException> { OwnerProofs.actionsRead(null, identity, "owner", "repo") }
        assertEq(
            "Exception: Actions are owner-key gated. Sign in with the repo owner account and local owner key to view workflow logs.",
            signedOut.toString(),
        )
        val other = assertFails<DartException> {
            OwnerProofs.actionsRead(session("mona", identity.publicKeyB64url), identity, "Owner", "repo")
        }
        assertEq("Exception: Actions are owner-key gated. This device is signed in as mona, not Owner.", other.toString())
        val unpaired = assertFails<DartException> { OwnerProofs.actionsRead(session("owner", "other-key"), identity, "owner", "repo") }
        expect(unpaired.toString().contains("Pair this mobile device with the owner key"))
        assertFails<DartException> { OwnerProofs.actionsRead(session("owner", identity.publicKeyB64url), null, "owner", "repo") }
    }

    fun testDesktopCommandIsSignedOverTheCanonicalText() {
        val identity = TestIdentity()
        val proof = OwnerProofs.desktopCommand(
            session("owner", identity.publicKeyB64url), identity, "owner", "repo", "release.draft", "release:v1", nowMillis = 99,
        )
        val canonical = "forkmesh-desktop-command-v1\nowner\nrepo\nrelease.draft\nrelease:v1\n99"
        expect(TestIdentity.verify(canonical.toByteArray(), proof.sig, identity.publicKeyB64url))
        assertEq(
            "Exception: Sign in with the repo owner account to request desktop control.",
            assertFails<DartException> { OwnerProofs.desktopCommand(null, identity, "owner", "repo", "action.rerun", "run:1") }.toString(),
        )
        assertEq(
            "Exception: Only the repo owner can request desktop control for this repo.",
            assertFails<DartException> {
                OwnerProofs.desktopCommand(session("mona", identity.publicKeyB64url), identity, "owner", "repo", "action.rerun", "run:1")
            }.toString(),
        )
        assertEq(
            "Exception: Pair this mobile device with the owner key before requesting desktop control.",
            assertFails<DartException> {
                OwnerProofs.desktopCommand(session("owner", "x"), identity, "owner", "repo", "action.rerun", "run:1")
            }.toString(),
        )
    }

    fun testWorkflowAndRunRows() {
        val workflow = ActionWorkflow(name = "CI", triggers = listOf("push", "pull_request"), stepCount = 1, manual = true, valid = false)
        assertEq(listOf("push, pull_request", "1 step", "Manual run", "Invalid"), RepoActivity.workflowChips(workflow))
        assertEq(listOf("No triggers", "3 steps"), RepoActivity.workflowChips(ActionWorkflow(stepCount = 3)))

        val run = ActionRun(
            id = 4, workflowName = "CI", workflowPath = ".forkmesh/workflows/ci.yml", commit = "0123456789ab",
            ref = "refs/heads/main", status = "success", startedAtMs = 1000, finishedAtMs = 66_000,
        )
        assertEq("main • 01234567", RepoActivity.runSubtitle(run))
        assertEq(listOf("Success", "main", "01234567", "1m 5s", ".forkmesh/workflows/ci.yml"), RepoActivity.runChips(run))
        assertEq("run:4", RepoActivity.rerunTarget(run))
        assertEq(mapOf("runId" to 4, "workflowPath" to ".forkmesh/workflows/ci.yml"), RepoActivity.rerunPayload(run))
        assertEq(mapOf<String, Any?>("runId" to 5), RepoActivity.rerunPayload(ActionRun(id = 5)))
        assertEq("", RepoActivity.runSubtitle(ActionRun(id = 5)))
        assertEq("1 workflows · 1 runs", RepoActivity.actionsCountLabel(RepoActions(listOf(workflow), listOf(run))))
        expect(!RepoActivity.hasActiveRuns(RepoActions(runs = listOf(run))))
        expect(RepoActivity.hasActiveRuns(RepoActions(runs = listOf(run.copy(status = "awaiting_approval")))))
        assertEq("No log has been pushed by the desktop node yet.", RepoActivity.logText("  \n"))
    }

    fun testAgentRows() {
        val agent = AgentSession(
            id = 1, issueNumber = 7, status = "running", provider = "claude-code", branchName = "agent/7", numTurns = 3,
            durationMs = 65_000, costUsd = 0.5, prNumber = 9, filesChanged = 2, ahead = 1, behind = 0, conflicted = true,
            totalTokens = 1200,
        )
        assertEq("Issue #7 • Claude Code • Running", RepoActivity.agentSubtitle(agent))
        assertEq(
            listOf("#7", "Claude Code", "Running", "agent/7", "3 turns", "1m 5s", "$0.50", "PR #9", "2 files · ↑1 ↓0", "Conflict", "1200 tokens"),
            RepoActivity.agentChips(agent, nowMillis = 0),
        )
        assertEq("Agent • Unknown", RepoActivity.agentSubtitle(AgentSession(id = 2)))
        expect(RepoActivity.hasActiveSessions(listOf(agent)))
        assertEq("Done", RepoActivity.transcriptStatusLabel(agent, "completed"))
        assertEq("Running", RepoActivity.transcriptStatusLabel(agent, ""))
        assertEq("mona", RepoActivity.agentOwnerAccount(" Mona ", "owner"))
        assertEq("owner", RepoActivity.agentOwnerAccount(null, "owner"))
        assertEq("No transcript has been pushed by the desktop node yet.", RepoActivity.transcriptText(""))
    }

    fun testBadgeTones() {
        assertEq(BadgeTone.Success, RepoActivity.badgeTone("live"))
        assertEq(BadgeTone.Danger, RepoActivity.badgeTone("offline"))
        assertEq(BadgeTone.Danger, RepoActivity.badgeTone("Changes requested"))
        assertEq(BadgeTone.Accent, RepoActivity.badgeTone("merged"))
        assertEq(BadgeTone.Secondary, RepoActivity.badgeTone("1 step"))
        assertEq(BadgeTone.Danger, RepoActivity.actionStatusTone("Cancelled"))
        assertEq(BadgeTone.Accent, RepoActivity.actionStatusTone("Awaiting approval"))
        assertEq(BadgeTone.Tertiary, RepoActivity.actionStatusTone("Unknown"))
        assertEq(BadgeTone.Success, RepoActivity.agentStatusTone("Merged"))
        assertEq(BadgeTone.Accent, RepoActivity.agentStatusTone("Waiting"))
    }

    fun testMirrorAndSyncRows() {
        assertEq(
            StatusRow("cloud_done_outlined", BadgeTone.Success, "node/repo", "Online mirror", "live"),
            RepoActivity.mirrorRow(RepoMirror(owner = "node", name = "repo", online = true)),
        )
        assertEq(
            StatusRow("cloud_off_outlined", BadgeTone.Tertiary, "repo", "Offline or recently unseen", "offline"),
            RepoActivity.mirrorRow(RepoMirror(owner = "", name = "repo")),
        )
        assertEq(
            StatusRow("warning_amber_outlined", BadgeTone.Warning, "Behind", "2 mirrors lag", "behind"),
            RepoActivity.syncStatusRow(mapOf("severity" to "warning", "label" to "Behind", "summary" to "2 mirrors lag")),
        )
        assertEq(StatusRow("sync", BadgeTone.Accent, "", "", ""), RepoActivity.syncStatusRow(emptyMap()))
    }

    fun testReleaseRequests() {
        assertEq(listOf("release.draft", "release.notes", "release.create", "release.publish"), RepoActivity.releaseRequests.map { it.command })
        val release = RepoRelease(tag = "v1.2.0", channel = "stable")
        assertEq("release:v1.2.0", RepoActivity.releaseTarget(release))
        assertEq(mapOf("tag" to "v1.2.0", "channel" to "stable"), RepoActivity.releasePayload(release))
        assertEq(mapOf<String, Any?>("tag" to "v2"), RepoActivity.releasePayload(RepoRelease(tag = "v2")))
    }
}
