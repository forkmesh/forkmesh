import FMApi
import FMBase
import FMChat
import FMControl
import XCTest

@testable import FMCode

@MainActor
final class FakeMergeQueueChannel: DesktopControlCalling {
    var requests: [(String, JSONObject)] = []
    var replies: [String: JSONObject] = [:]
    var failures: [String: String] = [:]
    var taps: [UUID: @MainActor (String, JSONObject) -> Void] = [:]

    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        requests.append((method, params))
        if let code = failures[method] { throw LocalControlException(code) }
        return replies[method] ?? [:]
    }

    func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID {
        let id = UUID()
        taps[id] = tap
        return id
    }

    func removeEventTap(_ id: UUID) { taps.removeValue(forKey: id) }

    func emit(_ event: String, _ data: JSONObject) { for tap in taps.values { tap(event, data) } }
}

final class CollabMergeRulesTests: XCTestCase {
    let plan = PullMergePlan(
        number: 3, expectedBaseOid: String(repeating: "a", count: 40), expectedHeadOid: String(repeating: "b", count: 40),
        expectedPullsOid: String(repeating: "c", count: 40))

    func inputs(status: String = "open", events: [ReviewEvent] = [], owner: Bool? = true, plan: PullMergePlan?, loaded: Bool = true) -> PullMergeInputs {
        PullMergeInputs(status: status, author: "author", events: events, isOwner: owner, plan: plan, planLoaded: loaded)
    }

    func testStatesFollowTheDesktopBlockers() {
        XCTAssertEqual(PullMergeRules.state(inputs(status: "merged", plan: nil)), .merged)
        XCTAssertEqual(PullMergeRules.state(inputs(status: "closed", plan: nil)), .closed)
        XCTAssertEqual(PullMergeRules.state(inputs(owner: nil, plan: nil)), .checking)
        XCTAssertEqual(PullMergeRules.state(inputs(owner: false, plan: plan)), .notOwner)
        XCTAssertEqual(PullMergeRules.state(inputs(plan: nil, loaded: false)), .checking)
        XCTAssertEqual(PullMergeRules.state(inputs(plan: nil)), .notPinned)
        var conflicted = plan
        conflicted.clean = false
        conflicted.conflicts = ["a.swift"]
        XCTAssertEqual(PullMergeRules.state(inputs(plan: conflicted)), .conflicts(["a.swift"]))
        XCTAssertEqual(PullMergeRules.state(inputs(plan: plan)), .needsIndependentReview)
        XCTAssertTrue(PullMergeRules.state(inputs(plan: plan)).needsWaiver)
        let approved = [ReviewEvent(type: "review", author: "x", state: "approve")]
        XCTAssertEqual(PullMergeRules.state(inputs(events: approved, plan: plan)), .ready)
        let blocked = approved + [ReviewEvent(type: "review", author: "y", authorName: "yan", state: "request-changes")]
        XCTAssertEqual(PullMergeRules.state(inputs(events: blocked, plan: plan)), .changesRequested(["yan"]))
        XCTAssertFalse(PullMergeRules.state(inputs(events: blocked, plan: plan)).canMerge)
        XCTAssertEqual(
            PullMergeRules.headline(.conflicts(["a", "b"]), base: "main"), "2 files conflict with main: a, b.")
        XCTAssertEqual(PullMergeRules.blockers(.ready, base: "main"), [])
        XCTAssertEqual(PullMergeMethod.offered, [.merge])
    }

    func testBranchStateLabels() {
        XCTAssertEqual(PullBranchState(ahead: 2, behind: 1).label, "2 ahead · 1 behind")
        XCTAssertEqual(PullBranchState().label, "Up to date")
        XCTAssertEqual(PullBranchState(conflicts: ["x"], clean: false).label, "1 conflict")
        XCTAssertEqual(PullBranchState(conflicts: [], clean: false).tone, .danger)
    }
}

final class CollabConflictTests: XCTestCase {
    let text = "top\n<<<<<<< HEAD\nours 1\nours 2\n||||||| base\nbase\n=======\ntheirs\n>>>>>>> feature\nmid\n<<<<<<< HEAD\nA\n=======\nB\n>>>>>>> feature\nend\n"

    func testParsesHunksWithDiff3Bases() throws {
        let file = try ConflictResolution.parse(path: "f", text: text).get()
        XCTAssertEqual(file.hunks.count, 2)
        XCTAssertEqual(file.hunks[0].ours, ["ours 1", "ours 2"])
        XCTAssertEqual(file.hunks[0].base, ["base"])
        XCTAssertEqual(file.hunks[0].theirs, ["theirs"])
        XCTAssertEqual(file.hunks[0].oursLabel, "HEAD")
        XCTAssertEqual(file.hunks[1].theirsLabel, "feature")
        XCTAssertNil(file.hunks[1].base)
        XCTAssertEqual(file.hunks[0].startLine, 2)
    }

    func testResolvesOnlyWhenEveryHunkHasAChoice() throws {
        let file = try ConflictResolution.parse(path: "f", text: text).get()
        XCTAssertNil(ConflictResolution.resolve(file, [0: .ours]))
        XCTAssertEqual(ConflictResolution.unresolved(file, [0: .ours]), [1])
        XCTAssertEqual(ConflictResolution.resolve(file, [0: .theirs, 1: .both]), "top\ntheirs\nmid\nA\nB\nend\n")
        XCTAssertEqual(ConflictResolution.resolve(file, [0: .ours, 1: .bothTheirsFirst]), "top\nours 1\nours 2\nmid\nB\nA\nend\n")
    }

    func testMalformedMarkersFail() {
        XCTAssertThrowsError(try ConflictResolution.parse(path: "f", text: "<<<<<<< a\nx\n=======\ny\n").get())
        XCTAssertThrowsError(try ConflictResolution.parse(path: "f", text: "<<<<<<< a\n<<<<<<< b\n").get())
        XCTAssertFalse(try ConflictResolution.parse(path: "f", text: "plain").get().hasConflicts)
        XCTAssertTrue(ConflictResolution.agentPrompt(branch: "b", base: "main", files: ["x"]).contains("- x"))
    }

    func testConflictFixPromptNamesTheBase() {
        XCTAssertEqual(
            ConflictResolution.sessionPrompt(base: "main"),
            "Merge `main` into your branch and resolve all merge conflicts. Make sure the build and tests still pass, then commit.")
        let listed = ConflictResolution.sessionPrompt(base: "", files: ["a.swift", "b.swift"])
        XCTAssertTrue(listed.hasPrefix("Merge the base branch into your branch"))
        XCTAssertTrue(listed.hasSuffix("Conflicting files:\n- a.swift\n- b.swift"))
    }
}

final class CollabMergeQueueTests: XCTestCase {
    let listReply: JSONObject = [
        "repos": [
            [
                "owner": "o", "name": "r", "enabled": true, "paused": false, "open": true,
                "entries": [
                    ["number": 4, "title": "A", "state": "merging", "detail": "Merging", "queuedAtMs": 1],
                    ["number": 9, "state": "blocked", "detail": "Waiting for an independent peer approval"],
                    ["number": 0, "state": "queued"],
                ],
            ],
            ["owner": "o", "name": "off", "enabled": false],
            ["owner": "", "name": "bad"],
        ],
    ]

    @MainActor func testListParsesAndBoundsRepos() async throws {
        let channel = FakeMergeQueueChannel()
        channel.replies["mergequeue.list"] = listReply
        let remote = MergeQueueRemote(channel: channel)
        let repos = try await remote.list(owner: "o", name: "r")
        XCTAssertEqual(channel.requests.first?.0, "mergequeue.list")
        XCTAssertEqual(channel.requests.first?.1.str("owner"), "o")
        XCTAssertEqual(repos.count, 2)
        XCTAssertEqual(repos[0].entries.map(\.number), [4, 9])
        XCTAssertEqual(repos[0].summary, "2 queued · 1 in progress · 1 blocked")
        XCTAssertEqual(repos[1].summary, "Off")
        XCTAssertEqual(MergeQueueRules.actions(for: repos[1]), [.enable])
        XCTAssertEqual(MergeQueueRules.actions(for: repos[0]), [.pause, .process, .clear, .disable])
        XCTAssertTrue(MergeQueueRules.entryActions(repos[0].entries[0], in: repos[0]).isEmpty)
        XCTAssertEqual(MergeQueueRules.entryActions(repos[0].entries[1], in: repos[0]).map(\.0), [.move, .remove])
        XCTAssertFalse(MergeQueueRules.canEnqueue(pullStatus: "open", repo: repos[0], number: 4))
        XCTAssertTrue(MergeQueueRules.canEnqueue(pullStatus: "open", repo: repos[0], number: 5))
        XCTAssertFalse(MergeQueueRules.canEnqueue(pullStatus: "merged", repo: repos[0], number: 5))
    }

    @MainActor func testEveryChangeIsTokenConfirmed() async throws {
        let channel = FakeMergeQueueChannel()
        channel.replies["mergequeue.prepare"] = ["token": "T1", "summary": "Move #9 up", "expiresInMs": 60000]
        channel.replies["mergequeue.run"] = ["message": "Moved", "queue": listReply.maps("repos")[0].jsonValue]
        let remote = MergeQueueRemote(channel: channel)
        let request = MergeQueueRequest(owner: "o", name: "r", action: .move, number: 9, delta: -1)
        let result = try await remote.perform(request)
        XCTAssertEqual(channel.requests.map(\.0), ["mergequeue.prepare", "mergequeue.run"])
        XCTAssertEqual(channel.requests[0].1.int("delta"), -1)
        XCTAssertNil(channel.requests[0].1["token"])
        XCTAssertEqual(channel.requests[1].1.str("token"), "T1")
        XCTAssertEqual(channel.requests[1].1.str("action"), "move")
        XCTAssertEqual(result.message, "Moved")
        XCTAssertEqual(result.repo?.entries.count, 2)
        XCTAssertNil(MergeQueueRequest(owner: "o", name: "r", action: .pause).params["number"])
    }

    @MainActor func testRefusalsReadAsSentences() async {
        let channel = FakeMergeQueueChannel()
        channel.failures["mergequeue.prepare"] = "unsupported"
        let remote = MergeQueueRemote(channel: channel)
        do {
            _ = try await remote.perform(MergeQueueRequest(owner: "o", name: "r", action: .process))
            XCTFail("expected a refusal")
        } catch {
            XCTAssertTrue(MergeQueueRemote.message(for: error).contains("Update ForkMesh"))
        }
        channel.failures = [:]
        channel.replies["mergequeue.prepare"] = [:]
        do {
            _ = try await remote.prepare(MergeQueueRequest(owner: "o", name: "r", action: .process))
            XCTFail("expected a missing token")
        } catch {
            XCTAssertEqual(MergeQueueRemote.message(for: error), "The confirmation expired. Try again.")
        }
        XCTAssertEqual(MergeQueueRules.errorMessage("repo_not_open").prefix(4), "Open")
    }

    @MainActor func testChangeBellsCarryTheRepository() {
        let channel = FakeMergeQueueChannel()
        let remote = MergeQueueRemote(channel: channel)
        var seen: [String] = []
        let id = remote.onChange { owner, name in seen.append("\(owner)/\(name)") }
        channel.emit("mergequeue.changed", ["owner": "o", "name": "r"])
        channel.emit("agentops.changed", ["owner": "x", "name": "y"])
        XCTAssertEqual(seen, ["o/r"])
        remote.removeTap(id)
        XCTAssertTrue(channel.taps.isEmpty)
    }
}

extension JSONObject {
    var jsonValue: JSONValue { .object(self) }
}
