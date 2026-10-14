import FMBase
import FMControl
import Foundation
import XCTest

@testable import FMAgents

final class RemoteAgentOpsClientTests: XCTestCase {
    @MainActor func testApprovalsAreParsedAndJunkIdsDropped() async throws {
        let channel = FakeAgentOpsChannel()
        channel.reply(
            "agentops.approvals",
            [
                "approvals": [
                    approvalJSON(approvalA, session: 3, command: "rm -rf build\nsecond line"),
                    .object(["id": "../nope", "sessionId": 1]),
                ]
            ])
        let client = AgentOpsClient(channel: channel)
        let list = try await client.approvals()
        XCTAssertEqual(list.count, 1)
        XCTAssertEqual(list[0].sessionId, 3)
        XCTAssertEqual(list[0].kind, .command)
        XCTAssertEqual(list[0].headline, "rm -rf build")
        XCTAssertEqual(list[0].lifetimes, ApprovalLifetime.allCases)
        XCTAssertEqual(list[0].subtitle, "Fix the build · me/app")
    }

    @MainActor func testPrepareAnswerSendsActionAndLifetime() async throws {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.prepare", confirmationJSON("approve"))
        let clock = FixedClock(epochMillis: 5_000)
        let client = AgentOpsClient(channel: channel, clock: clock)
        let confirmation = try await client.prepareAnswer(approvalA, allow: true, lifetime: .session)
        XCTAssertEqual(channel.params("agentops.prepare")?.str("action"), "approve")
        XCTAssertEqual(channel.params("agentops.prepare")?.str("lifetime"), "session")
        XCTAssertEqual(confirmation.issuedAtMs, 5_000)
        XCTAssertFalse(confirmation.isExpired(nowMs: 64_999))
        XCTAssertTrue(confirmation.isExpired(nowMs: 65_000))
        try await client.answer(approvalA, allow: false, token: "tok")
        let answer = try XCTUnwrap(channel.params("agentops.answer"))
        XCTAssertEqual(answer.str("decision"), "deny")
        XCTAssertNil(answer["lifetime"], "a denial carries no lifetime")
    }

    @MainActor func testPrepareWithoutTokenFails() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.prepare", ["action": "approve"])
        let client = AgentOpsClient(channel: channel)
        do {
            _ = try await client.prepareAnswer(approvalA, allow: true)
            XCTFail("expected a failure")
        } catch {
            XCTAssertEqual(AgentOpsClient.errorCode(error), "confirmation_required")
        }
    }

    @MainActor func testBulkCanonicalisesIds() async throws {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.prepare", confirmationJSON("stop"))
        channel.reply("agentops.bulk", ["action": "stop", "done": [1, 3], "skipped": [2]])
        let client = AgentOpsClient(channel: channel)
        _ = try await client.prepareBulk(.stop, ids: [3, 1, 3, -4, 2])
        XCTAssertEqual(channel.params("agentops.prepare")?["ids"], [1, 2, 3])
        let outcome = try await client.bulk(.stop, ids: [2, 1, 3], token: "tok")
        XCTAssertEqual(channel.params("agentops.bulk")?["ids"], [1, 2, 3])
        XCTAssertEqual(outcome.done, [1, 3])
        XCTAssertEqual(outcome.skipped, [2])
        XCTAssertEqual(outcome.message, "Stopped 2 agents; 1 was not in a state to stop.")
    }

    @MainActor func testMergeSendsTypedBranch() async throws {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.prepare", confirmationJSON("merge", typeToConfirm: "agent/fix"))
        channel.reply("agentops.merge", ["id": 7, "branch": "agent/fix", "merged": true])
        let client = AgentOpsClient(channel: channel)
        let confirmation = try await client.prepareMerge(sessionId: 7)
        XCTAssertTrue(confirmation.destructive)
        XCTAssertEqual(confirmation.typeToConfirm, "agent/fix")
        let outcome = try await client.merge(sessionId: 7, token: confirmation.token, typed: "agent/fix")
        XCTAssertEqual(outcome.branch, "agent/fix")
        XCTAssertNil(outcome.stats, "an older desktop sends no stats")
        XCTAssertEqual(outcome.message(), "Merged agent/fix and cleaned up.")
        XCTAssertEqual(channel.params("agentops.merge")?.str("typed"), "agent/fix")
    }

    @MainActor func testMergeReplyCarriesLandedStats() async throws {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.prepare", confirmationJSON("merge", typeToConfirm: "agent/fix"))
        channel.reply(
            "agentops.merge",
            [
                "id": 7, "branch": "agent/fix", "merged": true,
                "stats": [
                    "base": "main", "commit": "0123456789abcdef", "commits": 4, "files": 3, "insertions": 40,
                    "deletions": 2,
                    "velocity": [
                        ["id": "1h", "label": "1 hour", "count": 2, "top": 5, "avg": 0.4],
                        [
                            "id": "7d", "label": "7 days", "count": 30, "top": 41, "avg": 12.6,
                            "series": [2, 0, 5, 9, 4, 6, 4],
                        ],
                    ],
                ],
            ])
        let client = AgentOpsClient(channel: channel)
        let confirmation = try await client.prepareMerge(sessionId: 7)
        let outcome = try await client.merge(sessionId: 7, token: confirmation.token, typed: "agent/fix")
        let stats = try XCTUnwrap(outcome.stats)
        XCTAssertEqual(stats.base, "main")
        XCTAssertEqual(stats.shortCommit, "0123456")
        XCTAssertEqual(stats.commitsText, "4 commits")
        XCTAssertEqual(stats.filesText, "3 files")
        XCTAssertEqual(stats.insertionsText, "+40")
        XCTAssertEqual(stats.deletionsText, "\u{2212}2")
        XCTAssertEqual(stats.summary, "4 commits \u{00B7} 3 files +40 \u{2212}2")
        XCTAssertEqual(stats.velocity.map(\.label), ["1 hour", "7 days"])
        XCTAssertEqual(stats.velocity[0].detail, "top 5 \u{00B7} avg 0.4")
        XCTAssertEqual(stats.velocity[1].detail, "top 41 \u{00B7} avg 13")
        XCTAssertEqual(stats.velocity[0].series, [])
        XCTAssertEqual(stats.velocity[1].series, [2, 0, 5, 9, 4, 6, 4])
        XCTAssertEqual(stats.velocity[1].peak, 9)
        XCTAssertEqual(stats.velocity[0].share, 0.4, accuracy: 0.001)
        XCTAssertEqual(stats.velocity[0].spoken, "1 hour: 2 merges, best 5, average 0.4")
        XCTAssertEqual(stats.insertionShare ?? 0, 40.0 / 42.0, accuracy: 0.001)
        XCTAssertEqual(outcome.message(), "Merged agent/fix and cleaned up. 4 commits \u{00B7} 3 files +40 \u{2212}2.")
    }

    @MainActor func testMergeLeaderboardReadsTheNewestMergeWithoutAToken() async throws {
        let channel = FakeAgentOpsChannel()
        channel.reply(
            "agentops.mergestats",
            [
                "repo": "me/app", "branch": "agent/fix",
                "stats": [
                    "base": "main", "commit": "0123456789abcdef", "commits": 2, "files": 1, "insertions": 3,
                    "deletions": 1,
                    "velocity": [["id": "24h", "label": "24 hours", "count": 4, "top": 9, "avg": 2.5]],
                ],
            ])
        let client = AgentOpsClient(channel: channel)
        let board = try await client.mergeStats()
        XCTAssertEqual(channel.methods(), ["agentops.mergestats"])
        XCTAssertEqual(channel.params("agentops.mergestats"), [:])
        XCTAssertEqual(board.repo, "me/app")
        XCTAssertEqual(board.branch, "agent/fix")
        XCTAssertEqual(board.stats?.summary, "2 commits \u{00B7} 1 file +3 \u{2212}1")
        XCTAssertEqual(board.subtitle, "me/app \u{00B7} newest merge: agent/fix \u{00B7} into main")
        _ = try await client.mergeStats(sessionId: 7)
        XCTAssertEqual(channel.params("agentops.mergestats")?["id"], .int(7))
    }

    @MainActor func testMergeLeaderboardOnAnOldDesktopSaysToUpdate() async {
        let channel = FakeAgentOpsChannel()
        channel.failures["agentops.mergestats"] = AgentOpsError("unsupported")
        let model = MergeLeaderboardModel(client: AgentOpsClient(channel: channel))
        await model.refresh()
        XCTAssertTrue(model.loaded)
        XCTAssertNil(model.board.stats)
        XCTAssertTrue(model.errorText.contains("Update ForkMesh"))
    }

    func testMergeStatsWithoutALandingHaveNoSummary() {
        let stats = MergeStats(json: ["stats": ["base": "main", "files": 0, "velocity": []]])
        XCTAssertNotNil(stats)
        XCTAssertFalse(stats?.hasLanding ?? true)
        XCTAssertEqual(stats?.summary, "")
        XCTAssertEqual(stats?.commitsText, "")
        XCTAssertNil(stats?.insertionShare)
        XCTAssertEqual(MergeStats(json: ["stats": "x"]), nil)
        XCTAssertEqual(MergeVelocityWindow(id: "1h", label: "1 hour", count: 3, top: 1, avg: 0).detail, "top 3 \u{00B7} avg 0")
    }

    @MainActor func testWorktreesUsageAndGrantsParse() async throws {
        let channel = FakeAgentOpsChannel()
        channel.reply(
            "agentops.worktrees",
            [
                "repo": "me/app", "base": "main",
                "worktrees": [
                    ["name": "app", "branch": "main", "isMain": true],
                    ["name": "agent-7", "branch": "agent/fix", "ahead": 2, "behind": 1, "sessionId": 7],
                ],
            ])
        channel.reply(
            "agentops.usage",
            [
                "providers": [
                    [
                        "id": "claude-code", "label": "Claude Code",
                        "windows": [
                            ["id": "5h", "label": "5-hour", "usedPercent": 100, "state": "exhausted", "resetAtMs": 90_000],
                            ["id": "weekly", "label": "Weekly", "usedPercent": -1, "state": "unknown"],
                        ],
                        "accounts": [
                            ["id": "a", "label": "Work", "active": true, "signedIn": true, "identity": "me@example.com"],
                            [
                                "id": "b", "label": "Home", "active": false,
                                "windows": [["id": "weekly", "label": "Weekly", "usedPercent": 40, "state": "ok"]],
                            ],
                        ],
                        "credits": ["used": 3.5, "limit": 20.0, "currency": "USD"],
                    ]
                ],
                "spend": ["claudeUsd": 1.5], "atMs": 1_000,
            ])
        channel.reply(
            "agentops.grants",
            ["grants": [["id": "g", "capability": "file.write", "kind": "file", "resource": "a.txt", "decision": "allow"]]])
        let client = AgentOpsClient(channel: channel)
        let worktrees = try await client.worktrees(owner: "me", name: "app")
        XCTAssertEqual(channel.params("agentops.worktrees")?.str("owner"), "me")
        XCTAssertEqual(worktrees.worktrees.count, 2)
        XCTAssertEqual(worktrees.worktrees[0].syncLabel, "Main checkout")
        XCTAssertEqual(worktrees.worktrees[1].syncLabel, "2 ahead, 1 behind")
        XCTAssertEqual(worktrees.worktrees[1].sessionId, 7)
        XCTAssertNil(worktrees.worktrees[0].sessionId)

        let usage = try await client.usage()
        let claude = try XCTUnwrap(usage.providers.first)
        XCTAssertEqual(claude.worstState, .exhausted)
        XCTAssertEqual(claude.windows[0].remainingPercent, 0)
        XCTAssertNil(claude.windows[1].usedPercent)
        XCTAssertEqual(claude.windows[0].resetLabel(nowMs: 30_000), "resets in 1m")
        let hour: Int64 = 3_600_000
        let weekly = UsageWindowMeter(
            id: "weekly", label: "Weekly", usedPercent: 59, resetAtMs: 168 * hour, state: .ok)
        XCTAssertEqual(weekly.windowMs, 168 * hour)
        XCTAssertEqual(weekly.segments, 7)
        XCTAssertEqual(weekly.periodLabel(nowMs: 30 * hour), "Day 2 of 7")
        XCTAssertEqual(weekly.paceLabel(nowMs: 30 * hour), "41% over pace")
        let session = UsageWindowMeter(
            id: "5h", label: "5-hour", usedPercent: 10, resetAtMs: 5 * hour, state: .ok)
        XCTAssertEqual(session.segments, 5)
        XCTAssertEqual(session.periodLabel(nowMs: 2 * hour + 1), "Hour 3 of 5")
        XCTAssertEqual(session.paceLabel(nowMs: 2 * hour), "30% under pace")
        let codex = UsageWindowMeter(json: [
            "id": "weekly", "label": "Weekly", "usedPercent": 50, "resetAtMs": 36_000_000,
            "windowMs": 72_000_000, "state": "ok",
        ])
        XCTAssertEqual(codex.windowMs, 20 * hour)
        XCTAssertEqual(codex.paceLabel(nowMs: 0), "on pace")
        XCTAssertEqual(UsageWindowMeter(id: "w", label: "Credits", usedPercent: 1, state: .ok).paceLabel(nowMs: 0), "")
        XCTAssertEqual(claude.activeAccount?.label, "Work")
        XCTAssertEqual(claude.accounts[0].signedIn, true)
        XCTAssertEqual(claude.accounts[0].identity, "me@example.com")
        XCTAssertNil(claude.accounts[1].signedIn, "an older desktop does not say")
        XCTAssertEqual(claude.accounts[1].windows.first?.usedPercent, 40)
        XCTAssertTrue(claude.accounts[0].windows.isEmpty)
        XCTAssertEqual(claude.credits?.label, "3.50 of 20.00 USD")
        XCTAssertEqual(claude.credits?.usedPercent, 18)
        XCTAssertEqual(claude.credits?.state, .ok)
        XCTAssertEqual(claude.credits?.caption, "$3.50 of $20.00 (18%)")
        XCTAssertEqual(claude.sessionWindow?.id, "5h")
        XCTAssertEqual(claude.weeklyWindow?.id, "weekly")
        XCTAssertEqual(UsageCredits(used: 17, limit: 20, currency: "EUR").state, .high)
        XCTAssertEqual(UsageCredits(used: 25, limit: 20, currency: "GBP").usedPercent, 100)
        XCTAssertEqual(UsageCredits(used: 25, limit: 20, currency: "GBP").state, .exhausted)
        XCTAssertEqual(UsageCredits(used: 4, limit: nil, currency: "").caption, "$4.00 used, no monthly limit")
        XCTAssertNil(UsageCredits(used: 4, limit: nil, currency: "").usedPercent)
        XCTAssertEqual(UsageCredits(used: nil, limit: nil, currency: "").state, .unknown)
        XCTAssertEqual(UsageCredits.money(2, currency: "cad"), "2.00 CAD")
        let codexLike = UsageProviderMeter(
            id: "codex", label: "Codex",
            windows: [
                UsageWindowMeter(id: "5h limit", label: "5h limit", usedPercent: 3, state: .ok),
                UsageWindowMeter(id: "weekly limit", label: "Weekly limit", usedPercent: 9, state: .ok),
            ])
        XCTAssertEqual(codexLike.weeklyWindow?.id, "weekly limit")
        XCTAssertEqual(usage.totalSpendUsd, 1.5)
        let reminders = usage.reminders(nowMs: 10_000)
        XCTAssertEqual(reminders.map(\.id), ["usage.claude-code.5h"])
        XCTAssertEqual(reminders.first?.fireAtMs, 90_000)
        XCTAssertTrue(usage.reminders(nowMs: 100_000).isEmpty, "no reminder for a reset in the past")
        XCTAssertEqual(usage.activeSession?.provider.id, "claude-code", "an older desktop names no active provider")
        XCTAssertEqual(usage.activeSession?.window.id, "5h")

        let codexWindow = UsageWindowMeter(id: "5h", label: "5-hour", usedPercent: 12, state: .ok)
        let claudeWindow = UsageWindowMeter(id: "5h", label: "5-hour", usedPercent: 70, state: .ok)
        let both = UsageSummary(
            providers: [
                UsageProviderMeter(id: "claude-code", label: "Claude Code", windows: [claudeWindow]),
                UsageProviderMeter(id: "codex", label: "Codex", windows: [codexWindow]),
            ],
            activeProvider: "codex")
        XCTAssertEqual(both.activeSession?.provider.id, "codex")
        XCTAssertEqual(both.activeSession?.window.usedPercent, 12)
        let unmetered = UsageSummary(
            providers: [
                UsageProviderMeter(id: "codex", label: "Codex"),
                UsageProviderMeter(id: "claude-code", label: "Claude Code", windows: [claudeWindow]),
            ],
            activeProvider: "codex")
        XCTAssertEqual(unmetered.activeSession?.provider.id, "claude-code")
        XCTAssertNil(UsageSummary().activeSession)

        let grants = try await client.grants()
        XCTAssertEqual(grants.first?.kind, .file)
        XCTAssertTrue(grants.first?.allows ?? false)
    }

    @MainActor func testAccountActionsAnswerTheNewUsage() async throws {
        let channel = FakeAgentOpsChannel()
        let summary: JSONObject = [
            "providers": [
                [
                    "id": "codex", "label": "Codex", "windows": [],
                    "accounts": [
                        ["id": "default", "label": "Codex", "active": false],
                        ["id": "n1", "label": "Side", "active": true, "signedIn": false],
                    ],
                ]
            ],
        ]
        var added = summary
        added["account"] = "n1"
        channel.reply("agentops.accounts.select", summary)
        channel.reply("agentops.accounts.rename", summary)
        channel.reply("agentops.accounts.add", added)
        let client = AgentOpsClient(channel: channel)
        let selected = try await client.selectAccount(provider: "codex", account: "n1")
        XCTAssertEqual(channel.params("agentops.accounts.select")?.str("provider"), "codex")
        XCTAssertEqual(channel.params("agentops.accounts.select")?.str("account"), "n1")
        XCTAssertEqual(selected.providers.first?.activeAccount?.id, "n1")
        _ = try await client.renameAccount(provider: "codex", account: "n1", label: "Side 2")
        XCTAssertEqual(channel.params("agentops.accounts.rename")?.str("label"), "Side 2")
        let (_, account) = try await client.addAccount(provider: "codex", label: "Side")
        XCTAssertEqual(account, "n1")
        XCTAssertEqual(channel.params("agentops.accounts.add")?.str("label"), "Side")
    }

    @MainActor func testBellsAreFilteredToChanged() async {
        let channel = FakeAgentOpsChannel()
        let client = AgentOpsClient(channel: channel)
        var seen: [Int] = []
        let tap = client.onChanged { data in seen.append(data.int("count")) }
        channel.emit("agentops.changed", ["count": 2])
        channel.emit("agentops.other", ["count": 9])
        channel.emit("agents.changed", ["count": 5])
        XCTAssertEqual(seen, [2])
        client.removeTap(tap)
        XCTAssertEqual(channel.tapCount, 0)
    }

    @MainActor func testErrorCodesAndText() async {
        XCTAssertEqual(AgentOpsClient.errorCode(FakeControlError(code: "agent_running")), "agent_running")
        XCTAssertTrue(AgentOpsClient.isUnsupported(FakeControlError(code: "unsupported")))
        XCTAssertFalse(AgentOpsClient.isUnsupported(FakeControlError(code: "timeout")))
        XCTAssertEqual(
            AgentOpsClient.describe(FakeControlError(code: "agent_running")),
            "The agent is still working on that branch. Stop it first.")
        XCTAssertEqual(
            AgentOpsClient.describe(code: "confirmation_expired"), ControlErrorText.describe(code: "confirmation_expired"))
        XCTAssertNil(AgentOpsClient.errorCode(NSError(domain: "x", code: 1)))
        XCTAssertEqual(AgentOpsClient.canonical([5, 5, 0, 2]), [2, 5])
    }

    @MainActor func testUsageFormat() async {
        XCTAssertEqual(UsageFormat.remaining(ms: 1), "1m")
        XCTAssertEqual(UsageFormat.remaining(ms: 90 * 60_000), "1h 30m")
        XCTAssertEqual(UsageFormat.remaining(ms: 3 * 3_600_000), "3h")
        XCTAssertEqual(UsageFormat.remaining(ms: 50 * 3_600_000), "2d 2h")
        XCTAssertEqual(UsageFormat.usd(12.5), "$12.50")
    }
}
