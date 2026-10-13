import FMApi
import FMBase
import FMLocalAI
import FMNet
import Foundation
import XCTest
@testable import FMAgents

@MainActor
final class NextTaskAdvisorTests: XCTestCase {
    private final class Script {
        var replies: [String] = []
        var calls: [[LocalChatMessage]] = []
        var temperatures: [Double] = []
        var reason: String?
    }

    private func advisor(
        _ script: Script, prefs: InMemoryKeyValueStore, log: NextTaskTrainingLog? = nil, clock: @escaping () -> Int = { 1000 }
    ) -> NextTaskAdvisor {
        NextTaskAdvisor(
            prefs: prefs, readiness: { script.reason },
            generate: { (messages: [LocalChatMessage], parameters: LocalGenerationParameters) async throws -> String in
                script.calls.append(messages)
                script.temperatures.append(parameters.temperature)
                return script.replies.isEmpty ? "" : script.replies.removeFirst()
            }, log: log, now: clock)
    }

    private let context: @MainActor () async -> NextTaskContext = {
        NextTaskContext(
            repo: "alice/mobile",
            live: [NextTaskWork(title: "Fix login crash", repo: "alice/mobile", status: "Running")],
            issues: [NextTaskIssue(number: 7, title: "Dark mode", labels: ["ui"], priority: 2)])
    }

    func testParseReadsTheThreeLines() {
        let parsed = NextTaskPrompt.parse("""
            **TITLE:** Add dark mode toggle
            TASK: Add a dark mode switch to Settings.
            It follows the system by default.
            WHY: Issue #7 has the most votes.
            """)
        XCTAssertEqual(parsed?.title, "Add dark mode toggle")
        XCTAssertEqual(parsed?.task, "Add a dark mode switch to Settings. It follows the system by default.")
        XCTAssertEqual(parsed?.why, "Issue #7 has the most votes.")
    }

    func testParseFallsBackToLooseText() {
        let parsed = NextTaskPrompt.parse("Write tests for the sync queue. Cover retries.")
        XCTAssertEqual(parsed?.title, "Write tests for the sync queue")
        XCTAssertEqual(parsed?.task, "Write tests for the sync queue. Cover retries.")
        XCTAssertNil(NextTaskPrompt.parse("   \n  "))
    }

    func testParseLessonsKeepsFourBullets() {
        let lessons = NextTaskPrompt.parseLessons("Notes:\n- Likes UI polish\n* Skips refactors\n3. Prefers tests\n- a\n- b")
        XCTAssertEqual(lessons, ["Likes UI polish", "Skips refactors", "Prefers tests", "a"])
    }

    func testRecentWorkComesFromSessionsAndLaunchesNewestFirst() {
        let group = AgentRepoGroup(
            repo: Repository(owner: "alice", name: "mobile"),
            sessions: [AgentSession(id: 1, issueTitle: "Old work", status: "done", finishedAtMs: 10)],
            launched: [
                AgentLaunchSession(sessionId: "a", status: "failed", title: "Broken build", updatedAtMs: 30, lastError: "tests red"),
                AgentLaunchSession(sessionId: "b", status: "completed", title: "old work", updatedAtMs: 5),
            ])
        let work = NextTaskContext.recent([group])
        XCTAssertEqual(work.map(\.title), ["Broken build", "Old work"])
        XCTAssertEqual(work.first?.note, "tests red")
        XCTAssertEqual(work.last?.status, "Done")
    }

    func testIssuesKeepOpenOnesByPriority() {
        let issues = NextTaskContext.issues([
            Issue(number: 1, title: "Low", priority: 0),
            Issue(number: 2, title: "Closed", status: "closed", priority: 5),
            Issue(number: 3, title: "High", priority: 3),
        ])
        XCTAssertEqual(issues.map(\.number), [3, 1])
    }

    func testSuggestionAppearsAndThePromptCarriesTheContext() async {
        let script = Script()
        script.replies = ["TITLE: Dark mode\nTASK: Add dark mode.\nWHY: Top issue."]
        let model = advisor(script, prefs: InMemoryKeyValueStore())
        model.suggestIfNeeded(context)
        XCTAssertEqual(model.phase, .thinking)
        await model.waitForIdle()
        XCTAssertEqual(model.suggestion, NextTaskSuggestion(title: "Dark mode", task: "Add dark mode.", why: "Top issue."))
        let prompt = script.calls.first?.last?.content ?? ""
        XCTAssertTrue(prompt.contains("Repository: alice/mobile"))
        XCTAssertTrue(prompt.contains("- Fix login crash [alice/mobile] (Running)"))
        XCTAssertTrue(prompt.contains("- #7 Dark mode (ui; priority 2)"))
        model.suggestIfNeeded(context)
        XCTAssertEqual(script.calls.count, 1)
    }

    func testOffWhenGemmaIsNotReady() {
        let script = Script()
        script.reason = "Turn on Gemma to get task ideas."
        let model = advisor(script, prefs: InMemoryKeyValueStore())
        model.suggestIfNeeded(context)
        XCTAssertEqual(model.phase, .off("Turn on Gemma to get task ideas."))
        XCTAssertTrue(script.calls.isEmpty)
    }

    func testRefreshRecordsTheSkipAndExploresMore() async {
        let script = Script()
        script.replies = ["TITLE: One\nTASK: Do one.", "TITLE: Two\nTASK: Do two."]
        let prefs = InMemoryKeyValueStore()
        let model = advisor(script, prefs: prefs)
        model.suggestIfNeeded(context)
        await model.waitForIdle()
        model.refresh(context)
        await model.waitForIdle()
        XCTAssertEqual(model.suggestion?.title, "Two")
        XCTAssertEqual(model.ledger.titles(.skipped, limit: 5), ["One"])
        XCTAssertGreaterThan(script.temperatures[1], script.temperatures[0])
        let prompt = script.calls[1].last?.content ?? ""
        XCTAssertTrue(prompt.contains("Suggestions they skipped:\n- One"))
    }

    func testARepeatOfASkippedTaskIsRetried() async {
        let script = Script()
        script.replies = ["TITLE: One\nTASK: Do one.", "TITLE: one\nTASK: Again.", "TITLE: Fresh\nTASK: New."]
        let model = advisor(script, prefs: InMemoryKeyValueStore())
        model.suggestIfNeeded(context)
        await model.waitForIdle()
        model.refresh(context)
        await model.waitForIdle()
        XCTAssertEqual(model.suggestion?.title, "Fresh")
        XCTAssertEqual(script.calls.count, 3)
        let retry = script.calls[2]
        XCTAssertEqual(retry.count, 4)
        XCTAssertEqual(retry[2].role, .assistant)
        XCTAssertTrue(retry[3].content.contains("\"one\" is too close to \"One\", which the developer skipped"))
    }

    func testStartRecordsAndLearnsAfterEnoughDecisions() async {
        let script = Script()
        var clock = 1000
        let prefs = InMemoryKeyValueStore()
        let model = advisor(script, prefs: prefs, clock: { clock })
        for index in 0..<3 {
            model.ledger.record(.skipped, NextTaskSuggestion(title: "Skip \(index)", task: "x"), atMs: clock)
            clock += 1
        }
        script.replies = ["TITLE: Ship it\nTASK: Ship.", "- Likes shipping\n- Avoids chores", "TITLE: Next\nTASK: Next."]
        model.suggestIfNeeded(context)
        await model.waitForIdle()
        clock += 1
        XCTAssertEqual(model.started()?.title, "Ship it")
        XCTAssertEqual(model.phase, .idle)
        XCTAssertTrue(model.learning)
        await model.waitForIdle()
        XCTAssertFalse(model.learning)
        XCTAssertEqual(model.ledger.lessons, ["Likes shipping", "Avoids chores"])
        XCTAssertFalse(model.ledger.dueForReflection)
        XCTAssertTrue((script.calls[1].last?.content ?? "").contains("started: Ship it"))
        model.suggestIfNeeded(context)
        await model.waitForIdle()
        XCTAssertTrue((script.calls[2].last?.content ?? "").contains("What this developer prefers (learned):\n- Likes shipping"))
        XCTAssertTrue((script.calls[2].last?.content ?? "").contains("Suggestions they started:\n- Ship it"))
    }

    func testLedgerKeepsTheNewestEntries() {
        let ledger = NextTaskLedger(store: InMemoryKeyValueStore())
        for index in 0..<(NextTaskLedger.most + 5) {
            ledger.record(.skipped, NextTaskSuggestion(title: "T\(index)\tx", task: "a\nb"), atMs: index)
        }
        XCTAssertEqual(ledger.entries.count, NextTaskLedger.most)
        XCTAssertEqual(ledger.entries.first?.title, "T\(NextTaskLedger.most + 4) x")
        XCTAssertEqual(ledger.entries.first?.task, "a b")
        XCTAssertEqual(ledger.skipStreak, NextTaskLedger.most)
    }

    func testRepeatsCatchRewordedIdeas() {
        let walk = NextTaskSuggestion(title: "Implement Walk Detection Algorithm", task: "Write a detection algorithm for walking from the camera feed.")
        XCTAssertTrue(walk.repeats(NextTaskSuggestion(title: "Walking detection", task: "")))
        XCTAssertTrue(walk.repeats(NextTaskSuggestion(title: "Detect walks from camera", task: "")))
        XCTAssertTrue(walk.repeats(NextTaskSuggestion(
            title: "Camera step tracking", task: "Write the detection algorithm that finds walking in the camera feed.")))
        XCTAssertFalse(walk.repeats(NextTaskSuggestion(title: "Dark mode toggle", task: "Add a dark mode switch to Settings.")))
        XCTAssertFalse(NextTaskSuggestion(title: "Fix login crash", task: "").repeats(NextTaskSuggestion(title: "Login screen redesign", task: "")))
    }

    func testDenyRemembersTheReasonAndNeverShowsItAgain() async throws {
        let script = Script()
        script.replies = [
            "TITLE: Walk detection\nTASK: Detect walking.\nWHY: Core feature.",
            "TITLE: Implement walking detection\nTASK: Detect walks.",
            "TITLE: Offline cache\nTASK: Cache the repo list.",
        ]
        let prefs = InMemoryKeyValueStore()
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("next-task-\(UUID().uuidString)")
        let log = NextTaskTrainingLog(directory: { folder })
        let model = advisor(script, prefs: prefs, log: log)
        model.suggestIfNeeded(context)
        await model.waitForIdle()
        model.deny(.done, context)
        await model.waitForIdle()
        XCTAssertEqual(model.suggestion?.title, "Offline cache")
        XCTAssertEqual(model.ledger.denied.map(\.title), ["Walk detection"])
        XCTAssertEqual(model.ledger.denied.first?.reason, "done")
        XCTAssertEqual(model.ledger.denied.first?.repo, "alice/mobile")
        XCTAssertEqual(model.ledger.entries.first?.verdict, .denied)
        let prompt = script.calls[1].last?.content ?? ""
        XCTAssertTrue(prompt.contains("Ideas they rejected, never suggest these or anything like them:\n- Walk detection (already done)"))
        XCTAssertTrue(script.calls[2].last?.content.contains("which the developer rejected") ?? false)
        let saved = log.entries()
        XCTAssertEqual(saved.map(\.verdict), ["denied"])
        XCTAssertEqual(saved.first?.reason, "done")
        XCTAssertEqual(saved.first?.prompt.first?.role, "system")
        XCTAssertTrue(saved.first?.prompt.last?.content.contains("Repository: alice/mobile") ?? false)
        _ = model.started()
        XCTAssertEqual(log.entries().map(\.verdict), ["denied", "started"])
        XCTAssertEqual(model.ledger.denied.count, 1)
        try? FileManager.default.removeItem(at: folder.appendingPathComponent(NextTaskTrainingLog.fileName))
    }

    func testDeniedIdeasOutliveTheRollingLedger() {
        let ledger = NextTaskLedger(store: InMemoryKeyValueStore())
        ledger.record(.denied, NextTaskSuggestion(title: "Walk detection", task: "x"), atMs: 1, reason: .vague)
        for index in 0..<(NextTaskLedger.most + 5) {
            ledger.record(.skipped, NextTaskSuggestion(title: "T\(index)", task: "x"), atMs: index + 2)
        }
        XCTAssertFalse(ledger.entries.contains { $0.verdict == .denied })
        XCTAssertEqual(ledger.titles(.denied, limit: 5), ["Walk detection"])
        let avoid = NextTaskPrompt.avoid(NextTaskContext(), ledger: ledger, shown: [])
        XCTAssertNotNil(NextTaskPrompt.repeated(NextTaskSuggestion(title: "Walking detection", task: ""), in: avoid))
    }

    func testDoneWorkIsAvoidedAndRepeatsFallBackToTheTopIssue() async {
        let script = Script()
        script.replies = Array(repeating: "TITLE: Fix login crash again\nTASK: Fix the login crash.", count: 3)
        let model = advisor(script, prefs: InMemoryKeyValueStore())
        model.suggestIfNeeded(context)
        await model.waitForIdle()
        XCTAssertEqual(script.calls.count, NextTaskPrompt.attempts)
        XCTAssertEqual(model.suggestion?.title, "Dark mode")
        XCTAssertTrue(model.suggestion?.task.contains("issue #7") ?? false)
        let done = NextTaskContext(recent: [
            NextTaskWork(title: "Add walk detection", status: "Merged"),
            NextTaskWork(title: "Broken sync", status: "Failed"),
        ])
        let avoid = NextTaskPrompt.avoid(done, ledger: NextTaskLedger(store: nil), shown: [])
        XCTAssertNotNil(NextTaskPrompt.repeated(NextTaskSuggestion(title: "Walk detection", task: ""), in: avoid))
        XCTAssertNil(NextTaskPrompt.repeated(NextTaskSuggestion(title: "Broken sync", task: ""), in: avoid))
    }

    func testThePromptCarriesDocsCommitsAndClosedIssues() {
        let context = NextTaskContext(
            repo: "alice/mobile", issues: [NextTaskIssue(number: 3, title: "Sync", summary: "Pull to refresh fails")],
            closed: [NextTaskIssue(number: 2, title: "Walk detection")], commits: ["Add pedometer screen"],
            docs: [NextTaskDoc(path: "README.md", text: "# Mobile\nA step tracker.")], docIndex: ["PLAN.md", "api/"])
        let prompt = NextTaskPrompt.describe(context, ledger: NextTaskLedger(store: nil), shown: [])
        XCTAssertTrue(prompt.contains("Documentation files: PLAN.md, api/"))
        XCTAssertTrue(prompt.contains("From README.md:\n# Mobile\nA step tracker."))
        XCTAssertTrue(prompt.contains("Recent commits, newest first (done):\n- Add pedometer screen"))
        XCTAssertTrue(prompt.contains("- #3 Sync: Pull to refresh fails"))
        XCTAssertTrue(prompt.contains("Recently closed issues (done):\n- #2 Walk detection"))
    }

    func testCommitsSkipMergesAndRepeats() {
        let rows: [JSONObject] = [
            ["subject": "Merge 12-x into main"], ["subject": "Add pedometer\n\nbody"], ["message": "add pedometer"], ["subject": "Fix sync"],
        ]
        XCTAssertEqual(NextTaskContext.commits(rows), ["Add pedometer", "Fix sync"])
    }

    func testRepoReaderPicksTheUsefulDocs() {
        let root = [
            RepoTreeEntry(name: "docs", path: "docs", type: "dir"), RepoTreeEntry(name: "main.swift", path: "main.swift", type: "file"),
            RepoTreeEntry(name: "README.md", path: "README.md", type: "file"), RepoTreeEntry(name: "CHANGELOG.md", path: "CHANGELOG.md", type: "file"),
        ]
        let docs = [
            RepoTreeEntry(name: "PLAN.md", path: "docs/PLAN.md", type: "file"), RepoTreeEntry(name: "README.md", path: "docs/README.md", type: "file"),
            RepoTreeEntry(name: "logo.png", path: "docs/logo.png", type: "file"), RepoTreeEntry(name: "api", path: "docs/api", type: "dir"),
        ]
        XCTAssertEqual(NextTaskRepoReader.pick(root: root, docs: docs), ["README.md", "docs/README.md", "docs/PLAN.md", "CHANGELOG.md"])
        XCTAssertEqual(NextTaskRepoReader.index(docs), ["PLAN.md", "README.md", "api/"])
        XCTAssertEqual(
            NextTaskRepoReader.excerpt("# Title\n\n[![badge](x)](y)\n<p>html</p>\n```\ncode\n```\n| a | b |\n|---|---|\nText", 100),
            "# Title\n| a | b |\nText")
        let docsOut = NextTaskRepoReader.documents([("README.md", String(repeating: "r", count: 3000)), ("docs/PLAN.md", "Plan")])
        XCTAssertEqual(docsOut.map(\.path), ["README.md", "docs/PLAN.md"])
        XCTAssertEqual(docsOut.first?.text.count, NextTaskRepoReader.readmeMost)
    }

    func testRepoReaderLoadsHistoryDocsAndIssues() async {
        let http = FakeHTTPClient { (request: HTTPRequest) async throws -> HTTPResponse in
            let path = request.url.path
            let query = URLComponents(url: request.url, resolvingAgainstBaseURL: false)?.queryItems ?? []
            let dir = query.first { $0.name == "path" }?.value ?? ""
            if path.hasSuffix("/issues") {
                return HTTPResponse(json: [["number": 1, "title": "Open one", "status": "open"], ["number": 2, "title": "Done one", "status": "closed"]])
            }
            if path.hasSuffix("/history") { return HTTPResponse(json: ["commits": [["subject": "Add pedometer"]]]) }
            if path.hasSuffix("/tree") && dir.isEmpty {
                return HTTPResponse(json: ["entries": [["name": "README.md", "type": "file"], ["name": "docs", "type": "dir"]]])
            }
            if path.hasSuffix("/tree") { return HTTPResponse(json: ["entries": [["name": "PLAN.md", "type": "file"]]]) }
            if path.hasSuffix("/blobs") {
                return HTTPResponse(json: ["blobs": ["README.md": ["content": "# App\nSteps."], "docs/PLAN.md": ["content": "Next: sync."]]])
            }
            return HTTPResponse(status: 404, text: "not found")
        }
        let api = ApiService(settings: StaticApiSettings(serverUrl: "ws://test.local/ws"), http: http, sleep: { _ in })
        let loaded = await NextTaskRepoReader.load(api: api, owner: "alice", name: "mobile")
        XCTAssertEqual(loaded.issues.map(\.number), [1])
        XCTAssertEqual(loaded.closed.map(\.number), [2])
        XCTAssertEqual(loaded.commits, ["Add pedometer"])
        XCTAssertEqual(loaded.docIndex, ["PLAN.md"])
        XCTAssertEqual(loaded.docs, [NextTaskDoc(path: "README.md", text: "# App\nSteps."), NextTaskDoc(path: "docs/PLAN.md", text: "Next: sync.")])
    }

    func testTrainingLogKeepsTheNewestLines() {
        let data = Data("one\ntwo\nthree\n".utf8)
        XCTAssertEqual(String(decoding: NextTaskTrainingLog.trim(data, to: 8), as: UTF8.self), "three\n")
        XCTAssertEqual(NextTaskTrainingLog.trim(data, to: 100), data)
    }
}
