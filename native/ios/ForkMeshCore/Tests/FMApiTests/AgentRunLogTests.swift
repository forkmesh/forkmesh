import XCTest
import FMBase
@testable import FMApi

final class AgentRunLogTests: XCTestCase {
    private func log(_ json: JSONObject, pull: String = "") -> AgentRunLog {
        AgentRunLog(AgentTaskChain(AgentLaunchSession(json: json), pullStatus: pull), repo: "jett/site")
    }

    private func texts(_ log: AgentRunLog) -> [String] { log.lines.map(\.text) }

    func testAQueuedRunShowsTheCommandThePromptAndTheWait() {
        let queued = log([
            "id": "s", "status": "queued", "targetNode": "fmnext-run-1", "provider": "claude",
            "createdBy": "jett", "createdAt": 1_700_000_000, "createPr": true,
            "history": [["role": "user", "text": "Fix the header\nand the footer"]],
        ])
        XCTAssertEqual(texts(queued), [
            "$ forkmesh agent run --repo jett/site --provider claude --on fmnext-run-1 --pr",
            "[1/5] Prompt · Sent by @jett",
            "› Fix the header",
            "  and the footer",
            "[2/5] Mirror · Waiting for fmnext-run-1 to pick it up",
        ])
        XCTAssertEqual(queued.lines[0].tone, .command)
        XCTAssertEqual(queued.lines[0].atMs, 1_700_000_000_000)
        XCTAssertEqual(queued.lines[4].tone, .active)
        XCTAssertTrue(queued.isLive)
        XCTAssertFalse(queued.isStreaming)
        XCTAssertEqual(queued.status, "live")
        XCTAssertEqual(queued.host, "claude@fmnext-run-1")
    }

    func testARunningRunStreamsTheLiveTranscript() {
        let running = log([
            "id": "s", "status": "running", "targetNode": "n1", "localAgentId": 4,
            "history": [["role": "user", "text": "Go"]],
            "agentInfo": ["branchName": "agent/go", "baseBranch": "main"],
            "live": ["text": "Looking around.\n\n▸ Bash: ls -la\n\nDone looking.", "at": 1_700_000_100],
        ])
        XCTAssertTrue(running.isStreaming)
        XCTAssertEqual(Array(texts(running).suffix(5)), [
            "[2/5] Mirror · Ran on n1",
            "[3/5] Branch · agent/go from main",
            "Looking around.",
            "▸ Bash: ls -la",
            "Done looking.",
        ])
        XCTAssertEqual(running.lines.last { $0.text.hasPrefix("▸") }?.tone, .tool)
        XCTAssertEqual(running.lines.first { $0.text.hasPrefix("[3/5]") }?.tone, .active)
    }

    func testARunningRunWithNoOutputSaysItIsWaiting() {
        let running = log(["id": "s", "status": "running", "localAgentId": 4])
        XCTAssertEqual(texts(running).last, "# waiting for the agent's first output…")
    }

    func testLiveTextIsIgnoredOnceTheRunEnds() {
        let done = log([
            "id": "s", "status": "completed", "localAgentId": 4, "createdAt": 1_000, "completedAt": 1_192,
            "history": [["role": "user", "text": "Go"], ["role": "assistant", "text": "▸ Edit: a.txt\n\nAll set.", "at": 1_192]],
            "live": ["text": "stale"],
            "agentInfo": ["branchName": "agent/go", "prNumber": 12],
        ], pull: "open")
        XCTAssertFalse(done.isStreaming)
        XCTAssertFalse(texts(done).contains("stale"))
        XCTAssertEqual(Array(texts(done).suffix(5)), [
            "▸ Edit: a.txt",
            "All set.",
            "✓ agent finished in 3m 12s",
            "[4/5] Pull request · #12",
            "[5/5] Merge · Open — review and merge #12",
        ])
        XCTAssertEqual(done.status, "exit 0")
        XCTAssertEqual(done.elapsed(nowMs: 9_999_999_999), "3m 12s")
    }

    func testAFailedRunEndsOnItsError() {
        let failed = log(["id": "s", "status": "failed", "targetNode": "n1", "lastError": "Claude is not signed in"])
        XCTAssertEqual(failed.lines.last?.text, "✗ Claude is not signed in")
        XCTAssertEqual(failed.lines.last?.tone, .failure)
        XCTAssertEqual(failed.status, "exit 1")
        XCTAssertFalse(failed.isLive)
    }

    func testAMergedRunReadsMerged() {
        let merged = log([
            "id": "s", "status": "completed", "localAgentId": 4,
            "agentInfo": ["branchName": "agent/go", "prNumber": 3],
        ], pull: "merged")
        XCTAssertEqual(merged.status, "merged")
        XCTAssertEqual(merged.lines.last?.text, "[5/5] Merge · Merged")
        XCTAssertEqual(merged.lines.last?.tone, .step)
    }

    func testFollowUpsAndLongOutputAreBounded() {
        let body = (1...(AgentRunLog.maxOutputLines + 5)).map { "line \($0)" }.joined(separator: "\n")
        let long = log([
            "id": "s", "status": "completed", "localAgentId": 4,
            "history": [
                ["role": "user", "text": "Go"],
                ["role": "assistant", "text": "first"],
                ["role": "user", "author": "ana", "text": "Now more"],
                ["role": "assistant", "text": .string(body)],
            ],
        ])
        XCTAssertTrue(texts(long).contains("… 7 earlier lines"))
        XCTAssertFalse(texts(long).contains("› @ana Now more"))
        XCTAssertTrue(texts(long).contains("line \(AgentRunLog.maxOutputLines + 5)"))
        let short = log([
            "id": "s", "status": "completed",
            "history": [["role": "user", "text": "Go"], ["role": "user", "author": "ana", "text": "Now more"]],
        ])
        XCTAssertTrue(texts(short).contains("› @ana Now more"))
    }

    func testDurations() {
        XCTAssertEqual(AgentRunLog.duration(from: 1_000, to: 13_000), "12s")
        XCTAssertEqual(AgentRunLog.duration(from: 1_000, to: 1_000 + 3_725_000), "1h 02m")
        XCTAssertEqual(AgentRunLog.duration(from: 0, to: 5_000), "")
        XCTAssertEqual(AgentRunLog.duration(from: 5_000, to: 1_000), "")
    }
}
