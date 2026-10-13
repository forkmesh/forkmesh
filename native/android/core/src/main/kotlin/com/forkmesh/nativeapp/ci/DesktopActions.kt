package com.forkmesh.nativeapp.ci

import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.control.ControlFamilyChannel
import com.forkmesh.nativeapp.control.DesktopControlFamilyName
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import com.forkmesh.nativeapp.model.modelInt

data class DesktopActionsLog(val text: String, val from: Long, val next: Long, val reset: Boolean) {
    companion object {
        fun fromJson(json: Map<String, Any?>): DesktopActionsLog = DesktopActionsLog(
            text = json.str("text"),
            from = modelInt(json["from"]).coerceAtLeast(0),
            next = modelInt(json["next"]).coerceAtLeast(0),
            reset = json.bool("reset"),
        )
    }
}

class DesktopActions(channel: ControlCalling, desktopName: String) {
    private val family = ControlFamilyChannel(DesktopControlFamilyName.Repos, channel)
    val desktopName: String = desktopName.trim().ifEmpty { "your desktop" }

    fun summary(repo: RepoRef): ActionsSummary =
        summary(family.call("actions", mapOf("repo" to repo.slug), TIMEOUT_MS), desktopName)

    fun log(repo: RepoRef, runId: Int, since: Long?): DesktopActionsLog {
        val params = linkedMapOf<String, Any?>("repo" to repo.slug, "run" to runId.toLong())
        if (since != null) params["since"] = since
        return DesktopActionsLog.fromJson(family.call("actions.log", params, TIMEOUT_MS))
    }

    fun control(repo: RepoRef, kind: ActionsControlKind, runId: Int?, workflow: String?, branch: String?): String {
        val action = action(kind) ?: throw LocalControlException("unsupported_action")
        val params = linkedMapOf<String, Any?>("repo" to repo.slug, "action" to action)
        if (runId != null) params["run"] = runId.toLong()
        if (workflow != null) params["path"] = workflow
        if (branch != null) params["branch"] = bareBranch(branch)
        val token = family.call("prepare", params, TIMEOUT_MS).str("token")
        if (token.isEmpty()) throw LocalControlException("not_started")
        return family.call("run", mapOf("token" to token), TIMEOUT_MS).str("message")
    }

    fun onChange(tap: () -> Unit): Any = family.addEventTap { suffix, _ -> if (suffix == "changed") tap() }

    fun removeTap(token: Any) = family.removeEventTap(token)

    companion object {
        const val TIMEOUT_MS = 8_000L
        const val MAX_LOG_CHARS = 64 * 1024

        fun action(kind: ActionsControlKind): String? = when (kind) {
            ActionsControlKind.Dispatch -> "workflow.run"
            ActionsControlKind.Rerun -> "run.rerun"
            ActionsControlKind.Cancel -> "run.stop"
            ActionsControlKind.Approve -> "run.approve"
            ActionsControlKind.Reject -> "run.reject"
            ActionsControlKind.SetWorkflowEnabled, ActionsControlKind.SetRepositoryEnabled -> null
        }

        fun bareBranch(branch: String): String = branch.trim().removePrefix("refs/heads/")

        fun summary(reply: Map<String, Any?>, node: String): ActionsSummary {
            val workflows = reply.maps("workflows").map { item ->
                mapOf(
                    "path" to item.str("path"),
                    "name" to item.str("name"),
                    "events" to item.strings("triggers"),
                    "manual" to item.bool("manual"),
                    "enabled" to !item.bool("disabled"),
                    "runsOn" to item.str("runsOn").split(",").map { it.trim() }.filter { it.isNotEmpty() },
                    "valid" to (item["valid"] != false),
                )
            }
            val runs = reply.maps("runs").map { item ->
                val branch = item.str("branch")
                item + mapOf(
                    "workflowPath" to item.str("path"),
                    "ref" to if (branch.isEmpty() || branch.startsWith("refs/")) branch else "refs/heads/$branch",
                    "pullNumber" to modelInt(item["pull"]),
                )
            }
            return ActionsSummary.fromJson(
                mapOf(
                    "ok" to true,
                    "schemaVersion" to 2L,
                    "node" to node,
                    "controlAvailable" to true,
                    "actionsEnabled" to reply.bool("enabled"),
                    "workflows" to workflows,
                    "runs" to runs,
                ),
            )
        }

        fun explain(error: Throwable): String = when (val code = (error as? LocalControlException)?.code.orEmpty()) {
            "unknown_repo" -> "Your desktop does not have this repository."
            "unknown_run" -> "The desktop no longer has that run."
            "wrong_state" -> "That run has already moved on."
            "actions_disabled" -> "Actions are turned off for this repository on the desktop."
            "workflow_disabled" -> "That workflow is turned off on the desktop."
            "no_mirror" -> "The desktop has no mirror of this repository to run from."
            "unsupported", "unknown_method", "unsupported_action" -> "Update the desktop to do that from here."
            "not_owner" -> "The desktop is signed in to another account."
            "not_paired" -> "Pair this phone with your desktop first."
            "" -> "The desktop did not answer."
            else -> code.replace('_', ' ').replaceFirstChar { it.uppercase() } + "."
        }
    }
}
