package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.localrepos.GitAuthor
import com.forkmesh.nativeapp.localrepos.GitWorkspace
import com.forkmesh.nativeapp.localrepos.LocalPushRef
import com.forkmesh.nativeapp.localrepos.LocalPushResult
import com.forkmesh.nativeapp.localrepos.LocalRepoRecord
import com.forkmesh.nativeapp.localrepos.PullRequestService
import com.forkmesh.nativeapp.localrepos.withScratch
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class AgentDesktopHandoffTest {
    private class ScriptedProvider(private val turns: MutableList<AgentTurn>) : AgentProvider {
        override val id: String = "fake"
        override val model: String = "m"

        override fun send(systemPrompt: String, history: List<Any>, tools: List<AgentTool>, transport: HttpTransport): AgentTurn =
            turns.removeAt(0)

        override fun appendToolResults(history: MutableList<Any>, results: List<AgentToolResult>) {
            history.add(mapOf("role" to "tool", "results" to results.map { it.output }))
        }

        override fun appendUser(history: MutableList<Any>, text: String, images: List<PromptAttachment>) {
            history.add(mapOf("role" to "user", "content" to text))
        }
    }

    private class FakeDesktop : AgentHandoffDesktop {
        val calls = ArrayList<Map<String, String>>()
        var failure: Exception? = null
        override val desktopName: String = "studio"

        override fun handoff(owner: String, name: String, branch: String, base: String, task: String, title: String): Int {
            failure?.let { throw it }
            calls.add(mapOf("owner" to owner, "name" to name, "branch" to branch, "base" to base, "task" to task, "title" to title))
            return 42
        }
    }

    private val author = GitAuthor("Alice Mobile", "alice@example.com")

    private fun run(dir: File, turns: List<AgentTurn>, prompt: String): Triple<GitWorkspace, AgentSessionService, AgentRunHandle> {
        val ws = GitWorkspace(File(dir, "repo"))
        ws.init(mapOf("a.txt" to "one\n"), "base\n", author)
        val sessions = AgentSessionService(MemoryPrefStore(), launcher = { it.run() })
        val task = sessions.submit("repo", prompt, id = "task-123456789")!!
        val provider = ScriptedProvider(turns.toMutableList())
        val handle = sessions.start(task, ws, { CodingAgent(provider, it, transport = { error("no network") }) }, author)
        return Triple(ws, sessions, handle)
    }

    private fun pullRequests(pushed: MutableList<String>) = PullRequestService(
        pushBranch = { _, _, _, branch ->
            pushed.add(branch)
            LocalPushResult(listOf(LocalPushRef("refs/heads/$branch", "a".repeat(40), "updated")), 1, 1)
        },
        submitPull = { _, _, _, _, _, _ -> },
    )

    private fun record(ws: GitWorkspace, publishedAs: String? = "alice/repo") =
        LocalRepoRecord(name = "repo", path = ws.worktree.path, createdAt = 0, publishedAs = publishedAs)

    fun testAFinishedRunMovesToTheDesktopWithItsUncommittedWork() = withScratch("agent-handoff") { dir ->
        val (ws, sessions, handle) = run(
            dir,
            listOf(
                AgentTurn("", listOf(AgentToolCall("c", "write_file", mapOf("path" to "a.txt", "content" to "two\n"))), "a"),
                AgentTurn("Half way there.", emptyList(), "b"),
            ),
            "change a.txt to two",
        )
        try {
            val desktop = FakeDesktop()
            val pushed = ArrayList<String>()
            val rec = record(ws)
            expect(AgentDesktopHandoff.canMove(handle, rec, null))
            val move = AgentDesktopHandoff.move(handle, sessions, ws, rec, pullRequests(pushed), desktop, author)
            assertEq(AgentDesktopMove(42, "studio", handle.branch), move)
            assertEq("Continuing on studio as agent #42, on ${handle.branch}.", move.message)
            assertEq(listOf(handle.branch), pushed)
            val call = desktop.calls.single()
            assertEq("alice", call["owner"])
            assertEq("repo", call["name"])
            assertEq(handle.branch, call["branch"])
            assertEq("main", call["base"])
            assertEq("change a.txt to two", call["title"])
            val task = call.getValue("task")
            expect(task.contains("branch ${handle.branch} (started from main)"), task)
            expect(task.contains("Task:\nchange a.txt to two"), task)
            expect(task.contains("Tool: write_file a.txt"), task)
            expect(task.contains("Agent: Half way there."), task)
            expect(!task.contains("Me: change a.txt to two"), task)
            expect(ws.status().isClean)
            assertEq("main", ws.branches().single { it.isCurrent }.name)
            expect(!AgentDesktopHandoff.canMove(handle, rec, move.branch))
        } finally {
            ws.dispose()
        }
    }

    fun testAnOldDesktopKeepsTheAgentOnThePhoneAndSaysToUpdate() = withScratch("agent-handoff-old") { dir ->
        val (ws, sessions, handle) = run(dir, listOf(AgentTurn("Done.", emptyList(), "a")), "anything")
        try {
            val desktop = FakeDesktop().apply { failure = LocalControlException("unsupported") }
            val message = try {
                AgentDesktopHandoff.move(handle, sessions, ws, record(ws), pullRequests(ArrayList()), desktop, author)
                "moved"
            } catch (failure: AgentDesktopMoveException) {
                failure.message
            }
            assertEq("Update ForkMesh on the desktop to move agents to it.", message)
        } finally {
            ws.dispose()
        }
    }

    fun testARepositoryOnlyOnThisPhoneCannotMove() = withScratch("agent-handoff-local") { dir ->
        val (ws, sessions, handle) = run(dir, listOf(AgentTurn("Done.", emptyList(), "a")), "anything")
        try {
            val rec = record(ws, publishedAs = null)
            expect(!AgentDesktopHandoff.canMove(handle, rec, null))
            val pushed = ArrayList<String>()
            val refused = try {
                AgentDesktopHandoff.move(handle, sessions, ws, rec, pullRequests(pushed), FakeDesktop(), author)
                false
            } catch (_: AgentDesktopMoveException) {
                true
            }
            expect(refused)
            expect(pushed.isEmpty())
        } finally {
            ws.dispose()
        }
    }

    fun testALongConversationKeepsItsNewestStepsWithinTheDesktopLimit() {
        val events = listOf<AgentEvent>(AgentUserMessage("fix it")) + (0 until 200).map { AgentAssistantMessage("step $it " + "x".repeat(200)) }
        val task = AgentDesktopHandoff.task("fix it", events, "agent/abc-fix", "main")
        expect(task.length <= AgentDesktopHandoff.MAX_TASK_CHARS)
        expect(task.contains("… earlier steps omitted"))
        expect(task.contains("Agent: step 199 "))
        expect(!task.contains("Agent: step 0 "))
    }

    fun testAFollowUpInstructionIsKeptAndTheTitleIsTheFirstLine() {
        val events = listOf(AgentUserMessage("fix it"), AgentAssistantMessage("ok"), AgentUserMessage("also the tests"))
        val task = AgentDesktopHandoff.task("fix it", events, "agent/abc-fix", "main")
        expect(task.endsWith("Agent: ok\nMe: also the tests"), task)
        assertEq("Fix login", AgentDesktopHandoff.title("Fix login\nmore detail"))
    }
}
