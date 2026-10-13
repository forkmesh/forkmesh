import FMApi
import FMBase
import FMTools
import Foundation
import XCTest

@testable import FMPlanning

@MainActor
final class RecordingTasksCall {
    var calls: [(method: String, suffix: String, body: JSONObject?)] = []
    var answer: JSONObject = ["ok": true, "task": ["id": "t1"]]
    var failSuffixes: Set<String> = []

    var call: ToolsApiCall {
        { method, suffix, body in
            self.calls.append((method, suffix, body))
            if self.failSuffixes.contains("\(method) \(suffix)") { throw OrgApiException("forbidden", "forbidden") }
            return self.answer
        }
    }
}

final class TaskOperationsTests: XCTestCase {
    func testFilterFromRoute() {
        XCTAssertEqual(TaskListFilter(route: "Done").index, 2)
        XCTAssertEqual(TaskListFilter(route: nil), .mine)
        XCTAssertEqual(TaskListFilter(route: "weird"), .mine)
    }

    func testAttachmentValidation() {
        let small = TaskAttachmentDraft(name: "a.png", mime: "image/png", data: Data(count: 10))
        XCTAssertNil(TaskAttachmentDraft.validate([small]))
        XCTAssertNotNil(TaskAttachmentDraft.validate(Array(repeating: small, count: 5)))
        XCTAssertNotNil(TaskAttachmentDraft.validate([TaskAttachmentDraft(name: "a.pdf", mime: "application/pdf", data: Data())]))
        XCTAssertNotNil(TaskAttachmentDraft.validate([TaskAttachmentDraft(name: "b", mime: "image/png", data: Data(count: 300_000))]))
        XCTAssertEqual(small.json.objectValue?["file"], .string(Data(count: 10).base64EncodedString()))
    }

    func testConversionTextAndAgentInfo() {
        XCTAssertEqual(PlanningConversion.title(noteTitle: "", markdown: "\n\n## Plan the launch\nbody"), "Plan the launch")
        XCTAssertEqual(PlanningConversion.title(noteTitle: "  Notes ", markdown: "x"), "Notes")
        let details = PlanningConversion.details(noteTitle: "N", markdown: "hello", publicURL: "https://x/notes/1")
        XCTAssertEqual(details, "hello\n\n---\nConverted from the note \"N\" (published at https://x/notes/1)")
        let long = PlanningConversion.details(noteTitle: "N", markdown: String(repeating: "a", count: 10_000), publicURL: nil)
        XCTAssertLessThanOrEqual(long.count, PlanningConversion.taskDetailsMax)
        XCTAssertTrue(long.hasSuffix("Converted from the note \"N\""))
        XCTAssertEqual(PlanningConversion.agentPrompt(task: ["id": "t9", "title": "Fix it", "details": " Soon "]), "[task:t9] Fix it\n\nSoon")
        XCTAssertNil(TaskAgentInfo(task: ["id": "x"]))
        XCTAssertEqual(TaskAgentInfo(task: ["agent": ["provider": "claude", "status": "waiting", "sessionId": "s1"]])?.label,
                       "Claude: Waiting for you")
        XCTAssertEqual(TaskAgentInfo(task: ["agentSessionId": "s2"])?.sessionId, "s2")
    }

    @MainActor func testOperationsSendTheWorkerShapes() async {
        let recorder = RecordingTasksCall()
        let ops = TaskOperations(call: recorder.call)
        let empty = await ops.quickAdd(title: "  ")
        XCTAssertEqual(empty, .failure(TaskOperationError(message: "Give the task a title.")))
        _ = await ops.quickAdd(title: "Ship", attachments: [TaskAttachmentDraft(name: "a.png", mime: "image/png", data: Data([1]))])
        XCTAssertEqual(recorder.calls.last?.body?["destination"], "department")
        XCTAssertEqual(recorder.calls.last?.body?["attachments"]?.arrayValue?.count, 1)
        _ = await ops.followUp(parentId: "t1", title: "Next")
        XCTAssertEqual(recorder.calls.last?.body, ["title": "Next", "parentTaskId": "t1"])
        let badRepo = await ops.assignToAgent("t1", repository: "nope")
        XCTAssertEqual(badRepo, .failure(TaskOperationError(message: "Choose a repository as owner/name.")))
        _ = await ops.assignToAgent("t1", repository: "acme/app")
        XCTAssertEqual(recorder.calls.last?.method, "PATCH")
        XCTAssertEqual(recorder.calls.last?.body, ["assigneeKind": "agent", "repository": "acme/app"])
        let badPriority = await ops.setPriority("t1", 0)
        XCTAssertEqual(badPriority, .failure(TaskOperationError(message: "Priority runs from 1 to 99.")))

        recorder.answer = ["ok": true, "history": .array([.object(["id": "h1", "kind": "created", "actor": "bob", "summary": "made", "createdAt": 5])])]
        let historyError = await ops.loadHistory("t1")
        XCTAssertNil(historyError)
        XCTAssertEqual(ops.history.map(\.summary), ["made"])
        XCTAssertEqual(recorder.calls.last?.suffix, "/t1/history")

        var filed: [String] = []
        let moved = await ops.moveToIssue(["id": "t1", "title": "Bug", "details": "d"], repository: "acme/app") { owner, name, title, body in
            filed.append("\(owner)/\(name):\(title):\(body)")
        }
        XCTAssertNil(moved)
        XCTAssertEqual(filed, ["acme/app:Bug:d\n\n---\nMoved from organization task t1."])
        XCTAssertEqual(recorder.calls.last?.method, "DELETE")

        recorder.failSuffixes = ["DELETE /t3"]
        let result = await ops.deleteClosed([
            ["id": "t2", "status": "done"], ["id": "t3", "status": "done"], ["id": "t4", "status": "active"],
        ])
        XCTAssertEqual(result.deleted, 1)
        XCTAssertEqual(result.failed, 1)
    }

    @MainActor func testNoteConversions() async {
        let recorder = RecordingTasksCall()
        let task = await NoteConversions.toTask(noteTitle: "", markdown: "# Idea\nmore", publicURL: nil, call: recorder.call)
        XCTAssertEqual(task, .success(["id": "t1"]))
        XCTAssertEqual(recorder.calls.last?.body?["title"], "Idea")
        var filed = 0
        let issue = await NoteConversions.toIssue(noteTitle: "T", markdown: "m", publicURL: nil, repository: "a/b") { _, _, _, _ in
            filed += 1
        }
        XCTAssertNil(issue)
        XCTAssertEqual(filed, 1)
        let bad = await NoteConversions.toIssue(noteTitle: "T", markdown: "m", publicURL: nil, repository: "") { _, _, _, _ in }
        XCTAssertEqual(bad, "Choose a repository as owner/name.")
    }
}
