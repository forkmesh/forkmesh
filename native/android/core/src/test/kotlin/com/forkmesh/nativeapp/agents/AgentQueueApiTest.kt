package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class AgentQueueApiTest {
    private class Recorder(private val answer: (HttpClient.Request) -> Pair<Int, String>) : HttpTransport {
        val requests = ArrayList<HttpClient.Request>()

        override fun send(request: HttpClient.Request): HttpClient.Response {
            requests.add(request)
            val (status, body) = answer(request)
            return HttpClient.Response(status, emptyMap(), body.toByteArray())
        }
    }

    private fun api(transport: HttpTransport, token: String = "tok") =
        AgentQueueApi({ "https://app.example.com" }, { token }, transport)

    fun testLaunchSendsOriginBearerAndBody() {
        val recorder = Recorder { 200 to """{"queueState":"queued","targetOnline":true,"session":{"id":"s9"}}""" }
        val photo = PromptAttachment("p1", "photo.jpg", byteArrayOf(1, 2, 3))
        val result = api(recorder).launch("alice", "app", "", model = "opus", targetNode = "m1", title = "T", attachments = listOf(photo))
        val request = recorder.requests.single()
        assertEq("POST", request.method)
        assertEq("https://app.example.com/api/repo/alice/app/agent-sessions", request.url)
        assertEq("https://app.example.com", request.headers["Origin"])
        assertEq("Bearer tok", request.headers["Authorization"])
        val body = Json.parse(String(request.body!!)).asJsonMap()
        assertEq("See the attached photos.", body["prompt"])
        assertEq("auto", body["provider"])
        assertEq("opus", body["model"])
        assertEq(false, body["createPr"])
        assertEq("AQID", ((body["attachments"] as List<*>).single() as Map<*, *>)["data"])
        assertEq("s9", result.session?.sessionId)
    }

    fun testLaunchRefusesWithoutSessionOrPrompt() {
        val recorder = Recorder { 200 to "{}" }
        assertEq(401, assertFails<AgentLaunchException> { api(recorder, "").launch("a", "b", "go") }.status)
        assertEq("Write what the agent should do.", assertFails<AgentLaunchException> { api(recorder).launch("a", "b", "  ") }.message)
        val tooMany = List(6) { PromptAttachment("i$it", "x", ByteArray(1)) }
        expect(assertFails<AgentLaunchException> { api(recorder).launch("a", "b", "x", attachments = tooMany) }.message.contains("5 photos"))
        expect(recorder.requests.isEmpty())
    }

    fun testOptionsFallBackToOrgRoute() {
        val recorder = Recorder { request ->
            if (request.url.contains("/agent-sessions")) 403 to """{"error":"forbidden"}"""
            else 200 to """{"engineeringAccess":true,"sessions":[{"id":"o"}]}"""
        }
        val options = api(recorder).launchOptions("mirror35", "app")
        expect(options.orgScope)
        expect(options.canQueue)
        assertEq("https://app.example.com/api/orgs/mirror35/repos/app/agent-bots", recorder.requests[1].url)
        expect(recorder.requests[0].headers["Origin"] == null)
    }

    fun testListsCarryFinishedBefore() {
        val recorder = Recorder { request ->
            if (request.url.endsWith("/api/account/agent-sessions")) {
                200 to """{"sessions":[{"id":"a","status":"completed","updatedAt":1700000000500}],"finishedBefore":1700000000500}"""
            } else {
                200 to """{"sessions":[{"id":"b","status":"failed","updatedAt":1700000000900}],"finishedBefore":1700000000900}"""
            }
        }
        val queue = api(recorder)
        assertEq(1700000000500L, queue.accountSessions().finishedBefore)
        assertEq(1700000000900L, queue.launchOptions("o", "r").finishedBefore)
        assertEq(0L, AgentLaunchOptions.fromJson(mapOf("sessions" to emptyList<Any?>())).finishedBefore)
        assertEq(0L, api(recorder, "").accountSessions().finishedBefore)
    }

    fun testRefusalCarriesRelayMessage() {
        val recorder = Recorder { 403 to """{"error":"forbidden"}""" }
        val error = assertFails<AgentLaunchException> { api(recorder).steer("a", "b", "s", "next") }
        assertEq("Only the owner of this repository can start agents on its nodes.", error.message)
        assertEq("forbidden", error.code)
        val custom = Recorder { 409 to """{"error":"x","message":"Busy right now."}""" }
        assertEq("Busy right now.", assertFails<AgentLaunchException> { api(custom).steer("a", "b", "s", "n") }.message)
    }

    fun testRetryResumesOrRequeues() {
        val recorder = Recorder { 200 to """{"session":{"id":"new"}}""" }
        val claimed = AgentLaunchSession("s/1", status = "failed", localAgentId = 3, history = listOf(AgentLaunchMessage("user", text = "go")))
        val resumed = api(recorder).retry("o", "r", claimed)
        expect(resumed.resumed)
        assertEq("https://app.example.com/api/repo/o/r/agent-sessions/s%2F1", recorder.requests.last().url)
        val unclaimed = claimed.copy(localAgentId = 0, provider = "codex", requestedModel = "sol", targetNode = "m", createPr = true)
        val requeued = api(recorder).retry("o", "r", unclaimed)
        expect(!requeued.resumed)
        val body = Json.parse(String(recorder.requests.last().body!!)).asJsonMap()
        assertEq("codex", body["provider"])
        assertEq("sol", body["model"])
        assertEq("m", body["targetNode"])
    }

    fun testStopCancelAccountAndSession() {
        val recorder = Recorder { request ->
            when {
                request.url.endsWith("/api/account/agent-sessions") -> 200 to """{"sessions":[{"id":"a","org":"o","repo":"r"}]}"""
                request.method == "GET" -> 200 to """{"session":{"id":"z","history":[{"role":"user","text":"hi"}]}}"""
                request.method == "DELETE" -> 200 to "{}"
                else -> 200 to """{"stopped":false,"message":"Asked mirror35 to stop."}"""
            }
        }
        val queue = api(recorder)
        assertEq("Asked mirror35 to stop.", queue.stop("o", "r", "z").label)
        assertEq("stop", Json.parse(String(recorder.requests.last().body!!)).asJsonMap()["action"])
        queue.cancel("o", "r", "z", orgScope = true)
        assertEq("DELETE", recorder.requests.last().method)
        val account = queue.accountSessions()
        assertEq("r", account.sessions.single().repo)
        assertEq(0L, account.finishedBefore)
        assertEq("hi", queue.session("o", "r", "z").firstPrompt)
    }

    fun testAttachmentFetchChecksImage() {
        val png = byteArrayOf(0x89.toByte(), 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0)
        val transport = HttpTransport { request ->
            if (request.url.endsWith("/attachments/0123456789abcdef0123456789abcdef")) HttpClient.Response(200, emptyMap(), png)
            else HttpClient.Response(410, emptyMap(), ByteArray(0))
        }
        val queue = api(transport)
        assertEq(10, queue.attachment("o", "r", "s", "0123456789abcdef0123456789abcdef").size)
        assertFails<AgentAttachmentError> { queue.attachment("o", "r", "s", "../etc") }
        val gone = assertFails<AgentAttachmentError> { queue.attachment("o", "r", "t", "ffffffffffffffffffffffffffffffff") }
        expect(gone.message.contains("a week"))
    }

    fun testNodeModelPreference() {
        val prefs = MemoryPrefStore()
        val preference = NodeModelPreference(prefs)
        assertEq("opus", preference.read().alias)
        preference.write(AgentModelCatalog.NODE_MODELS.first { it.alias == "haiku" })
        assertEq("haiku", preference.read().alias)
        prefs.putString(NodeModelPreference.KEY, "gone")
        assertEq("opus", preference.read().alias)
    }
}
