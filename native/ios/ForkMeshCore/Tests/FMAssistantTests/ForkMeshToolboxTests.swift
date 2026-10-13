import FMApi
import FMBase
import FMLocalAI
import FMPlanning
import XCTest
@testable import FMAssistant

@MainActor
final class ForkMeshToolboxTests: XCTestCase {
    private let utc = TimeZone(identifier: "UTC")!
    private let nowDate = Date(timeIntervalSince1970: 1_790_000_000)
    private var nowMs: Int64 { Int64(nowDate.timeIntervalSince1970 * 1000) }

    private struct Unavailable: Error, CustomStringConvertible {
        let description: String
    }

    private func task(_ id: String, _ title: String, assignee: String = "ada", status: String = "idle", details: String = "") -> JSONValue {
        .object([
            "id": .string(id), "title": .string(title), "assignee": .string(assignee), "assigneeKind": "user",
            "status": .string(status), "details": .string(details), "updatedAt": .int(nowMs),
        ])
    }

    private func sources(taskPages: [JSONObject]? = nil, calls: Counter = Counter()) -> AssistantSources {
        let pages = taskPages ?? [[
            "actor": "ada",
            "tasks": .array([task("t1", "Ship the release"), task("t2", "Review docs", assignee: "bob"), task("t3", "Old work", status: "done")]),
        ]]
        return AssistantSources(
            account: { AssistantAccount(username: "ada", email: "ada@example.com") },
            orgs: { calls.orgs += 1; return [OrgSummary(name: "forkmesh", role: "owner")] },
            tasks: { offset in
                calls.taskPages.append(offset)
                return pages.first { $0.int("offset") == offset } ?? pages[0]
            },
            task: { id in
                id == "t1" ? ["id": "t1", "title": "Ship the release", "assignee": "ada", "status": "active", "details": "Tag 1.3",
                              "replies": .array([.object(["author": "bob", "body": "On it", "createdAt": .int(self.nowMs)])])] : JSONObject()
            },
            notes: { [["id": "n1", "title": "Launch plan", "updatedAt": .int(self.nowMs)], ["id": "n2", "title": "Groceries"]] },
            note: { id in id == "n1" ? ["id": "n1", "title": "Launch plan", "markdown": "# Launch\nMonday"] : JSONObject() },
            repositories: {
                [
                    Repository(owner: "ada", name: "site", description: "Homepage", updatedMs: 5),
                    Repository(owner: "forkmesh", name: "core", updatedMs: 9),
                    Repository(owner: "zed", name: "other", description: "Someone else's"),
                ]
            },
            issues: { owner, name in
                if owner == "zed" { throw Unavailable(description: "offline") }
                return [Issue(number: 1, title: "\(name) crash on launch", body: "stack"), Issue(number: 2, title: "Old \(name) bug", status: "closed")]
            },
            pulls: { _, _ in [PublishedPull(number: 4, title: "Fix crash", base: "main", head: "fix")] },
            chatConversations: { ["#general", "#random"] },
            chatMessages: {
                [
                    ChatMessage(id: "m2", conversation: "#general", senderId: "b", senderName: "bob", text: "release is tomorrow", timestamp: self.nowDate),
                    ChatMessage(id: "m1", conversation: "@bob", senderId: "b", senderName: "bob", text: "hi", timestamp: self.nowDate.addingTimeInterval(-60)),
                ]
            },
            agents: {
                [
                    AgentLaunchSession(sessionId: "s1", owner: "forkmesh", repo: "core", provider: "claude", status: "running", title: "Fix release crash", updatedAtMs: 2),
                    AgentLaunchSession(sessionId: "s2", owner: "ada", repo: "site", status: "completed", title: "Old run", updatedAtMs: 1, completedAtMs: 1),
                ]
            },
            pings: { _ in
                NotificationPage(notifications: [
                    ForkNotification(id: "p1", kind: "mention", title: "bob mentioned you", actor: "bob", ts: Int(self.nowMs)),
                    ForkNotification(id: "p2", kind: "review", title: "Review merged", ts: 1, readAt: 2),
                ], unread: 1)
            },
            calendar: { start, end in
                calls.calendar.append((start, end))
                return [CalendarEvent(id: "e1", startAt: self.nowMs + 3_600_000, endAt: self.nowMs + 7_200_000, title: "Standup", location: "Office")]
            }
        )
    }

    final class Counter {
        var orgs = 0
        var taskPages: [Int] = []
        var calendar: [(Int64, Int64)] = []
    }

    private func toolbox(_ sources: AssistantSources) -> ForkMeshToolbox {
        ForkMeshToolbox(sources: sources, timeZone: utc, now: { self.nowDate })
    }

    private func run(_ box: ForkMeshToolbox, _ name: String, _ args: JSONObject = [:]) async -> String {
        await box.invoke(LocalToolCall(id: "c", name: name, arguments: args)).text
    }

    func testBriefingSaysWhoAndWhatIsOpen() async {
        let calls = Counter()
        let box = toolbox(sources(calls: calls))
        let text = await box.briefing()
        XCTAssertTrue(text.contains("Signed in as ada (ada@example.com)."), text)
        XCTAssertTrue(text.contains("Organizations: forkmesh (owner)."), text)
        XCTAssertTrue(text.contains("Open tasks assigned to you: 1:"), text)
        XCTAssertTrue(text.contains("[t1] Ship the release"), text)
        XCTAssertTrue(text.contains("Other open tasks on the board: 1."), text)
        XCTAssertTrue(text.contains("Pings: 1 unread:"), text)
        XCTAssertTrue(text.contains("bob mentioned you"), text)
        XCTAssertTrue(text.contains("Agents: 1 running of 2 recent:"), text)
        XCTAssertFalse(text.contains("Old work"), text)

        _ = await box.briefing()
        XCTAssertEqual(calls.taskPages, [0], "a second briefing within a minute is cached")
        XCTAssertEqual(calls.orgs, 1)
    }

    func testBriefingSignedOut() async {
        var src = sources()
        src.account = { nil }
        let text = await toolbox(src).briefing()
        XCTAssertTrue(text.contains("Not signed in"), text)
    }

    func testBriefingExplainsAMissingTaskBoard() async {
        var src = sources()
        src.tasks = { _ in throw Unavailable(description: "OrgApiException(org_member_required)") }
        let text = await toolbox(src).briefing()
        XCTAssertTrue(text.contains("Tasks: the user is not a member of an organization with a task board."), text)
    }

    func testListTasksDefaultsToMineAndFilters() async {
        let box = toolbox(sources())
        let mine = await run(box, "list_tasks")
        XCTAssertTrue(mine.hasPrefix("Tasks (open, assigned to ada): 1"), mine)
        XCTAssertTrue(mine.contains("[t1] Ship the release · open · assigned to ada"), mine)
        XCTAssertFalse(mine.contains("Review docs"))

        let all = await run(box, "list_tasks", ["filter": "all"])
        XCTAssertTrue(all.contains("Review docs") && all.contains("Old work"), all)

        let found = await run(box, "list_tasks", ["filter": "open", "query": "docs"])
        XCTAssertTrue(found.contains("Review docs") && !found.contains("Ship"), found)

        let one = await run(box, "list_tasks", ["id": "t1"])
        XCTAssertTrue(one.contains("in progress") && one.contains("Tag 1.3") && one.contains("bob (2026-09-21): On it"), one)
    }

    func testListTasksReadsLaterPages() async {
        let calls = Counter()
        let pages: [JSONObject] = [
            ["offset": 0, "actor": "ada", "hasMore": true, "nextOffset": 1, "tasks": .array([task("t1", "First")])],
            ["offset": 1, "actor": "ada", "hasMore": false, "tasks": .array([task("t2", "Second")])],
        ]
        let text = await run(toolbox(sources(taskPages: pages, calls: calls)), "list_tasks")
        XCTAssertEqual(calls.taskPages, [0, 1])
        XCTAssertTrue(text.contains("First") && text.contains("Second"), text)
    }

    func testNotesByTitleReadTheBody() async {
        let box = toolbox(sources())
        let list = await run(box, "read_notes")
        XCTAssertTrue(list.contains("[n1] Launch plan") && list.contains("[n2] Groceries"), list)
        let body = await run(box, "read_notes", ["title": "launch"])
        XCTAssertTrue(body.contains("Note \"Launch plan\" [n1]") && body.contains("# Launch\nMonday"), body)
    }

    func testRepositoriesAndIssuesCoverTheUserAndTheirOrgs() async {
        let box = toolbox(sources())
        let repos = await run(box, "list_repositories")
        XCTAssertTrue(repos.contains("forkmesh/core") && repos.contains("ada/site") && !repos.contains("zed/other"), repos)

        let issues = await run(box, "list_issues")
        XCTAssertTrue(issues.contains("forkmesh/core#1 core crash on launch") && issues.contains("ada/site#1"), issues)
        XCTAssertFalse(issues.contains("Old"), issues)

        let one = await run(box, "list_issues", ["repository": "site", "number": 1])
        XCTAssertTrue(one.hasPrefix("Issue ada/site#1") && one.contains("stack"), one)

        let failed = await run(box, "list_issues", ["repository": "zed/other"])
        XCTAssertTrue(failed.contains("Could not read: zed/other: offline"), failed)

        let pulls = await run(box, "list_pull_requests", ["repository": "forkmesh/core"])
        XCTAssertTrue(pulls.contains("forkmesh/core#4 Fix crash · open · fix → main"), pulls)
    }

    func testChatAgentsPingsAndCalendar() async {
        let calls = Counter()
        let box = toolbox(sources(calls: calls))
        let chat = await run(box, "read_chat", ["conversation": "general"])
        XCTAssertTrue(chat.contains("#general bob: release is tomorrow") && !chat.contains("hi"), chat)

        let agents = await run(box, "list_agents", ["running_only": true])
        XCTAssertTrue(agents.contains("Fix release crash") && !agents.contains("Old run"), agents)

        let pings = await run(box, "list_pings", ["unread_only": true])
        XCTAssertTrue(pings.contains("bob mentioned you") && !pings.contains("Review merged"), pings)

        let calendar = await run(box, "list_calendar", ["days_ahead": 2])
        XCTAssertTrue(calendar.contains("Standup @ Office"), calendar)
        XCTAssertEqual(calls.calendar.count, 1)
    }

    func testSearchSpansEverySource() async {
        let text = await run(toolbox(sources()), "search_forkmesh", ["query": "release"])
        for section in ["Tasks", "Chat", "Agent runs"] {
            XCTAssertTrue(text.contains(section), "\(section) missing from \(text)")
        }
        let none = await run(toolbox(sources()), "search_forkmesh", ["query": "zzz"])
        XCTAssertTrue(none.hasPrefix("Nothing in tasks"), none)
    }

    func testUnknownToolNamesTheRealOnes() async {
        let text = await run(toolbox(sources()), "delete_everything")
        XCTAssertTrue(text.contains("There is no tool named delete_everything") && text.contains("list_tasks"), text)
    }

    func testSpecsAreObjectSchemas() {
        let names = ForkMeshToolbox.specs.map(\.name)
        XCTAssertEqual(Set(names).count, names.count)
        XCTAssertEqual(names.count, 21)
        for spec in ForkMeshToolbox.specs {
            XCTAssertEqual(spec.parameters.str("type"), "object", spec.name)
            XCTAssertNotNil(spec.parameters["properties"]?.objectValue, spec.name)
        }
    }
}
