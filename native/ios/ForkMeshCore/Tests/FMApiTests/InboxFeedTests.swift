import FMBase
import Foundation
import XCTest

@testable import FMApi

@MainActor
final class FakeInboxClient: InboxClient {
    var calls: [(method: String, path: String, body: JSONObject?)] = []
    var pings: [JSONValue] = []
    var pauseUntil: Int64 = 0
    var tasks: JSONObject = ["ok": true, "actor": "alice", "tasks": .array([])]
    var offline = false
    var tasksError: Error?

    func accountApiCall(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject {
        calls.append((method, path, body))
        if offline { throw OrgApiException("http_0", "offline") }
        if method == "GET" {
            return ["ok": true, "notifications": .array(pings), "unread": 0, "total": .int(Int64(pings.count)),
                    "pauseUntil": .int(pauseUntil)]
        }
        if method == "DELETE", body?["all"] == true { pings.removeAll() }
        if method == "DELETE", let id = body?.str("id") { pings.removeAll { $0.objectValue?.str("id") == id } }
        if let until = body?["pauseUntil"] { return ["ok": true, "pauseUntil": until] }
        return ["ok": true]
    }

    func tasksApi(_ method: String, _ suffix: String, _ body: JSONObject?) async throws -> JSONObject {
        if let tasksError { throw tasksError }
        if offline { throw OrgApiException("http_0", "offline") }
        return tasks
    }
}

final class InboxFeedTests: XCTestCase {
    private static func ping(_ id: String, kind: String, ts: Int, read: Bool = false, href: String = "", repo: String = "",
                             meta: JSONObject = [:]) -> JSONValue {
        .object([
            "id": .string(id), "kind": .string(kind), "title": .string("\(kind) \(id)"), "body": "b", "repo": .string(repo),
            "href": .string(href), "actor": "bob", "ts": .int(Int64(ts)), "readAt": .int(read ? 5 : 0), "meta": .object(meta),
        ])
    }

    func testCategoriesAndRoutes() {
        XCTAssertEqual(InboxCategory.of(kind: "agent_approval"), .approval)
        XCTAssertEqual(InboxCategory.of(kind: "pull_submitted"), .review)
        XCTAssertEqual(InboxCategory.of(kind: "issue_assigned"), .assigned)
        XCTAssertEqual(InboxCategory.of(kind: "release_published"), .ping)
        let repo = RepoRef(owner: "acme", name: "app")
        XCTAssertEqual(InboxRoutes.route(kind: "pull_submitted", href: "", meta: ["number": 4], repo: "acme/app"),
                       .pull(repo, number: 4, tab: .conversation))
        XCTAssertEqual(InboxRoutes.route(kind: "issue_assigned", href: "", meta: [:], repo: "acme/app"), .repo(repo, section: .issues))
        XCTAssertEqual(InboxRoutes.route(kind: "mention", href: "/acme/app/issues/9", meta: [:], repo: ""), .issue(repo, number: 9))
        XCTAssertEqual(InboxRoutes.route(kind: "repo_activity", href: "/acme/app/commits/feature/x", meta: [:], repo: ""),
                       .commits(repo, ref: "feature/x"))
        XCTAssertEqual(InboxRoutes.route(kind: "agent_approval", href: "", meta: ["sessionId": "s:1", "approvalId": "a.2"], repo: ""),
                       .approval(id: "s:1/a.2"))
        XCTAssertEqual(InboxRoutes.route(kind: "desktop_ping", href: "/acme/app/agents/12", meta: ["sessionId": "12"], repo: "acme/app"),
                       .agent(id: "12"))
        XCTAssertEqual(InboxRoutes.route(kind: "desktop_ping", href: "", meta: [:], repo: "acme/app"), .repo(repo, section: nil))
        XCTAssertEqual(InboxRoutes.approvalParts("s:1/a.2")?.approvalId, "a.2")
        XCTAssertNil(InboxRoutes.approvalParts("nope"))
        XCTAssertNil(InboxRoutes.route(kind: "credits_refilled", href: "https://evil.example", meta: [:], repo: ""))
    }

    func testEveryPingKeepsItsKindColourWhenRead() {
        let tone = { (kind: String, read: Bool) -> PingTone in
            InboxItem(ping: ForkNotification(json: InboxFeedTests.ping("1", kind: kind, ts: 1, read: read).objectValue!)).tone
        }
        XCTAssertEqual(tone("error_group", true), .danger)
        XCTAssertEqual(tone("host_online", true), .success)
        XCTAssertEqual(tone("host_offline", false), .warning)
        XCTAssertEqual(tone("agent_approval", true), .warning)
        XCTAssertEqual(tone("mention", true), .info)
        XCTAssertEqual(tone("pull_submitted", true), .accent)
        XCTAssertEqual(tone("issue_assigned", true), .warning)
        XCTAssertEqual(tone("organization_task_started", true), .accent)
        XCTAssertEqual(tone("a_future_kind", false), .neutral)
        XCTAssertEqual(notificationToneForKind(" Release_Published "), .accent)
        let local = InboxItem(id: "local:1", source: .local, category: .ping, kind: "local_failure", title: "Sync failed")
        XCTAssertEqual(local.tone, .danger)
    }

    func testMergePinsUnreadApprovalsFiltersAndCounts() {
        let pings = [
            InboxItem(ping: ForkNotification(json: InboxFeedTests.ping("1", kind: "mention", ts: 50).objectValue!)),
            InboxItem(ping: ForkNotification(json: InboxFeedTests.ping("2", kind: "agent_approval", ts: 10,
                                                                       meta: ["sessionId": "s", "approvalId": "a"]).objectValue!)),
            InboxItem(ping: ForkNotification(json: InboxFeedTests.ping("3", kind: "pull_submitted", ts: 40, read: true).objectValue!)),
        ]
        let task = InboxFeed.taskItem(["id": "t1", "title": "Do", "assigneeKind": "user", "assignee": "Alice", "status": "idle",
                                       "updatedAt": 30], actor: "alice")!
        XCTAssertNil(InboxFeed.taskItem(["id": "t2", "assigneeKind": "user", "assignee": "bob", "status": "idle"], actor: "alice"))
        XCTAssertNil(InboxFeed.taskItem(["id": "t3", "assigneeKind": "user", "assignee": "alice", "status": "done"], actor: "alice"))
        let merged = InboxFeed.merge(pings: pings + [pings[0]], tasks: [task], local: [])
        XCTAssertEqual(merged.map(\.id), ["ping:2", "ping:1", "ping:3", "task:t1@30"])
        XCTAssertEqual(merged[0].route, .approval(id: "s/a"))
        XCTAssertEqual(InboxFeed.filter(merged, .assigned).map(\.id), ["task:t1@30"])
        XCTAssertEqual(InboxFeed.filter(merged, .pings).count, 3)
        XCTAssertEqual(InboxFeed.filter(merged, .reviews, unreadOnly: true).count, 0)
        let counts = InboxFeed.unreadCounts(merged)
        XCTAssertEqual(counts[.all], 3)
        XCTAssertEqual(counts[.approvals], 1)
        XCTAssertEqual(counts[.mentions], 1)
    }

    @MainActor func testModelLoadsMarksReadAndQueuesOffline() async {
        let client = FakeInboxClient()
        client.pings = [
            InboxFeedTests.ping("p1", kind: "mention", ts: 100, repo: "acme/app", meta: ["number": 2]),
            InboxFeedTests.ping("p2", kind: "issue_assigned", ts: 90),
        ]
        client.tasks = ["ok": true, "actor": "alice", "tasks": .array([
            .object(["id": "t1", "title": "Ship", "assigneeKind": "user", "assignee": "alice", "status": "active", "updatedAt": 95]),
        ])]
        let store = InMemoryKeyValueStore()
        let clock = FixedClock(epochMillis: 1_000_000)
        let model = InboxModel(client: client, node: { "Alice" }, store: store, clock: clock)
        await model.load()
        XCTAssertEqual(model.items.map(\.id), ["ping:p1", "task:t1@95", "ping:p2"])
        XCTAssertTrue(client.calls[0].path.hasPrefix("/api/notifications?node=alice&limit=100"))
        XCTAssertEqual(model.unreadTotal, 3)
        model.filter = .assigned
        XCTAssertEqual(model.visible.map(\.id), ["task:t1@95", "ping:p2"])

        await model.markRead(model.items[1])
        let reopened = InboxModel(client: client, node: { "alice" }, store: store, clock: clock)
        await reopened.load()
        XCTAssertEqual(reopened.items.first { $0.source == .task }?.unread, false)

        client.offline = true
        await model.markRead(model.items[0])
        XCTAssertEqual(model.items[0].sync, .pending)
        XCTAssertEqual(model.pendingSyncCount, 1)
        await model.delete(model.items.first { $0.id == "ping:p2" }!)
        XCTAssertEqual(model.pendingSyncCount, 2)
        client.offline = false
        client.calls.removeAll()
        await model.load()
        XCTAssertEqual(model.pendingSyncCount, 0)
        XCTAssertEqual(client.calls.prefix(2).map(\.method), ["POST", "DELETE"])
    }

    @MainActor func testAResumeDuringOrJustAfterALoadDoesNotReadAgain() async {
        let client = FakeInboxClient()
        client.pings = [InboxFeedTests.ping("p1", kind: "mention", ts: 100)]
        let clock = FixedClock(epochMillis: 1_000_000)
        let model = InboxModel(client: client, node: { "alice" }, store: InMemoryKeyValueStore(), clock: clock)
        func reads() -> Int { client.calls.filter { $0.method == "GET" }.count }

        async let first: Void = model.loadIfStale()
        async let resume: Void = model.loadIfStale()
        _ = await (first, resume)
        XCTAssertEqual(reads(), 1)
        XCTAssertEqual(model.items.map(\.id), ["ping:p1"])

        clock.advance(by: 5)
        await model.loadIfStale()
        XCTAssertEqual(reads(), 1)

        await model.load()
        XCTAssertEqual(reads(), 2, "pull to refresh always reads")

        clock.advance(by: 16)
        await model.loadIfStale()
        XCTAssertEqual(reads(), 3)
    }

    @MainActor func testOfflineOperationsSurviveReloadAndHideDeletedPings() async {
        let client = FakeInboxClient()
        client.pings = [InboxFeedTests.ping("p1", kind: "mention", ts: 1), InboxFeedTests.ping("p2", kind: "mention", ts: 2)]
        let store = InMemoryKeyValueStore()
        let model = InboxModel(client: client, node: { "alice" }, store: store)
        await model.load()
        client.offline = true
        await model.delete(model.items.first { $0.remoteId == "p1" }!)
        await model.markRead(model.items.first { $0.remoteId == "p2" }!)
        await model.load()
        client.offline = false
        let queue = PingSyncQueue(store: store)
        XCTAssertEqual(queue.operations.count, 2)
        XCTAssertEqual(queue.operations.first { $0.remoteId == "p1" }?.attempts, 1)
        let after = InboxModel(client: client, node: { "alice" }, store: store)
        client.offline = true
        await after.load()
        XCTAssertTrue(after.errors.contains("Pings could not be loaded."))
        client.offline = false
        client.calls.removeAll()
        await after.load()
        XCTAssertNil(after.item(remoteId: "p1"))
    }

    @MainActor func testDeleteAllPingsClearsTasksToo() async {
        let client = FakeInboxClient()
        client.pings = [InboxFeedTests.ping("p1", kind: "mention", ts: 5), InboxFeedTests.ping("p2", kind: "code_push", ts: 4)]
        client.tasks = ["ok": true, "actor": "alice", "tasks": .array([
            .object(["id": "t1", "title": "Ship", "assigneeKind": "user", "assignee": "alice", "status": "active", "updatedAt": 3]),
        ])]
        let store = InMemoryKeyValueStore()
        let local = LocalPingLog(store: store, clock: FixedClock(epochMillis: 0))
        local.record(surface: "git", title: "Push failed", message: "no route")
        let model = InboxModel(client: client, node: { "alice" }, store: store, localPings: local)
        await model.load()
        XCTAssertTrue(model.canDeleteAllPings)
        client.offline = true
        await model.delete(model.items.first { $0.id == "ping:p2" }!)
        XCTAssertEqual(model.pendingSyncCount, 1)
        let refused = await model.deleteAllPings()
        XCTAssertEqual(refused, "Pings could not be deleted.")
        XCTAssertEqual(model.items.count, 3)
        client.offline = false
        var changed = 0
        model.onChanged = { changed += 1 }
        let failure = await model.deleteAllPings()
        XCTAssertNil(failure)
        XCTAssertEqual(client.calls.last?.method, "DELETE")
        XCTAssertEqual(client.calls.last?.body?["all"], true)
        XCTAssertEqual(client.calls.last?.body?["node"], "alice")
        XCTAssertEqual(model.items.map(\.id), [])
        XCTAssertEqual(model.pendingSyncCount, 0)
        XCTAssertTrue(PingSyncQueue(store: store).operations.isEmpty)
        XCTAssertTrue(LocalPingLog(store: store).pings.isEmpty)
        XCTAssertEqual(changed, 1)
        XCTAssertFalse(model.canDeleteAllPings)
        await model.load()
        XCTAssertEqual(model.items.map(\.id), [])
        let reopened = InboxModel(client: client, node: { "alice" }, store: store)
        await reopened.load()
        XCTAssertEqual(reopened.items.map(\.id), [])
        client.tasks = ["ok": true, "actor": "alice", "tasks": .array([
            .object(["id": "t1", "title": "Ship", "assigneeKind": "user", "assignee": "alice", "status": "active", "updatedAt": 9]),
        ])]
        await model.load()
        XCTAssertEqual(model.items.map(\.id), ["task:t1@9"])
        XCTAssertTrue(model.canDeleteAllPings)
        let guestStore = InMemoryKeyValueStore()
        let guestLocal = LocalPingLog(store: guestStore)
        guestLocal.record(surface: "git", title: "Push failed", message: "no route")
        let guest = InboxModel(client: client, node: { "" }, store: guestStore, localPings: guestLocal)
        let before = client.calls.count
        _ = await guest.deleteAllPings()
        XCTAssertEqual(client.calls.count, before)
        XCTAssertTrue(guestLocal.pings.isEmpty)
    }

    @MainActor func testTasksForbiddenIsQuietAndPauseRoundTrips() async {
        let client = FakeInboxClient()
        client.tasksError = OrgApiException("org_member_required", "org member required")
        let clock = FixedClock(epochMillis: 10_000_000)
        let model = InboxModel(client: client, node: { "alice" }, store: InMemoryKeyValueStore(), clock: clock)
        await model.load()
        XCTAssertTrue(model.errors.isEmpty)
        let failure = await model.pause(.oneHour)
        XCTAssertNil(failure)
        XCTAssertEqual(client.calls.last?.body?["pauseUntil"], .int(10_000_000 + 3_600_000))
        XCTAssertTrue(model.isPaused)
        XCTAssertEqual(model.pauseLabel, "Pings paused for 1 more hour")
        _ = await model.pause(nil)
        XCTAssertFalse(model.isPaused)
        let signedOut = InboxModel(client: client, node: { "" }, store: InMemoryKeyValueStore())
        await signedOut.load()
        XCTAssertEqual(signedOut.errors, ["Sign in to see your inbox."])
    }

    @MainActor func testLocalPingsRedactDedupeAndRateLimit() {
        let clock = FixedClock(epochMillis: 50_000_000)
        var next = 0
        let log = LocalPingLog(store: InMemoryKeyValueStore(), clock: clock, makeId: {
            next += 1
            return "l\(next)"
        })
        let first = log.record(surface: "clone", title: "Clone failed", message: "Authorization: Bearer abcdefghijk token=supersecret /Users/me/x")
        XCTAssertEqual(first?.message.contains("abcdefghijk"), false)
        XCTAssertEqual(first?.message.contains("supersecret"), false)
        XCTAssertEqual(first?.message.contains("/Users/me"), false)
        XCTAssertEqual(log.record(surface: "clone", title: "Clone failed", message: "Authorization: Bearer abcdefghijk token=supersecret /Users/me/x")?.id, "l1")
        for index in 0..<10 { log.record(surface: "s", title: "t\(index)", message: "m") }
        XCTAssertEqual(log.pings.count, LocalPingLog.maxPerHour)
        XCTAssertEqual(log.items.first?.sync, .localOnly)
        log.markAllRead()
        XCTAssertTrue(log.items.allSatisfy { !$0.unread })
        clock.advance(by: 3_700)
        XCTAssertNotNil(log.record(surface: "s", title: "later", message: "m"))
    }

    @MainActor func testChatPingsShowInTheInboxAsTheyArrive() async {
        let store = InMemoryKeyValueStore()
        let local = LocalPingLog(store: store)
        let client = FakeInboxClient()
        let model = InboxModel(client: client, node: { "alice" }, store: store, localPings: local)
        await model.load()
        XCTAssertEqual(model.items.map(\.id), [])
        local.recordChat(
            messageId: "m1", sender: "Pat", title: "Pat in #general", message: "lunch?", conversation: "#general", tsMs: 5,
            read: false)
        XCTAssertEqual(model.items.map(\.id), ["local:chat:m1"])
        XCTAssertEqual(model.unreadCounts[.pings], 1)
        XCTAssertNil(local.recordChat(
            messageId: "m1", sender: "Pat", title: "Pat in #general", message: "lunch?", conversation: "#general", tsMs: 5,
            read: false))
        local.markConversationRead("#general")
        XCTAssertEqual(model.unreadCounts[.pings], 0)
        await model.delete(model.items[0])
        XCTAssertTrue(local.pings.isEmpty)
        XCTAssertFalse(client.calls.contains { $0.method == "DELETE" })
    }

    @MainActor func testReportedLocalPingUpdatesInbox() async {
        let store = InMemoryKeyValueStore()
        let local = LocalPingLog(store: store)
        let model = InboxModel(client: FakeInboxClient(), node: { "alice" }, store: store, localPings: local)
        await model.load()
        let ping = local.record(surface: "genie", title: "The spider said", message: "Opened in Zoho Mail.")
        XCTAssertEqual(model.items.map(\.id), ["local:\(ping!.id)"])
        XCTAssertEqual(model.unreadCounts[.pings], 1)
    }

    @MainActor func testPromptForPingAndTask() {
        let model = InboxModel(client: FakeInboxClient(), node: { "alice" }, store: InMemoryKeyValueStore())
        let item = InboxItem(id: "task:t", source: .task, category: .assigned, kind: "organization_task", title: "Ship", body: "Now",
                             repo: "acme/app")
        XCTAssertEqual(model.prompt(for: item), ComposeTarget(repo: RepoRef(owner: "acme", name: "app"), text: "Ship\n\nNow"))
    }

    func testNotificationCategoriesAndCommands() {
        XCTAssertEqual(NotificationCategoryId.agentApproval.actions, [.approve, .deny, .open])
        XCTAssertEqual(NotificationCategoryId.forPing(kind: "agent_approval"), .agentApproval)
        XCTAssertEqual(NotificationCategoryId.forPing(kind: "x", topic: "direct-messages"), .dm)
        XCTAssertTrue(NotificationActionId.approve.requiresAuthentication)
        XCTAssertTrue(NotificationActionId.merge.isDestructive)
        XCTAssertTrue(NotificationActionId.reply.isTextInput)

        let approval = NotificationPayload(category: "agent-approval", fields: ["sessionId": "s1", "approvalId": "a1", "alertId": "x"])
        XCTAssertEqual(NotificationActionRouter.command(actionId: "fm.approve", payload: approval, text: nil),
                       .approve(sessionId: "s1", approvalId: "a1"))
        XCTAssertEqual(NotificationActionRouter.command(actionId: "fm.deny", payload: approval, text: nil),
                       .deny(sessionId: "s1", approvalId: "a1"))
        XCTAssertEqual(NotificationActionRouter.command(actionId: "com.apple.UNNotificationDefaultActionIdentifier", payload: approval, text: nil),
                       .open(.approval(id: "s1/a1"), href: ""))
        let broken = NotificationPayload(category: "agent-approval", fields: [:])
        XCTAssertEqual(NotificationActionRouter.command(actionId: "fm.approve", payload: broken, text: nil), .ignore(reason: "no approval"))

        let dm = NotificationPayload(category: "dm", fields: ["channel": "bob", "alertId": "n1"])
        XCTAssertEqual(NotificationActionRouter.command(actionId: "fm.reply", payload: dm, text: "  hi "), .reply(channel: "bob", text: "hi"))
        XCTAssertEqual(NotificationActionRouter.command(actionId: "fm.reply", payload: dm, text: " "), .ignore(reason: "empty reply"))
        XCTAssertEqual(NotificationActionRouter.route(for: dm), .dm(user: "bob"))
        XCTAssertEqual(NotificationActionRouter.command(actionId: "fm.markRead", payload: dm, text: nil), .markRead(alertId: "n1"))

        let ci = NotificationPayload(category: "ci-failed", fields: ["repo": "acme/app", "runId": "7"])
        XCTAssertEqual(NotificationActionRouter.route(for: ci), .run(RepoRef(owner: "acme", name: "app"), id: "7"))
        if case .compose(let target) = NotificationActionRouter.command(actionId: "fm.sendToAgent", payload: ci, text: nil) {
            XCTAssertEqual(target.repo, RepoRef(owner: "acme", name: "app"))
        } else {
            XCTFail("expected a compose command")
        }
        let pr = NotificationPayload(category: "pr-review", fields: ["repo": "acme/app", "number": "5"])
        XCTAssertEqual(NotificationActionRouter.command(actionId: "fm.approvePull", payload: pr, text: nil),
                       .approvePull(RepoRef(owner: "acme", name: "app"), number: 5))
        let finished = NotificationPayload(category: "agent-finished", fields: ["sessionId": "s9"])
        XCTAssertEqual(NotificationActionRouter.command(actionId: "fm.continue", payload: finished, text: "go on"),
                       .continueRun(sessionId: "s9", text: "go on"))
        XCTAssertEqual(NotificationActionRouter.command(actionId: "fm.merge", payload: finished, text: nil), .merge(sessionId: "s9"))
        let ping = NotificationPayload(category: "", fields: ["kind": "release_published", "href": "/acme/app/releases"])
        XCTAssertEqual(ping.category, .ping)
        XCTAssertEqual(NotificationActionRouter.route(for: ping), .repo(RepoRef(owner: "acme", name: "app"), section: .releases))
    }
}
