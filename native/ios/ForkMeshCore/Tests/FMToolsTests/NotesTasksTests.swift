// The Notes and Tasks screens fed a fake
// account API answering from memory, driven through their models.

import FMApi
import FMBase
import XCTest
@testable import FMTools

/// Records every call and answers with `handler`.
@MainActor
final class FakeToolsApi {
    typealias Handler = (_ method: String, _ suffix: String, _ body: JSONObject?) throws -> JSONObject

    var handler: Handler
    private(set) var calls: [(method: String, suffix: String, body: JSONObject?)] = []

    init(_ handler: @escaping Handler = { method, suffix, _ in throw OrgApiException("unexpected", "unexpected \(method) \(suffix)") }) {
        self.handler = handler
    }

    var call: ToolsApiCall {
        { [self] method, suffix, body in
            self.calls.append((method, suffix, body))
            return try self.handler(method, suffix, body)
        }
    }

    var last: (method: String, suffix: String, body: JSONObject?)? { calls.last }
}

let noteId = String(repeating: "a", count: 32)
let taskId = String(repeating: "b", count: 32)
/// Where the tests' clock stands.
let toolsNowMillis: Int64 = 1_789_387_200_000

func noteJSON(version: Int = 3, role: String = "owner") -> JSONObject {
    [
        "id": .string(noteId),
        "title": "Launch plan",
        "markdown": "# Launch\n\n- ship it",
        "visibility": "private",
        "version": .int(Int64(version)),
        "createdAt": 1,
        "updatedAt": .int(toolsNowMillis),
        "role": .string(role),
        "shares": [["type": "user", "name": "bob", "role": "editor", "createdAt": 1]],
        "links": [],
    ]
}

func taskJSON(status: String = "idle", assignee: String = "alice", replies: [JSONValue] = []) -> JSONObject {
    [
        "id": .string(taskId),
        "kind": "task",
        "title": "Write the release notes",
        "details": "Cover notes and tasks on mobile.",
        "assignee": .string(assignee),
        "assigneeKind": "user",
        "department": "product-design",
        "priority": 50,
        "status": .string(status),
        "elapsedMs": .int(90 * 60000),
        "createdBy": "carol",
        "createdAt": 1,
        "updatedAt": 2,
        "completedAt": .int(status == "done" ? 3 : 0),
        "completionNote": "",
        "qa": ["status": "unknown", "reviewer": ""],
        "lastCheckin": .null,
        "replies": .array(replies),
        "replyCount": .int(Int64(replies.count)),
    ]
}

func merged(_ base: JSONObject, _ changes: JSONObject) -> JSONObject {
    var out = base
    for (key, value) in changes { out[key] = value }
    return out
}

final class NotesTests: XCTestCase {
    let clock = FixedClock(epochMillis: toolsNowMillis)

    @MainActor
    func testListsNotesAndOpensOneForEditing() async {
        let api = FakeToolsApi { method, suffix, _ in
            switch (method, suffix) {
            case ("GET", ""):
                return ["ok": true, "notes": [
                    .object(merged(noteJSON(), ["markdown": .null])),
                    .object(merged(noteJSON(role: "viewer"), ["id": .string(String(repeating: "c", count: 32)), "title": "Team handbook", "shares": []])),
                ]]
            case ("GET", _):
                return ["ok": true, "note": .object(noteJSON())]
            default:
                throw OrgApiException("unexpected", "unexpected")
            }
        }
        let list = NotesListModel(call: api.call, clock: clock)
        await list.load()
        XCTAssertEqual(list.rows.map(\.title), ["Launch plan", "Team handbook"])
        XCTAssertTrue(list.rows[0].subtitle.contains("Shared with 1"))
        XCTAssertEqual(list.rows[0].subtitle, "Edited just now · Shared with 1")
        XCTAssertTrue(list.rows[1].subtitle.contains("Read only"))

        list.query = "hand"
        XCTAssertEqual(list.rows.map(\.title), ["Team handbook"])
        list.query = ""

        let editor = NoteEditorModel(noteId: list.rows[0].id, call: api.call, clock: clock)
        await editor.load()
        XCTAssertEqual(api.last?.method, "GET")
        XCTAssertEqual(api.last?.suffix, "/\(noteId)")
        XCTAssertNil(api.last?.body)
        XCTAssertEqual(editor.screenTitle, "Edit note")
        XCTAssertEqual(editor.markdown, "# Launch\n\n- ship it")
    }

    @MainActor
    func testSavesAnEditWithTheBaseVersion() async {
        let api = FakeToolsApi { method, _, body in
            switch method {
            case "GET": return ["ok": true, "note": .object(noteJSON())]
            case "PATCH":
                return ["ok": true, "note": .object(merged(noteJSON(version: 4), ["markdown": body!["markdown"]!, "title": body!["title"]!]))]
            default: throw OrgApiException("unexpected", "unexpected")
            }
        }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: clock)
        await editor.load()
        XCTAssertFalse(editor.saveEnabled)
        editor.markdown = "# Launch\n\n- ship it\n- tell people"
        XCTAssertTrue(editor.saveEnabled)
        XCTAssertTrue(editor.confirmsLeave)
        let outcome = await editor.save()

        let last = api.last!
        XCTAssertEqual(last.method, "PATCH")
        XCTAssertEqual(last.suffix, "/\(noteId)")
        XCTAssertEqual(last.body?["baseVersion"], 3)
        XCTAssertEqual(last.body?["markdown"], "# Launch\n\n- ship it\n- tell people")
        XCTAssertEqual(outcome.notice?.message, "Note saved")
        XCTAssertEqual(outcome.notice?.isError, false)
        XCTAssertTrue(editor.meta.contains("Version 4"))
        XCTAssertFalse(editor.dirty)
    }

    @MainActor
    func testAStaleSaveExplainsTheVersionConflict() async {
        let api = FakeToolsApi { method, _, _ in
            if method == "GET" { return ["ok": true, "note": .object(noteJSON())] }
            throw OrgApiException("version_conflict", "version conflict")
        }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: clock)
        await editor.load()
        editor.title = "Plan"
        let outcome = await editor.save()
        XCTAssertTrue(outcome.notice?.message.hasPrefix("Someone else saved this note first") == true)
        XCTAssertEqual(outcome.notice?.isError, true)
        XCTAssertFalse(editor.saving)
    }

    @MainActor
    func testANewNoteIsCreatedOnFirstSave() async {
        let api = FakeToolsApi { _, _, body in
            ["ok": true, "note": .object(merged(noteJSON(version: 1), ["title": body!["title"]!]))]
        }
        let editor = NoteEditorModel(noteId: nil, call: api.call, clock: clock)
        await editor.load()
        XCTAssertEqual(editor.screenTitle, "New note")
        XCTAssertEqual(editor.meta, "Only you can see a new note until you share or publish it.")
        editor.title = "Idea"
        editor.markdown = "Body"
        _ = await editor.save()

        XCTAssertEqual(api.calls.count, 1)
        XCTAssertEqual(api.calls[0].method, "POST")
        XCTAssertEqual(api.calls[0].suffix, "")
        XCTAssertEqual(api.calls[0].body, ["title": "Idea", "markdown": "Body", "visibility": "private"])
        XCTAssertEqual(editor.screenTitle, "Edit note")
    }

    @MainActor
    func testAViewerReadsWithoutASaveButton() async {
        let api = FakeToolsApi { _, _, _ in ["ok": true, "note": .object(noteJSON(role: "viewer"))] }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: clock)
        await editor.load()
        XCTAssertEqual(editor.screenTitle, "Note")
        XCTAssertFalse(editor.showsSave)
        XCTAssertFalse(editor.canEdit)
        XCTAssertTrue(editor.meta.hasSuffix("read only"))
        XCTAssertEqual(editor.menuItems.map(\.value), ["history"])
    }

    @MainActor
    func testTheOwnerMenuPublishingHistoryAndSharing() async {
        var published = false
        let api = FakeToolsApi { method, suffix, body in
            switch (method, suffix) {
            case ("PATCH", _):
                published = body?["visibility"] == "public"
                return ["ok": true, "note": .object(merged(noteJSON(version: 4), ["visibility": published ? "public" : "private", "title": "Server title"]))]
            case ("GET", "/\(noteId)/versions"):
                return ["ok": true, "versions": [["version": 4, "createdAt": .int(toolsNowMillis), "author": "bob"], ["version": 2, "createdAt": 1]]]
            case ("POST", "/\(noteId)/versions"):
                return ["ok": true, "note": .object(merged(noteJSON(version: 5), ["markdown": "old text"]))]
            case ("POST", "/\(noteId)/shares"):
                return ["ok": true, "shares": [["type": "organization", "name": body!["name"]!, "role": "viewer"]]]
            case ("DELETE", "/\(noteId)"):
                return ["ok": true]
            default:
                return ["ok": true, "note": .object(noteJSON())]
            }
        }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: clock)
        await editor.load()
        XCTAssertEqual(editor.menuItems.map(\.value), ["history", "share", "publish", "delete"])
        editor.title = "Typed but unsaved"
        let visibility = await editor.setVisibility("public")
        XCTAssertEqual(visibility.notice?.message, "Note published")
        XCTAssertEqual(api.last?.body, ["baseVersion": 3, "visibility": "public"])
        XCTAssertEqual(editor.title, "Typed but unsaved")
        XCTAssertEqual(editor.menuItems.map(\.value), ["history", "share", "unpublish", "delete"])

        let history = editor.historyModel()!
        await history.load()
        XCTAssertEqual(history.versions?.count, 2)
        XCTAssertTrue(history.isCurrent(history.versions![0]))
        XCTAssertEqual(history.title(history.versions![1]), "Version 2")
        XCTAssertTrue(history.subtitle(history.versions![0]).hasSuffix(" · by @bob"))
        XCTAssertFalse(history.subtitle(history.versions![1]).contains("by @"))
        let restored = await history.restore(2)
        XCTAssertEqual(api.last?.body, ["version": 2, "baseVersion": 4])
        XCTAssertEqual(editor.applyRestored(restored.note!).notice?.message, "Version restored")
        XCTAssertEqual(editor.markdown, "old text")

        let share = editor.shareModel()!
        share.type = "organization"
        share.name = "@@Acme "
        let failure = await share.add()
        XCTAssertNil(failure)
        XCTAssertEqual(api.last?.body, ["type": "organization", "name": "acme", "role": "viewer"])
        XCTAssertEqual(share.shareSubtitle(share.shares[0]), "Organization · Viewer")
        XCTAssertEqual(share.name, "")

        let deleted = await editor.delete()
        XCTAssertTrue(deleted.close)
        XCTAssertFalse(editor.confirmsLeave)
    }

    @MainActor
    func testSharingWithAnUnknownNameSaysSo() async {
        let api = FakeToolsApi { _, _, _ in throw OrgApiException("principal_not_found", "principal not found") }
        let share = NoteShareModel(noteId: noteId, initial: [], call: api.call)
        share.name = "ghost"
        let failure = await share.add()
        XCTAssertEqual(failure, "No user has that name.")
        XCTAssertEqual(share.name, "ghost")
    }

    @MainActor
    func testAListThatCannotLoadExplainsWhy() async {
        let api = FakeToolsApi { _, _, _ in throw ApiException("offline") }
        let list = NotesListModel(call: api.call, clock: clock)
        await list.load()
        XCTAssertTrue(list.showsError)
        XCTAssertEqual(list.error, "Notes are unavailable right now.")
    }
}

final class TasksTests: XCTestCase {
    let clock = FixedClock(epochMillis: toolsNowMillis)

    func list(_ tasks: [JSONObject]) -> JSONObject {
        [
            "ok": true, "actor": "alice", "canManage": false, "tasks": .array(tasks.map { .object($0) }),
            "hasMore": false, "nextOffset": .null, "members": ["alice", "bob"], "departments": ["general", "product-design"],
        ]
    }

    @MainActor
    func testFiltersTheBoardIntoMineOpenAndDone() async {
        let board = list([
            taskJSON(),
            merged(taskJSON(assignee: "bob"), ["id": .string(String(repeating: "c", count: 32)), "title": "Bob work"]),
            merged(taskJSON(status: "done"), ["id": .string(String(repeating: "d", count: 32)), "title": "Old work"]),
        ])
        let api = FakeToolsApi { _, _, _ in board }
        let model = TasksListModel(call: api.call, clock: clock)
        await model.load()
        XCTAssertEqual(model.rows.map(\.title), ["Write the release notes"])
        XCTAssertEqual(model.rows[0].subtitle, "@alice · Product design · updated 56y ago")
        model.filter = 1
        XCTAssertEqual(model.rows.map(\.title), ["Write the release notes", "Bob work"])
        model.filter = 2
        XCTAssertEqual(model.rows.map(\.title), ["Old work"])
        XCTAssertEqual(model.rows[0].status, "Done")
        XCTAssertEqual(model.rows[0].tone, .success)
        XCTAssertEqual(model.lists.map(\.count), [1, 2, 1])
    }

    func peopleBoard() -> JSONObject {
        func task(_ id: String, _ status: String, _ changes: JSONObject) -> JSONObject {
            merged(taskJSON(status: status), merged(["id": .string(id), "title": .string(id)], changes))
        }
        var board = list([
            task("a1", "idle", [:]),
            task("a2", "done", [:]),
            task("w1", "idle", ["assignee": "wendy"]),
            task("w2", "active", ["assignee": "Wendy"]),
            task("w3", "done", ["assignee": "wendy"]),
            task("b1", "idle", ["assignee": "bob"]),
            task("bot1", "idle", ["assigneeKind": "claude", "assignee": ""]),
            task("bot2", "done", ["assigneeKind": "agent", "assignee": ""]),
            task("u1", "idle", ["assigneeKind": "unassigned", "assignee": ""]),
            task("u2", "idle", ["assignee": ""]),
        ])
        board["members"] = ["wendy", "bob", "alice", "Carol", " bob ", ""]
        return board
    }

    @MainActor
    func testPeopleAreYouFirstThenAToZWithOpenCounts() async {
        let board = peopleBoard()
        let model = TasksListModel(call: FakeToolsApi { _, _, _ in board }.call, clock: clock)
        await model.load()
        XCTAssertEqual(model.memberNames, ["alice", "bob", "Carol", "wendy"])
        XCTAssertEqual(model.people.map(\.id),
                       ["all", "user:alice", "user:bob", "user:carol", "user:wendy", "agent", "unassigned"])
        XCTAssertEqual(model.people.map(\.label),
                       ["Everyone", "@alice (you)", "@bob", "@Carol", "@wendy", "Bot", "Unassigned"])
        XCTAssertEqual(model.people.map(\.openCount), [7, 1, 1, 0, 2, 1, 2])
        XCTAssertEqual(model.people.map(\.isMember), [false, true, true, true, true, false, false])
        XCTAssertEqual(model.people[4].accessibilityLabel, "@wendy, 2 open")
        XCTAssertEqual(model.people[0].icon, "groups_outlined")
    }

    @MainActor
    func testTabsAreIconsWhoseCountsFollowTheSelectedPerson() async {
        let board = peopleBoard()
        let model = TasksListModel(call: FakeToolsApi { _, _, _ in board }.call, clock: clock)
        await model.load()
        XCTAssertEqual(model.filterTabs.map(\.icon), ["person_outline", "radio_button_checked", "check_circle_outline"])
        XCTAssertEqual(model.filterTabs.map(\.count), [1, 7, 3])
        XCTAssertEqual(model.filterTabs[1].accessibilityLabel, "Open, 7")

        model.selectFilter(1)
        model.selectPerson("user:wendy")
        XCTAssertEqual(model.person, "user:wendy")
        XCTAssertEqual(model.filterTabs.map(\.count), [1, 2, 1])
        XCTAssertEqual(model.rows.map(\.id), ["w1", "w2"])
        model.selectFilter(2)
        XCTAssertEqual(model.person, "user:wendy")
        XCTAssertEqual(model.rows.map(\.id), ["w3"])

        model.selectPerson("agent")
        XCTAssertEqual(model.filterTabs.map(\.count), [1, 1, 1])
        XCTAssertEqual(model.rows.map(\.id), ["bot2"])
        model.selectPerson("unassigned")
        XCTAssertEqual(model.filterTabs.map(\.count), [1, 2, 0])
        XCTAssertEqual(model.people.map(\.openCount), [7, 1, 1, 0, 2, 1, 2])
    }

    @MainActor
    func testTappingTheSelectedPersonAgainShowsEveryone() async {
        let board = peopleBoard()
        let model = TasksListModel(call: FakeToolsApi { _, _, _ in board }.call, clock: clock)
        await model.load()
        model.selectFilter(1)
        model.selectPerson("user:bob")
        XCTAssertEqual(model.rows.map(\.id), ["b1"])
        model.selectPerson("user:bob")
        XCTAssertEqual(model.person, TaskPerson.everyone)
        XCTAssertEqual(model.visible.count, 7)
        model.selectPerson("user:bob")
        model.selectPerson("user:wendy")
        XCTAssertEqual(model.person, "user:wendy")
        model.selectPerson(TaskPerson.everyone)
        XCTAssertEqual(model.person, TaskPerson.everyone)
    }

    @MainActor
    func testMineCountsOnlyYouAndResetsThePerson() async {
        let board = peopleBoard()
        let model = TasksListModel(call: FakeToolsApi { _, _, _ in board }.call, clock: clock)
        await model.load()
        XCTAssertEqual(model.filter, 0)
        model.selectPerson("user:wendy")
        XCTAssertEqual(model.filter, 1)
        XCTAssertEqual(model.filterTabs[0].count, 1)
        model.selectFilter(0)
        XCTAssertEqual(model.person, TaskPerson.everyone)
        XCTAssertEqual(model.rows.map(\.id), ["a1"])
        model.selectFilter(2)
        model.selectPerson("user:bob")
        XCTAssertEqual(model.filter, 2)
    }

    @MainActor
    func testAPersonWhoLeftTheBoardFallsBackToEveryone() async {
        var board = peopleBoard()
        let api = FakeToolsApi { _, _, _ in board }
        let model = TasksListModel(call: api.call, clock: clock)
        await model.load()
        model.selectPerson("user:wendy")
        board["members"] = ["alice", "bob"]
        await model.load()
        XCTAssertEqual(model.person, TaskPerson.everyone)
        model.selectPerson("agent")
        await model.load()
        XCTAssertEqual(model.person, "agent")
    }

    @MainActor
    func testANonMemberSeesWhyTheBoardIsEmpty() async {
        let api = FakeToolsApi { _, _, _ in throw OrgApiException("org_member_required", "org member required") }
        let model = TasksListModel(call: api.call, clock: clock)
        await model.load()
        XCTAssertTrue(model.showsError)
        XCTAssertTrue(model.error.contains("Ask an owner to add you"))
        XCTAssertFalse(model.showsNewTask)
    }

    @MainActor
    func testCreatesATaskAssignedToTheMember() async {
        let api = FakeToolsApi()
        api.handler = { [self] method, suffix, body in
            switch (method, suffix) {
            case ("GET", ""): return list([])
            case ("POST", ""): return ["ok": true, "task": .object(merged(taskJSON(), ["title": body!["title"]!]))]
            case ("GET", _): return ["ok": true, "task": .object(taskJSON())]
            default: throw OrgApiException("unexpected", "unexpected")
            }
        }
        let model = TasksListModel(call: api.call, clock: clock)
        await model.load()
        XCTAssertEqual(model.emptyTitle, "Nothing assigned to you")
        let form = model.formModel()
        XCTAssertEqual(form.assignees, ["alice"])
        XCTAssertFalse(form.showsAssignee)
        form.title = "Write the release notes"
        let created = await form.submit()

        let post = api.calls.first { $0.method == "POST" }!
        XCTAssertEqual(post.body, [
            "title": "Write the release notes", "details": "", "department": "general",
            "assigneeKind": "user", "assignee": "alice",
        ])
        // The new task opens straight away.
        let detail = model.detailModel(taskId: ToolsJSON.show(created.task?["id"]))
        await detail.load()
        XCTAssertEqual(detail.actions, [.start, .complete])
    }

    @MainActor
    func testTheAssigneeStartsFinishesAndReplies() async {
        var current = taskJSON()
        let api = FakeToolsApi { _, suffix, body in
            if suffix.hasSuffix("/start") { current = taskJSON(status: "active") }
            if suffix.hasSuffix("/complete") { current = taskJSON(status: "done") }
            if suffix.hasSuffix("/replies") {
                current["replies"] = [["id": .string(String(repeating: "e", count: 32)), "author": "alice", "body": body!["body"]!, "createdAt": 1]]
            }
            return ["ok": true, "task": .object(current)]
        }
        let detail = TaskDetailModel(taskId: taskId, actor: "alice", canManage: false, call: api.call, clock: clock)
        await detail.load()
        XCTAssertEqual(detail.titleText, "Write the release notes")
        XCTAssertTrue(detail.facts.contains { $0.label == "Time spent" && $0.value == "1h 30m" })
        XCTAssertEqual(detail.department, "Product design")
        XCTAssertEqual(detail.actions, [.start, .complete])

        let started = await detail.perform(.start)
        XCTAssertEqual(api.last?.suffix, "/\(taskId)/start")
        XCTAssertEqual(started?.message, "Timer started")
        XCTAssertEqual(started?.isError, false)
        XCTAssertEqual(detail.actions, [.stop, .checkin, .complete])

        let finished = await detail.complete(note: "Drafted and posted")
        XCTAssertEqual(api.last?.suffix, "/\(taskId)/complete")
        XCTAssertEqual(api.last?.body, ["completionNote": "Drafted and posted"])
        XCTAssertEqual(finished?.message, "Task finished and sent to review")
        XCTAssertEqual(detail.actions, [.reopen])

        detail.reply = "Posted to #general"
        let replied = await detail.sendReply()
        XCTAssertNil(replied)
        XCTAssertEqual(api.last?.suffix, "/\(taskId)/replies")
        XCTAssertEqual(api.last?.body, ["body": "Posted to #general"])
        XCTAssertEqual(detail.replies.map(detail.replyBody), ["Posted to #general"])
        XCTAssertEqual(detail.reply, "")
    }

    @MainActor
    func testSomeoneElsesTaskOffersNoTimer() async {
        let api = FakeToolsApi { _, _, _ in ["ok": true, "task": .object(taskJSON(assignee: "bob"))] }
        let detail = TaskDetailModel(taskId: taskId, actor: "alice", canManage: false, call: api.call, clock: clock)
        await detail.load()
        XCTAssertFalse(detail.actions.contains(.start))
        XCTAssertFalse(detail.actions.contains(.complete))
        XCTAssertFalse(detail.showsMenu)
    }

    @MainActor
    func testAMemberRemindsTheAssigneeToCheckIn() async {
        let api = FakeToolsApi { _, _, _ in
            ["ok": true, "task": .object(merged(taskJSON(status: "active", assignee: "bob"), [
                "lastReminder": ["at": .int(toolsNowMillis - 120_000), "by": "alice", "note": "any news?"],
            ]))]
        }
        let detail = TaskDetailModel(taskId: taskId, actor: "alice", canManage: false, call: api.call, clock: clock)
        await detail.load()
        XCTAssertEqual(detail.actions, [.remind])
        let cancelled = await detail.remind(note: nil)
        XCTAssertNil(cancelled)
        let notice = await detail.remind(note: "  any news?  ")
        XCTAssertEqual(api.last?.suffix, "/\(taskId)/remind")
        XCTAssertEqual(api.last?.body, ["note": "any news?"])
        XCTAssertEqual(notice?.message, "Reminder sent to @bob")
        XCTAssertEqual(detail.lastReminder, "@alice · 2m ago · any news?")

        let own = TaskDetailModel(taskId: taskId, actor: "bob", canManage: false, call: api.call, clock: clock)
        await own.load()
        XCTAssertFalse(own.actions.contains(.remind))
    }

    @MainActor
    func testTheLastReminderSaysWhetherTheAssigneeSawIt() async {
        var reminder: JSONObject = [
            "at": .int(toolsNowMillis - 120_000), "by": "alice", "to": "bob", "note": "",
            "delivered": true, "phones": .int(2), "seenAt": .int(0), "seenVia": "",
        ]
        let api = FakeToolsApi { _, _, _ in
            ["ok": true, "task": .object(merged(taskJSON(status: "active", assignee: "bob"), [
                "lastReminder": .object(reminder),
            ]))]
        }
        let detail = TaskDetailModel(taskId: taskId, actor: "alice", canManage: false, call: api.call, clock: clock)
        await detail.load()
        XCTAssertEqual(detail.lastReminder, "@alice · 2m ago · Not seen yet: @bob was pinged and pushed to 2 phones")

        reminder["seenAt"] = .int(toolsNowMillis - 60_000)
        reminder["seenVia"] = "ping"
        XCTAssertEqual(detail.reminderStatus(reminder), "Seen: @bob opened the ping 1m ago")

        reminder["seenAt"] = .int(0)
        reminder["delivered"] = false
        XCTAssertEqual(detail.reminderStatus(reminder), "Not delivered: the ping could not be filed")
    }

    @MainActor
    func testCheckinsAndARefusal() async {
        var fail = false
        let api = FakeToolsApi { _, _, _ in
            if fail { throw OrgApiException("assignee_only", "assignee only") }
            return ["ok": true, "task": .object(merged(taskJSON(status: "active"), [
                "lastCheckin": ["state": "needs_help", "at": .int(toolsNowMillis - 120_000), "note": "stuck"],
            ]))]
        }
        let detail = TaskDetailModel(taskId: taskId, actor: "alice", canManage: false, call: api.call, clock: clock)
        await detail.load()
        let notice = await detail.checkin(state: "needs_help", note: "stuck")
        XCTAssertEqual(notice?.message, "Checked in")
        XCTAssertEqual(api.last?.body, ["state": "needs_help", "note": "stuck"])
        XCTAssertEqual(detail.lastCheckin, "Needs help · 2m ago · stuck")
        fail = true
        let refused = await detail.perform(.stop)
        XCTAssertEqual(refused?.message, "Only the assignee can do that.")
        XCTAssertEqual(refused?.isError, true)
    }

    @MainActor
    func testLoadMoreSkipsTasksAlreadyListed() async {
        let api = FakeToolsApi()
        api.handler = { [self] _, suffix, _ in
            if suffix == "?offset=2" {
                return ["ok": true, "hasMore": false, "tasks": [
                    .object(taskJSON()),
                    .object(merged(taskJSON(), ["id": "z", "title": "Later"])),
                ]]
            }
            var page = list([taskJSON()])
            page["hasMore"] = true
            page["nextOffset"] = 2
            return page
        }
        let model = TasksListModel(call: api.call, clock: clock)
        await model.load()
        XCTAssertTrue(model.showsLoadMore)
        let more = await model.loadMore()
        XCTAssertNil(more)
        XCTAssertEqual(model.tasks.count, 2)
        XCTAssertFalse(model.showsLoadMore)
    }

    @MainActor
    func testAManagerPicksAnAssigneeAndEditsOrDeletes() async {
        let api = FakeToolsApi { method, _, body in
            if method == "DELETE" { return ["ok": true] }
            return ["ok": true, "task": .object(merged(taskJSON(), ["title": body?["title"] ?? "Write the release notes"]))]
        }
        let form = TaskFormModel(actor: "alice", canManage: true, members: ["alice", "bob"], departments: [], existing: nil, call: api.call)
        XCTAssertEqual(form.assignees, ["alice", "bob"])
        XCTAssertEqual(form.departments, ["general"])
        XCTAssertEqual(form.assigneeLabel("alice"), "@alice (you)")
        XCTAssertEqual(form.assigneeLabel(""), "Unassigned")
        form.title = "Unowned"
        form.assignee = ""
        _ = await form.submit()
        XCTAssertEqual(api.last?.body?["assigneeKind"], "unassigned")
        XCTAssertNil(api.last?.body?["assignee"])

        let detail = TaskDetailModel(taskId: taskId, actor: "alice", canManage: true, call: api.call, clock: clock)
        await detail.load()
        XCTAssertTrue(detail.showsMenu)
        let edit = detail.editModel()!
        XCTAssertEqual(edit.sheetTitle, "Edit task")
        edit.title = "Renamed"
        let saved = await edit.submit()
        XCTAssertEqual(api.last?.method, "PATCH")
        XCTAssertEqual(api.last?.body, ["title": "Renamed", "details": "Cover notes and tasks on mobile."])
        detail.applyEdited(saved.task!)
        XCTAssertEqual(detail.titleText, "Renamed")
        let deleted = await detail.delete()
        XCTAssertTrue(deleted.close)
        XCTAssertEqual(api.last?.body, [:])
    }

    func testTaskText() {
        XCTAssertEqual(TaskText.elapsed(30_000), "under a minute")
        XCTAssertEqual(TaskText.elapsed(45 * 60000), "45m")
        XCTAssertEqual(TaskText.elapsed(120 * 60000), "2h")
        XCTAssertEqual(TaskText.checkinLabel("going_well"), "Going well")
        XCTAssertEqual(TaskText.checkinLabel("on_hold"), "On hold")
        XCTAssertEqual(TaskText.assigneeLabel(["assigneeKind": "agent"]), "Agent")
        XCTAssertEqual(TaskText.assigneeLabel(["assigneeKind": "user", "assignee": ""]), "Unassigned")
        XCTAssertEqual(TaskText.errorText(ApiException("x"), "fallback"), "fallback")
        XCTAssertEqual(TaskText.errorText(OrgApiException("weird", "Weird thing"), "fallback"), "Weird thing")
    }
}
