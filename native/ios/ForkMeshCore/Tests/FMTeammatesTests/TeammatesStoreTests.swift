import FMBase
import FMChat
import FMControl
import Foundation
import XCTest

@testable import FMTeammates

@MainActor
final class FakeTeamChannel: DesktopControlCalling {
    var requests: [(String, JSONObject)] = []
    var replies: [String: JSONObject] = [:]
    var failures: [String: Error] = [:]
    private var taps: [UUID: @MainActor (String, JSONObject) -> Void] = [:]

    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        requests.append((method, params))
        if let failure = failures[method] { throw failure }
        return replies[method] ?? [:]
    }

    func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID {
        let id = UUID()
        taps[id] = tap
        return id
    }

    func removeEventTap(_ id: UUID) { taps.removeValue(forKey: id) }

    func emit(_ event: String, _ data: JSONObject = [:]) {
        for tap in taps.values { tap(event, data) }
    }

    func count(_ method: String) -> Int { requests.filter { $0.0 == method }.count }

    func last(_ method: String) -> JSONObject? { requests.last { $0.0 == method }?.1 }

    var tapCount: Int { taps.count }
}

final class TeammatesStoreTests: XCTestCase {
    static let listJSON: JSONObject = [
        "repo": "f/forkmesh", "repos": ["f/forkmesh", "f/other"], "available": true,
        "summary": ["total": 3, "working": 1, "scheduled": 1, "idle": 1],
        "teammates": [
            ["id": "zed", "name": "Zed Idle", "status": "Idle", "schedule": ["mode": "on-demand"]],
            ["id": "qa", "name": "Quinn", "status": "Working", "provider": "codex", "model": "gpt",
             "schedule": ["mode": "daily", "time": "07:30", "active": true, "human": "Daily at 07:30"],
             "mood": ["key": "needs-you", "emoji": "🙋", "label": "Needs you", "motion": "wave", "attention": true,
                      "intensity": 0.8, "reasons": ["waiting for your reply", "ran today"]]],
            ["id": "cfo", "name": "Cass", "status": "Scheduled", "workflow": "contributor-payouts",
             "schedule": ["mode": "weekly", "days": [1, 9], "active": true]],
        ],
    ]

    @MainActor func testListSortsAndFilters() async throws {
        let channel = FakeTeamChannel()
        channel.replies["teammates.list"] = Self.listJSON
        let store = TeammatesStore(channel: channel)
        await store.refresh()
        let snapshot = try XCTUnwrap(store.snapshot)
        XCTAssertEqual(snapshot.sorted.map(\.id), ["qa", "cfo", "zed"])
        XCTAssertEqual(snapshot.filtered(.working).map(\.id), ["qa"])
        XCTAssertEqual(snapshot.summary.line, "3 special agents · 1 working · 1 scheduled · 1 idle")
        XCTAssertEqual(snapshot.automaticSchedules, 2)
        XCTAssertEqual(snapshot.teammates[2].schedule.days, [1], "an impossible day is dropped")
        XCTAssertFalse(try XCTUnwrap(store.teammate("cfo")).takesPrompts)
        XCTAssertEqual(try XCTUnwrap(store.teammate("qa")).initials, "Q")
        XCTAssertEqual(try XCTUnwrap(store.teammate("zed")).initials, "ZI")
        let mood = try XCTUnwrap(store.teammate("qa")).mood
        XCTAssertEqual(mood.emoji, "🙋")
        XCTAssertEqual(mood.motion, "wave")
        XCTAssertTrue(mood.attention)
        XCTAssertEqual(mood.line, "Needs you: waiting for your reply, ran today")
        XCTAssertTrue(try XCTUnwrap(store.teammate("zed")).mood.isEmpty, "an older desktop sends no mood")
    }

    @MainActor func testChangedBellRefreshes() async {
        let channel = FakeTeamChannel()
        channel.replies["teammates.list"] = Self.listJSON
        let store = TeammatesStore(channel: channel)
        channel.emit("insights.changed")
        for _ in 0..<5 { await Task.yield() }
        XCTAssertEqual(channel.count("teammates.list"), 0)
        channel.emit("teammates.changed")
        for _ in 0..<5 { await Task.yield() }
        XCTAssertEqual(channel.count("teammates.list"), 1)
        XCTAssertEqual(store.revision, 1)
        store.stop()
        XCTAssertEqual(channel.tapCount, 0)
    }

    @MainActor func testPerformCarriesTheToken() async throws {
        let channel = FakeTeamChannel()
        channel.replies["teammates.prepare"] = ["token": "t", "action": "grade", "subject": "4:5", "summary": "Grade"]
        channel.replies["teammates.run"] = ["started": true, "message": "Graded 5 out of 5."]
        let store = TeammatesStore(channel: channel)
        let message = try await store.perform(.grade(session: 4, grade: 5))
        XCTAssertEqual(message, "Graded 5 out of 5.")
        XCTAssertEqual(channel.last("teammates.prepare")?["session"], .int(4))
        let sent = try XCTUnwrap(channel.last("teammates.run"))
        XCTAssertEqual(sent.str("token"), "t")
        XCTAssertEqual(sent.str("action"), "grade")
        XCTAssertEqual(sent["grade"], .int(5))
        XCTAssertEqual(channel.count("teammates.list"), 1, "a change refreshes the list")
    }

    @MainActor func testUnsupportedDesktop() async {
        let channel = FakeTeamChannel()
        channel.failures["teammates.list"] = LocalControlException("unsupported")
        let store = TeammatesStore(channel: channel)
        await store.refresh()
        XCTAssertTrue(store.unsupported)
    }

    @MainActor func testDetailBriefingStatsAndRoom() async throws {
        let channel = FakeTeamChannel()
        channel.replies["teammates.get"] = [
            "id": "qa", "name": "Quinn", "mode": "Plan", "instructions": "Test everything",
            "backupModels": [["provider": "claude-code", "model": "opus"]],
            "taskAgents": [["taskType": "new-issues", "provider": "codex", "model": "gpt"]],
            "permissions": [["id": "read-repositories", "label": "Read repositories"]],
            "logMonitors": [["badge": "error", "threshold": 3, "taskType": "error-logs", "active": true]],
        ]
        channel.replies["teammates.briefing"] = [
            "updates": 2, "completed": 1, "inProgress": 1,
            "entries": [["id": "qa", "name": "Quinn", "sessionId": 3, "output": "All green"], ["id": "zed", "name": "Zed"]],
        ]
        channel.replies["teammates.stats"] = [
            "teammates": [["id": "qa", "name": "Quinn", "runs": 4, "succeeded": 3, "costUsd": 1.5]],
            "tasks": [["session": 9, "title": "t", "status": "success", "gradable": true, "grade": 0]],
            "totalTasks": 1,
        ]
        channel.replies["teammates.room"] = ["messages": [["id": "m1", "sender": "Quinn", "text": "done", "t": 5]]]
        let store = TeammatesStore(channel: channel)
        let detail = try await store.detail("qa")
        XCTAssertEqual(detail.mode, "Plan")
        XCTAssertEqual(detail.backupModels.first?.model, "opus")
        XCTAssertEqual(detail.taskAgents.first?.taskType, "new-issues")
        XCTAssertEqual(detail.permissions.first?.label, "Read repositories")
        let briefing = try await store.briefing()
        XCTAssertEqual(briefing.headline, "2 updates · 1 done · 1 in progress")
        XCTAssertEqual(briefing.entries.filter(\.hasUpdate).map(\.id), ["qa"])
        let stats = try await store.stats()
        XCTAssertEqual(stats.teammates[0].successRate, 0.75)
        XCTAssertTrue(stats.tasks[0].gradable)
        let room = try await store.room()
        XCTAssertEqual(room.first?.text, "done")
        XCTAssertEqual(channel.last("teammates.room")?["limit"], .int(100))
    }

    func testErrorText() {
        XCTAssertEqual(TeamErrorText.describe(code: "field_not_editable:permissions"), "permissions is edited on the desktop.")
        XCTAssertEqual(TeamErrorText.describe(code: "bad_field:model"), "The desktop refused the model field.")
    }
}

final class TeammateEditorTests: XCTestCase {
    func sampleDetail() -> TeammateDetail {
        TeammateDetail(json: [
            "id": "qa", "name": "Quinn", "avatar": "sol", "provider": "codex", "model": "gpt-5", "skills": ["pytest"],
            "schedule": ["mode": "daily", "time": "07:30", "active": true],
            "backupModels": [["provider": "claude-code", "model": "opus"]],
        ])
    }

    func testUnchangedDraftSendsNothing() {
        let original = TeammateDraft(detail: sampleDetail())
        XCTAssertTrue(TeammateEditorLogic.changes(from: original, to: original).isEmpty)
        XCTAssertTrue(TeammateEditorLogic.validate(original).isEmpty)
    }

    func testChangedFieldsOnly() {
        let original = TeammateDraft(detail: sampleDetail())
        var draft = original
        draft.focus = "  flaky tests  "
        draft.schedule.days = [5, 1]
        draft.schedule.mode = "custom"
        draft.enabled = false
        let changes = TeammateEditorLogic.changes(from: original, to: draft)
        XCTAssertEqual(Set(changes.map(\.0)), ["focus", "schedule", "enabled"])
        XCTAssertEqual(changes.str("focus"), "flaky tests")
        XCTAssertEqual(changes.obj("schedule").str("mode"), "custom")
        XCTAssertEqual(changes["enabled"], .bool(false))
    }

    func testNewTeammateSendsWhatIsSet() {
        var draft = TeammateDraft()
        draft.name = "Dee"
        draft.model = "gpt-5"
        let changes = TeammateEditorLogic.changes(from: nil, to: draft)
        XCTAssertEqual(changes.str("name"), "Dee")
        XCTAssertEqual(changes.str("provider"), "codex")
        XCTAssertNil(changes["focus"])
        XCTAssertNotNil(changes["schedule"])
    }

    func testValidation() {
        var draft = TeammateDraft()
        XCTAssertTrue(TeammateEditorLogic.validate(draft).contains(.name))
        XCTAssertTrue(TeammateEditorLogic.validate(draft).contains(.model))
        draft.name = "Dee"
        draft.model = "gpt-5"
        XCTAssertTrue(TeammateEditorLogic.validate(draft).isEmpty)
        draft.relayChannel = "team"
        draft.schedule.time = "25:00"
        draft.mode = "Yolo"
        draft.backupModels = [AgentChoiceRow(provider: "codex", model: "")]
        let issues = TeammateEditorLogic.validate(draft)
        XCTAssertTrue(issues.contains(.relayChannel))
        XCTAssertTrue(issues.contains(.schedule))
        XCTAssertTrue(issues.contains(.mode))
        XCTAssertTrue(issues.contains(.choices))
        XCTAssertTrue(TeammateEditorLogic.usesDays("custom"))
        XCTAssertFalse(TeammateEditorLogic.usesTime("hourly"))
    }
}
