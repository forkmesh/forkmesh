package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.control.DesktopCommandContract
import com.forkmesh.nativeapp.api.DartException
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.ActionRun
import com.forkmesh.nativeapp.model.ActionWorkflow
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.model.RepoActions
import com.forkmesh.nativeapp.model.RepoMirror
import com.forkmesh.nativeapp.model.RepoRelease
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.security.DeviceIdentity

enum class BadgeTone { Success, Danger, Warning, Accent, Secondary, Tertiary }

data class OwnerProof(val ownerAccount: String, val ts: String, val sig: String)

data class StatusRow(val icon: String, val tone: BadgeTone, val title: String, val subtitle: String, val badge: String)

object OwnerProofs {
    fun actionsReadCanonical(owner: String, name: String, ts: String): String =
        "forkmesh-actions-read-v1\n$owner\n$name\n$ts"

    fun desktopCommandCanonical(owner: String, name: String, command: String, target: String, ts: String): String =
        DesktopCommandContract.canonical(owner, name, command, target, ts)

    fun actionsRead(
        session: AuthSession?,
        identity: DeviceIdentity?,
        owner: String,
        name: String,
        nowMillis: Long = System.currentTimeMillis(),
    ): OwnerProof {
        val account = session?.nodeName?.trim()?.lowercase() ?: ""
        if (session == null || identity == null || account.isEmpty()) {
            throw DartException(
                "Actions are owner-key gated. Sign in with the repo owner account and local owner key to view workflow logs.",
            )
        }
        if (account != owner.trim().lowercase()) {
            throw DartException("Actions are owner-key gated. This device is signed in as $account, not $owner.")
        }
        if (session.pubkey.trim() != identity.publicKeyB64url) {
            throw DartException(
                "Actions are owner-key gated. Pair this mobile device with the owner key before viewing workflow logs.",
            )
        }
        val ts = nowMillis.toString()
        val sig = identity.sign(actionsReadCanonical(owner, name, ts).toByteArray(Charsets.UTF_8))
        return OwnerProof(account, ts, sig)
    }

    fun desktopCommand(
        session: AuthSession?,
        identity: DeviceIdentity?,
        owner: String,
        name: String,
        command: String,
        target: String,
        nowMillis: Long = System.currentTimeMillis(),
    ): OwnerProof {
        if (!DesktopCommandContract.isValid(command, target)) throw DartException(DesktopCommandContract.UNSUPPORTED_MESSAGE)
        val account = session?.nodeName?.trim()?.lowercase() ?: ""
        if (session == null || identity == null || account.isEmpty()) {
            throw DartException("Sign in with the repo owner account to request desktop control.")
        }
        if (session.pubkey.trim() != identity.publicKeyB64url) {
            throw DartException("Pair this mobile device with the owner key before requesting desktop control.")
        }
        val ts = nowMillis.toString()
        val sig = identity.sign(desktopCommandCanonical(owner, name, command, target, ts).toByteArray(Charsets.UTF_8))
        return OwnerProof(account, ts, sig)
    }

    fun desktopCommandOwner(repo: Repository, account: String): String =
        if (account.trim().lowercase() == repo.owner.trim().lowercase()) repo.owner else repo.displayOwner
}

object RepoActivity {
    const val DESKTOP_REQUEST_NOT_QUEUED = "Desktop request was not queued."

    fun badgeTone(text: String): BadgeTone {
        val normalized = text.lowercase()
        if (normalized.contains("open") || normalized.contains("live") || normalized.contains("signed")) {
            return BadgeTone.Success
        }
        if (normalized.contains("closed") || normalized.contains("offline") || normalized.contains("request")) {
            return BadgeTone.Danger
        }
        if (normalized.contains("merged")) return BadgeTone.Accent
        return BadgeTone.Secondary
    }

    fun actionStatusTone(statusLabel: String): BadgeTone = when (statusLabel) {
        "Success" -> BadgeTone.Success
        "Failed", "Rejected", "Cancelled" -> BadgeTone.Danger
        "Running", "Queued", "Awaiting approval" -> BadgeTone.Accent
        else -> BadgeTone.Tertiary
    }

    fun agentStatusTone(statusLabel: String): BadgeTone = when (statusLabel) {
        "Merged", "Done" -> BadgeTone.Success
        "Failed", "Stopped" -> BadgeTone.Danger
        "Running", "Queued", "Waiting" -> BadgeTone.Accent
        else -> BadgeTone.Tertiary
    }

    fun actionsCountLabel(actions: RepoActions): String =
        "${actions.workflows.size} workflows · ${actions.runs.size} runs"

    fun hasActiveRuns(actions: RepoActions): Boolean = actions.runs.any { it.isActive }

    fun workflowChips(workflow: ActionWorkflow): List<String> {
        val chips = ArrayList<String>()
        chips += workflow.triggerLabel
        if (workflow.stepCount > 0) chips += if (workflow.stepCount == 1) "1 step" else "${workflow.stepCount} steps"
        if (workflow.manual) chips += "Manual run"
        if (!workflow.valid) chips += "Invalid"
        return chips
    }

    fun runSubtitle(run: ActionRun): String =
        listOf(run.refLabel, run.shortCommit).filter { it.isNotEmpty() }.joinToString(" • ")

    fun runChips(run: ActionRun): List<String> {
        val chips = ArrayList<String>()
        chips += run.statusLabel
        if (run.refLabel.isNotEmpty()) chips += run.refLabel
        if (run.shortCommit.isNotEmpty()) chips += run.shortCommit
        if (run.durationLabel.isNotEmpty()) chips += run.durationLabel
        if (run.workflowPath.isNotEmpty()) chips += run.workflowPath
        return chips
    }

    fun rerunTarget(run: ActionRun): String = "run:${run.id}"

    fun rerunPayload(run: ActionRun): Map<String, Any?> = linkedMapOf<String, Any?>("runId" to run.id).apply {
        if (run.workflowPath.isNotEmpty()) put("workflowPath", run.workflowPath)
    }

    const val RERUN_QUEUED = "Desktop request queued. Approve it in the ForkMesh desktop app to rerun."

    fun logText(log: String): String = log.trim().ifEmpty { "No log has been pushed by the desktop node yet." }

    fun agentOwnerAccount(sessionNodeName: String?, repoOwner: String): String {
        val node = sessionNodeName?.trim()?.lowercase() ?: ""
        return node.ifEmpty { repoOwner }
    }

    fun hasActiveSessions(sessions: List<AgentSession>): Boolean = sessions.any { it.isActive }

    fun agentSubtitle(session: AgentSession): String {
        val parts = ArrayList<String>()
        if (session.issueNumber > 0) parts += "Issue #${session.issueNumber}"
        parts += session.providerLabel
        parts += session.latestStatusSummary(80)
        return parts.filter { it.isNotEmpty() }.joinToString(" • ")
    }

    fun agentChips(session: AgentSession, nowMillis: Long = System.currentTimeMillis()): List<String> {
        val chips = ArrayList<String>()
        if (session.issueNumber > 0) chips += "#${session.issueNumber}"
        chips += session.providerLabel
        chips += session.latestStatusSummary(80)
        val countdown = session.rateLimitCountdownLabel(nowMillis)
        if (countdown.isNotEmpty()) chips += countdown
        if (session.branchName.isNotEmpty()) chips += session.branchName
        if (session.numTurns > 0) chips += "${session.numTurns} turns"
        if (session.durationLabel.isNotEmpty()) chips += session.durationLabel
        if (session.costLabel.isNotEmpty()) chips += session.costLabel
        if (session.prLabel.isNotEmpty()) chips += session.prLabel
        if (session.diffLabel.isNotEmpty()) chips += session.diffLabel
        if (session.conflicted) chips += "Conflict"
        if (session.tokenLabel.isNotEmpty()) chips += session.tokenLabel
        return chips
    }

    fun transcriptStatusLabel(session: AgentSession, transcriptStatus: String): String =
        if (transcriptStatus.isNotEmpty()) AgentSession(id = session.id, status = transcriptStatus).statusLabel else session.statusLabel

    fun transcriptText(transcript: String): String =
        transcript.trim().ifEmpty { "No transcript has been pushed by the desktop node yet." }

    fun mirrorRow(mirror: RepoMirror): StatusRow = StatusRow(
        icon = if (mirror.online) "cloud_done_outlined" else "cloud_off_outlined",
        tone = if (mirror.online) BadgeTone.Success else BadgeTone.Tertiary,
        title = mirror.label,
        subtitle = if (mirror.online) "Online mirror" else "Offline or recently unseen",
        badge = if (mirror.online) "live" else "offline",
    )

    fun syncStatusRow(status: Map<String, Any?>): StatusRow {
        val severity = status["severity"]
        val label = jsonText(status["label"] ?: "")
        return StatusRow(
            icon = when (severity) {
                "ok" -> "verified_outlined"
                "error" -> "error_outline"
                "warning" -> "warning_amber_outlined"
                else -> "sync"
            },
            tone = when (severity) {
                "ok" -> BadgeTone.Success
                "error" -> BadgeTone.Danger
                "warning" -> BadgeTone.Warning
                else -> BadgeTone.Accent
            },
            title = label,
            subtitle = jsonText(status["summary"] ?: ""),
            badge = label.lowercase(),
        )
    }

    data class ReleaseRequest(val label: String, val icon: String, val command: String, val successMessage: String, val filled: Boolean)

    val releaseRequests: List<ReleaseRequest> = listOf(
        ReleaseRequest(
            "Draft release", "edit_note_outlined", "release.draft",
            "Draft release request queued. Approve it in the ForkMesh desktop app.", filled = false,
        ),
        ReleaseRequest(
            "Generate notes", "notes_outlined", "release.notes",
            "Generate notes request queued. Approve it in the ForkMesh desktop app.", filled = false,
        ),
        ReleaseRequest(
            "Create release", "new_releases_outlined", "release.create",
            "Create release request queued. Approve it in the ForkMesh desktop app.", filled = true,
        ),
        ReleaseRequest(
            "Publish artifacts", "cloud_upload_outlined", "release.publish",
            "Publish artifacts request queued. Approve it in the ForkMesh desktop app.", filled = true,
        ),
    )

    fun releaseTarget(release: RepoRelease): String = "release:${release.tag}"

    fun releasePayload(release: RepoRelease): Map<String, Any?> = linkedMapOf<String, Any?>("tag" to release.tag).apply {
        if (release.channel.isNotEmpty()) put("channel", release.channel)
    }
}
