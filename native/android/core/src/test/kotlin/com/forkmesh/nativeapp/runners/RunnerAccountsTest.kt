package com.forkmesh.nativeapp.runners

import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class RunnerAccountsTest {
    private val work = "0123456789abcdef0123456789abcdef"
    private val home = "fedcba9876543210fedcba9876543210"
    private val requestId = "00112233445566778899aabbccddeeff"
    private val now = 1_770_000_000_000L

    private fun json(text: String): JsonMap = Json.parse(text).asJsonMap()

    private fun payload(pending: Boolean = false, activeHome: Boolean = false, requestState: String = if (pending) "pending" else "done"): String =
        """
        {"ok":true,"runners":[
          {"name":"fmnext-r1","scope":"pool","status":"active","lastSeenAt":5,"availability":{
            "claude-code":{"binaryFound":true,"loginState":"available","auth":{"state":"ok"},"accounts":[
              {"id":"$work","label":"Work","email":"w@example.com","plan":"Max","active":${!activeHome},
               "usage":{"fetchedAt":7,"windows":[
                 {"id":"five_hour","label":"5-hour session","usedPercent":41.6,"resetsAt":${now + 3_600_000},"windowMinutes":300},
                 {"id":"seven_day","label":"Weekly","usedPercent":140}]}},
              {"id":"$home","email":"h@example.com","active":$activeHome,"usage":{"error":"token_expired","windows":[]}},
              {"id":"not-hex","label":"Bad","active":false}]},
            "codex":{"binaryFound":false,"loginState":"missing"}},
           "logins":[],
           "accountRequests":[
             {"id":"$requestId","provider":"claude-code","action":"activate","account":"$home","state":"$requestState","message":"${if (requestState == "failed") "Login rejected." else ""}","createdAt":9},
             {"id":"short","provider":"claude-code","action":"activate","account":"$work","state":"pending"},
             {"id":"$work","provider":"claude-code","action":"activate","account":"$home","state":"failed","createdAt":3}]},
          {"name":"fmnext-r2","status":"active","availability":{
            "claude-code":{"binaryFound":false,"accounts":[]},
            "codex":{"binaryFound":true,"loginState":"missing"}}},
          {"name":"fmnext-r3","status":"revoked","availability":{
            "claude-code":{"binaryFound":true,"accounts":[{"id":"$work","label":"Old","active":true}]}}},
          {"name":"Bad Name","status":"active","availability":{"codex":{"binaryFound":true}}}
        ]}
        """

    fun testParsesAccountsUsageAndRequests() {
        val runners = RunnerAccounts.parse(json(payload(pending = true)))
        assertEq(listOf("fmnext-r1", "fmnext-r2", "fmnext-r3"), runners.map { it.name })
        val claude = runners[0].provider("claude-code")
        expect(claude.reported)
        expect(claude.binaryFound)
        assertEq(listOf(work, home), claude.accounts.map { it.id })
        val first = claude.accounts[0]
        assertEq("Work", first.displayName)
        assertEq("Max", first.plan)
        expect(first.active)
        expect(first.usageRead)
        assertEq(7L, first.usageFetchedAtMs)
        assertEq(2, first.usage.size)
        assertEq(100.0, first.usage[1].usedPercent)
        assertEq(300L, first.usage[0].windowMinutes)
        assertEq("h@example.com", claude.accounts[1].displayName)
        assertEq("token_expired", claude.accounts[1].usageError)
        assertEq(first, claude.activeAccount)
        assertEq("Account", RunnerAccount(work).displayName)
        assertEq(listOf(requestId, work), runners[0].accountRequests.map { it.id })
        assertEq(requestId, runners[0].accountRequest("claude-code", home)?.id)
        assertEq(9L, runners[0].accountRequest("claude-code", home)?.createdAtMs)
        assertEq(null, runners[0].accountRequest("codex", home))
        expect(runners[0].accountRequestPending("claude-code", home))
        expect(!runners[0].accountRequestPending("claude-code", work))
        expect(!runners[0].accountRequestPending("codex", home))
        expect(!runners[1].provider("claude-code").binaryFound)
    }

    fun testSectionsListActiveRunnersWithCliOrAccounts() {
        val runners = RunnerAccounts.parse(json(payload(pending = true)))
        val sections = RunnerAccounts.sections(runners)
        assertEq(listOf("fmnext-r1:claude-code", "fmnext-r2:codex"), sections.map { it.key })
        assertEq(setOf(home), sections[0].pendingAccounts)
        expect(sections[1].canSignIn)
        assertEq(listOf("fmnext-r2:codex"), RunnerAccounts.sections(runners, "codex").map { it.key })
        expect(RunnerAccounts.hasAccounts(runners))
        expect(RunnerAccounts.anyPending(runners))
        assertEq(2, RunnerAccounts.accountCount(runners))
        expect(!RunnerAccounts.hasAccounts(RunnerAccounts.parse(json("""{"runners":[{"name":"r","status":"active","availability":{"codex":{"binaryFound":true}}}]}"""))))
        assertEq("Add Claude account on fmnext-r1…", RunnerAccounts.addLabel(sections[0]))
        assertEq("Sign in Codex on fmnext-r2…", RunnerAccounts.addLabel(sections[1]))
    }

    fun testUsageTextMatchesDesktop() {
        val accounts = RunnerAccounts.parse(json(payload())).first().provider("claude-code").accounts
        assertEq("5-hour session 42% (resets in 1 h 0 min) · Weekly 100%", RunnerAccounts.usageText(accounts[0], now))
        assertEq("usage refreshes after its next run", RunnerAccounts.usageText(accounts[1], now))
        assertEq("usage not read yet", RunnerAccounts.usageText(RunnerAccount(work), now))
        assertEq("no usage limits reported", RunnerAccounts.usageText(RunnerAccount(work, usageRead = true), now))
        assertEq(
            "Weekly 3% · usage service unreachable",
            RunnerAccounts.usageText(RunnerAccount(work, usageRead = true, usageError = "unreachable", usage = listOf(RunnerUsageWindow("w", "Weekly", 3.4))), now),
        )
        assertEq("the provider refused this login; sign it in again", RunnerAccounts.usageErrorText("unauthorized"))
        assertEq("usage limits need a subscription sign-in", RunnerAccounts.usageErrorText("no_oauth_login"))
        assertEq("saved login missing; sign it in again", RunnerAccounts.usageErrorText("missing_login"))
        assertEq("usage unavailable (boom)", RunnerAccounts.usageErrorText("boom"))
        assertEq("1 min", RunnerAccounts.durationText(5_000))
        assertEq("59 min", RunnerAccounts.durationText(59 * 60_000L))
        assertEq("47 h 59 min", RunnerAccounts.durationText(47 * 3_600_000L + 59 * 60_000L))
        assertEq("3 days", RunnerAccounts.durationText(60 * 3_600_000L))
        assertEq("Work · Max · switching…", RunnerAccounts.accountTitle(accounts[0], pending = true))
        assertEq("h@example.com", RunnerAccounts.accountTitle(accounts[1], pending = false))
    }

    private class FakeTransport(val answers: MutableMap<String, ArrayDeque<Pair<Int, String>>>) : HttpTransport {
        val seen = ArrayList<HttpClient.Request>()
        override fun send(request: HttpClient.Request): HttpClient.Response {
            seen += request
            val key = request.method + " " + request.url.substringAfter("https://app.test")
            val queue = answers[key]
            val (status, body) = when {
                queue == null || queue.isEmpty() -> 404 to "{\"error\":\"not_found\"}"
                queue.size == 1 -> queue.first()
                else -> queue.removeFirst()
            }
            return HttpClient.Response(status, emptyMap(), body.toByteArray())
        }

        fun paths(): List<String> = seen.map { it.method + " " + it.url.substringAfter("https://app.test") }
    }

    private fun answers(vararg pairs: Pair<String, List<Pair<Int, String>>>): MutableMap<String, ArrayDeque<Pair<Int, String>>> =
        pairs.associateTo(LinkedHashMap()) { it.first to ArrayDeque(it.second) }

    fun testActivatePostsAndRereadsWhilePending() {
        val transport = FakeTransport(
            answers(
                "GET /api/runners" to listOf(200 to payload(), 200 to payload(pending = true), 200 to payload(pending = true), 200 to payload(activeHome = true)),
                "POST /api/runners/fmnext-r1/accounts" to listOf(202 to """{"ok":true,"request":{"id":"$requestId","state":"pending"}}"""),
            ),
        )
        val sleeps = ArrayList<Long>()
        val store = RunnerAccountsStore(RunnerAttentionApi({ "https://app.test" }, { "tok" }, transport), { now }) { sleeps += it }
        var published = 0
        store.addListener { published++ }
        expect(store.refresh())
        expect(store.state.hasAccounts)
        expect(!store.refresh())
        val target = store.state.runners[0].provider("claude-code").accounts[1]
        expect(store.activate("fmnext-r1", "claude-code", target))
        val post = transport.seen.first { it.method == "POST" }
        assertEq(mapOf("provider" to "claude-code", "account" to home, "action" to "activate"), Json.parse(String(post.body!!, Charsets.UTF_8)))
        assertEq("Bearer tok", post.headers["Authorization"])
        assertEq("application/json", post.headers["Content-Type"])
        assertEq("https://app.test", post.headers["Origin"])
        assertEq(
            listOf("GET /api/runners", "POST /api/runners/fmnext-r1/accounts", "GET /api/runners", "GET /api/runners", "GET /api/runners"),
            transport.paths(),
        )
        assertEq(listOf(2_000L, 2_000L), sleeps)
        expect(!store.state.anyPending)
        expect(store.state.runners[0].provider("claude-code").accounts[1].active)
        assertEq("fmnext-r1 now runs Claude Code as h@example.com.", store.state.notice)
        expect(!store.state.noticeError)
        expect(published > 4)
        expect(!store.activate("fmnext-r1", "claude-code", store.state.runners[0].provider("claude-code").accounts[1]))
    }

    fun testSwitchOutcomeFailureAndStillPending() {
        val transport = FakeTransport(
            answers(
                "GET /api/runners" to listOf(200 to payload(pending = true), 200 to payload(requestState = "failed")),
                "POST /api/runners/fmnext-r1/accounts" to listOf(202 to """{"ok":true}"""),
            ),
        )
        val store = RunnerAccountsStore(RunnerAttentionApi({ "https://app.test" }, { "tok" }, transport), { now }) { }
        store.activate("fmnext-r1", "claude-code", RunnerAccount(home, email = "h@example.com"))
        assertEq("Could not change the runner account: Login rejected.", store.state.notice)
        expect(store.state.noticeError)
        val stuck = FakeTransport(
            answers(
                "GET /api/runners" to listOf(200 to payload(pending = true)),
                "POST /api/runners/fmnext-r1/accounts" to listOf(202 to """{"ok":true}"""),
            ),
        )
        val waiting = RunnerAccountsStore(RunnerAttentionApi({ "https://app.test" }, { "tok" }, stuck), { now }) { }
        waiting.activate("fmnext-r1", "claude-code", RunnerAccount(home, email = "h@example.com"))
        assertEq("Switching fmnext-r1 to h@example.com; the runner applies it on its next check-in.", waiting.state.notice)
        expect(!waiting.state.noticeError)
    }

    fun testPendingRereadsStopAfterThirty() {
        val transport = FakeTransport(
            answers(
                "GET /api/runners" to listOf(200 to payload(pending = true)),
                "POST /api/runners/fmnext-r1/accounts" to listOf(202 to """{"ok":true}"""),
            ),
        )
        var sleeps = 0
        val store = RunnerAccountsStore(RunnerAttentionApi({ "https://app.test" }, { "tok" }, transport), { now }) { sleeps++ }
        store.activate("fmnext-r1", "claude-code", RunnerAccount(home))
        assertEq(RunnerAccountsStore.PENDING_MAX_READS, sleeps)
        assertEq(1 + 1 + RunnerAccountsStore.PENDING_MAX_READS, transport.seen.size)
    }

    fun testActivateFailureShowsMessage() {
        val transport = FakeTransport(
            answers(
                "GET /api/runners" to listOf(200 to payload()),
                "POST /api/runners/fmnext-r1/accounts" to listOf(404 to """{"error":"unknown_account"}"""),
            ),
        )
        val store = RunnerAccountsStore(RunnerAttentionApi({ "https://app.test" }, { "tok" }, transport), { now }) { }
        expect(!store.activate("fmnext-r1", "claude-code", RunnerAccount(home)))
        assertEq("Could not change the runner account: the runner no longer has that account", store.state.notice)
        expect(store.state.noticeError)
        assertEq("", store.state.error)
        transport.answers["POST /api/runners/fmnext-r1/accounts"] = ArrayDeque(listOf(429 to """{"error":"rate_limited","message":"Slow down."}"""))
        store.activate("fmnext-r1", "claude-code", RunnerAccount(home))
        assertEq("Could not change the runner account: Slow down.", store.state.notice)
        expect(!store.activate("Bad Name", "claude-code", RunnerAccount(home)))
        expect(!store.activate("fmnext-r1", "gemini", RunnerAccount(home)))
    }

    fun testUnauthorizedHidesAndErrorsKeepRunners() {
        val transport = FakeTransport(answers("GET /api/runners" to listOf(200 to payload())))
        var clock = now
        val store = RunnerAccountsStore(RunnerAttentionApi({ "https://app.test" }, { "tok" }, transport), { clock }) { }
        store.refresh()
        expect(store.state.hasAccounts)
        transport.answers["GET /api/runners"] = ArrayDeque(listOf(502 to "bad gateway"))
        store.refresh(force = true)
        expect(store.state.hasAccounts)
        assertEq("Could not load runners: HTTP 502", store.state.error)
        transport.answers["GET /api/runners"] = ArrayDeque(listOf(401 to """{"error":"invalid_session"}"""))
        clock += RunnerAccountsStore.REFRESH_FLOOR_MS
        expect(store.refresh())
        expect(store.state.hidden)
        expect(!store.state.hasAccounts)
        assertEq(0, store.state.sections.size)
        assertEq("", store.state.error)
        val signedOut = RunnerAccountsStore(RunnerAttentionApi({ "https://app.test" }, { "" }, transport))
        val before = transport.seen.size
        signedOut.refresh()
        expect(signedOut.state.hidden)
        assertEq(before, transport.seen.size)
    }
}
