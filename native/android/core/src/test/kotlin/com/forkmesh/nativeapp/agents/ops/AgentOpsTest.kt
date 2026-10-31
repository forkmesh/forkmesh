package com.forkmesh.nativeapp.agents.ops

import com.forkmesh.nativeapp.control.ControlErrorText
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class AgentOpsTest {
    fun testApprovalsAreParsedAndJunkIdsDropped() {
        val channel = FakeControlChannel()
        channel.reply(
            "agentops.approvals",
            mapOf("approvals" to listOf(approvalJson(approvalA, 3, command = "rm -rf build\nsecond line"), mapOf("id" to "../nope", "sessionId" to 1L))),
        )
        val list = AgentOpsClient(channel).approvals()
        assertEq(1, list.size)
        assertEq(3, list[0].sessionId)
        assertEq(ApprovalKind.Command, list[0].kind)
        assertEq("rm -rf build", list[0].headline)
        assertEq(ApprovalLifetime.entries.toList(), list[0].lifetimes)
        assertEq("Fix the build · me/app", list[0].subtitle)
    }

    fun testPrepareAnswerSendsActionAndLifetime() {
        val channel = FakeControlChannel()
        channel.reply("agentops.prepare", confirmationJson("approve"))
        val client = AgentOpsClient(channel) { 5_000L }
        val confirmation = client.prepareAnswer(approvalA, true, ApprovalLifetime.Session)
        assertEq("approve", channel.params("agentops.prepare").str("action"))
        assertEq("session", channel.params("agentops.prepare").str("lifetime"))
        assertEq(5_000L, confirmation.issuedAtMs)
        expect(!confirmation.isExpired(64_999))
        expect(confirmation.isExpired(65_000))
        client.answer(approvalA, false, token = "tok")
        val answer = channel.params("agentops.answer")!!
        assertEq("deny", answer.str("decision"))
        expect(!answer.containsKey("lifetime"), "a denial carries no lifetime")
    }

    fun testPrepareWithoutTokenFails() {
        val channel = FakeControlChannel()
        channel.reply("agentops.prepare", mapOf("action" to "approve"))
        val error = runCatching { AgentOpsClient(channel).prepareAnswer(approvalA, true) }.exceptionOrNull()
        assertEq("confirmation_required", error?.let(AgentOpsClient::errorCode))
    }

    fun testBulkCanonicalisesIds() {
        val channel = FakeControlChannel()
        channel.reply("agentops.prepare", confirmationJson("stop"))
        channel.reply("agentops.bulk", mapOf("action" to "stop", "done" to listOf(1L, 3L), "skipped" to listOf(2L)))
        val client = AgentOpsClient(channel)
        client.prepareBulk(BulkAgentAction.Stop, listOf(3, 1, 3, -4, 2))
        assertEq(listOf(1L, 2L, 3L), channel.params("agentops.prepare")!!["ids"])
        val outcome = client.bulk(BulkAgentAction.Stop, listOf(2, 1, 3), "tok")
        assertEq(listOf(1L, 2L, 3L), channel.params("agentops.bulk")!!["ids"])
        assertEq(listOf(1, 3), outcome.done)
        assertEq(listOf(2), outcome.skipped)
        assertEq("Stopped 2 agents; 1 was not in a state to stop.", outcome.message)
    }

    fun testDiffIsReadAndSplitIntoLines() {
        val channel = FakeControlChannel()
        channel.reply(
            "agentops.diff",
            mapOf(
                "id" to 7L, "branch" to "fm/fix", "base" to "main",
                "files" to listOf(
                    mapOf("path" to "a.cpp", "added" to 2L, "removed" to 1L, "binary" to false),
                    mapOf("path" to "img.png", "binary" to true),
                ),
                "committed" to "diff --git a/a.cpp b/a.cpp\n@@ -1 +1,2 @@\n-old\n+new\n+more\n ctx\n",
                "uncommitted" to "", "untracked" to listOf("notes.txt"), "hasWorktree" to true, "truncated" to true,
            ),
        )
        val model = AgentDiffModel(AgentOpsClient(channel), 7)
        model.load()
        assertEq(7L, channel.params("agentops.diff")?.get("id"))
        val diff = model.diff!!
        assertEq("fm/fix", diff.branch)
        assertEq(2, diff.added)
        assertEq(1, diff.removed)
        expect(diff.files[1].binary)
        assertEq(listOf("notes.txt"), diff.untracked)
        expect(diff.truncated)
        assertEq(
            listOf(AgentDiff.LineKind.Header, AgentDiff.LineKind.Hunk, AgentDiff.LineKind.Removed, AgentDiff.LineKind.Added, AgentDiff.LineKind.Added, AgentDiff.LineKind.Context),
            AgentDiff.lines(diff.committed).map { it.kind },
        )
        val review = model.review!!
        assertEq("agent/fm/fix", review.scope)
        assertEq(listOf("Committed on the branch"), review.sections.map { it.title })
        assertEq(listOf("a.cpp"), review.files.map { it.file.path })
        assertEq(2, review.additions)
        val old = FakeControlChannel()
        old.failures["agentops.diff"] = LocalControlException("unsupported")
        val stale = AgentDiffModel(AgentOpsClient(old), 7)
        stale.load()
        expect(stale.diff == null)
        expect(stale.errorText.contains("Update ForkMesh"))
    }

    fun testDeletePreparesThenSpendsTheToken() {
        val channel = FakeControlChannel()
        channel.reply("agentops.prepare", confirmationJson("delete", token = "tok-del"))
        channel.reply("agentops.delete", mapOf("id" to 7L))
        val model = AgentDeleteModel(AgentOpsClient(channel), 7)
        model.prepare()
        assertEq("delete", channel.params("agentops.prepare").str("action"))
        assertEq(AgentOpsAction.Delete, model.confirmation?.action)
        expect(model.run())
        expect(model.deleted)
        assertEq("tok-del", channel.params("agentops.delete").str("token"))
        expect(!model.run())
        expect(AgentOpsAction.Delete.isDestructive)
        val busy = FakeControlChannel()
        busy.failures["agentops.prepare"] = AgentOpsException("agent_running")
        val refused = AgentDeleteModel(AgentOpsClient(busy), 7)
        refused.prepare()
        expect(refused.confirmation == null)
        assertEq(AgentOpsClient.describe("agent_running"), refused.errorText)
    }

    fun testMergeSendsTypedBranch() {
        val channel = FakeControlChannel()
        channel.reply("agentops.prepare", confirmationJson("merge", typeToConfirm = "agent/fix"))
        channel.reply("agentops.merge", mapOf("id" to 7L, "branch" to "agent/fix", "merged" to true))
        val client = AgentOpsClient(channel)
        val confirmation = client.prepareMerge(7)
        expect(confirmation.destructive)
        assertEq("agent/fix", confirmation.typeToConfirm)
        val outcome = client.merge(7, confirmation.token, "agent/fix")
        assertEq("agent/fix", outcome.branch)
        expect(outcome.stats == null, "an older desktop sends no stats")
        assertEq("Merged agent/fix and cleaned up.", outcome.message())
        assertEq("agent/fix", channel.params("agentops.merge").str("typed"))
    }

    fun testMergeReplyCarriesLandedStats() {
        val channel = FakeControlChannel()
        channel.reply("agentops.prepare", confirmationJson("merge", typeToConfirm = "agent/fix"))
        channel.reply(
            "agentops.merge",
            mapOf(
                "id" to 7L, "branch" to "agent/fix", "merged" to true,
                "stats" to mapOf(
                    "base" to "main", "commit" to "0123456789abcdef", "commits" to 4L, "files" to 3L,
                    "insertions" to 40L, "deletions" to 2L,
                    "velocity" to listOf(
                        mapOf("id" to "1h", "label" to "1 hour", "count" to 2L, "top" to 5L, "avg" to 0.4),
                        mapOf(
                            "id" to "7d", "label" to "7 days", "count" to 30L, "top" to 41L, "avg" to 12.6,
                            "series" to listOf(2L, 0L, 5L, 9L, 4L, 6L, 4L),
                        ),
                    ),
                ),
            ),
        )
        val client = AgentOpsClient(channel)
        val confirmation = client.prepareMerge(7)
        val outcome = client.merge(7, confirmation.token, "agent/fix")
        val stats = outcome.stats ?: error("stats missing")
        assertEq("main", stats.base)
        assertEq("0123456", stats.shortCommit)
        assertEq("4 commits", stats.commitsText)
        assertEq("3 files", stats.filesText)
        assertEq("+40", stats.insertionsText)
        assertEq("\u22122", stats.deletionsText)
        assertEq("4 commits \u00B7 3 files +40 \u22122", stats.summary)
        assertEq(listOf("1 hour", "7 days"), stats.velocity.map { it.label })
        assertEq("top 5 \u00B7 avg 0.4", stats.velocity[0].detail)
        assertEq("top 41 \u00B7 avg 13", stats.velocity[1].detail)
        assertEq(emptyList<Int>(), stats.velocity[0].series)
        assertEq(listOf(2, 0, 5, 9, 4, 6, 4), stats.velocity[1].series)
        assertEq(9, stats.velocity[1].peak)
        assertEq(0.4f, stats.velocity[0].share)
        assertEq("1 hour: 2 merges, best 5, average 0.4", stats.velocity[0].spoken)
        assertEq(40f / 42f, stats.insertionShare)
        assertEq("Merged agent/fix and cleaned up. 4 commits \u00B7 3 files +40 \u22122.", outcome.message())
    }

    fun testMergeLeaderboardReadsTheNewestMergeWithoutAToken() {
        val channel = FakeControlChannel()
        channel.reply(
            "agentops.mergestats",
            mapOf(
                "repo" to "me/app",
                "branch" to "agent/fix",
                "stats" to mapOf(
                    "base" to "main", "commit" to "0123456789abcdef", "commits" to 2L, "files" to 1L,
                    "insertions" to 3L, "deletions" to 1L,
                    "velocity" to listOf(mapOf("id" to "24h", "label" to "24 hours", "count" to 4L, "top" to 9L, "avg" to 2.5)),
                ),
            ),
        )
        val client = AgentOpsClient(channel)
        val board = client.mergeStats()
        assertEq(listOf("agentops.mergestats"), channel.methods())
        assertEq(emptyMap<String, Any?>(), channel.params("agentops.mergestats"))
        assertEq("me/app", board.repo)
        assertEq("agent/fix", board.branch)
        assertEq("2 commits \u00B7 1 file +3 \u22121", board.stats?.summary)
        assertEq("me/app \u00B7 newest merge: agent/fix \u00B7 into main", board.subtitle)
        client.mergeStats(7)
        assertEq(7L, channel.params("agentops.mergestats")?.get("id"))
    }

    fun testMergeLeaderboardOnAnOldDesktopSaysToUpdate() {
        val channel = FakeControlChannel()
        channel.failures["agentops.mergestats"] = LocalControlException("unsupported")
        val model = MergeLeaderboardModel(AgentOpsClient(channel))
        model.refresh()
        expect(model.loaded)
        assertEq(null, model.board.stats)
        expect(model.errorText.contains("Update ForkMesh"))
    }

    fun testMergeStatsWithoutALandingHaveNoSummary() {
        val stats = MergeStats.from(mapOf("stats" to mapOf("base" to "main", "files" to 0L, "velocity" to emptyList<Any>())))
        expect(stats != null)
        expect(stats?.hasLanding == false)
        assertEq("", stats?.summary)
        assertEq("", stats?.commitsText)
        assertEq(null, stats?.insertionShare)
        expect(MergeStats.from(mapOf("stats" to "x")) == null)
        assertEq("top 3 \u00B7 avg 0", MergeVelocityWindow("1h", "1 hour", 3, 1, 0.0).detail)
    }

    fun testWorktreesUsageAndGrantsParse() {
        val channel = FakeControlChannel()
        channel.reply(
            "agentops.worktrees",
            mapOf(
                "repo" to "me/app",
                "base" to "main",
                "worktrees" to listOf(
                    mapOf("name" to "app", "branch" to "main", "isMain" to true),
                    mapOf("name" to "agent-7", "branch" to "agent/fix", "ahead" to 2L, "behind" to 1L, "sessionId" to 7L),
                ),
            ),
        )
        channel.reply(
            "agentops.usage",
            mapOf(
                "providers" to listOf(
                    mapOf(
                        "id" to "claude-code",
                        "label" to "Claude Code",
                        "windows" to listOf(
                            mapOf("id" to "5h", "label" to "5-hour", "usedPercent" to 100L, "state" to "exhausted", "resetAtMs" to 90_000L),
                            mapOf("id" to "weekly", "label" to "Weekly", "usedPercent" to -1L, "state" to "unknown"),
                        ),
                        "accounts" to listOf(mapOf("id" to "a", "label" to "Work", "active" to true)),
                        "credits" to mapOf("used" to 3.5, "limit" to 20.0, "currency" to "USD"),
                    ),
                ),
                "spend" to mapOf("claudeUsd" to 1.5),
                "atMs" to 1_000L,
            ),
        )
        channel.reply(
            "agentops.grants",
            mapOf("grants" to listOf(mapOf("id" to "g", "capability" to "file.write", "kind" to "file", "resource" to "a.txt", "decision" to "allow"))),
        )
        val client = AgentOpsClient(channel)
        val worktrees = client.worktrees("me", "app")
        assertEq("me", channel.params("agentops.worktrees").str("owner"))
        assertEq(2, worktrees.worktrees.size)
        assertEq("Main checkout", worktrees.worktrees[0].syncLabel)
        assertEq("2 ahead, 1 behind", worktrees.worktrees[1].syncLabel)
        assertEq(7, worktrees.worktrees[1].sessionId)
        assertEq(null, worktrees.worktrees[0].sessionId)

        val usage = client.usage()
        val claude = usage.providers.first()
        assertEq(UsageState.Exhausted, claude.worstState)
        assertEq(0, claude.windows[0].remainingPercent)
        assertEq(null, claude.windows[1].usedPercent)
        assertEq("resets in 1m", claude.windows[0].resetLabel(30_000))
        val hour = 3_600_000L
        val weekly = UsageWindowMeter("weekly", "Weekly", 59, 168 * hour, UsageState.Ok)
        assertEq(168 * hour, weekly.windowMs)
        assertEq(7, weekly.segments)
        assertEq("Day 2 of 7", weekly.periodLabel(30 * hour))
        assertEq("41% over pace", weekly.paceLabel(30 * hour))
        val session = UsageWindowMeter("5h", "5-hour", 10, 5 * hour, UsageState.Ok)
        assertEq(5, session.segments)
        assertEq("Hour 3 of 5", session.periodLabel(2 * hour + 1))
        assertEq("30% under pace", session.paceLabel(2 * hour))
        val codexWeekly = UsageWindowMeter.from(
            mapOf("id" to "weekly", "label" to "Weekly", "usedPercent" to 50, "resetAtMs" to 36_000_000L, "windowMs" to 72_000_000L, "state" to "ok"),
        )
        assertEq(72_000_000L, codexWeekly.windowMs)
        assertEq("on pace", codexWeekly.paceLabel(0))
        assertEq("", UsageWindowMeter("w", "Credits", 1, null, UsageState.Ok).paceLabel(0))
        assertEq("Work", claude.activeAccount?.label)
        assertEq("3.50 of 20.00 USD", claude.credits?.label)
        assertEq(1.5, usage.totalSpendUsd)
        val reminders = usage.reminders(10_000)
        assertEq(listOf("usage.claude-code.5h"), reminders.map { it.id })
        assertEq(90_000L, reminders.first().fireAtMs)
        expect(usage.reminders(100_000).isEmpty(), "no reminder for a reset in the past")

        val grants = client.grants()
        assertEq(ApprovalKind.File, grants.first().kind)
        expect(grants.first().allows)
    }

    private fun accountsSummary(activeId: String, label: String = "Side"): Map<String, Any?> = mapOf(
        "providers" to listOf(
            mapOf(
                "id" to "codex",
                "label" to "Codex",
                "windows" to listOf(mapOf("id" to "5h", "label" to "5h", "usedPercent" to 40L, "state" to "ok")),
                "accounts" to listOf(
                    mapOf(
                        "id" to "default", "label" to "Default", "active" to (activeId == "default"), "signedIn" to true,
                        "identity" to "me@example.com", "windows" to emptyList<Any>(),
                    ),
                    mapOf(
                        "id" to "n1", "label" to label, "active" to (activeId == "n1"), "signedIn" to false,
                        "windows" to listOf(mapOf("id" to "weekly", "label" to "Weekly", "usedPercent" to 90L, "state" to "high")),
                    ),
                ),
            ),
        ),
        "spend" to mapOf("claudeUsd" to 2.0, "openAiUsd" to 0.5),
        "activeProvider" to "codex",
    )

    fun testUsageAccountsCarryTheirOwnWindowsAndSignInState() {
        val summary = UsageSummary.from(accountsSummary("default"))
        val codex = summary.provider("codex")!!
        val (main, side) = codex.accounts
        assertEq(true, main.signedIn)
        expect(main.isDefault && !side.isDefault)
        assertEq("me@example.com", main.identity)
        assertEq("me@example.com", main.detail)
        assertEq("In use", main.statusLabel)
        assertEq(listOf("5h"), codex.windowsFor(main).map { it.id })
        assertEq(false, side.signedIn)
        expect(side.signedOut)
        assertEq("Not signed in", side.statusLabel)
        assertEq("Not signed in", side.detail)
        assertEq(listOf("weekly"), codex.windowsFor(side).map { it.id })
        assertEq(UsageState.High, codex.windowsFor(side).single().state)
        val older = UsageAccount.from(mapOf("id" to "x", "label" to "", "active" to false))
        assertEq(null, older.signedIn)
        assertEq("Available", older.statusLabel)
        assertEq("Signed in", older.detail)
        assertEq("Account", older.name)
        expect(codex.windowsFor(older).isEmpty(), "an idle account without readings shows none")
        assertEq(2.0, summary.spendUsd("claude-code"))
        assertEq(0.5, summary.spendUsd("codex"))
        assertEq(null, summary.spendUsd("other"))
        expect(summary.usedForNewAgents("codex") && !summary.usedForNewAgents("claude-code"))
        expect(!UsageSummary().usedForNewAgents(""), "an older desktop names no provider")
        expect(!summary.isEmpty)
        expect(UsageSummary().isEmpty)
    }

    fun testAccountActionsSendProviderAndAccountAndReplaceTheSummary() {
        val channel = FakeControlChannel()
        channel.reply("agentops.usage", accountsSummary("default"))
        channel.reply("agentops.accounts.select", accountsSummary("n1"))
        channel.reply("agentops.accounts.rename", accountsSummary("n1", "Side 2"))
        channel.reply("agentops.accounts.add", accountsSummary("n2") + ("account" to "n2"))
        channel.reply("agentops.accounts.delete", accountsSummary("default"))
        val model = AgentUsageModel(AgentOpsClient(channel))
        model.refresh()
        assertEq("default", model.summary?.provider("codex")?.activeAccount?.id)

        expect(model.selectAccount("codex", "n1"))
        assertEq("codex", channel.params("agentops.accounts.select").str("provider"))
        assertEq("n1", channel.params("agentops.accounts.select").str("account"))
        assertEq("n1", model.summary?.provider("codex")?.activeAccount?.id)

        expect(model.renameAccount("codex", "n1", "  Side 2  "))
        assertEq("Side 2", channel.params("agentops.accounts.rename").str("label"))
        assertEq("Side 2", model.summary?.provider("codex")?.activeAccount?.label)
        expect(!model.renameAccount("codex", "n1", "   "), "a blank name is not sent")
        assertEq(1, channel.methods().count { it == "agentops.accounts.rename" })

        assertEq("n2", model.addAccount("codex", "Work"))
        assertEq("Work", channel.params("agentops.accounts.add").str("label"))
        assertEq(null, model.addAccount("codex", " "))
        assertEq("x".repeat(80), AgentOpsClient.accountLabel("x".repeat(90)))

        expect(model.deleteAccount("codex", "n1"))
        assertEq("n1", channel.params("agentops.accounts.delete").str("account"))
        assertEq("", model.errorText)
        expect(!model.busy)

        channel.failures["agentops.accounts.delete"] = LocalControlException("default_account")
        expect(!model.deleteAccount("codex", "default"))
        assertEq("The default account cannot be deleted. Log it out instead.", model.errorText)
        assertEq("default", model.summary?.provider("codex")?.activeAccount?.id, "a failed action keeps the last summary")
    }

    fun testBellsAreFilteredToChanged() {
        val channel = FakeControlChannel()
        val client = AgentOpsClient(channel)
        val seen = ArrayList<Int>()
        val tap = client.onChanged { seen.add(it.int("count")) }
        channel.emit("agentops.changed", mapOf("count" to 2L))
        channel.emit("agentops.other", mapOf("count" to 9L))
        channel.emit("agents.changed", mapOf("count" to 5L))
        assertEq(listOf(2), seen)
        client.removeTap(tap)
        assertEq(0, channel.tapCount)
    }

    fun testErrorCodesAndText() {
        assertEq("agent_running", AgentOpsClient.errorCode(LocalControlException("agent_running")))
        expect(AgentOpsClient.isUnsupported(LocalControlException("unsupported")))
        expect(!AgentOpsClient.isUnsupported(LocalControlException("timeout")))
        assertEq("The agent is still working on that branch. Stop it first.", AgentOpsClient.describe(LocalControlException("agent_running")))
        assertEq(ControlErrorText.describe("confirmation_expired"), AgentOpsClient.describe("confirmation_expired"))
        assertEq(null, AgentOpsClient.errorCode(IllegalStateException("x")))
        assertEq(listOf(2, 5), AgentOpsClient.canonical(listOf(5, 5, 0, 2)))
    }

    fun testUsageFormat() {
        assertEq("1m", UsageFormat.remaining(1))
        assertEq("1h 30m", UsageFormat.remaining(90 * 60_000L))
        assertEq("3h", UsageFormat.remaining(3 * 3_600_000L))
        assertEq("2d 2h", UsageFormat.remaining(50 * 3_600_000L))
        assertEq("$12.50", UsageFormat.usd(12.5))
    }

    fun testQueueRefreshSortsNewestFirst() {
        val channel = FakeControlChannel()
        channel.reply("agentops.approvals", mapOf("approvals" to listOf(approvalJson(approvalA, 1, 10), approvalJson(approvalB, 2, 20))))
        val queue = ApprovalsQueue(AgentOpsClient(channel))
        queue.refresh()
        assertEq(listOf(approvalB, approvalA), queue.approvals.map { it.id })
        assertEq(2, queue.count)
        assertEq(setOf(1, 2), queue.sessionIdsNeedingYou)
        assertEq(listOf(approvalA), queue.approvals(1).map { it.id })
        assertEq(2, queue.approval(approvalB)?.sessionId)
    }

    fun testQueueApproveRunsPrepareThenAnswer() {
        val channel = FakeControlChannel()
        channel.reply("agentops.approvals", mapOf("approvals" to listOf(approvalJson(approvalA, 1))))
        channel.reply("agentops.prepare", confirmationJson("approve", "t1"))
        val queue = ApprovalsQueue(AgentOpsClient(channel))
        queue.refresh()
        expect(queue.approve(approvalA, ApprovalLifetime.Worktree))
        assertEq(listOf("agentops.approvals", "agentops.prepare", "agentops.answer"), channel.methods())
        assertEq("t1", channel.params("agentops.answer").str("token"))
        assertEq("worktree", channel.params("agentops.answer").str("lifetime"))
        expect(queue.isEmpty)
        assertEq(approvalA, queue.lastAnswered)
    }

    fun testQueueDenyAlreadyAnsweredDropsTheRow() {
        val channel = FakeControlChannel()
        channel.reply("agentops.approvals", mapOf("approvals" to listOf(approvalJson(approvalA, 1))))
        channel.failures["agentops.prepare"] = LocalControlException("unknown_approval")
        val queue = ApprovalsQueue(AgentOpsClient(channel))
        queue.refresh()
        expect(!queue.deny(approvalA))
        expect(queue.isEmpty)
        assertEq(AgentOpsClient.describe("unknown_approval"), queue.errorText)
    }

    fun testQueueRefusesBadIdLocally() {
        val channel = FakeControlChannel()
        expect(!ApprovalsQueue(AgentOpsClient(channel)).approve("nope"))
        expect(channel.requests.isEmpty())
    }

    fun testQueueOlderDesktopIsUnsupportedNotAnError() {
        val channel = FakeControlChannel()
        channel.failures["agentops.approvals"] = LocalControlException("unsupported")
        val queue = ApprovalsQueue(AgentOpsClient(channel))
        queue.refresh()
        expect(queue.unsupported)
        assertEq("", queue.errorText)
    }

    fun testQueueBellRefreshesAndStopRemovesTheTap() {
        val channel = FakeControlChannel()
        channel.reply("agentops.approvals", mapOf("approvals" to listOf(approvalJson(approvalA, 1))))
        val queue = ApprovalsQueue(AgentOpsClient(channel))
        queue.start()
        queue.start()
        assertEq(1, channel.tapCount)
        channel.emit("agentops.changed", mapOf("what" to "approvals"))
        channel.emit("agentops.changed", mapOf("what" to "approvals"))
        assertEq(1, queue.count)
        expect(channel.methods().count { it == "agentops.approvals" } <= 2)
        queue.stop()
        assertEq(0, channel.tapCount)
    }

    fun testQueueNotifiesListeners() {
        val channel = FakeControlChannel()
        channel.reply("agentops.approvals", mapOf("approvals" to listOf(approvalJson(approvalA, 1))))
        val queue = ApprovalsQueue(AgentOpsClient(channel))
        var calls = 0
        queue.addListener { calls++ }
        queue.refresh()
        expect(calls >= 2)
        expect(queue.revision >= 2)
    }

    fun testMergeIsOneClickAndSendsTheBranch() {
        val channel = FakeControlChannel()
        channel.reply("agentops.prepare", confirmationJson("merge", typeToConfirm = "agent/fix"))
        channel.reply("agentops.merge", mapOf("branch" to "agent/fix", "merged" to true, "stats" to mapOf("files" to 2L, "commit" to "abc")))
        val model = MergeCleanupModel(AgentOpsClient(channel), 7)
        expect(!model.canRun)
        model.prepare()
        expect(model.canRun)
        expect(model.run())
        assertEq("agent/fix", channel.params("agentops.merge").str("typed"))
        assertEq("agent/fix", model.mergedBranch)
        assertEq(2, model.mergeStats?.files)
        expect(!model.canRun, "a merged session cannot be merged again")
    }

    fun testMergeNowSendsTheDesktopsBranchName() {
        val channel = FakeControlChannel()
        channel.reply("agentops.prepare", confirmationJson("merge", typeToConfirm = "agent/fix"))
        channel.reply("agentops.merge", mapOf("branch" to "agent/fix", "merged" to true))
        val model = MergeCleanupModel(AgentOpsClient(channel), 7)
        expect(model.mergeNow())
        assertEq(listOf("agentops.prepare", "agentops.merge"), channel.methods())
        assertEq("agent/fix", model.mergedBranch)
        expect(!model.mergeNow(), "a merged session cannot be merged again")
    }

    fun testDirtyCheckoutOffersSetAsideAndTheNoteLands() {
        val channel = FakeControlChannel()
        channel.reply("agentops.prepare", confirmationJson("merge", typeToConfirm = "agent/fix"))
        channel.failures["agentops.merge"] = LocalControlException("checkout_dirty", "main has uncommitted changes in a.txt")
        val model = MergeCleanupModel(AgentOpsClient(channel), 7)
        expect(!model.mergeNow())
        assertEq(true, channel.params("agentops.merge")?.get("offerSetAside"))
        expect(channel.params("agentops.merge")?.get("setAside") == null)
        expect(model.canSetAside)
        expect(model.errorText.contains("uncommitted"))
        channel.failures.remove("agentops.merge")
        channel.reply("agentops.merge", mapOf("branch" to "agent/fix", "merged" to true, "setAside" to "Your edits are back."))
        expect(model.mergeNow(setAside = true))
        assertEq(true, channel.params("agentops.merge")?.get("setAside"))
        expect(!model.canSetAside)
        assertEq("Your edits are back.", model.setAsideNote)
        assertEq("Merged agent/fix and cleaned up. Your edits are back.", model.outcome?.message())
    }

    fun testMergeNowStopsWhenPrepareFails() {
        val channel = FakeControlChannel()
        channel.failures["agentops.prepare"] = LocalControlException("agent_running")
        val model = MergeCleanupModel(AgentOpsClient(channel), 1)
        expect(!model.mergeNow())
        expect("agentops.merge" !in channel.methods())
        assertEq(AgentOpsClient.describe("agent_running"), model.errorText)
    }

    fun testExpiredMergeTokenIsNotSent() {
        val channel = FakeControlChannel()
        channel.reply("agentops.prepare", confirmationJson("merge", typeToConfirm = "b"))
        val clock = MutableClock(0)
        val model = MergeCleanupModel(AgentOpsClient(channel, clock.read), 1)
        model.prepare()
        clock.nowMs = 61_000
        expect(!model.run())
        expect("agentops.merge" !in channel.methods())
        assertEq(ControlErrorText.describe("confirmation_expired"), model.errorText)
    }

    fun testMergeConflictIsFlaggedForAnAgentFix() {
        val channel = FakeControlChannel()
        channel.reply("agentops.prepare", confirmationJson("merge", typeToConfirm = "agent/fix"))
        channel.failures["agentops.merge"] = LocalControlException("conflicts")
        val model = MergeCleanupModel(AgentOpsClient(channel), 7)
        model.prepare()
        expect(!model.conflicted)
        expect(!model.run())
        expect(model.conflicted)
        assertEq(AgentOpsClient.describe("conflicts"), model.errorText)
        val refused = FakeControlChannel()
        refused.reply("agentops.prepare", confirmationJson("merge", typeToConfirm = "agent/fix"))
        refused.failures["agentops.merge"] = LocalControlException("merge_failed")
        val other = MergeCleanupModel(AgentOpsClient(refused), 7)
        other.prepare()
        expect(!other.run())
        expect(!other.conflicted)
        assertEq(AgentOpsClient.describe("merge_failed"), other.errorText)
    }

    fun testAFailedMergeSaysWhyWhenTheDesktopDoes() {
        val channel = FakeControlChannel()
        channel.reply("agentops.prepare", confirmationJson("merge", typeToConfirm = "agent/fix"))
        channel.failures["agentops.merge"] =
            LocalControlException("merge_failed", "Couldn't merge agent/fix: main has uncommitted changes.")
        val model = MergeCleanupModel(AgentOpsClient(channel), 7)
        model.prepare()
        expect(!model.run())
        assertEq("The merge did not finish. Couldn't merge agent/fix: main has uncommitted changes.", model.errorText)
        assertEq(
            AgentOpsClient.describe("merge_failed"),
            AgentOpsClient.describe(LocalControlException("merge_failed", "  ")),
        )
    }

    fun testMergeFailureExplains() {
        val channel = FakeControlChannel()
        channel.failures["agentops.prepare"] = LocalControlException("agent_running")
        val model = MergeCleanupModel(AgentOpsClient(channel), 1)
        model.prepare()
        assertEq(null, model.confirmation)
        assertEq(AgentOpsClient.describe("agent_running"), model.errorText)
    }

    fun testBulkUsesSelectionOrEveryEligibleRow() {
        val channel = FakeControlChannel()
        channel.reply("agentops.prepare", confirmationJson("stop"))
        channel.reply("agentops.bulk", mapOf("done" to listOf(1L), "skipped" to emptyList<Long>()))
        val model = BulkActionsModel(AgentOpsClient(channel))
        val rows = listOf(
            BulkCandidate(1, isActive = true, stoppable = true),
            BulkCandidate(2, isActive = false, stoppable = false),
            BulkCandidate(3, isActive = true, stoppable = false),
            BulkCandidate(4, isActive = false, stoppable = false, merged = true),
        )
        assertEq(listOf(1), model.eligible(BulkAgentAction.Stop, rows))
        assertEq(listOf(2), model.eligible(BulkAgentAction.ContinueRun, rows))
        model.toggle(2)
        assertEq(emptyList<Int>(), model.eligible(BulkAgentAction.Stop, rows))
        model.toggle(2)
        model.prepare(BulkAgentAction.Stop, rows)
        assertEq(BulkAgentAction.Stop, model.pendingAction)
        val outcome = model.run()
        assertEq(listOf(1), outcome?.done)
        assertEq(listOf(1L), channel.params("agentops.bulk")!!["ids"])
        assertEq(null, model.confirmation)
    }

    fun testBulkWithNothingEligibleSendsNothing() {
        val channel = FakeControlChannel()
        val model = BulkActionsModel(AgentOpsClient(channel))
        model.prepare(BulkAgentAction.Stop, listOf(BulkCandidate(1, isActive = false, stoppable = false)))
        expect(channel.requests.isEmpty())
        assertEq("Nothing to stop.", model.errorText)
    }

    fun testUsageAndWorktreesModels() {
        val channel = FakeControlChannel()
        channel.reply("agentops.usage", mapOf("providers" to listOf(mapOf("id" to "codex", "label" to "Codex", "windows" to emptyList<Any>()))))
        channel.reply(
            "agentops.worktrees",
            mapOf("worktrees" to listOf(mapOf("name" to "app", "branch" to "main", "isMain" to true), mapOf("name" to "wt", "branch" to "agent/x"))),
        )
        val client = AgentOpsClient(channel)
        val usage = AgentUsageModel(client)
        usage.refresh()
        assertEq("codex", usage.summary?.providers?.first()?.id)
        val worktrees = WorktreesModel(client, "me", "app")
        worktrees.refresh()
        expect(worktrees.loaded)
        assertEq(listOf("agent/x"), worktrees.agentWorktrees.map { it.branch })
    }

    fun testGrantsRevokeRemovesTheRow() {
        val channel = FakeControlChannel()
        channel.reply("agentops.grants", mapOf("grants" to listOf(mapOf("id" to "g1", "capability" to "process.execute"))))
        channel.reply("agentops.prepare", confirmationJson("revoke"))
        val model = CapabilityGrantsModel(AgentOpsClient(channel))
        model.refresh()
        val grant = model.grants[0]
        val confirmation = model.prepareRevoke(grant)!!
        expect(model.revoke(grant, confirmation))
        expect(model.grants.isEmpty())
        assertEq("g1", channel.params("agentops.revoke").str("grantId"))
    }

    fun testSortOrdersCoverTheDesktopMenu() {
        assertEq(12, AgentListSort.entries.size)
        val a = AgentListItem(
            AgentSession(id = 1, issueTitle = "beta", status = "running", createdAtMs = 100, costUsd = 1.0, totalTokens = 50, filesChanged = 3),
            priority = 10,
        )
        val b = AgentListItem(
            AgentSession(id = 2, issueNumber = 4, issueTitle = "Alpha", status = "success", createdAtMs = 200, finishedAtMs = 900, costUsd = 5.0, totalTokens = 10, filesChanged = 9),
            priority = 90,
        )
        val c = AgentListItem(AgentSession(id = 3, issueTitle = "gamma", status = "waiting", createdAtMs = 50, durationMs = 5_000), needsYou = true)
        val items = listOf(a, b, c)
        fun ids(sort: AgentListSort) = AgentListOrdering.sorted(items, sort, 1_000).map { it.session.id }
        assertEq(listOf(1, 3, 2), ids(AgentListSort.Priority))
        assertEq(7, AgentListItem(AgentSession(id = 4, priority = 7)).priority)
        assertEq(99, AgentSession.fromJson(mapOf("id" to 5, "priority" to 250)).priority)
        assertEq(50, AgentSession.fromJson(mapOf("id" to 6)).priority)
        assertEq(listOf(2, 1, 3), ids(AgentListSort.NewestAdded))
        assertEq(listOf(3, 1, 2), ids(AgentListSort.OldestAdded))
        assertEq(listOf(2, 1, 3), ids(AgentListSort.RecentActivity))
        assertEq(listOf(3, 1, 2), ids(AgentListSort.Status))
        assertEq(listOf(2, 1, 3), ids(AgentListSort.Title))
        assertEq(2, ids(AgentListSort.IssueNumber).first())
        assertEq(1, ids(AgentListSort.TokenUsage).first())
        assertEq(2, ids(AgentListSort.Cost).first())
        assertEq(3, ids(AgentListSort.Runtime).first())
        assertEq(2, ids(AgentListSort.FilesChanged).first())
        val wrapped = AgentListOrdering.sorted(listOf("x", "y", "z"), AgentListSort.NewestAdded) { name ->
            when (name) {
                "x" -> a
                "y" -> b
                else -> c
            }
        }
        assertEq(listOf("y", "x", "z"), wrapped)
    }

    fun testFiltersMatchTheAgentsRoutes() {
        val items = listOf(
            AgentListItem(AgentSession(id = 1, status = "running", createdAtMs = 100)),
            AgentListItem(AgentSession(id = 2, status = "queued", createdAtMs = 100)),
            AgentListItem(AgentSession(id = 3, status = "waiting", createdAtMs = 100)),
            AgentListItem(AgentSession(id = 4, status = "success", createdAtMs = 100, finishedAtMs = 200)),
        )
        fun ids(filter: AgentListFilter, now: Long = 0) =
            AgentListOrdering.apply(items, filter, AgentListSort.OldestAdded, now).map { it.session.id }
        assertEq(listOf(1, 2, 3, 4), ids(AgentListFilter.All))
        assertEq(listOf(3), ids(AgentListFilter.NeedsYou))
        assertEq(listOf(1, 3), ids(AgentListFilter.Running))
        assertEq(listOf(2), ids(AgentListFilter.Queued))
        assertEq(listOf(4), ids(AgentListFilter.Recent))
        assertEq(emptyList<Int>(), ids(AgentListFilter.Recent, 200 + 2 * 86_400_000L))
    }

    fun testApprovalNotificationParsesTheServerAlert() {
        val note = AgentApprovalNotification.from(mapOf("aps" to mapOf("category" to "agent-approval"), "approvalId" to approvalA, "sessionId" to "12"))
        assertEq(approvalA, note?.approvalId)
        assertEq("12", note?.sessionId)
        assertEq(null, AgentApprovalNotification.from(mapOf("category" to "ping", "approvalId" to "x")))
        assertEq(null, AgentApprovalNotification.from(mapOf("category" to "agent-approval")))
        assertEq(null, AgentApprovalNotification.from(mapOf("category" to "agent-approval", "approvalId" to "x".repeat(65))))
        expect(AgentApprovalNotification.from(mapOf("alert" to mapOf("category" to "agent-approval", "approvalId" to "a"))) != null)
        expect(AgentApprovalNotification.Action.Approve.requiresAuthentication)
        expect(!AgentApprovalNotification.Action.Deny.requiresAuthentication)
        expect(AgentApprovalNotification.Action.Deny.isDestructive)
        expect(AgentApprovalNotification.Action.Open.opensApp)
    }

    fun testApproveFromTheNotificationAnswersInPlace() {
        val channel = FakeControlChannel()
        channel.reply("agentops.prepare", confirmationJson("approve"))
        val queue = ApprovalsQueue(AgentOpsClient(channel))
        expect(AgentApprovalNotification(approvalA).perform(AgentApprovalNotification.Action.Approve, queue))
        assertEq("allow", channel.params("agentops.answer").str("decision"))
        assertEq("once", channel.params("agentops.answer").str("lifetime"))
        val failing = FakeControlChannel()
        failing.failures["agentops.prepare"] = LocalControlException("not_connected")
        expect(!AgentApprovalNotification(approvalA).perform(AgentApprovalNotification.Action.Deny, ApprovalsQueue(AgentOpsClient(failing))))
    }
}
