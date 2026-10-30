package com.forkmesh.nativeapp.ci

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.model.RepoRelease
import com.forkmesh.nativeapp.model.RepoReleaseAsset
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class FakeCiClient : CiClient {
    var summary = ActionsSummary()
    val controls = ArrayList<ActionsControlRequest>()
    var controlFailure: Exception? = null
    val webhookCalls = ArrayList<Pair<String, Map<String, Any?>>>()
    val webhookAnswers = HashMap<String, Map<String, Any?>>()
    val scans = HashMap<String, Map<String, Any?>>()
    val scanFailures = HashSet<String>()
    val triages = ArrayList<List<String>>()
    var releaseList = emptyList<RepoRelease>()
    var mirrors: Map<String, Any?> = emptyMap()
    val announcements = ArrayList<List<String>>()

    override fun actionsRuns(repo: RepoRef) = summary
    override fun actionsControl(repo: RepoRef, request: ActionsControlRequest) {
        controlFailure?.let { throw it }
        controls.add(request)
    }
    override fun webhooks(repo: RepoRef, action: String, fields: Map<String, Any?>): Map<String, Any?> {
        webhookCalls.add(action to fields)
        return webhookAnswers[action] ?: emptyMap()
    }
    override fun securityScans(repo: RepoRef, operation: String, query: List<Pair<String, String>>): Map<String, Any?> {
        if (operation in scanFailures) throw CiError(404, "not_found")
        return scans[operation] ?: emptyMap()
    }
    override fun securityTriage(repo: RepoRef, scanId: String, findingId: String, status: String, note: String): Map<String, Any?> {
        triages.add(listOf(scanId, findingId, status, note))
        return emptyMap()
    }
    override fun releases(owner: String, name: String) = releaseList
    override fun mirrorsPayload(repo: RepoRef) = mirrors
    override fun clearRepoCache(owner: String, name: String) {}
    override fun announceRelease(
        repo: RepoRef,
        tag: String,
        title: String,
        body: String,
        account: String,
        sign: (ByteArray) -> String,
        nowMillis: Long,
    ) {
        announcements.add(listOf(tag, title, body, account, sign(CiRequests.announceCanonical(account, "$nowMillis").toByteArray())))
    }
}

class CiTest {
    private val repo = RepoRef("alice", "tool")

    private fun v2(runs: List<Map<String, Any?>>, controls: List<Map<String, Any?>> = emptyList()) = linkedMapOf(
        "ok" to true,
        "schemaVersion" to 2L,
        "node" to "desk",
        "controlAvailable" to true,
        "actionsEnabled" to true,
        "workflows" to listOf(linkedMapOf("path" to ".forkmesh/ci.yml", "name" to "CI", "manual" to true)),
        "runs" to runs,
        "controls" to controls,
    )

    fun testSummaryParsesV2AndSortsNewestFirst() {
        val summary = ActionsSummary.fromJson(
            v2(
                listOf(
                    linkedMapOf("id" to 1L, "workflow" to "CI", "status" to "passed", "createdAt" to 10L),
                    linkedMapOf("id" to 2L, "workflow" to "CI", "status" to "awaiting_approval", "createdAt" to 20L),
                    linkedMapOf("id" to 0L, "workflow" to "bad"),
                ),
            ),
        )
        assertEq(listOf(2, 1), summary.runs.map { it.id })
        assertEq(ActionsRunStatus.AwaitingApproval, summary.runs[0].status)
        assertEq(ActionsRunStatus.Success, summary.runs[1].status)
        expect(summary.controlAvailable)
        assertEq(true, summary.workflows.single().canDispatch)
        val v1 = ActionsSummary.fromJson(linkedMapOf("ok" to true, "controlAvailable" to true, "workflows" to listOf(linkedMapOf("path" to "x"))))
        expect(!v1.controlAvailable)
        assertEq(null, v1.actionsEnabled)
        assertEq(0, v1.workflows.size)
    }

    fun testControlRequestValidation() {
        val id = "abcdefghijklmnop"
        expect(ActionsControlRequest(id, ActionsControlKind.Rerun, "desk", runId = 3).isValid)
        expect(!ActionsControlRequest(id, ActionsControlKind.Rerun, "desk").isValid)
        expect(!ActionsControlRequest("short", ActionsControlKind.Rerun, "desk", runId = 3).isValid)
        expect(
            ActionsControlRequest(id, ActionsControlKind.Dispatch, "desk", workflow = ".forkmesh/ci.yml", ref = "refs/heads/main").isValid,
        )
        expect(!ActionsControlRequest(id, ActionsControlKind.Dispatch, "desk", workflow = "ci.yml", ref = "refs/heads/main").isValid)
        expect(!ActionsControlRequest(id, ActionsControlKind.Dispatch, "desk", workflow = ".forkmesh/ci.yml", ref = "main").isValid)
        assertEq("refs/heads/dev", ActionsControlRequest.branchRef(" dev "))
        assertEq("refs/tags/v1", ActionsControlRequest.branchRef("refs/tags/v1"))
        expect(ActionsControlRequest.REQUEST_ID.matches(ActionsControlRequest.makeRequestId()))
        val body = ActionsControlRequest(id, ActionsControlKind.SetRepositoryEnabled, "desk", enabled = false).body
        assertEq(linkedMapOf<String, Any?>("requestId" to id, "action" to "set-repository-enabled", "node" to "desk", "enabled" to false), body)
    }

    fun testModelSendsAndSettlesControls() {
        val client = FakeCiClient()
        var now = 1_000L
        val model = RepoActionsModel(client, repo, now = { now }, makeRequestId = { "req_0123456789abcdef" })
        client.summary = ActionsSummary.fromJson(v2(listOf(linkedMapOf("id" to 5L, "status" to "failed"))))
        model.load()
        assertEq(listOf(ActionsControlKind.Rerun), model.actions(model.runs[0]))
        expect(model.send(ActionsControlKind.Rerun, runId = 5))
        assertEq("rerun", client.controls.single().kind.raw)
        expect(model.isPending(5))
        assertEq(emptyList<ActionsControlKind>(), model.actions(model.runs[0]))
        client.summary = ActionsSummary.fromJson(
            v2(
                listOf(linkedMapOf("id" to 5L, "status" to "running")),
                listOf(linkedMapOf("requestId" to "req_0123456789abcdef", "status" to "refused", "reason" to "not_owner")),
            ),
        )
        model.load()
        expect(!model.isPending(5))
        assertEq(CiNotice("Re-run was refused: not owner.", false), model.notice)
        client.controlFailure = CiError(403, "forbidden")
        expect(!model.send(ActionsControlKind.Cancel, runId = 5))
        expect(!model.needsFollowUp)
        client.controlFailure = null
        expect(model.send(ActionsControlKind.Cancel, runId = 5))
        now += RepoActionsModel.PENDING_TIMEOUT_MS + 1
        model.settle()
        expect(!model.needsFollowUp)
    }

    fun testNoControlWhenTheNodeDoesNotAcceptIt() {
        val client = FakeCiClient()
        client.summary = ActionsSummary.fromJson(linkedMapOf("ok" to false))
        val model = RepoActionsModel(client, repo)
        model.load()
        expect(model.isUnavailable)
        expect(!model.send(ActionsControlKind.Rerun, runId = 1))
        expect(model.emptyMessage.startsWith("No node"))
        assertEq(0, client.controls.size)
    }

    fun testLogAndFailurePrompt() {
        val log = "step one\n\u001B[31merror: boom\u001B[0m\nafter\n\n"
        assertEq(listOf("step one", "error: boom", "after"), CiLog.lines(log))
        val run = ActionsRunSummary(
            id = 9,
            workflow = "CI",
            ref = "refs/heads/dev",
            commit = "0123456789",
            status = ActionsRunStatus.Failed,
            logTail = log,
            pullNumber = 4,
            steps = listOf(ActionsStep("test", "failed"), ActionsStep("lint", "success")),
        )
        val prompt = CiPrompts.fixFailedRun(run, repo)
        assertEq(4, prompt.pull)
        expect(prompt.text.startsWith("The CI workflow failed on dev (run #9, commit 01234567)."))
        expect(prompt.text.contains("Failed steps: test."))
        expect(prompt.text.contains("error: boom"))
        assertEq("1m 5s", ActionsRunSummary(1, startedAtMs = 1_000, finishedAtMs = 66_000).durationLabel(0))
        assertEq("2h 0m", ActionsRunSummary(1, startedAtMs = 1, finishedAtMs = 7_200_001).durationLabel(0))
    }

    fun testFailedRunPromptKeepsTheFailureAtTheEndOfALongLog() {
        val build = (0 until 180).map { "[ 33%] Building CXX object CMakeFiles/forkmesh-window-tests.dir/src/File$it.cpp.o" }
        val log = (listOf("esh-window-tests.dir/src/ClientErrorReports.cpp.o") + build +
            listOf("!! Process crashed", "", "==> FAILED: Critical Qt client contracts exceeded the process time limit."))
            .joinToString("\n")
        expect(log.length > com.forkmesh.nativeapp.navigation.MessageHandoff.MAX_MESSAGE_LENGTH)
        val run = ActionsRunSummary(
            id = 8092,
            workflow = "Desktop client CI tests",
            commit = "f86a1b189b07",
            ref = "refs/heads/main",
            status = ActionsRunStatus.Failed,
            logTail = log,
            steps = listOf(ActionsStep("Critical Qt client contracts", "failed")),
        )
        val prompt = CiPrompts.fixFailedRun(run, repo)
        expect(prompt.text.contains("exceeded the process time limit"))
        expect(prompt.text.contains("Failed steps: Critical Qt client contracts."))
        expect(!prompt.text.contains("ClientErrorReports"))
        expect(!com.forkmesh.nativeapp.navigation.MessageHandoff.cleaned(log).contains("exceeded the process time limit"))
        val target = prompt.composeTarget
        assertEq("alice/tool", "${target.repo?.owner}/${target.repo?.name}")
        assertEq(prompt.text, target.text)
    }

    fun testPullChecksKeepTheLatestRunPerWorkflow() {
        val runs = listOf(
            ActionsRunSummary(1, workflowPath = "a", status = ActionsRunStatus.Failed, createdAtMs = 1, pullNumber = 7),
            ActionsRunSummary(2, workflowPath = "a", status = ActionsRunStatus.Success, createdAtMs = 2, pullNumber = 7),
            ActionsRunSummary(3, workflowPath = "b", status = ActionsRunStatus.Running, createdAtMs = 3, pullNumber = 7),
            ActionsRunSummary(4, workflowPath = "c", status = ActionsRunStatus.Failed, createdAtMs = 3, pullNumber = 8),
        )
        val checks = PullChecksSummary(runs, 7)
        assertEq(2, checks.runs.size)
        assertEq("pending", checks.state)
        assertEq("1 of 2 checks running", checks.label)
        assertEq("No checks", PullChecksSummary(runs, 99).label)
    }

    fun testCiErrorMessagesAndDecode() {
        assertEq("Only the repository owner and organization writers can do that.", CiError(403, "").message)
        assertEq("Some Odd Thing.", CiError(400, "some_odd_thing").message)
        assertEq("The server answered 418.", CiError(418, "").message)
        val error = assertFails<CiError> { CiRequests.decode(ApiJsonResult(200, linkedMapOf("error" to "lease_held"))) }
        assertEq("A scan is already running.", error.message)
        assertEq(mapOf("ok" to true), CiRequests.decode(ApiJsonResult(200, mapOf("ok" to true))))
        assertEq("forkmesh-issues-pull-v1\nalice\n12", CiRequests.announceCanonical("alice", "12"))
    }

    fun testReleasesArtifactsAndAnnouncement() {
        val client = FakeCiClient()
        val asset = RepoReleaseAsset("app.apk", "aa", size = 10, os = "android", arch = "arm64")
        client.releaseList = listOf(RepoRelease("v1", assets = listOf(asset, asset.copy(name = "b", size = 5))))
        val model = RepoReleasesModel(client, repo)
        model.load()
        assertEq(2, model.artifacts.size)
        assertEq(15L, model.artifactsTotalBytes)
        val draft = ReleaseAnnouncementDraft.forRelease(model.releases[0], repo)
        assertEq("tool v1", draft.title)
        assertEq("alice/tool v1 is out.\nDownloads: android / arm64.", draft.body)
        assertEq("Sign in on this device to announce a release.", model.announce(draft, "v1", "", null, 1))
        assertEq("Announced on the Fediverse.", model.announce(draft, "v1", "Alice", { "sig:${String(it)}" }, 12))
        assertEq(listOf("v1", "tool v1", draft.body, "alice", "sig:forkmesh-issues-pull-v1\nalice\n12"), client.announcements.single())
        draft.title = " "
        assertEq("Give the announcement a title.", model.announce(draft, "v1", "alice", { "" }, 1))
    }

    fun testMirrorNodesSortAndLights() {
        val client = FakeCiClient()
        client.mirrors = linkedMapOf(
            "mirrors" to listOf(
                linkedMapOf("node" to "b", "status" to "offline"),
                linkedMapOf("node" to "c", "status" to "online", "behind" to 2L),
                linkedMapOf("node" to "a", "status" to "live", "cloneAvailable" to true, "branch" to "main", "commit" to "0123456789", "latencyMs" to 40L),
            ),
        )
        val model = RepoMirrorsModel(client, repo)
        model.load()
        assertEq(listOf("a", "c", "b"), model.nodes.map { it.node })
        assertEq(listOf("current", "behind", "offline"), model.nodes.map { it.light })
        assertEq("main @ 01234567 · 40 ms", model.nodes[0].subtitle)
        model.onlineOnly = true
        assertEq(2, model.shown.size)
    }

    fun testSecurityFallsBackToLatestAndTriages() {
        val client = FakeCiClient()
        client.scanFailures.add("history")
        client.scans["latest"] = linkedMapOf(
            "latest" to linkedMapOf(
                "scanId" to "s1",
                "clipboard" to linkedMapOf("commitHash" to "abcdef0123", "findings" to linkedMapOf("high" to 1L, "informational" to 2L)),
                "rich" to linkedMapOf(
                    "findings" to listOf(
                        linkedMapOf("id" to "F2", "severity" to "info", "path" to "b", "line" to 3L),
                        linkedMapOf("id" to "F1", "severity" to "high", "path" to "a"),
                    ),
                ),
            ),
        )
        client.scans["lease"] = linkedMapOf("due" to true)
        val model = RepoSecurityModel(client, repo)
        model.load()
        assertEq(null, model.error)
        val scan = model.latest!!
        assertEq("1 high · 2 info", scan.headline)
        assertEq(SecuritySeverity.High, scan.worst)
        assertEq(listOf("f1", "f2"), scan.findings.map { it.id })
        assertEq("b:3", scan.findings[1].location)
        expect(model.scanStatus!!.label.startsWith("A new scan is due"))
        model.severityFilter = SecuritySeverity.Info
        assertEq(1, model.findings.size)
        expect(!model.triage(scan.findings[0], "bogus"))
        expect(model.triage(scan.findings[0], "dismissed", "noise"))
        assertEq(listOf("s1", "f1", "dismissed", "noise"), client.triages.single())
        assertEq("dismissed", model.latest!!.findings[0].reviewStatus)
    }

    fun testWebhooksDraftAndLifecycle() {
        assertEq("Use an https:// address.", RepoWebhookDraft("http://x.test", setOf("a")).validationError)
        assertEq("Choose at least one event.", RepoWebhookDraft("https://x.test").validationError)
        assertEq(null, RepoWebhookDraft("https://x.test/hook", setOf("b", "a")).validationError)
        assertEq(listOf("a", "b"), RepoWebhookDraft("https://x.test/hook", setOf("b", "a")).fields["events"])
        val client = FakeCiClient()
        client.webhookAnswers["list"] = linkedMapOf(
            "webhooks" to listOf(linkedMapOf("id" to "w1", "url" to "https://x.test", "active" to true, "consecutiveFailures" to 2L)),
            "limits" to linkedMapOf("maxWebhooks" to 1L),
            "events" to listOf(linkedMapOf("type" to "repository.push")),
        )
        client.webhookAnswers["create"] = linkedMapOf("webhook" to linkedMapOf("id" to "w2", "secret" to "s3cret"))
        client.webhookAnswers["test"] = linkedMapOf("status" to 204L)
        val model = RepoWebhooksModel(client, repo)
        model.load()
        assertEq("2 failed deliveries", model.webhooks[0].healthLabel)
        assertEq("repository.push", model.eventTypes.single().label)
        expect(!model.canAdd)
        expect(!model.create(RepoWebhookDraft("https://y.test", setOf("repository.push"))))
        assertEq("A repository can have at most 1 webhooks.", model.error)
        assertEq("Test delivered (HTTP 204).", model.test("w1"))
        expect(model.delete("w1"))
        expect(model.webhooks.isEmpty())
        client.webhookAnswers["list"] = emptyMap()
        expect(model.create(RepoWebhookDraft("https://y.test", setOf("repository.push"))))
        assertEq("w2" to "s3cret", model.revealedSecret)
        assertEq("create", client.webhookCalls.first { it.first == "create" }.first)
    }
}
