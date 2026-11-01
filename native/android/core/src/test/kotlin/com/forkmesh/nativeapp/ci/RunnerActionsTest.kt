package com.forkmesh.nativeapp.ci

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class RunnerActionsTest {
    private val repo = RepoRef("alice", "tool")
    private val head = "0123456789abcdef0123456789abcdef01234567"

    private fun runnerSummary(runs: List<Map<String, Any?>> = emptyList()) = linkedMapOf<String, Any?>(
        "ok" to true,
        "node" to "runner",
        "source" to "runner",
        "controlAvailable" to true,
        "workflows" to listOf(
            linkedMapOf("path" to ".forkmesh/ci.yml", "name" to "CI", "events" to listOf("push")),
            linkedMapOf("path" to ".forkmesh/deploy.yml", "name" to "Deploy", "events" to listOf("push", "workflow_dispatch")),
        ),
        "runs" to runs,
    )

    private fun checksJson(
        required: Boolean = true,
        reason: String = "author",
        author: String = "bob",
        trust: String = "",
        canApprove: Boolean = true,
        approvedBy: String = "",
        sha: String = head,
    ) = linkedMapOf<String, Any?>(
        "ok" to true,
        "number" to 7L,
        "configured" to true,
        "head" to sha,
        "runs" to listOf(
            linkedMapOf("id" to 7000001L, "workflow" to "CI", "workflowPath" to ".forkmesh/ci.yml", "status" to "awaiting-approval", "commit" to sha, "createdAt" to 5L),
        ),
        "approval" to linkedMapOf("required" to required, "reason" to reason, "author" to author, "authorTrust" to trust, "approvedBy" to approvedBy),
        "canApprove" to canApprove,
        "viewer" to "alice",
        "previousHeads" to 1L,
    )

    private class RecordingApi(val answers: MutableList<ApiJsonResult>) : ApiService(TestSettings()) {
        val calls = ArrayList<String>()
        val bodies = ArrayList<Map<String, Any?>?>()

        override fun jsonExchange(
            method: String,
            path: String,
            query: List<Pair<String, String>>,
            body: Map<String, Any?>?,
            timeoutMs: Int,
            extraHeaders: Map<String, String>,
        ): ApiJsonResult {
            calls.add("$method $path")
            bodies.add(body)
            return answers.removeAt(0)
        }
    }

    fun testRunnerSummaryKeepsControlsAndHidesSwitches() {
        val summary = ActionsSummary.fromJson(runnerSummary())
        expect(summary.isRunner)
        expect(summary.controlAvailable)
        assertEq(listOf(".forkmesh/ci.yml", ".forkmesh/deploy.yml"), summary.workflows.map { it.path })
        assertEq(listOf(false, true), summary.workflows.map { it.canDispatch })
        assertEq(null, summary.actionsEnabled)
        val client = FakeCiClient()
        client.summary = summary
        val model = RepoActionsModel(client, repo)
        model.load()
        expect(model.canControl)
        expect(model.isRunner)
        expect(!model.showsSwitches)
        expect(model.canManageSecrets)
        assertEq(listOf(".forkmesh/deploy.yml"), model.dispatchableWorkflows.map { it.path })
        assertEq("No workflow runs yet.", model.emptyMessage)
        client.controlAnswer = mapOf("ok" to true, "status" to "applied", "action" to "dispatch", "runId" to 300L)
        expect(model.dispatch(model.dispatchableWorkflows.single(), "main"))
        assertEq(".forkmesh/deploy.yml", client.controls.single().workflow)
        assertEq("refs/heads/main", client.controls.single().ref)
        expect(!model.needsFollowUp)
        client.controls.clear()
        expect(!model.send(ActionsControlKind.SetWorkflowEnabled, workflow = ".forkmesh/ci.yml", enabled = false))
        expect(!model.send(ActionsControlKind.SetRepositoryEnabled, enabled = false))
        assertEq(0, client.controls.size)
        assertEq(false, model.notice?.good)

        val v2 = ActionsSummary.fromJson(
            linkedMapOf("ok" to true, "schemaVersion" to 2L, "node" to "desk", "controlAvailable" to true, "actionsEnabled" to true),
        )
        expect(!v2.isRunner)
        client.summary = v2
        model.load()
        expect(model.showsSwitches)
        expect(!model.canManageSecrets)
        expect(model.send(ActionsControlKind.SetRepositoryEnabled, enabled = false))
        assertEq("set-repository-enabled", client.controls.single().kind.raw)
    }

    fun testRunnerControlAppliesAtOnce() {
        val client = FakeCiClient()
        client.summary = ActionsSummary.fromJson(runnerSummary(listOf(linkedMapOf("id" to 1000001L, "status" to "failed"))))
        client.controlAnswer = mapOf("ok" to true, "status" to "applied", "action" to "rerun", "runId" to 2000001L)
        val model = RepoActionsModel(client, repo, makeRequestId = { "req_0123456789abcdef" })
        model.load()
        assertEq(listOf(ActionsControlKind.Rerun), model.actions(model.runs[0]))
        expect(model.send(ActionsControlKind.Rerun, runId = 1000001))
        expect(!model.isPending(1000001))
        expect(!model.needsFollowUp)
        assertEq(CiNotice("Re-run applied.", true), model.notice)
    }

    fun testRunnerRunsShowTheFullLog() {
        val client = FakeCiClient()
        client.summary = ActionsSummary.fromJson(
            runnerSummary(listOf(linkedMapOf("id" to 1000001L, "status" to "running", "logTail" to "tail only"))),
        )
        client.logs[1000001] = RunnerRunLog(1000001, "first line\n\u001B[31msecond\u001B[0m\n", 9)
        val model = RepoActionsModel(client, repo)
        model.load()
        assertEq("tail only", model.run(1000001)?.logTail)
        expect(!model.hasFullLog(1000001))
        model.follow(1000001)
        assertEq(listOf(1000001), client.logCalls)
        expect(model.hasFullLog(1000001))
        assertEq("first line\n\u001B[31msecond\u001B[0m\n", model.run(1000001)?.logTail)
        assertEq(listOf("first line", "second"), CiLog.fullLines(model.fullLog(1000001)!!))
        model.loadLog(5)
        assertEq("This run was not found.", model.logProblems[5])
        client.logFailure = CiError(403, "forbidden")
        model.loadLog(1000001)
        expect(model.hasFullLog(1000001))
        assertEq("Only the repository owner and organization writers can do that.", model.logProblems[1000001])

        val desktopOnly = FakeCiClient()
        desktopOnly.summary = ActionsSummary.fromJson(linkedMapOf("ok" to true, "schemaVersion" to 2L, "node" to "desk", "runs" to listOf(linkedMapOf("id" to 3L, "status" to "running"))))
        val other = RepoActionsModel(desktopOnly, repo)
        other.load()
        other.loadLog(3)
        assertEq(0, desktopOnly.logCalls.size)
    }

    fun testFullLogLinesAndChunks() {
        val lines = CiLog.fullLines((1..250).joinToString("\r\n") { "line $it" } + "\n\n")
        assertEq(250, lines.size)
        assertEq("line 250", lines.last())
        val chunks = CiLog.chunks(lines, 100)
        assertEq(3, chunks.size)
        assertEq("line 201", chunks[2].lines().first())
        assertEq(5, CiLog.fullLines((1..9).joinToString("\n"), 5).size)
    }

    fun testPullChecksParsingAndButtons() {
        val waiting = RunnerPullChecks.fromJson(checksJson())
        assertEq(7, waiting.number)
        assertEq(head, waiting.head)
        assertEq("0123456", waiting.shortHead)
        assertEq(listOf(7000001), waiting.runs.map { it.id })
        assertEq(ActionsRunStatus.AwaitingApproval, waiting.runs[0].status)
        assertEq(PullChecksReason.Author, waiting.approval.reason)
        assertEq("@bob is not trusted to run code here.", waiting.approval.reasonLabel)
        assertEq(1, waiting.previousHeads)
        assertEq(listOf(PullChecksAction.Approve, PullChecksAction.Trust), waiting.actions)
        assertEq("Always run for @bob", PullChecksAction.Trust.label("bob"))
        assertEq("Approve and run", PullChecksAction.Approve.label("bob"))
        assertEq("Stop running automatically", PullChecksAction.Untrust.label("bob"))
        expect(PullChecksAction.Approve.needsPresence)
        expect(!PullChecksAction.Untrust.needsPresence)
        expect(waiting.summary.startsWith("Waiting for a maintainer"))

        val workflows = RunnerPullChecks.fromJson(checksJson(reason = "workflows", trust = "writer"))
        assertEq(PullChecksReason.Workflows, workflows.approval.reason)
        expect(workflows.approval.reasonLabel.contains(".forkmesh/"))
        assertEq(listOf(PullChecksAction.Approve), workflows.actions)

        val allowed = RunnerPullChecks.fromJson(checksJson(required = false, reason = "", trust = "allowlist", approvedBy = "alice"))
        assertEq(listOf(PullChecksAction.Untrust), allowed.actions)
        assertEq("alice", allowed.approval.approvedBy)
        expect(allowed.approval.trustLabel.contains("@bob"))

        assertEq(emptyList<PullChecksAction>(), RunnerPullChecks.fromJson(checksJson(canApprove = false)).actions)
        assertEq(listOf(PullChecksAction.Trust), RunnerPullChecks.fromJson(checksJson(sha = "")).actions)
        assertEq(emptyList<PullChecksAction>(), RunnerPullChecks.fromJson(checksJson(required = false, author = "")).actions)
        val none = RunnerPullChecks.fromJson(linkedMapOf("ok" to true, "number" to 3L, "configured" to false, "runs" to emptyList<Any?>()))
        expect(!none.visible)
        assertEq("This repository has no pull request workflows.", none.summary)
    }

    fun testPullChecksRequests() {
        assertEq(linkedMapOf<String, Any?>("action" to "approve", "head" to head), PullChecksRequest.approve(head.uppercase()).body)
        assertEq(linkedMapOf<String, Any?>("action" to "trust", "account" to "bob"), PullChecksRequest.trust("@bob").body)
        assertEq(linkedMapOf<String, Any?>("action" to "untrust", "account" to "bob"), PullChecksRequest.untrust("bob").body)
        expect(PullChecksRequest.approve(head).isValid)
        expect(!PullChecksRequest.approve("abc").isValid)
        expect(!PullChecksRequest.trust("").isValid)
        expect(!PullChecksRequest.trust("My Instance").isValid)
    }

    fun testPullChecksModelActsAndRefreshes() {
        val client = FakeCiClient()
        client.checks = RunnerPullChecks.fromJson(checksJson())
        val model = PullChecksModel(client, repo, 7)
        model.load()
        expect(model.visible)
        assertEq(listOf(PullChecksAction.Approve, PullChecksAction.Trust), model.actions)
        client.checksAnswer = mapOf("ok" to true, "approved" to 2L)
        client.checks = RunnerPullChecks.fromJson(checksJson(required = false, approvedBy = "alice"))
        expect(model.perform(PullChecksAction.Approve))
        assertEq(PullChecksRequest.approve(head), client.checksRequests.last())
        assertEq(CiNotice("Approved 0123456. 2 checks are queued.", true), model.notice)
        assertEq("alice", model.checks?.approval?.approvedBy)
        assertEq(null, model.busy)

        val other = "fedcba9876543210fedcba9876543210fedcba98"
        client.checksActionFailure = CiError(409, "stale_head")
        expect(!model.perform(PullChecksAction.Approve, other))
        assertEq(CiNotice("The pull request has a newer commit. Review it before approving.", false), model.notice)
        client.checksActionFailure = CiError(409, "not_awaiting_approval")
        expect(!model.perform(PullChecksAction.Approve, other))
        assertEq("These checks are no longer waiting for approval.", model.notice?.text)
        client.checksActionFailure = null

        client.checksAnswer = mapOf("ok" to true, "trusted" to "bob", "approved" to 1L)
        expect(model.perform(PullChecksAction.Trust))
        assertEq(PullChecksRequest.trust("bob"), client.checksRequests.last())
        assertEq("Checks for @bob now run on their own. 1 check is queued.", model.notice?.text)
        client.checksAnswer = mapOf("ok" to true, "untrusted" to "bob")
        expect(model.perform(PullChecksAction.Untrust))
        assertEq("Checks for @bob need approval again.", model.notice?.text)

        client.checks = RunnerPullChecks.fromJson(checksJson(author = ""))
        model.load()
        val before = client.checksRequests.size
        expect(!model.perform(PullChecksAction.Trust))
        assertEq(before, client.checksRequests.size)
        assertEq(false, model.notice?.good)
    }

    fun testPullChecksHiddenWithoutARunner() {
        val client = FakeCiClient()
        client.checksFailure = CiError(404, "not_available")
        val model = PullChecksModel(client, repo, 7)
        model.load()
        expect(model.unavailable)
        expect(!model.visible)
        assertEq(null, model.error)
        client.checksFailure = CiError(401, "invalid_session")
        val signedOut = PullChecksModel(client, repo, 7)
        signedOut.load()
        expect(signedOut.visible)
        assertEq("Sign in again to manage this repository.", signedOut.error)
    }

    fun testApiClientPathsBodiesAndErrors() {
        val api = RecordingApi(
            mutableListOf(
                ApiJsonResult(200, checksJson()),
                ApiJsonResult(200, mapOf("ok" to true, "approved" to 1L)),
                ApiJsonResult(200, mapOf("ok" to true, "runId" to 7000001L, "log" to "hello", "updatedAt" to 12L)),
                ApiJsonResult(
                    200,
                    mapOf("ok" to true, "secrets" to listOf(mapOf("name" to "API_KEY", "updatedAt" to 3L, "updatedBy" to "alice"), mapOf("name" to "bad name"))),
                ),
                ApiJsonResult(200, mapOf("ok" to true, "name" to "API_KEY", "updatedAt" to 4L)),
                ApiJsonResult(200, mapOf("ok" to true, "name" to "API_KEY", "updatedAt" to 5L)),
                ApiJsonResult(200, mapOf("ok" to true, "status" to "applied")),
            ),
        )
        val client = ApiCiClient(api)
        assertEq(7, client.pullChecks(repo, 7).number)
        client.pullChecksAction(repo, 7, PullChecksRequest.approve(head))
        val log = client.runLog(repo, 7000001)
        assertEq(RunnerRunLog(7000001, "hello", 12), log)
        assertEq(listOf(ActionSecret("API_KEY", 3, "alice")), client.actionSecrets(repo))
        client.putActionSecret(repo, "API_KEY", "s3cret")
        client.deleteActionSecret(repo, "API_KEY")
        assertEq("applied", client.actionsControl(repo, ActionsControlRequest("req_0123456789abcdef", ActionsControlKind.Rerun, "runner", runId = 1))["status"])
        assertEq(
            listOf(
                "GET /api/repo/alice/tool/pulls/7/checks",
                "POST /api/repo/alice/tool/pulls/7/checks",
                "GET /api/repo/alice/tool/actions/runs/7000001/log",
                "GET /api/repo/alice/tool/actions/secrets",
                "PUT /api/repo/alice/tool/actions/secrets/API_KEY",
                "DELETE /api/repo/alice/tool/actions/secrets/API_KEY",
                "POST /api/repo/alice/tool/actions/control",
            ),
            api.calls,
        )
        assertEq(mapOf("action" to "approve", "head" to head), api.bodies[1])
        assertEq(null, api.bodies[0])
        assertEq(mapOf("value" to "s3cret"), api.bodies[4])
        assertEq(null, api.bodies[5])

        val failing = RecordingApi(
            mutableListOf(
                ApiJsonResult(409, mapOf("error" to "stale_head", "message" to "newer")),
                ApiJsonResult(404, mapOf("error" to "not_available")),
                ApiJsonResult(404, mapOf("error" to "run_not_found")),
                ApiJsonResult(403, mapOf("error" to "forbidden")),
                ApiJsonResult(400, mapOf("error" to "invalid_name")),
                ApiJsonResult(409, mapOf("error" to "too_many_secrets")),
            ),
        )
        val bad = ApiCiClient(failing)
        assertEq("stale_head", assertFails<CiError> { bad.pullChecksAction(repo, 7, PullChecksRequest.approve(head)) }.code)
        assertEq("not_available", assertFails<CiError> { bad.pullChecks(repo, 7) }.code)
        assertEq("This run was not found.", assertFails<CiError> { bad.runLog(repo, 1) }.message)
        assertEq(ActionSecrets.FORBIDDEN, ActionSecrets.explain(assertFails<CiError> { bad.actionSecrets(repo) }))
        assertEq(CiError(400, "invalid_name").message, assertFails<CiError> { bad.putActionSecret(repo, "X", "y") }.message)
        assertEq("This repository already has the most secrets it can have.", assertFails<CiError> { bad.putActionSecret(repo, "X", "y") }.message)
    }

    fun testCiErrorMessagesForRunnerCodes() {
        assertEq("These checks are no longer waiting for approval.", CiError(409, "not_awaiting_approval").message)
        assertEq("Only the pull request's verified author can be trusted from here.", CiError(409, "author_unknown").message)
        assertEq("A secret value is 1 byte to 48 KiB of text.", CiError(400, "invalid_value").message)
        assertEq("This instance does not run checks for pull requests.", CiError(404, "not_available").message)
        assertEq("This repository was not found, or it is private.", CiError(404, "not_found").message)
    }

    fun testSecretsModelValidatesAndWrites() {
        expect(ActionSecrets.NAME.matches("_A1"))
        expect(!ActionSecrets.NAME.matches("1ABC"))
        expect(!ActionSecrets.NAME.matches("A".repeat(65)))
        assertEq(null, ActionSecret.fromJson(mapOf("name" to "lower")))
        val client = FakeCiClient()
        val model = ActionSecretsModel(client, repo)
        model.load()
        expect(model.loaded)
        expect(model.secrets.isEmpty())
        expect(!model.save("1abc", "v"))
        assertEq(CiError(400, "invalid_name").message, model.notice?.text)
        expect(!model.save("api_key", ""))
        assertEq("Enter the secret's value.", model.notice?.text)
        expect(!model.save("API_KEY", "x".repeat(ActionSecrets.VALUE_MAX_BYTES + 1)))
        assertEq("A secret value is 1 byte to 48 KiB of text.", model.notice?.text)
        assertEq(0, client.secretCalls.size)
        expect(model.save(" api_key ", "x".repeat(ActionSecrets.VALUE_MAX_BYTES)))
        assertEq(CiNotice("Added API_KEY.", true), model.notice)
        assertEq(listOf("API_KEY"), model.secrets.map { it.name })
        expect(model.exists("api_key"))
        expect(model.save("API_KEY", "new"))
        assertEq("PUT API_KEY new", client.secretCalls.last())
        assertEq("Replaced API_KEY.", model.notice?.text)
        expect(model.delete("API_KEY"))
        assertEq("DELETE API_KEY", client.secretCalls.last())
        assertEq("Deleted API_KEY.", model.notice?.text)
        expect(model.secrets.isEmpty())
        client.secretFailure = CiError(403, "forbidden")
        expect(!model.save("TOKEN", "v"))
        assertEq(CiNotice(ActionSecrets.FORBIDDEN, false), model.notice)
        model.load()
        assertEq(ActionSecrets.FORBIDDEN, model.error)
    }
}
