import XCTest

@testable import FMSurfaces

final class AgentVoiceTests: XCTestCase {
    private let pool = [
        AgentVoiceCandidate(
            id: "desktop:754", number: 754, title: "Fix login", repo: "forkmesh/app", statusLabel: "Done",
            updatedAtMs: 500),
        AgentVoiceCandidate(
            id: "desktop:75", number: 75, title: "Siri merge", repo: "forkmesh/app", statusLabel: "Running",
            isActive: true, updatedAtMs: 100),
        AgentVoiceCandidate(id: "node:abc", title: "Docs pass", repo: "forkmesh/www", statusLabel: "Queued",
                            isActive: true, updatedAtMs: 900),
    ]

    func testReadsSpokenNumbers() {
        XCTAssertEqual(AgentVoice.number(in: "754"), 754)
        XCTAssertEqual(AgentVoice.number(in: "Agent 754"), 754)
        XCTAssertEqual(AgentVoice.number(in: "agent #754"), 754)
        XCTAssertEqual(AgentVoice.number(in: "agent number 1,204"), 1204)
        XCTAssertEqual(AgentVoice.number(in: "seven five four"), 754)
        XCTAssertEqual(AgentVoice.number(in: "agent seven oh two"), 702)
        XCTAssertNil(AgentVoice.number(in: "fix for login"))
        XCTAssertNil(AgentVoice.number(in: "agent"))
        XCTAssertNil(AgentVoice.number(in: "0"))
    }

    func testRowIdsRoundTrip() {
        XCTAssertEqual(AgentVoice.rowId(number: 754), "desktop:754")
        XCTAssertEqual(AgentVoice.number(rowId: "desktop:754"), 754)
        XCTAssertEqual(AgentVoice.number(rowId: "754"), 754)
        XCTAssertNil(AgentVoice.number(rowId: "node:abc"))
        XCTAssertNil(AgentVoice.number(rowId: "desktop:0"))
    }

    func testNamesDesktopAgentsByNumber() {
        XCTAssertEqual(pool[0].name, "Agent 754")
        XCTAssertEqual(pool[2].name, "Docs pass")
        XCTAssertEqual(AgentVoice.name(number: 0, title: " "), "Agent run")
        XCTAssertEqual(pool[0].detail, "Fix login · Done · forkmesh/app")
    }

    func testMatchesByNumberBeforeText() {
        XCTAssertEqual(AgentVoice.match(pool, query: "agent 754").map(\.id), ["desktop:754"])
        XCTAssertEqual(AgentVoice.match(pool, query: "75").map(\.id), ["desktop:75"])
        XCTAssertEqual(AgentVoice.match(pool, query: "login").map(\.id), ["desktop:754"])
        XCTAssertEqual(AgentVoice.match(pool, query: "www docs").map(\.id), ["node:abc"])
        XCTAssertEqual(AgentVoice.match(pool, query: "").map(\.id), ["node:abc", "desktop:75", "desktop:754"])
        XCTAssertTrue(AgentVoice.match(pool, query: "agent 999").isEmpty)
    }

    func testSentences() {
        XCTAssertEqual(AgentVoice.status(pool[0]), "Agent 754, Fix login, is done in forkmesh/app.")
        XCTAssertEqual(
            AgentVoice.status(AgentVoiceCandidate(id: "desktop:9", number: 9, isActive: true)), "Agent 9 is running.")
        XCTAssertEqual(
            AgentVoice.changes(files: 3, added: 40, removed: 2, merged: false, base: "main"),
            "It changed 3 files, 40 lines added and 2 removed.")
        XCTAssertEqual(AgentVoice.changes(files: 0, added: 0, removed: 0, merged: true, base: "main"),
                       "It is already merged into main.")
        XCTAssertEqual(AgentVoice.changes(files: 0, added: 0, removed: 0, merged: false, base: ""),
                       "It has no changes yet.")
        XCTAssertEqual(AgentVoice.merged("Agent 754", branch: "agent/754-fix"),
                       "Merged Agent 754 from agent/754-fix and cleaned up.")
        XCTAssertEqual(AgentVoice.merged("Agent 754", branch: "agent/754-fix", files: 3, added: 40, removed: 2),
                       "Merged Agent 754 from agent/754-fix and cleaned up. It changed 3 files, 40 lines added and 2 removed.")
        XCTAssertEqual(AgentVoice.merged("Agent 754", branch: "", files: 0), "Merged Agent 754 and cleaned up.")
        XCTAssertEqual(AgentVoice.stopped([]), "No ForkMesh agents are running.")
        XCTAssertEqual(AgentVoice.stopped(["Agent 1"]), "Stopped Agent 1.")
        XCTAssertEqual(AgentVoice.stopped(["Agent 1", "Agent 2"]), "Stopped 2 agents.")
    }

    func testDeleteAllSentences() {
        XCTAssertEqual(AgentVoice.deleteAllQuestion(count: 0, running: 0, unmerged: 0),
                       "There are no agents on your desktop to delete.")
        XCTAssertEqual(AgentVoice.deleteAllQuestion(count: 1, running: 0, unmerged: 0),
                       "Delete the only agent on your desktop and its worktree?")
        XCTAssertEqual(AgentVoice.deleteAllQuestion(count: 12, running: 1, unmerged: 5),
                       "Delete all 12 agents on your desktop and their worktrees? 1 is running and will be stopped. "
                           + "5 have unmerged branches; that work is lost.")
        XCTAssertEqual(AgentVoice.deleteAllQuestion(count: 3, running: 2, unmerged: 1),
                       "Delete all 3 agents on your desktop and their worktrees? 2 are running and will be stopped. "
                           + "1 has an unmerged branch; that work is lost.")
        XCTAssertEqual(AgentVoice.deletedAll(deleted: 1, failed: 0), "Deleted 1 agent.")
        XCTAssertEqual(AgentVoice.deletedAll(deleted: 10, failed: 2), "Deleted 10 agents. 2 could not be deleted.")
        XCTAssertEqual(AgentVoice.deletedAll(deleted: 4, failed: 1), "Deleted 4 agents. 1 could not be deleted.")
        XCTAssertEqual(AgentVoice.deletedAll(deleted: 0, failed: 1), "The agent could not be deleted.")
        XCTAssertEqual(AgentVoice.deletedAll(deleted: 0, failed: 3), "None of the 3 agents could be deleted.")
    }
}
