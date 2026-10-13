package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.ByteArrayInputStream
import java.io.IOException
import java.io.InputStream

class AgentLiveStreamTest {
    private class FakeConnection(
        private val status: Int,
        private val body: String,
        private val type: String = "text/event-stream; charset=utf-8",
    ) : AgentLiveConnection {
        var closed = false

        override fun status(): Int = status

        override fun contentType(): String = type

        override fun body(): InputStream = ByteArrayInputStream(body.toByteArray(Charsets.UTF_8))

        override fun close() {
            closed = true
        }
    }

    private class Script(vararg connections: AgentLiveConnection) : AgentLiveConnector {
        private val queue = ArrayDeque(connections.toList())
        val requests = ArrayList<HttpClient.Request>()

        override fun connect(request: HttpClient.Request): AgentLiveConnection {
            requests.add(request)
            return queue.removeFirstOrNull() ?: throw IOException("no more connections")
        }
    }

    private fun api(token: String = "tok") =
        AgentQueueApi({ "https://app.example.com/" }, { token }, HttpTransport { throw IOException("unused") })

    fun testFullTextFrameReplaces() {
        val parser = AgentLiveParser()
        val frames = parser.feed("retry: 1000\n\nevent: live\ndata: {\"at\":1700000000000,\"text\":\"hello\\nworld\"}\n\n")
        assertEq(listOf<AgentLiveFrame>(AgentLiveFrame.Text(1_700_000_000_000, "hello\nworld")), frames)
        val live = AgentLiveText()
        live.apply(AgentLiveFrame.Text(1, "old"))
        expect(live.apply(frames.single()))
        assertEq("hello\nworld", live.text)
        assertEq(1_700_000_000_000, live.atMs)
    }

    fun testAppendFrameExtendsText() {
        val parser = AgentLiveParser()
        val frames = parser.feed(
            "event: live\ndata: {\"at\":1700000000000,\"text\":\"ab\"}\n\n" +
                "event: live\ndata: {\"at\":1700000000500,\"append\":\"cd\"}\n\n",
        )
        assertEq(2, frames.size)
        assertEq(AgentLiveFrame.Append(1_700_000_000_500, "cd"), frames[1])
        val live = AgentLiveText()
        frames.forEach { live.apply(it) }
        assertEq("abcd", live.text)
        assertEq(1_700_000_000_500, live.atMs)
    }

    fun testAppendBeforeTextIsIgnored() {
        val live = AgentLiveText()
        expect(!live.apply(AgentLiveFrame.Append(5, "lost")))
        expect(!live.hasText)
        assertEq("", live.text)
    }

    fun testKeepaliveCommentsProduceNothing() {
        val parser = AgentLiveParser()
        assertEq(emptyList<AgentLiveFrame>(), parser.feed(": keepalive\n\n:\n\n: ping 2\r\n\r\n"))
        val frames = parser.feed("event: live\r\ndata: {\"at\":1700000000000,\"text\":\"x\"}\r\n\r\n")
        assertEq(listOf<AgentLiveFrame>(AgentLiveFrame.Text(1_700_000_000_000, "x")), frames)
    }

    fun testEndFrameCarriesStatus() {
        val frames = AgentLiveParser().feed("event: end\ndata: {\"status\":\"completed\"}\n\n")
        assertEq(listOf<AgentLiveFrame>(AgentLiveFrame.End("completed")), frames)
    }

    fun testUnknownEventsAndBadDataAreIgnored() {
        val parser = AgentLiveParser()
        val frames = parser.feed(
            "event: status\ndata: {\"text\":\"no\"}\n\n" +
                "data: {\"text\":\"unnamed\"}\n\n" +
                "event: live\ndata: not json\n\n" +
                "event: live\ndata: {\"at\":1}\n\n" +
                "id: 4\nevent: live\ndata: {\"at\":1700000000000,\"text\":\"yes\"}\n\n",
        )
        assertEq(listOf<AgentLiveFrame>(AgentLiveFrame.Text(1_700_000_000_000, "yes")), frames)
    }

    fun testFrameSplitAcrossReads() {
        val parser = AgentLiveParser()
        val wire = "event: live\ndata: {\"at\":1700000000000,\"text\":\"caf\u00e9 \u2713 \uD83D\uDE80\"}\n\n".toByteArray(Charsets.UTF_8)
        val frames = ArrayList<AgentLiveFrame>()
        for (byte in wire) frames += parser.feed(byteArrayOf(byte))
        assertEq(listOf<AgentLiveFrame>(AgentLiveFrame.Text(1_700_000_000_000, "caf\u00e9 \u2713 \uD83D\uDE80")), frames)
        val split = AgentLiveParser()
        assertEq(emptyList<AgentLiveFrame>(), split.feed("event: li"))
        assertEq(emptyList<AgentLiveFrame>(), split.feed("ve\r"))
        assertEq(emptyList<AgentLiveFrame>(), split.feed("\ndata: {\"at\":1700000000000,"))
        assertEq(emptyList<AgentLiveFrame>(), split.feed("\"append\":\"z\"}\r"))
        assertEq(listOf<AgentLiveFrame>(AgentLiveFrame.Append(1_700_000_000_000, "z")), split.feed("\n\r\n"))
    }

    fun testMultilineDataJoinsWithNewline() {
        val frames = AgentLiveParser().feed("event: live\ndata: {\"at\":1700000000000,\ndata:\"text\":\"m\"}\n\n")
        assertEq(listOf<AgentLiveFrame>(AgentLiveFrame.Text(1_700_000_000_000, "m")), frames)
    }

    fun testLiveRequestUsesSessionPathAndBearer() {
        val owner = api().liveRequest("alice", "app", "s 1")
        assertEq("GET", owner.method)
        assertEq("https://app.example.com/api/repo/alice/app/agent-sessions/s%201/live", owner.url)
        assertEq("text/event-stream", owner.headers["Accept"])
        assertEq("Bearer tok", owner.headers["Authorization"])
        assertEq(AgentLiveStream.READ_TIMEOUT_MS, owner.readTimeoutMs)
        val org = api().liveRequest("acme", "app", "s1", orgScope = true)
        assertEq("https://app.example.com/api/orgs/acme/repos/app/agent-bots/s1/live", org.url)
    }

    fun testStreamReconnectsAfterCleanCloseAndStopsAtEnd() {
        val first = FakeConnection(
            200,
            ": hi\n\nevent: live\ndata: {\"at\":1700000000000,\"text\":\"a\"}\n\nevent: live\ndata: {\"at\":1700000000001,\"append\":\"b\"}\n\n",
        )
        val second = FakeConnection(200, "event: live\ndata: {\"at\":1700000000002,\"text\":\"abc\"}\n\nevent: end\ndata: {\"status\":\"completed\"}\n\n")
        val script = Script(first, second)
        val events = ArrayList<AgentLiveEvent>()
        val stream = api().live("alice", "app", "s1", connector = script) { events += it }
        stream.follow()
        assertEq(
            listOf(
                AgentLiveEvent.Text("a", 1_700_000_000_000),
                AgentLiveEvent.Text("ab", 1_700_000_000_001),
                AgentLiveEvent.Text("abc", 1_700_000_000_002),
                AgentLiveEvent.Ended("completed"),
            ),
            events,
        )
        assertEq(2, script.requests.size)
        expect(first.closed && second.closed)
        expect(!stream.isLive)
    }

    fun testStreamGivesUpOnRefusalAndWrongType() {
        val refused = ArrayList<AgentLiveEvent>()
        api().live("a", "b", "s", connector = Script(FakeConnection(404, ""))) { refused += it }.follow()
        assertEq(listOf<AgentLiveEvent>(AgentLiveEvent.Refused(404)), refused)
        val json = ArrayList<AgentLiveEvent>()
        api().live("a", "b", "s", connector = Script(FakeConnection(200, "{}", "application/json"))) { json += it }.follow()
        assertEq(listOf<AgentLiveEvent>(AgentLiveEvent.Refused(200)), json)
    }

    fun testStreamRetriesServerErrorsUntilStopped() {
        val events = ArrayList<AgentLiveEvent>()
        lateinit var stream: AgentLiveStream
        val script = Script(FakeConnection(503, ""))
        stream = api().live("a", "b", "s", connector = script) { event ->
            events += event
            if (event is AgentLiveEvent.Dropped) stream.stop()
        }
        stream.follow()
        assertEq(listOf<AgentLiveEvent>(AgentLiveEvent.Dropped("HTTP 503")), events)
        assertEq(1, script.requests.size)
    }

    fun testSessionTakesNewerLiveText() {
        val running = AgentLaunchSession(sessionId = "s", status = "running", liveText = "old", liveAtMs = 100)
        assertEq("new", running.withLive("new", 200).liveText)
        assertEq("old", running.withLive("stale", 50).liveText)
        val done = running.copy(status = "completed")
        assertEq("old", done.withLive("new", 200).liveText)
    }
}
