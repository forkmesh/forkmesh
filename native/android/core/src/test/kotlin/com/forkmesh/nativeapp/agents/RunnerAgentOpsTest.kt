package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.model.RepoComparison
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.security.DeployApproval
import com.forkmesh.nativeapp.security.DeployApprover
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class RunnerAgentOpsTest {
    private class FakePullInbox(
        private val comparison: () -> RepoComparison,
        private val number: Int = 31,
        private val defaultBranch: String = "trunk",
    ) : RunnerPullInbox {
        val compared = ArrayList<String>()
        val drafts = ArrayList<RunnerPullDraft>()
        val submittedCommits = ArrayList<String>()

        override fun defaultBranch(owner: String, repo: String): String = defaultBranch

        override fun compare(owner: String, repo: String, base: String, head: String): RepoComparison {
            compared += "$owner/$repo $base...$head"
            return comparison()
        }

        override fun submit(owner: String, repo: String, draft: RunnerPullDraft, comparison: RepoComparison): Int {
            drafts += draft
            submittedCommits += comparison.commits
            return number
        }
    }

    private val goodComparison = RepoComparison(baseOid = "a".repeat(40), headOid = "b".repeat(40), patch = "diff --git a/x b/x\n", commits = "From abc")

    private class Recorder(private val answers: MutableList<Pair<Int, String>>) : HttpTransport {
        val requests = ArrayList<HttpClient.Request>()

        override fun send(request: HttpClient.Request): HttpClient.Response {
            requests.add(request)
            val (status, body) = if (answers.size > 1) answers.removeAt(0) else answers.first()
            return HttpClient.Response(status, emptyMap(), body.toByteArray())
        }

        fun body(index: Int) = Json.parse(String(requests[index].body!!)).asJsonMap()
    }

    private val ref = RunnerSessionRef("alice", "app", "s1")

    private fun ops(recorder: Recorder, sleeps: MutableList<Long> = ArrayList()): RunnerAgentOps {
        var n = 0
        val api = AgentQueueApi({ "https://app.example.com" }, { "tok" }, recorder)
        return RunnerAgentOps(api, sleep = { sleeps += it }, requestId = { "request-id-${++n}" })
    }

    private val sessionJson = """{"id":"s1","org":"alice","repo":"app","status":"completed","localAgentId":7,
        "branch":"agent/12-fix-login","baseBranch":"main","runner":"fmnext-r1",
        "diffStats":{"files":3,"added":40,"removed":2,"ahead":2,"behind":1,"conflicted":false,"known":true},
        "merge":{"state":"","branchDeleted":false,"cleanupPending":[]},"canMerge":true,
        "agentInfo":{"branchName":"agent/s1","prNumber":4}}"""

    fun testSessionDecodesRunnerFields() {
        val session = AgentLaunchSession.fromJson(Json.parse(sessionJson).asJsonMap())
        assertEq("agent/12-fix-login", session.branchName)
        assertEq("main", session.baseBranch)
        assertEq("fmnext-r1", session.runner)
        expect(session.isOnRunner && session.canMerge && !session.isMergedBranch)
        assertEq(3, session.diffStats.files)
        assertEq(40, session.diffStats.added)
        assertEq(4, session.prNumber)
        val merged = AgentLaunchSession.fromJson(
            Json.parse("""{"id":"s2","branch":"agent/x","merge":{"state":"merged","branchDeleted":false,"cleanupPending":["fmnext-n2"]},"diffStats":{"known":false}}""").asJsonMap(),
        )
        expect(merged.merged && merged.needsCleanup)
        expect(!merged.diffStats.known)
        assertEq(-1, merged.diffStats.files)
        val fallback = AgentLaunchSession.fromJson(Json.parse("""{"id":"s3","agentInfo":{"branchName":"agent/s3"}}""").asJsonMap())
        assertEq("agent/s3", fallback.branchName)
        expect(AgentLaunchTarget.fromJson(Json.parse("""{"node":"fmnext-r1","runtimeMode":"runner-pool"}""").asJsonMap()).isMirror)
    }

    fun testDiffReadsPagedPath() {
        val recorder = Recorder(
            mutableListOf(
                200 to """{"ok":true,"id":"s1","branch":"agent/x","base":"main","files":[{"path":"a.txt","added":4,"removed":1},{"path":"b.txt","added":1,"removed":0}],
                    "committed":"diff --git a/a.txt b/a.txt\n","page":0,"pageSize":1,"pageCount":2,"commits":2,"ahead":2,"behind":0,"clean":true,"conflicts":[],"merged":false}""",
            ),
        )
        val diff = ops(recorder).diff(ref, page = 0, pageSize = 1)
        assertEq("https://app.example.com/api/repo/alice/app/agent-sessions/s1/diff?page=0&pageSize=1", recorder.requests.single().url)
        assertEq("GET", recorder.requests.single().method)
        expect(diff.hasMore && diff.diff.truncated)
        assertEq(5, diff.diff.added)
        assertEq("agent/x", diff.diff.branch)
        assertEq(true, diff.clean)
        val next = diff.copy(page = 1, diff = diff.diff.copy(committed = "diff --git a/b.txt b/b.txt\n", truncated = false))
        val joined = diff.appending(next)
        expect(joined.diff.committed.contains("a/a.txt") && joined.diff.committed.contains("a/b.txt"))
        expect(!joined.hasMore)
    }

    fun testDiffErrorsKeepTheirCode() {
        val recorder = Recorder(mutableListOf(409 to """{"error":"no_branch","message":"x"}"""))
        val error = assertFails<RunnerOpsException> { ops(recorder).diff(ref) }
        assertEq("no_branch", error.code)
        assertEq("The agent has not pushed a branch yet.", error.message)
    }

    fun testMergePollsUntilMerged() {
        val sleeps = ArrayList<Long>()
        val recorder = Recorder(
            mutableListOf(
                202 to """{"ok":true,"status":"processing","requestId":"request-id-1"}""",
                202 to """{"ok":true,"status":"processing","requestId":"request-id-1"}""",
                200 to """{"ok":true,"status":"merged","requestId":"request-id-1","branch":"agent/x","base":"main","mergeOid":"abc1234def",
                    "branchDeleted":true,"cleanupPending":[],"stats":{"base":"main","commit":"abc1234def","commits":2,"files":3,"insertions":40,"deletions":2}}""",
            ),
        )
        val result = ops(recorder, sleeps).merge(ref, branch = "agent/x")
        assertEq(3, recorder.requests.size)
        assertEq(listOf(1000L, 1000L), sleeps)
        for (index in 0..2) {
            val body = recorder.body(index)
            assertEq("merge", body["action"])
            assertEq(true, body["cleanup"])
            assertEq("request-id-1", body["requestId"])
            assertEq(listOf("action", "cleanup", "requestId"), body.keys.toList())
            assertEq("https://app.example.com/api/repo/alice/app/agent-sessions/s1", recorder.requests[index].url)
        }
        assertEq("merged", result.status)
        expect(result.branchDeleted)
        assertEq(40, result.stats?.insertions)
        assertEq("Merged agent/x into main and cleaned up. 2 commits · 3 files +40 −2.", result.summary)
    }

    fun testMergeSignsADeployApprovalKeptAcrossPolls() {
        val identity = Identity.fromSeed(ByteArray(32) { it.toByte() })
        val recorder = Recorder(
            mutableListOf(
                202 to """{"ok":true,"status":"processing"}""",
                200 to """{"ok":true,"status":"merged","branch":"agent/x","base":"main"}""",
            ),
        )
        val api = AgentQueueApi({ "https://app.example.com" }, { "tok" }, recorder, { _, _ -> "ForkMesh" })
        val ops = RunnerAgentOps(
            api,
            sleep = {},
            requestId = { "mobile_req" },
            approver = DeployApprover({ identity }, clock = { 1791500000000L }),
        )
        ops.merge(RunnerSessionRef("someone", "forkmesh", "sess-1", orgScope = true))
        assertEq(2, recorder.requests.size)
        assertEq("https://app.example.com/api/orgs/ForkMesh/repos/forkmesh/agent-bots/sess-1", recorder.requests[0].url)
        for (index in 0..1) {
            val body = recorder.body(index)
            assertEq(listOf("action", "cleanup", "requestId", "deployApproval"), body.keys.toList())
            assertEq("mobile_req", body["requestId"])
            val approval = body["deployApproval"] as Map<*, *>
            assertEq("A6EHv_POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg", approval["publicKey"])
            assertEq(1791500000000L, (approval["ts"] as Number).toLong())
            assertEq("pUSma40Gi6pAd31NUblKmuZqxZ3ZNlOMQKWV7WchNhMPrxKdudlpzRA_02zGnF3PgCrcMZ4d3qLgv2Rj5u7xAg", approval["signature"])
        }
        val update = Recorder(mutableListOf(200 to """{"ok":true,"status":"updated","branch":"agent/x","base":"main"}"""))
        RunnerAgentOps(AgentQueueApi({ "https://app.example.com" }, { "tok" }, update), requestId = { "r" }, approver = DeployApprover({ identity }))
            .update(ref)
        expect("deployApproval" !in update.body(0))
        val keyless = Recorder(mutableListOf(200 to """{"ok":true,"status":"merged","branch":"agent/x","base":"main"}"""))
        RunnerAgentOps(AgentQueueApi({ "https://app.example.com" }, { "tok" }, keyless), requestId = { "r" }, approver = DeployApprover({ null }))
            .merge(ref)
        assertEq(listOf("action", "cleanup", "requestId"), keyless.body(0).keys.toList())
    }

    fun testMergeConflictIsReportedWithFiles() {
        val recorder = Recorder(mutableListOf(409 to """{"ok":false,"error":"merge_conflict","conflicts":["a.txt","b.txt"],"message":"m"}"""))
        val model = RunnerMergeModel(ops(recorder), ref, "agent/x")
        expect(!model.run())
        assertEq(listOf("a.txt", "b.txt"), model.conflicts)
        expect(model.conflicted)
        expect(model.errorText.startsWith("agent/x does not merge cleanly: a.txt, b.txt."))
        assertEq(1, recorder.requests.size)
    }

    fun testStaleRefsRetryOnceWithANewRequestId() {
        val recorder = Recorder(
            mutableListOf(
                409 to """{"ok":false,"error":"stale_base"}""",
                200 to """{"ok":true,"status":"merged","branch":"agent/x","base":"main"}""",
            ),
        )
        val result = ops(recorder).merge(ref)
        assertEq("merged", result.status)
        assertEq("request-id-1", recorder.body(0)["requestId"])
        assertEq("request-id-2", recorder.body(1)["requestId"])
        val twice = Recorder(mutableListOf(409 to """{"ok":false,"error":"stale_head"}"""))
        assertEq("stale_head", assertFails<RunnerOpsException> { ops(twice).merge(ref) }.code)
        assertEq(2, twice.requests.size)
    }

    fun testCleanupAndItsGuard() {
        val ok = Recorder(mutableListOf(200 to """{"ok":true,"status":"cleaned","branch":"agent/x","branchDeleted":true,"cleanupPending":["fmnext-n3"]}"""))
        val result = ops(ok).cleanup(ref, "agent/x")
        assertEq("cleanup", ok.body(0)["action"])
        expect(ok.body(0)["cleanup"] == null)
        assertEq(listOf("fmnext-n3"), result.cleanupPending)
        assertEq("Removed agent/x; fmnext-n3 still to catch up.", result.summary)
        val refused = Recorder(mutableListOf(409 to """{"ok":false,"error":"not_merged"}"""))
        val model = RunnerMergeModel(ops(refused), ref, "agent/x", cleanupOnly = true)
        expect(!model.run())
        assertEq("This branch is not merged yet, so it was kept.", model.errorText)
        expect(!model.conflicted)
        val forbidden = Recorder(mutableListOf(403 to """{"error":"forbidden"}"""))
        assertEq(403, assertFails<RunnerOpsException> { ops(forbidden).merge(ref) }.status)
    }

    fun testDeleteDiscardsTheBranchBeforeTheRun() {
        val ok = Recorder(
            mutableListOf(
                200 to """{"ok":true,"status":"cleaned","branch":"agent/x","branchDeleted":true,"cleanupPending":[]}""",
                200 to """{"ok":true,"deleted":true}""",
            ),
        )
        val report = ops(ok).delete(ref, "agent/x", pull = 31)
        assertEq(listOf("POST", "DELETE"), ok.requests.map { it.method })
        assertEq(
            "Deleted the agent on ForkMesh with branch agent/x. Kept pull request #31: the phone cannot close it.",
            report.summary("on ForkMesh"),
        )
        assertEq("cleanup", ok.body(0)["action"])
        assertEq(true, ok.body(0)["discard"])
        val bare = Recorder(mutableListOf(200 to """{"ok":true,"deleted":true}"""))
        ops(bare).delete(ref, "")
        assertEq(listOf("DELETE"), bare.requests.map { it.method })
        val noBranch = Recorder(
            mutableListOf(409 to """{"ok":false,"error":"no_branch"}""", 200 to """{"ok":true,"deleted":true}"""),
        )
        val gone = ops(noBranch).delete(ref, "agent/x")
        assertEq(listOf("POST", "DELETE"), noBranch.requests.map { it.method })
        assertEq("Deleted the agent. Already gone: branch agent/x. No pull request was attached.", gone.summary())
        val member = Recorder(
            mutableListOf(403 to """{"error":"forbidden"}""", 200 to """{"ok":true,"deleted":true}"""),
        )
        ops(member).delete(ref, "agent/x")
        assertEq(listOf("POST", "DELETE"), member.requests.map { it.method })
        val kept = Recorder(mutableListOf(503 to """{"ok":false,"error":"branch_delete_pending","retryable":true}"""))
        assertEq("branch_delete_pending", assertFails<RunnerOpsException> { ops(kept).delete(ref, "agent/x") }.code)
        expect(kept.requests.none { it.method == "DELETE" })
    }

    fun testDeletingAnActiveRunStopsItFirst() {
        val recorder = Recorder(
            mutableListOf(
                200 to """{"ok":true,"status":"stopping"}""",
                200 to """{"ok":true,"status":"cleaned","branch":"agent/x","branchDeleted":true,"cleanupPending":[]}""",
                200 to """{"ok":true,"deleted":true}""",
            ),
        )
        ops(recorder).delete(ref, "agent/x", active = true)
        assertEq(listOf("POST", "POST", "DELETE"), recorder.requests.map { it.method })
        assertEq("stop", recorder.body(0)["action"])
        assertEq("cleanup", recorder.body(1)["action"])
        assertEq(true, recorder.body(1)["discard"])
        val failedStop = Recorder(mutableListOf(500 to """{"error":"boom"}""", 200 to """{"ok":true,"deleted":true}"""))
        ops(failedStop).delete(ref, "", active = true)
        assertEq(listOf("POST", "DELETE"), failedStop.requests.map { it.method })
        val idle = Recorder(mutableListOf(200 to """{"ok":true,"deleted":true}"""))
        ops(idle).delete(ref, "", active = false)
        assertEq(listOf("DELETE"), idle.requests.map { it.method })
    }

    fun testDeleteWaitsForARunningSessionBeforeDiscarding() {
        val running = 409 to """{"ok":false,"error":"session_running"}"""
        val recorder = Recorder(
            mutableListOf(
                running,
                running,
                200 to """{"ok":true,"status":"cleaned","branch":"agent/x","branchDeleted":true,"cleanupPending":[]}""",
                200 to """{"ok":true,"deleted":true}""",
            ),
        )
        val sleeps = ArrayList<Long>()
        ops(recorder, sleeps).delete(ref, "agent/x")
        assertEq(listOf("POST", "POST", "POST", "DELETE"), recorder.requests.map { it.method })
        assertEq(listOf(2000L, 2000L), sleeps)
    }

    fun testDeleteGivesUpOnTheBranchWhenTheSessionKeepsRunning() {
        val answers = MutableList(10) { 409 to """{"ok":false,"error":"session_running"}""" }
        answers += 200 to """{"ok":true,"deleted":true}"""
        val recorder = Recorder(answers)
        val sleeps = ArrayList<Long>()
        ops(recorder, sleeps).delete(ref, "agent/x")
        assertEq(10, recorder.requests.count { it.method == "POST" })
        assertEq("DELETE", recorder.requests.last().method)
        assertEq(11, recorder.requests.size)
        assertEq(List(9) { 2000L }, sleeps)
    }

    fun testContinueSteersTheSameSession() {
        val recorder = Recorder(mutableListOf(202 to """{"ok":true,"status":"queued"}"""))
        ops(recorder).continueRun(ref)
        assertEq("https://app.example.com/api/repo/alice/app/agent-sessions/s1", recorder.requests.single().url)
        assertEq("Continue where you left off.", recorder.body(0)["prompt"])
    }

    fun testLaunchSendsBaseAndBranchName() {
        val recorder = Recorder(mutableListOf(202 to """{"ok":true,"session":{"id":"s5","branch":"agent/3-fix-the-login"}}"""))
        val api = AgentQueueApi({ "https://app.example.com" }, { "tok" }, recorder)
        val result = api.launch("alice", "app", "Fix the login page", targetNode = "fmnext-r1", baseBranch = "dev", branchName = RunnerBranchName.slug("Fix the login page!"))
        val body = recorder.body(0)
        assertEq("dev", body["baseBranch"])
        assertEq("fix-the-login-page", body["branchName"])
        assertEq("agent/3-fix-the-login", result.session?.branchName)
        val plain = Recorder(mutableListOf(202 to "{}"))
        AgentQueueApi({ "https://x" }, { "tok" }, plain).launch("a", "b", "go")
        expect(!plain.body(0).containsKey("baseBranch") && !plain.body(0).containsKey("branchName"))
        assertEq("a-very-long-title-that", RunnerBranchName.slug("A very long title that keeps going", words = 5, limit = 22))
        assertEq("", RunnerBranchName.slug("!!!"))
    }

    fun testSessionDecodesCanOpenPullAndTopLevelPrNumber() {
        val open = AgentLaunchSession.fromJson(
            Json.parse("""{"id":"s1","branch":"agent/x","runner":"fmnext-r1","canOpenPull":true,"prNumber":0,"agentInfo":{"prNumber":0}}""").asJsonMap(),
        )
        expect(open.canOpenPull)
        assertEq(0, open.prNumber)
        val linked = AgentLaunchSession.fromJson(
            Json.parse("""{"id":"s2","branch":"agent/y","canOpenPull":false,"prNumber":12,"agentInfo":{"prNumber":4}}""").asJsonMap(),
        )
        expect(!linked.canOpenPull)
        assertEq(12, linked.prNumber)
        val reported = AgentLaunchSession.fromJson(Json.parse("""{"id":"s3","agentInfo":{"prNumber":4}}""").asJsonMap())
        assertEq(4, reported.prNumber)
        expect(!reported.canOpenPull)
    }

    fun testLinkPullPostsTheNumberOnce() {
        val recorder = Recorder(
            mutableListOf(200 to """{"ok":true,"status":"linked","branch":"agent/x","prNumber":31,"message":"Opened pull request #31 for agent/x."}"""),
        )
        val link = ops(recorder).linkPull(ref, 31, "agent/x")
        val request = recorder.requests.single()
        assertEq("POST", request.method)
        assertEq("https://app.example.com/api/repo/alice/app/agent-sessions/s1", request.url)
        assertEq(mapOf("action" to "link-pull", "number" to 31L), recorder.body(0))
        assertEq(31, link.number)
        assertEq("Opened pull request #31 for agent/x.", link.message)
        val org = Recorder(mutableListOf(200 to """{"ok":true,"status":"linked","prNumber":5}"""))
        assertEq("Opened pull request #5.", ops(org).linkPull(RunnerSessionRef("acme", "site", "s9", orgScope = true), 5).message)
        assertEq("https://app.example.com/api/orgs/acme/repos/site/agent-bots/s9", org.requests.single().url)
    }

    fun testLinkPullErrorsAreWorded() {
        val exists = Recorder(mutableListOf(409 to """{"ok":false,"error":"pull_exists","message":"agent/x already has pull request #7."}"""))
        val taken = assertFails<RunnerOpsException> { ops(exists).linkPull(ref, 31, "agent/x") }
        assertEq("pull_exists", taken.code)
        assertEq("agent/x already has pull request #7.", taken.message)
        val invalid = Recorder(mutableListOf(400 to """{"ok":false,"error":"invalid_pull","message":"x"}"""))
        assertEq("The pull request number is invalid.", assertFails<RunnerOpsException> { ops(invalid).linkPull(ref, 0) }.message)
        assertEq("agent/x already has a pull request.", RunnerAgentOps.describe("pull_exists", 409, "", "agent/x"))
        assertEq("The agent has not pushed a branch yet.", RunnerAgentOps.describe("no_branch", 409, ""))
        assertEq("Only an owner who can merge this repository can merge agent branches.", RunnerAgentOps.describe("forbidden", 403, ""))
    }

    fun testPullDescriptionNamesWhereTheRunRan() {
        assertEq(
            "Opened from the agent run on the fmnext-r1 runner (agent/x).",
            RunnerPullDraft.description("fmnext-r1", "fmnext-n1", "agent/x"),
        )
        assertEq(
            "Opened from the agent run on mirror-2 (agent/x).\n\nFixed the login.",
            RunnerPullDraft.description("", "mirror-2", "agent/x", "  Fixed the login.\n"),
        )
        assertEq("Opened from the agent run on a ForkMesh node (agent/x).", RunnerPullDraft.description("", "", "agent/x", "   "))
        val long = RunnerPullDraft.description("r", "", "agent/x", "y".repeat(5000))
        assertEq("Opened from the agent run on the r runner (agent/x).\n\n" + "y".repeat(4000), long)
        val session = AgentLaunchSession(
            sessionId = "s1",
            title = "",
            branchName = "agent/x",
            runner = "fmnext-r1",
            history = listOf(
                AgentLaunchMessage(role = "user", text = "Fix it"),
                AgentLaunchMessage(role = "assistant", text = "Done: fixed it."),
                AgentLaunchMessage(role = "assistant", text = "  "),
                AgentLaunchMessage(role = "user", text = "thanks"),
            ),
        )
        val draft = RunnerPullDraft.from(session, "trunk")
        assertEq("agent/x", draft.title)
        assertEq("trunk", draft.base)
        assertEq("agent/x", draft.head)
        assertEq("Opened from the agent run on the fmnext-r1 runner (agent/x).\n\nDone: fixed it.", draft.description)
        assertEq("Fix login", RunnerPullDraft.from(session.copy(title = " Fix login ", baseBranch = "dev"), "trunk").title)
        assertEq("dev", RunnerPullDraft.from(session.copy(baseBranch = "dev"), "trunk").base)
    }

    private val pullSession = """{"session":{"id":"s1","org":"alice","repo":"app","status":"completed","title":"Fix the login",
        "branch":"agent/12-fix-login","baseBranch":"main","runner":"fmnext-r1","canOpenPull":true,
        "history":[{"role":"user","text":"Fix login"},{"role":"assistant","text":"Fixed the login form."}]}}"""

    fun testOpenPullOpensThenLinks() {
        val recorder = Recorder(
            mutableListOf(
                200 to pullSession,
                200 to """{"ok":true,"status":"linked","branch":"agent/12-fix-login","prNumber":31,"message":"Opened pull request #31 for agent/12-fix-login."}""",
            ),
        )
        val inbox = FakePullInbox({ goodComparison })
        val opened = ops(recorder).openPull(ref, inbox)
        assertEq(listOf("alice/app main...agent/12-fix-login"), inbox.compared)
        val draft = inbox.drafts.single()
        assertEq("Fix the login", draft.title)
        assertEq("main", draft.base)
        assertEq("agent/12-fix-login", draft.head)
        assertEq("Opened from the agent run on the fmnext-r1 runner (agent/12-fix-login).\n\nFixed the login form.", draft.description)
        assertEq(listOf("From abc"), inbox.submittedCommits)
        assertEq(listOf("GET", "POST"), recorder.requests.map { it.method })
        assertEq(mapOf("action" to "link-pull", "number" to 31L), recorder.body(1))
        assertEq(RunnerPullOpened(31, "Opened pull request #31 for agent/12-fix-login."), opened)
    }

    fun testOpenPullFailuresAreWorded() {
        val empty = FakePullInbox({ goodComparison.copy(patch = "  ") })
        val nothing = assertFails<RunnerOpsException> { ops(Recorder(mutableListOf(200 to pullSession))).openPull(ref, empty) }
        assertEq("agent/12-fix-login has no changes against main, so there is nothing to open a pull request for.", nothing.message)
        expect(empty.drafts.isEmpty())
        val missing = FakePullInbox({ throw IllegalStateException("HTTP 404") })
        val absent = assertFails<RunnerOpsException> { ops(Recorder(mutableListOf(200 to pullSession))).openPull(ref, missing) }
        assertEq("agent/12-fix-login is not on ForkMesh yet, so no pull request was opened.", absent.message)
        val unresolved = FakePullInbox({ goodComparison.copy(headOid = "") })
        assertEq(
            "agent/12-fix-login is not on ForkMesh yet, so no pull request was opened.",
            assertFails<RunnerOpsException> { ops(Recorder(mutableListOf(200 to pullSession))).openPull(ref, unresolved) }.message,
        )
        val refused = Recorder(
            mutableListOf(200 to pullSession, 409 to """{"ok":false,"error":"pull_exists","message":"agent/12-fix-login already has pull request #7."}"""),
        )
        val opened = ops(refused).openPull(ref, FakePullInbox({ goodComparison }))
        assertEq(31, opened.number)
        expect(!opened.linked)
        assertEq(
            "Opened pull request #31, but the run could not be linked to it: agent/12-fix-login already has pull request #7.",
            opened.message,
        )
        val noBase = Recorder(mutableListOf(200 to pullSession.replace("\"baseBranch\":\"main\",", ""), 200 to """{"ok":true,"prNumber":31}"""))
        val fallback = FakePullInbox({ goodComparison })
        ops(noBase).openPull(ref, fallback)
        assertEq("trunk", fallback.drafts.single().base)
    }

    fun testUpdatePollsWithTheSameRequestId() {
        val sleeps = ArrayList<Long>()
        val recorder = Recorder(
            mutableListOf(
                202 to """{"status":"processing","requestId":"request-id-1"}""",
                200 to """{"ok":true,"status":"updated","branch":"agent/x","base":"main","head":"abc","message":"Updated agent/x from main."}""",
            ),
        )
        val result = ops(recorder, sleeps).update(ref, "agent/x", "main")
        assertEq(2, recorder.requests.size)
        assertEq(listOf(1000L), sleeps)
        for (index in 0..1) {
            assertEq(mapOf("action" to "update", "requestId" to "request-id-1"), recorder.body(index))
            assertEq("https://app.example.com/api/repo/alice/app/agent-sessions/s1", recorder.requests[index].url)
        }
        assertEq("updated", result.status)
        assertEq("Updated agent/x from main.", result.summary)
        val already = Recorder(mutableListOf(200 to """{"ok":true,"status":"already","branch":"agent/x","base":"main"}"""))
        assertEq("agent/x is already up to date with main.", ops(already).update(ref, "agent/x", "main").summary)
        assertEq("Updated agent/x from main.", RunnerMergeResult(status = "updated", branch = "agent/x", base = "main").summary)
    }

    fun testUpdateConflictIsWordedForUpdating() {
        val recorder = Recorder(mutableListOf(409 to """{"ok":false,"status":"failed","error":"merge_conflict","conflicts":["a.txt"],"message":"m"}"""))
        val model = RunnerMergeModel(ops(recorder), ref, "agent/x", updateOnly = true, base = "main")
        expect(!model.run())
        expect(model.conflicted)
        assertEq(listOf("a.txt"), model.conflicts)
        assertEq("agent/x conflicts with main: a.txt. Nothing was changed; fix the conflicts with the agent, then update again.", model.errorText)
        assertEq("update", recorder.body(0)["action"])
        val running = Recorder(mutableListOf(409 to """{"ok":false,"error":"session_running"}"""))
        assertEq("session_running", assertFails<RunnerOpsException> { ops(running).update(ref, "agent/x", "main") }.code)
    }

    fun testTaskIdComesFromTheKeyOrATag() {
        val hex = "7e8452f4e85b599d4c27786ff3b0226c"
        assertEq(hex, RunnerTaskLink.taskId("task:$hex"))
        assertEq(hex, RunnerTaskLink.taskId("", "[task:${hex.uppercase()}] Fix it"))
        assertEq(hex, RunnerTaskLink.taskId("", "Fix it", "  [task:$hex] Fix it\n\nDetails"))
        assertEq("", RunnerTaskLink.taskId("issue:4", "Fix [task:$hex]", "No tag"))
        assertEq("", RunnerTaskLink.taskId("task:t1", "[task:t1] Fix"))
        val session = AgentLaunchSession(sessionId = "s1", title = "Fix", history = listOf(AgentLaunchMessage(role = "user", text = "[task:$hex] Fix")))
        assertEq(hex, RunnerTaskLink.taskId(session))
    }

    fun testTaskNoteNamesTheMerge() {
        assertEq(
            "Merged agent/x into main (abc1234). Merged agent/x into main and cleaned up.",
            RunnerTaskLink.note("agent/x", "main", "abc1234def5678", "Merged agent/x into main and cleaned up."),
        )
        assertEq("Merged agent/x into main.", RunnerTaskLink.note("agent/x", "main", "", " "))
    }

    private val taskHex = "29bd3db4f33b382bd7a9a3ac6a1a42ee"

    private val mergedAnswer = 200 to """{"ok":true,"status":"merged","branch":"agent/x","base":"main","mergeOid":"abc1234def","branchDeleted":true,"cleanupPending":[],"message":"Merged agent/x into main and cleaned up."}"""

    fun testMergeCompletesTheLinkedTask() {
        val recorder = Recorder(
            mutableListOf(mergedAnswer, 200 to """{"session":{"id":"s1","taskKey":"task:$taskHex","branch":"agent/x","baseBranch":"main"}}"""),
        )
        val completed = ArrayList<Pair<String, String>>()
        val model = RunnerMergeModel(ops(recorder), ref, "agent/x", tasks = { id, note -> completed += id to note })
        expect(model.run())
        assertEq(listOf("POST", "GET"), recorder.requests.map { it.method })
        assertEq(listOf(taskHex to "Merged agent/x into main (abc1234). Merged agent/x into main and cleaned up."), completed)
        assertEq(RunnerTaskLink.COMPLETED, model.taskNote)
        assertEq("abc1234def", model.result?.mergeOid)
    }

    fun testTaskFailureDoesNotFailTheMerge() {
        val recorder = Recorder(
            mutableListOf(mergedAnswer, 200 to """{"session":{"id":"s1","title":"[task:$taskHex] Fix","branch":"agent/x"}}"""),
        )
        val model = RunnerMergeModel(ops(recorder), ref, "agent/x", tasks = { _, _ -> throw IllegalStateException("task gone") })
        expect(model.run())
        assertEq("merged", model.result?.status)
        assertEq("Merged, but the linked task was not updated: task gone", model.taskNote)
        val untagged = Recorder(mutableListOf(mergedAnswer, 200 to """{"session":{"id":"s1","title":"Fix","branch":"agent/x"}}"""))
        val calls = ArrayList<String>()
        val plain = RunnerMergeModel(ops(untagged), ref, "agent/x", tasks = { id, _ -> calls += id })
        expect(plain.run())
        expect(calls.isEmpty())
        assertEq("", plain.taskNote)
        val cleanup = Recorder(mutableListOf(200 to """{"ok":true,"status":"cleaned","branch":"agent/x"}"""))
        val cleaned = RunnerMergeModel(ops(cleanup), ref, "agent/x", cleanupOnly = true, tasks = { id, _ -> calls += id })
        expect(cleaned.run())
        assertEq(1, cleanup.requests.size)
        expect(calls.isEmpty())
    }
}
