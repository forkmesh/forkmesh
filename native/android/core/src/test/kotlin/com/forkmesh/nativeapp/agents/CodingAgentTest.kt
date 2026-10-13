package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.localrepos.GitAuthor
import com.forkmesh.nativeapp.localrepos.GitWorkspace
import com.forkmesh.nativeapp.localrepos.withScratch
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class CodingAgentTest {
    private class ScriptedProvider(private val turns: MutableList<AgentTurn>) : AgentProvider {
        val histories = ArrayList<List<Any>>()
        override val id: String = "fake"
        override val model: String = "m"

        override fun send(systemPrompt: String, history: List<Any>, tools: List<AgentTool>, transport: HttpTransport): AgentTurn {
            histories.add(ArrayList(history))
            return turns.removeAt(0)
        }

        override fun appendToolResults(history: MutableList<Any>, results: List<AgentToolResult>) {
            history.add(mapOf("role" to "tool", "results" to results.map { it.output }))
        }

        override fun appendUser(history: MutableList<Any>, text: String, images: List<PromptAttachment>) {
            history.add(mapOf("role" to "user", "content" to text, "images" to images.size))
        }
    }

    private fun workspace(dir: File): GitWorkspace {
        val workspace = GitWorkspace(File(dir, "repo"))
        workspace.init(mapOf("README.md" to "hello\nworld\n"), "init", GitAuthor("t", "t@example.com"))
        return workspace
    }

    fun testLoopRunsToolsThenCompletes() = withScratch("agent-loop") { dir ->
        val ws = workspace(dir)
        val provider = ScriptedProvider(
            mutableListOf(
                AgentTurn("Reading.", listOf(AgentToolCall("c1", "read_file", mapOf("path" to "README.md"))), "a1", 10, 2),
                AgentTurn("", listOf(AgentToolCall("c2", "replace_text", mapOf("path" to "README.md", "old" to "world", "new" to "there"))), "a2"),
                AgentTurn("Done.", emptyList(), "a3", 5, 1),
            ),
        )
        val agent = CodingAgent(provider, AgentToolset(ws), transport = { error("no network") }, retryDelayMs = 0)
        val events = ArrayList<AgentEvent>()
        agent.run("change world") { events.add(it) }
        val done = events.last() as AgentDone
        assertEq(AgentStopReason.Completed, done.reason)
        assertEq(3, done.turns)
        assertEq(15, done.inputTokens)
        val read = events.filterIsInstance<AgentToolResultEvent>().first().result
        assertEq("1  hello\n2  world\n", read.output)
        assertEq("hello\nthere\n", ws.readFile("README.md").text)
        assertEq(AgentAssistantMessage("Done."), events[events.size - 2])
    }

    fun testSteeringContinuesAndRefusalStops() = withScratch("agent-steer") { dir ->
        val ws = workspace(dir)
        val provider = ScriptedProvider(
            mutableListOf(AgentTurn("First.", emptyList(), "a"), AgentTurn("", emptyList(), "b", refusal = "No.")),
        )
        val agent = CodingAgent(provider, AgentToolset(ws), transport = { error("x") }, retryDelayMs = 0)
        agent.steer("also this")
        val events = ArrayList<AgentEvent>()
        agent.run("go") { events.add(it) }
        expect(events.contains(AgentUserMessage("also this")))
        assertEq(AgentNotice("No.", isError = true), events[events.size - 2])
        assertEq(AgentStopReason.Refused, (events.last() as AgentDone).reason)
    }

    fun testTransientErrorRetriesOnceThenFails() = withScratch("agent-retry") { dir ->
        val ws = workspace(dir)
        var calls = 0
        val provider = object : AgentProvider {
            override val id = "x"
            override val model = "m"
            override fun send(systemPrompt: String, history: List<Any>, tools: List<AgentTool>, transport: HttpTransport): AgentTurn {
                calls++
                throw AgentProviderException("overloaded", 529)
            }
            override fun appendToolResults(history: MutableList<Any>, results: List<AgentToolResult>) {}
            override fun appendUser(history: MutableList<Any>, text: String, images: List<PromptAttachment>) {}
        }
        val events = ArrayList<AgentEvent>()
        CodingAgent(provider, AgentToolset(ws), transport = { error("x") }, retryDelayMs = 0).run("go") { events.add(it) }
        assertEq(2, calls)
        assertEq(AgentStopReason.Error, (events.last() as AgentDone).reason)
    }

    fun testToolsetGuardsAndGrep() = withScratch("agent-tools") { dir ->
        val ws = workspace(dir)
        val tools = AgentToolset(ws)
        assertEq("Wrote src/a.kt (2 lines).", tools.invoke("1", "write_file", mapOf("path" to "src/a.kt", "content" to "val x = 1\nval y = 2\n")).output)
        val grep = tools.invoke("2", "grep", mapOf("pattern" to "val y")).output
        assertEq("src/a.kt:2: val y = 2", grep)
        val twice = tools.invoke("3", "replace_text", mapOf("path" to "src/a.kt", "old" to "val", "new" to "var")).output
        expect(twice.contains("appears 2 times"))
        expect(tools.invoke("4", "nope", emptyMap()).isError)
        expect(tools.invoke("5", "read_file", emptyMap()).output.contains("\"path\" is required"))
        val status = tools.invoke("6", "git_status", emptyMap()).output
        expect(status.contains("untracked src/a.kt"))
        tools.author = GitAuthor("me", "me@example.com")
        expect(tools.invoke("7", "git_commit", mapOf("message" to "Add a")).output.startsWith("Committed "))
        assertEq("Nothing to commit; the worktree is clean.", tools.invoke("8", "git_commit", mapOf("message" to "x")).output)
        val readOnly = AgentToolset(ws, readOnly = true).invoke("9", "write_file", mapOf("path" to "b", "content" to ""))
        expect(readOnly.isError && readOnly.output.contains("read-only"))
        expect(AgentToolset.truncate("x".repeat(30000)).endsWith("output truncated at 24000 characters."))
    }

    fun testSessionServiceBranchesAndRestores() = withScratch("agent-session") { dir ->
        val ws = workspace(dir)
        val prefs = MemoryPrefStore()
        val service = AgentSessionService(prefs, launcher = { it.run() })
        val task = service.submit("repo", "Fix the login redirect please", id = "task-123456789")!!
        val provider = ScriptedProvider(
            mutableListOf(
                AgentTurn("", listOf(AgentToolCall("c", "write_file", mapOf("path" to "x.txt", "content" to "x"))), "a"),
                AgentTurn("", listOf(AgentToolCall("d", "git_commit", mapOf("message" to "x"))), "b"),
                AgentTurn("ok", emptyList(), "c"),
            ),
        )
        val handle = service.start(task, ws, { CodingAgent(provider, it, transport = { error("x") }) }, GitAuthor("a", "a@b.c"))
        expect(handle.isComplete)
        assertEq("agent/456789-fix-login-redirect", handle.branch)
        assertEq("main", ws.branches().single { it.isCurrent }.name)
        expect(ws.branches().any { it.name == "agent/456789-fix-login-redirect" })
        expect(handle.events.last() is AgentNotice)
        assertEq(AgentTaskState.Done, service.queue.single().state)
        val records = AgentSessionService(prefs).apply { load() }.records
        expect(records.single().isFinished)
    }

    fun testSessionRefusesDirtyTreeAndSweepsStale() = withScratch("agent-dirty") { dir ->
        val ws = workspace(dir)
        ws.writeFile("dirty.txt", "x")
        var now = 1_000_000L
        val prefs = MemoryPrefStore()
        prefs.putString(
            AgentSessionService.RECORDS_KEY,
            Json.stringify(listOf(AgentBranchRecord("old", "repo", "agent/old", "main", 0).toJson())),
        )
        val service = AgentSessionService(prefs, clock = { now }, staleAfterMs = 10, launcher = { it.run() })
        service.load()
        val task = service.submit("repo", "go")!!
        val error = assertFails<AgentStartException> { service.start(task, ws, { error("no") }, GitAuthor("a", "a@b")) }
        assertEq(AgentStartRefusal.DirtyWorktree, error.refusal)
        expect(error.message.contains("1 uncommitted change."))
        expect(service.records.isEmpty())
        now += 1
        assertEq("agent/abc123-task", AgentSessionService.agentBranchName("xabc123", "please make it"))
    }

    fun testAnthropicAndOpenAiWireShapes() {
        val tool = AgentTool("read_file", "d", mapOf("type" to "object")) { "" }
        val anthropic = AnthropicProvider("sk-ant-key", "claude-opus-5")
        val body = anthropic.requestBody("sys", listOf(mapOf("role" to "user", "content" to "hi")), listOf(tool))
        assertEq("default", body["fallbacks"])
        assertEq("read_file", ((body["tools"] as List<*>).single() as Map<*, *>)["name"])
        val turn = anthropic.parse(
            Json.parse(
                """{"content":[{"type":"text","text":"Hi"},{"type":"tool_use","id":"t1","name":"read_file","input":{"path":"a"}}],
                "usage":{"input_tokens":4,"output_tokens":2},"stop_reason":"refusal","stop_details":{"category":"cyber"}}""",
            ).asJsonMap(),
        )
        assertEq("Hi", turn.text)
        assertEq("a", turn.toolCalls.single().arguments["path"])
        assertEq("The model declined this request (cyber).", turn.refusal)
        val history = ArrayList<Any>()
        anthropic.appendUser(history, "look", listOf(PromptAttachment("i", "p.jpg", byteArrayOf(9))))
        val content = (history.single() as Map<*, *>)["content"] as List<*>
        assertEq("image", (content[0] as Map<*, *>)["type"])
        anthropic.appendToolResults(history, listOf(AgentToolResult("t1", "read_file", "x", true), AgentToolResult("t2", "grep", "y")))
        assertEq(2, ((history.last() as Map<*, *>)["content"] as List<*>).size)

        val openai = OpenAiProvider("sk-key-0000000000000000")
        val parsed = openai.parse(
            Json.parse(
                """{"choices":[{"message":{"role":"assistant","content":null,"tool_calls":[{"id":"c","function":{"name":"grep","arguments":"{\"pattern\":\"x\"}"}}]}}],"usage":{"prompt_tokens":3}}""",
            ).asJsonMap(),
        )
        assertEq("x", parsed.toolCalls.single().arguments["pattern"])
        assertEq(3, parsed.inputTokens)
        val oh = ArrayList<Any>()
        openai.appendToolResults(oh, listOf(AgentToolResult("a", "n", "1"), AgentToolResult("b", "n", "2")))
        assertEq(2, oh.size)
        assertEq("system", (((openai.requestBody("s", emptyList(), emptyList())["messages"]) as List<*>)[0] as Map<*, *>)["role"])
    }

    fun testProviderHttpErrorsAndCredentials() {
        val transport = HttpTransport { HttpClient.Response(401, emptyMap(), """{"error":{"message":"invalid x-api-key"}}""".toByteArray()) }
        val error = assertFails<AgentProviderException> { AnthropicProvider("k").send("s", emptyList(), emptyList(), transport) }
        assertEq("Claude API error 401: invalid x-api-key", error.message)
        expect(!error.isRetryable)
        val secrets = MemorySecretStore()
        val store = AgentCredentialStore(secrets)
        assertEq(null, store.read())
        store.write(AgentCredentials(AgentProviderKind.OpenAi, " sk-abcdefghijklmnopqrstuv ", "gpt-5.3"))
        val stored = Json.parse(secrets.read(AgentCredentialStore.STORAGE_KEY)!!).asJsonMap()
        assertEq("openai", stored["kind"])
        assertEq("sk-abcdefghijklmnopqrstuv", stored["apiKey"])
        val read = store.read()!!
        assertEq("sk-abc…qrstuv".take(6), read.maskedKey.take(6))
        assertEq(AgentProviderKind.OpenAi, read.kind)
        secrets.write(AgentCredentialStore.STORAGE_KEY, """{"kind":"anthropic","apiKey":"sk-ant-x","model":""}""")
        assertEq("claude-opus-5-5", store.read()!!.model)
        val verify = store.verify(AgentCredentials(AgentProviderKind.Anthropic, "sk-ant-bad", "m"), transport)
        expect(verify!!.startsWith("Claude rejected this key."))
        expect(AgentProviderKind.Anthropic.looksLikeKey("sk-ant-0123456789abcdefg"))
        expect(!AgentProviderKind.Anthropic.looksLikeKey("sk-0123456789abcdefghij"))
    }
}
