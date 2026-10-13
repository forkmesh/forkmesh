package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class AgentRunLogTest {
    private fun log(text: String, pull: String = ""): AgentRunLog =
        AgentRunLog(AgentTaskChain(AgentLaunchSession.fromJson(Json.parse(text).asJsonMap()), pullStatus = pull), "jett/site")

    private fun texts(log: AgentRunLog): List<String> = log.lines.map { it.text }

    fun testAQueuedRunShowsTheCommandThePromptAndTheWait() {
        val queued = log(
            """{"id":"s","status":"queued","targetNode":"fmnext-run-1","provider":"claude","createdBy":"jett",
            "createdAt":1700000000,"createPr":true,"history":[{"role":"user","text":"Fix the header\nand the footer"}]}""",
        )
        assertEq(
            listOf(
                "$ forkmesh agent run --repo jett/site --provider claude --on fmnext-run-1 --pr",
                "[1/5] Prompt · Sent by @jett",
                "› Fix the header",
                "  and the footer",
                "[2/5] Mirror · Waiting for fmnext-run-1 to pick it up",
            ),
            texts(queued),
        )
        assertEq(AgentLogTone.Command, queued.lines[0].tone)
        assertEq(1_700_000_000_000L, queued.lines[0].atMs)
        assertEq(AgentLogTone.Active, queued.lines[4].tone)
        expect(queued.isLive)
        expect(!queued.isStreaming)
        assertEq("live", queued.status)
        assertEq("claude@fmnext-run-1", queued.host)
    }

    fun testARunningRunStreamsTheLiveTranscript() {
        val running = log(
            """{"id":"s","status":"running","targetNode":"n1","localAgentId":4,"history":[{"role":"user","text":"Go"}],
            "agentInfo":{"branchName":"agent/go","baseBranch":"main"},
            "live":{"text":"Looking around.\n\n▸ Bash: ls -la\n\nDone looking.","at":1700000100}}""",
        )
        expect(running.isStreaming)
        assertEq(
            listOf(
                "[2/5] Mirror · Ran on n1",
                "[3/5] Branch · agent/go from main",
                "Looking around.",
                "▸ Bash: ls -la",
                "Done looking.",
            ),
            texts(running).takeLast(5),
        )
        assertEq(AgentLogTone.Tool, running.lines.last { it.text.startsWith("▸") }.tone)
        assertEq(AgentLogTone.Active, running.lines.first { it.text.startsWith("[3/5]") }.tone)
    }

    fun testARunningRunWithNoOutputSaysItIsWaiting() {
        val running = log("""{"id":"s","status":"running","localAgentId":4}""")
        assertEq("# waiting for the agent's first output…", texts(running).last())
    }

    fun testLiveTextIsIgnoredOnceTheRunEnds() {
        val done = log(
            """{"id":"s","status":"completed","localAgentId":4,"createdAt":1000,"completedAt":1192,
            "history":[{"role":"user","text":"Go"},{"role":"assistant","text":"▸ Edit: a.txt\n\nAll set.","at":1192}],
            "live":{"text":"stale"},"agentInfo":{"branchName":"agent/go","prNumber":12}}""",
            pull = "open",
        )
        expect(!done.isStreaming)
        expect("stale" !in texts(done))
        assertEq(
            listOf(
                "▸ Edit: a.txt",
                "All set.",
                "✓ agent finished in 3m 12s",
                "[4/5] Pull request · #12",
                "[5/5] Merge · Open — review and merge #12",
            ),
            texts(done).takeLast(5),
        )
        assertEq("exit 0", done.status)
        assertEq("3m 12s", done.elapsed(9_999_999_999L))
    }

    fun testAFailedRunEndsOnItsError() {
        val failed = log("""{"id":"s","status":"failed","targetNode":"n1","lastError":"Claude is not signed in"}""")
        assertEq("✗ Claude is not signed in", failed.lines.last().text)
        assertEq(AgentLogTone.Failure, failed.lines.last().tone)
        assertEq("exit 1", failed.status)
        expect(!failed.isLive)
    }

    fun testAMergedRunReadsMerged() {
        val merged = log(
            """{"id":"s","status":"completed","localAgentId":4,"agentInfo":{"branchName":"agent/go","prNumber":3}}""",
            pull = "merged",
        )
        assertEq("merged", merged.status)
        assertEq("[5/5] Merge · Merged", merged.lines.last().text)
        assertEq(AgentLogTone.Step, merged.lines.last().tone)
    }

    fun testFollowUpsAndLongOutputAreBounded() {
        val body = (1..AgentRunLog.MAX_OUTPUT_LINES + 5).joinToString("\\n") { "line $it" }
        val long = log(
            """{"id":"s","status":"completed","localAgentId":4,"history":[{"role":"user","text":"Go"},
            {"role":"assistant","text":"first"},{"role":"user","author":"ana","text":"Now more"},
            {"role":"assistant","text":"$body"}]}""",
        )
        expect("… 7 earlier lines" in texts(long))
        expect("› @ana Now more" !in texts(long))
        expect("line ${AgentRunLog.MAX_OUTPUT_LINES + 5}" in texts(long))
        val short = log(
            """{"id":"s","status":"completed","history":[{"role":"user","text":"Go"},
            {"role":"user","author":"ana","text":"Now more"}]}""",
        )
        expect("› @ana Now more" in texts(short))
    }

    fun testDurations() {
        assertEq("12s", AgentRunLog.duration(1_000, 13_000))
        assertEq("1h 02m", AgentRunLog.duration(1_000, 1_000 + 3_725_000))
        assertEq("", AgentRunLog.duration(0, 5_000))
        assertEq("", AgentRunLog.duration(5_000, 1_000))
    }
}
