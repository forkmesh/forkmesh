package com.forkmesh.nativeapp.runners

import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class RunnerAttentionTest {
    private val loginId = "0123456789abcdef0123456789abcdef"

    private fun json(text: String): JsonMap = Json.parse(text).asJsonMap()

    private val runnersPayload = json(
        """
        {"ok":true,"runners":[
          {"name":"fmnext-r1","status":"active","availability":{
            "claude-code":{"binaryFound":true,"loginState":"expired","auth":{"state":"expired","message":"OAuth session expired"}},
            "codex":{"binaryFound":true,"loginState":"available","auth":{"state":"expiring","expiresAt":1770000000000}}},
           "logins":[{"id":"$loginId","provider":"claude-code","state":"running"}]},
          {"name":"fmnext-r2","status":"active","availability":{
            "claude-code":{"binaryFound":true,"loginState":"available","auth":{"state":"expired"}},
            "codex":{"binaryFound":false,"loginState":"expired"}}},
          {"name":"fmnext-r3","status":"revoked","availability":{
            "claude-code":{"binaryFound":true,"loginState":"expired"}}},
          {"name":"fmnext-r4","status":"active","availability":{
            "claude-code":{"binaryFound":true,"loginState":"missing","auth":{"state":"missing"}},
            "codex":{"binaryFound":true,"loginState":"available","auth":{"state":"ok"}}}}
        ]}
        """,
    )

    fun testDerivesLikeTheWebBanner() {
        val items = RunnerAttention.deriveFromRunners(runnersPayload)
        assertEq(listOf("fmnext-r1:claude-code", "fmnext-r2:claude-code", "fmnext-r1:codex"), items.map { it.key })
        assertEq(RunnerAttentionState.Expired, items[0].state)
        assertEq(loginId, items[0].openLoginId)
        assertEq("OAuth session expired", items[0].message)
        assertEq(RunnerAttentionState.Expired, items[1].state)
        assertEq("", items[1].openLoginId)
        assertEq(RunnerAttentionState.Expiring, items[2].state)
        assertEq(1770000000000L, items[2].expiresAtMs)
    }

    fun testAPausedRunnerStillAsksForSignIn() {
        val row = """{"name":"fmnext-r5","status":"paused","availability":{"claude-code":{"binaryFound":true,"loginState":"expired"}}}"""
        assertEq(listOf("fmnext-r5:claude-code"), RunnerAttention.deriveRunner(json(row)).map { it.key })
    }

    fun testFlatAttentionItems() {
        val items = RunnerAttention.parseAttention(
            json(
                """{"attention":[
                  {"runner":"fmnext-r1","provider":"codex","state":"expiring","expiresAt":5},
                  {"runner":"fmnext-r1","provider":"claude-code","state":"needs_login","loginId":"$loginId"},
                  {"runner":"Bad Name","provider":"codex","state":"expired"},
                  {"runner":"fmnext-r2","provider":"gemini","state":"expired"},
                  {"runner":"fmnext-r2","provider":"codex","state":"ok"},
                  {"name":"fmnext-r2","provider":"codex","auth":{"state":"missing"},"login":{"id":"nothex"}}
                ]}""",
            ),
        )
        assertEq(listOf("fmnext-r1:claude-code", "fmnext-r2:codex", "fmnext-r1:codex"), items.map { it.key })
        assertEq(loginId, items[0].openLoginId)
        assertEq("", items[1].openLoginId)
        assertEq(RunnerAttentionState.Expiring, items[2].state)
        assertEq(5L, items[2].expiresAtMs)
    }

    fun testGroupedAndItemsKeys() {
        val grouped = RunnerAttention.parseAttention(
            json("""{"items":[{"runner":"fmnext-r1","providers":[{"provider":"claude-code","state":"expired"},{"provider":"codex","loginState":"available"}]}]}"""),
        )
        assertEq(listOf("fmnext-r1:claude-code"), grouped.map { it.key })
        val mapped = RunnerAttention.parseAttention(
            json("""{"runners":[{"name":"fmnext-r1","providers":{"codex":{"state":"expiring"},"claude-code":"login_expired"}}]}"""),
        )
        assertEq(listOf("fmnext-r1:claude-code", "fmnext-r1:codex"), mapped.map { it.key })
        val rows = RunnerAttention.parseAttention(runnersPayload)
        assertEq(3, rows.size)
        assertEq(emptyList<RunnerAttentionItem>(), RunnerAttention.parseAttention(json("""{"ok":true}""")))
    }

    fun testDedupeKeepsExpiredAndSorts() {
        val items = RunnerAttention.normalize(
            listOf(
                RunnerAttentionItem("zeta", "codex", RunnerAttentionState.Expiring, openLoginId = loginId),
                RunnerAttentionItem("alpha", "codex", RunnerAttentionState.Expiring),
                RunnerAttentionItem("zeta", "codex", RunnerAttentionState.Expired),
                RunnerAttentionItem("beta", "claude-code", RunnerAttentionState.Expired, message = "a\u0007b"),
            ),
        )
        assertEq(listOf("beta:claude-code", "zeta:codex", "alpha:codex"), items.map { it.key })
        assertEq(RunnerAttentionState.Expired, items[1].state)
        assertEq(loginId, items[1].openLoginId)
        assertEq("ab", items[0].message)
    }

    fun testWording() {
        val expired = RunnerAttentionItem("fmnext-r1", "claude-code", RunnerAttentionState.Expired)
        val expiring = RunnerAttentionItem("fmnext-r1", "codex", RunnerAttentionState.Expiring, openLoginId = loginId)
        assertEq("fmnext-r1 needs Claude sign-in", RunnerAttention.title(expired))
        assertEq("fmnext-r1: Codex sign-in expires soon", RunnerAttention.title(expiring))
        assertEq("Jobs for Claude Code are not running on this runner.", RunnerAttention.detail(expired))
        assertEq("Sign in again before it expires.", RunnerAttention.detail(expiring))
        assertEq("Sign in again before it expires (Oct 1, 10:00).", RunnerAttention.detail(expiring, "Oct 1, 10:00"))
        assertEq("Sign in", RunnerAttention.buttonLabel(expired))
        assertEq("Open sign-in", RunnerAttention.buttonLabel(expiring))
        assertEq("2 runner sign-ins needed", RunnerAttention.summaryTitle(listOf(expired, expiring)))
        assertEq("fmnext-r1 needs Claude sign-in", RunnerAttention.summaryTitle(listOf(expired)))
        expect(RunnerAttention.anyExpired(listOf(expiring, expired)))
        expect(!RunnerAttention.anyExpired(listOf(expiring)))
        assertEq("Connect Codex", RunnerAttention.connectLabel("codex"))
    }

    fun testPingTargets() {
        assertEq(
            RunnerSignInTarget("fmnext-r1", "claude-code"),
            RunnerAttention.pingTarget("/dashboard/runners?runner=fmnext-r1&provider=claude-code"),
        )
        assertEq(
            RunnerSignInTarget("fmnext-r1", "codex"),
            RunnerAttention.pingTarget("https://app.forkmesh.com/dashboard/runners?provider=codex&runner=FMNEXT-R1"),
        )
        assertEq(null, RunnerAttention.pingTarget("/dashboard/runners"))
        assertEq(null, RunnerAttention.pingTarget("/dashboard/runnersx?runner=fmnext-r1&provider=codex"))
        assertEq(null, RunnerAttention.pingTarget("javascript:x/dashboard/runners?runner=fmnext-r1&provider=codex"))
        assertEq(null, RunnerAttention.pingTarget("/dashboard/runners?runner=fmnext-r1&provider=gemini"))
        expect(RunnerAttention.isRunnerPing("operational_alert", "/dashboard/runners?runner=r"))
        expect(!RunnerAttention.isRunnerPing("mention", "/dashboard/runners"))
        assertEq(
            "https://app.forkmesh.com/dashboard/runners?runner=fmnext-r1&provider=codex&connect=1",
            RunnerAttention.dashboardUrl("https://app.forkmesh.com/", "fmnext-r1", "codex"),
        )
    }

    fun testServerAttentionPayload() {
        val payload = json(
            """{"ok":true,"items":[
              {"runner":"fmnext-r1","provider":"claude-code","providerLabel":"Claude Code","state":"expired","scope":"pool",
               "canConnect":true,"online":true,"lastSeenAt":1,"expiresAt":0,"title":"fmnext-r1 needs Claude sign-in",
               "connectUrl":"/dashboard/runners?runner=fmnext-r1&provider=claude-code&connect=1","loginId":"$loginId"},
              {"runner":"fmnext-r9","provider":"codex","providerLabel":"Codex","state":"expiring","scope":"personal",
               "canConnect":false,"online":false,"lastSeenAt":1,"expiresAt":1770000000000,"title":"fmnext-r9: Codex sign-in expires soon",
               "connectUrl":"https://evil.example/dashboard/runners","loginId":""}
            ],"count":2,"expired":1,"runners":2,"checkedAt":5,"pollMs":120000}""",
        )
        val items = RunnerAttention.parseAttention(payload)
        assertEq(listOf("fmnext-r1:claude-code", "fmnext-r9:codex"), items.map { it.key })
        for ((item, row) in items.zip(payload.maps("items"))) assertEq(row.str("title"), RunnerAttention.title(item))
        assertEq(loginId, items[0].openLoginId)
        expect(items[0].canConnect)
        expect(!items[1].canConnect)
        assertEq(1770000000000L, items[1].expiresAtMs)
        assertEq(
            "https://app.forkmesh.com/dashboard/runners?runner=fmnext-r1&provider=claude-code&connect=1",
            RunnerAttention.connectUrl("https://app.forkmesh.com", items[0]),
        )
        assertEq("", items[1].connectUrl)
        assertEq(
            "https://app.forkmesh.com/dashboard/runners?runner=fmnext-r9&provider=codex&connect=1",
            RunnerAttention.connectUrl("https://app.forkmesh.com", items[1]),
        )
        assertEq(120000L, RunnerAttention.pollMs(payload))
        assertEq(RunnerAttention.DEFAULT_POLL_MS, RunnerAttention.pollMs(json("{}")))
        assertEq(RunnerAttention.MIN_POLL_MS, RunnerAttention.pollMs(json("""{"pollMs":10}""")))
        expect(!RunnerAttention.validConnectPath("/dashboard/runnersx"))
        expect(!RunnerAttention.validConnectPath("//evil.example/dashboard/runners"))
    }

    fun testStoreUsesServerPollFloor() {
        val transport = FakeTransport(mutableMapOf("/api/runners/attention" to (200 to """{"ok":true,"items":[],"runners":0,"pollMs":60000}""")))
        var now = 1_000L
        val store = RunnerAttentionStore(RunnerAttentionApi({ "https://app.test" }, { "tok" }, transport)) { now }
        expect(store.refresh())
        assertEq(60_000L, store.pollMs)
        now += 59_000
        expect(!store.refresh())
        now += 2_000
        expect(store.refresh())
        now += 1_000
        expect(!store.refresh(force = true))
        now += RunnerAttentionStore.FORCED_FLOOR_MS
        expect(store.refresh(force = true))
    }

    private class FakeTransport(val answers: MutableMap<String, Pair<Int, String>>) : HttpTransport {
        val seen = ArrayList<HttpClient.Request>()
        override fun send(request: HttpClient.Request): HttpClient.Response {
            seen += request
            val path = request.url.substringAfter("https://app.test")
            val (status, body) = answers[path] ?: (404 to "{\"error\":\"not_found\"}")
            return HttpClient.Response(status, emptyMap(), body.toByteArray())
        }
    }

    fun testStoreFallsBackWhenEndpointMissing() {
        val transport = FakeTransport(mutableMapOf("/api/runners" to (200 to Json.stringify(runnersPayload))))
        var now = 1_000L
        val store = RunnerAttentionStore(RunnerAttentionApi({ "https://app.test" }, { "tok" }, transport)) { now }
        var published = 0
        store.addListener { published++ }
        expect(store.refresh())
        assertEq(RunnerAttentionStore.Source.Runners, store.source)
        assertEq(3, store.items.size)
        assertEq(listOf("/api/runners/attention", "/api/runners"), transport.seen.map { it.url.substringAfter("https://app.test") })
        assertEq("Bearer tok", transport.seen[0].headers["Authorization"])
        now += 30_000
        expect(!store.refresh())
        expect(store.refresh(force = true))
        assertEq("/api/runners", transport.seen.last().url.substringAfter("https://app.test"))
        assertEq(3, transport.seen.size)
        now += RunnerAttentionStore.ENDPOINT_RETRY_MS
        transport.answers["/api/runners/attention"] = 200 to """{"attention":[]}"""
        expect(store.refresh(force = true))
        assertEq(RunnerAttentionStore.Source.Attention, store.source)
        assertEq(0, store.items.size)
        assertEq(3, published)
    }

    fun testStoreUnauthorizedIsEmptyAndErrorsKeepItems() {
        val transport = FakeTransport(mutableMapOf("/api/runners/attention" to (200 to """{"attention":[{"runner":"fmnext-r1","provider":"codex","state":"expired"}]}""")))
        var now = 1_000L
        val store = RunnerAttentionStore(RunnerAttentionApi({ "https://app.test" }, { "tok" }, transport)) { now }
        store.refresh()
        assertEq(1, store.items.size)
        transport.answers["/api/runners/attention"] = 502 to "bad gateway"
        now += RunnerAttentionStore.FORCED_FLOOR_MS
        store.refresh(force = true)
        assertEq(1, store.items.size)
        transport.answers["/api/runners/attention"] = 401 to """{"error":"invalid_session"}"""
        now += RunnerAttentionStore.FORCED_FLOOR_MS
        store.refresh(force = true)
        assertEq(0, store.items.size)
        val signedOut = RunnerAttentionStore(RunnerAttentionApi({ "https://app.test" }, { "" }, transport))
        expect(signedOut.refresh())
        assertEq(0, signedOut.items.size)
    }
}
