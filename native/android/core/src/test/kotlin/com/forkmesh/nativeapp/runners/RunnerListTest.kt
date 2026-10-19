package com.forkmesh.nativeapp.runners

import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class RunnerListTest {
    private val work = "0123456789abcdef0123456789abcdef"
    private val now = 1_770_000_000_000L

    private fun json(text: String): JsonMap = Json.parse(text).asJsonMap()

    private val payload =
        """
        {"ok":true,"now":$now,"runners":[
          {"name":"old-box","scope":"personal","status":"revoked","lastSeenAt":${now - 86_400_000},"activeLeases":0,"availability":{
            "claude-code":{"binaryFound":true,"loginState":"available","auth":{"state":"ok"}}}},
          {"name":"laptop","scope":"personal","status":"active","lastSeenAt":${now - 60_000},"activeLeases":2,"availability":{
            "claude-code":{"binaryFound":true,"loginState":"available","auth":{"state":"ok"},"accounts":[
              {"id":"$work","label":"Work","email":"w@example.com","plan":"Max","active":true}]},
            "codex":{"binaryFound":true,"loginState":"expired","auth":{"state":"expired","message":"Token\u0007 revoked","expiresAt":5}}}},
          {"name":"fmnext-r2","scope":"pool","status":"paused","lastSeenAt":${now - 11 * 60_000},"activeLeases":1,"availability":{
            "claude-code":{"binaryFound":true,"auth":{"state":"expiring"}},
            "codex":{"binaryFound":false,"loginState":"missing"}}},
          {"name":"fmnext-r3","status":"active","availability":{
            "claude-code":{"binaryFound":true,"loginState":"missing"},
            "gemini":{"binaryFound":true}}},
          {"name":"Bad Name","status":"active"}
        ]}
        """

    fun testParsesRunnersAndListsRevokedLast() {
        val rows = RunnerList.parse(json(payload))
        assertEq(listOf("laptop", "fmnext-r2", "fmnext-r3", "old-box"), rows.map { it.name })
        val laptop = rows[0]
        assertEq("personal", laptop.scope)
        assertEq(RunnerStatus.Active, laptop.status)
        assertEq(now - 60_000, laptop.lastSeenAtMs)
        assertEq(2, laptop.activeLeases)
        assertEq(listOf("claude-code", "codex"), laptop.providers.map { it.provider })
        val claude = laptop.providers[0]
        assertEq(RunnerAuth.Ok, claude.auth)
        assertEq("Work", claude.activeAccount?.displayName)
        assertEq(1, claude.accountCount)
        val codex = laptop.providers[1]
        assertEq(RunnerAuth.Expired, codex.auth)
        assertEq("Token revoked", codex.message)
        assertEq(5L, codex.expiresAtMs)
        expect(codex.needsSignIn)
        assertEq(null, codex.activeAccount)
        val paused = rows[1]
        assertEq(RunnerStatus.Paused, paused.status)
        assertEq(RunnerAuth.Expiring, paused.providers[0].auth)
        assertEq(RunnerAuth.NotInstalled, paused.providers[1].auth)
        assertEq(listOf("claude-code"), paused.signInProviders)
        assertEq(listOf("claude-code"), rows[2].providers.map { it.provider })
        assertEq(RunnerAuth.SignedOut, rows[2].providers[0].auth)
        assertEq(0, rows[2].activeLeases)
        assertEq(RunnerStatus.Revoked, rows[3].status)
        expect(rows[3].revoked)
        assertEq(emptyList<RunnerRow>(), RunnerList.parse(json("""{"runners":"nope"}""")))
    }

    fun testStatusAndOnlineWindow() {
        assertEq(RunnerStatus.Active, RunnerStatus.of(" Active "))
        assertEq(RunnerStatus.Paused, RunnerStatus.of("paused"))
        assertEq(RunnerStatus.Revoked, RunnerStatus.of("revoked"))
        assertEq(RunnerStatus.Unknown, RunnerStatus.of(""))
        assertEq(RunnerStatus.Unknown, RunnerStatus.of("deleted"))
        expect(RunnerList.online(now - 10 * 60_000, now))
        expect(!RunnerList.online(now - 10 * 60_000 - 1, now))
        expect(RunnerList.online(now + 5_000, now))
        expect(!RunnerList.online(0, now))
        val rows = RunnerList.parse(json(payload))
        expect(rows[0].online(now))
        expect(!rows[1].online(now))
        assertEq("Online", RunnerList.seenText(now - 60_000, now))
        assertEq("Seen 11m ago", RunnerList.seenText(now - 11 * 60_000, now))
        assertEq("Never seen", RunnerList.seenText(0, now))
    }

    fun testActionsForEachStatus() {
        assertEq(
            listOf(RunnerAction.SignIn, RunnerAction.Accounts, RunnerAction.Pause, RunnerAction.Revoke),
            RunnerList.actions(RunnerStatus.Active),
        )
        assertEq(
            listOf(RunnerAction.SignIn, RunnerAction.Accounts, RunnerAction.Resume, RunnerAction.Revoke),
            RunnerList.actions(RunnerStatus.Paused),
        )
        assertEq(emptyList<RunnerAction>(), RunnerList.actions(RunnerStatus.Revoked))
        assertEq(emptyList<RunnerAction>(), RunnerList.actions(RunnerStatus.Unknown))
        assertEq("/api/runners/pause", RunnerList.actionPath(RunnerAction.Pause))
        assertEq("/api/runners/resume", RunnerList.actionPath(RunnerAction.Resume))
        assertEq("/api/runners/revoke", RunnerList.actionPath(RunnerAction.Revoke))
        assertEq(null, RunnerList.actionPath(RunnerAction.SignIn))
        assertEq(null, RunnerList.actionPath(RunnerAction.Accounts))
        assertEq(mapOf("name" to "laptop"), RunnerList.actionBody(" Laptop "))
        assertEq(RunnerStatus.Paused, RunnerList.statusAfter(RunnerAction.Pause))
        assertEq(RunnerStatus.Active, RunnerList.statusAfter(RunnerAction.Resume))
        assertEq(RunnerStatus.Revoked, RunnerList.statusAfter(RunnerAction.Revoke))
    }

    fun testLabels() {
        assertEq("Paused", RunnerList.statusLabel(RunnerStatus.Paused))
        assertEq("Revoked", RunnerList.statusLabel(RunnerStatus.Revoked))
        assertEq("", RunnerList.statusLabel(RunnerStatus.Active))
        assertEq("No jobs", RunnerList.jobsText(0))
        assertEq("1 job", RunnerList.jobsText(1))
        assertEq("3 jobs", RunnerList.jobsText(3))
        val rows = RunnerList.parse(json(payload))
        assertEq("personal · 2 jobs", RunnerList.summary(rows[0]))
        assertEq("No jobs", RunnerList.summary(rows[2]))
        assertEq("Signed in · Work", RunnerList.providerText(rows[0].providers[0]))
        assertEq("Sign-in expired", RunnerList.providerText(rows[0].providers[1]))
        assertEq("Sign-in expires soon", RunnerList.providerText(rows[1].providers[0]))
        assertEq("Not installed", RunnerList.providerText(rows[1].providers[1]))
        assertEq("Not signed in", RunnerList.providerText(rows[2].providers[0]))
        assertEq("Revoke laptop?", RunnerList.revokeTitle("laptop"))
        expect(RunnerList.revokeMessage("laptop").contains("cannot be undone"))
        assertEq("Could not pause the runner: That runner is revoked.", RunnerList.actionError(RunnerAction.Pause, 409, json("""{"error":"runner_inactive","message":"That runner is revoked."}""")))
        assertEq("Could not resume the runner: that runner is revoked", RunnerList.actionError(RunnerAction.Resume, 409, emptyMap()))
        assertEq("Could not revoke the runner: that runner is not registered to this account", RunnerList.actionError(RunnerAction.Revoke, 404, emptyMap()))
        assertEq("Could not pause the runner: ForkMesh could not be reached", RunnerList.actionError(RunnerAction.Pause, 0, emptyMap()))
    }

    private class FakeTransport(private val answers: Map<String, Pair<Int, String>>) : HttpTransport {
        val seen = ArrayList<HttpClient.Request>()
        override fun send(request: HttpClient.Request): HttpClient.Response {
            seen += request
            val key = request.method + " " + request.url.substringAfter("https://app.test")
            val (status, body) = answers[key] ?: (404 to "{\"error\":\"not_found\"}")
            return HttpClient.Response(status, emptyMap(), body.toByteArray())
        }
    }

    private fun api(transport: HttpTransport, token: String = "tok") = RunnerAttentionApi({ "https://app.test/" }, { token }, transport)

    fun testLoadReportsSignedOutLoadedAndFailed() {
        val ok = FakeTransport(mapOf("GET /api/runners" to (200 to payload)))
        val loaded = RunnerList.load(api(ok)) as RunnerListResult.Loaded
        assertEq(4, loaded.rows.size)
        assertEq("Bearer tok", ok.seen.single().headers["Authorization"])
        val noSession = FakeTransport(emptyMap())
        assertEq(RunnerListResult.SignedOut, RunnerList.load(api(noSession, token = "")))
        expect(noSession.seen.isEmpty())
        assertEq(RunnerListResult.SignedOut, RunnerList.load(api(FakeTransport(mapOf("GET /api/runners" to (401 to "{}"))))))
        assertEq(
            RunnerListResult.Failed("Could not load runners: HTTP 500"),
            RunnerList.load(api(FakeTransport(mapOf("GET /api/runners" to (500 to "{}"))))),
        )
    }

    fun testChangePostsNameAndReadsStatus() {
        val transport = FakeTransport(
            mapOf(
                "POST /api/runners/pause" to (200 to """{"ok":true,"name":"laptop","status":"paused"}"""),
                "POST /api/runners/resume" to (409 to """{"error":"runner_inactive","message":"That runner is revoked."}"""),
                "POST /api/runners/revoke" to (200 to """{"ok":true}"""),
            ),
        )
        val paused = RunnerList.change(api(transport), RunnerAction.Pause, "Laptop")
        expect(paused.ok)
        assertEq(RunnerStatus.Paused, paused.status)
        assertEq("laptop is paused. It finishes the jobs it holds and takes no new ones.", paused.message)
        val post = transport.seen.single()
        assertEq("POST", post.method)
        assertEq("https://app.test/api/runners/pause", post.url)
        assertEq(mapOf("name" to "laptop"), Json.parse(String(post.body!!, Charsets.UTF_8)))
        assertEq("application/json", post.headers["Content-Type"])
        assertEq("https://app.test", post.headers["Origin"])
        val resumed = RunnerList.change(api(transport), RunnerAction.Resume, "laptop")
        expect(!resumed.ok)
        assertEq(null, resumed.status)
        assertEq("Could not resume the runner: That runner is revoked.", resumed.message)
        val revoked = RunnerList.change(api(transport), RunnerAction.Revoke, "laptop")
        assertEq(RunnerStatus.Revoked, revoked.status)
        val before = transport.seen.size
        expect(!RunnerList.change(api(transport), RunnerAction.Accounts, "laptop").ok)
        expect(!RunnerList.change(api(transport), RunnerAction.Pause, "Bad Name").ok)
        assertEq(before, transport.seen.size)
    }

    fun testAccountSectionsIncludePausedRunners() {
        val runners = RunnerAccounts.parse(json(payload))
        assertEq(
            listOf("laptop:claude-code", "laptop:codex", "fmnext-r2:claude-code", "fmnext-r3:claude-code"),
            RunnerAccounts.sections(runners).map { it.key },
        )
    }
}
