// Drives HomeShellModel (page selection, the prompt bar, push
// routing) and NotificationsScreenModel (delete,
// refusals, tap targets) over a real NotificationService and ApiService on
// FakeHTTPClient.

import FMAccount
import FMBase
import FMNet
import XCTest
@testable import FMApi

@MainActor
private final class FakeChat: ShellChatBridge {
    var relayState: ShellRelayState = .connected
    var onlineCount = 3
    var totalUnread = 4
    var badgeUnread = 2
    var currentConversation = "#general"
    var unreadConversations: [String] = []
    var markedBadgeSeen = 0
    var switched: [String] = []
    var connects = 0

    func connect() { connects += 1 }
    func markBadgeSeen() { markedBadgeSeen += 1 }
    func switchConversation(_ conversation: String) { switched.append(conversation) }
}

private func inboxPage(_ rows: [JSONValue], unread: Int) -> HTTPResponse {
    HTTPResponse(status: 200, json: ["notifications": .array(rows), "unread": .int(Int64(unread))])
}

private let issueRow: JSONValue = [
    "id": "a1", "kind": "mention", "title": "Mentioned", "repo": "mona/forkmesh", "href": "/mona/forkmesh/issues/7",
    "ts": 1_770_000_000_000,
]
private let desktopAgentRow: JSONValue = [
    "id": "d1", "kind": "desktop_ping", "title": "Agent finished", "repo": "", "href": "",
    "meta": ["node": "desk", "category": "agent-finished", "sessionId": "12"], "ts": 1_770_000_000_000,
]
private let bareRow: JSONValue = ["id": "a2", "kind": "host_offline", "title": "Host offline", "href": "https://example.com/x"]

@MainActor
private func makeInbox(_ http: FakeHTTPClient) -> (ApiService, NotificationService) {
    let api = ApiService(settings: StaticApiSettings(), http: http, sleep: { _ in })
    api.sessionTokenProvider = { "session" }
    let service = NotificationService(api: api, nodeName: { "alice" })
    return (api, service)
}

final class ShellInboxViewModelTests: XCTestCase {
    @MainActor
    func testSelectingPagesKeepsThePromptOnAgentsAndClearsTheChatBadge() async {
        let chat = FakeChat()
        let shell = HomeShellModel(chat: chat, notifications: nil)
        XCTAssertEqual(shell.destination.label, "Agents")
        XCTAssertEqual(shell.promptHint, "Start a new agent...")
        shell.select(HomeShellCatalog.chatIndex)
        XCTAssertEqual(shell.promptHint, "Start a new agent...")
        shell.select(HomeShellCatalog.codeIndex)
        shell.select(HomeShellCatalog.chatIndex)
        XCTAssertEqual(chat.markedBadgeSeen, 2)
        XCTAssertEqual(shell.promptHint, "Start a new agent...")
        XCTAssertEqual(shell.relayState, .connected)
        XCTAssertEqual(shell.chatUnread, 2)
    }

    @MainActor
    func testThePromptOpensTheAgentFormOnEveryPage() async {
        let chat = FakeChat()
        let shell = HomeShellModel(chat: chat, notifications: nil)
        shell.prompt = "   "
        let blank = await shell.submitPrompt()
        XCTAssertEqual(blank, .ignored)

        shell.select(HomeShellCatalog.chatIndex)
        shell.prompt = "again"
        let agent = await shell.submitPrompt()
        XCTAssertEqual(agent, .startAgent("again"))
        XCTAssertEqual(shell.prompt, "again")
        shell.agentStarted()
        XCTAssertEqual(shell.prompt, "")
        XCTAssertEqual(shell.agentsGeneration, 1)
        XCTAssertEqual(shell.index, HomeShellCatalog.agentsIndex)

        let offline = HomeShellModel(chat: nil, notifications: nil)
        offline.prompt = "x"
        let started = await offline.submitPrompt()
        XCTAssertEqual(started, .startAgent("x"))
        XCTAssertEqual(offline.relayState, .offline)
    }

    @MainActor
    func testAPagePromptTargetTakesTheSendWhileRegistered() async {
        let chat = FakeChat()
        let targets = ShellPromptTargets()
        let shell = HomeShellModel(chat: chat, notifications: nil, targets: targets)
        var received: [String] = []
        let target = ShellPromptTarget(
            id: "calories", label: "Log", icon: "restaurant_outlined", title: "Log to your diary",
            placeholder: "What did you eat?",
            submit: { (text: String) in
                received.append(text)
                if text == "boom" { throw ShellPromptFailure("Could not log.") }
                return "Logged \(text)."
            })
        let token = targets.register(target)
        XCTAssertEqual(shell.promptHint, "What did you eat?")
        XCTAssertEqual(shell.promptModeLabel, "Log to your diary. Switch to agent")

        shell.prompt = "  2 eggs "
        let logged = await shell.submitPrompt()
        XCTAssertEqual(logged, .handled("Logged 2 eggs."))
        XCTAssertEqual(received, ["2 eggs"])
        XCTAssertEqual(shell.prompt, "")

        shell.prompt = "boom"
        let failed = await shell.submitPrompt()
        XCTAssertEqual(failed, .failed("Could not log."))
        XCTAssertEqual(shell.prompt, "boom")
        XCTAssertFalse(shell.promptBusy)

        shell.togglePromptMode()
        XCTAssertNil(shell.promptTarget)
        XCTAssertEqual(shell.promptHint, "Start a new agent...")
        XCTAssertEqual(shell.promptModeLabel, "Prompt starts an agent. Switch to Log")
        let agent = await shell.submitPrompt()
        XCTAssertEqual(agent, .startAgent("boom"))
        shell.togglePromptMode()
        XCTAssertEqual(shell.promptTarget?.id, "calories")

        targets.unregister(token)
        XCTAssertNil(shell.promptTarget)
        shell.togglePromptMode()
        XCTAssertNil(shell.promptTarget)
        XCTAssertEqual(shell.promptHint, "Start a new agent...")
        XCTAssertEqual(shell.promptModeLabel, "Prompt starts an agent")
    }

    @MainActor
    func testAChatPushOpensTheConversationWithUnread() async {
        let chat = FakeChat()
        chat.unreadConversations = ["#mesh", "@bob", "#dev"]
        let shell = HomeShellModel(chat: chat, notifications: nil)
        let route = await shell.routePush(isChat: false, isDirectMessage: true, alertId: "", href: "")
        XCTAssertEqual(route, .chat)
        XCTAssertEqual(chat.switched, ["@bob"])
        XCTAssertEqual(shell.index, HomeShellCatalog.chatIndex)
        let room = await shell.routePush(isChat: true, isDirectMessage: false, alertId: "", href: "")
        XCTAssertEqual(room, .chat)
        XCTAssertEqual(chat.switched, ["@bob", "#dev"])
    }

    @MainActor
    func testAChatPushOpensTheConversationItNamesBeforeAnyUnreadArrives() async {
        let chat = FakeChat()
        let shell = HomeShellModel(chat: chat, notifications: nil)
        let direct = "direct:" + String(repeating: "0f", count: 16)
        let open = PushOpen(arguments: ["topic": "direct-messages", "conversation": direct])
        XCTAssertEqual(open.conversation, direct)
        let route = await shell.routePush(
            isChat: open.isChat, isDirectMessage: open.isDirectMessage, alertId: open.alertId, href: open.href,
            conversation: open.conversation)
        XCTAssertEqual(route, .chat)
        XCTAssertEqual(chat.switched, [direct])
        let room = await shell.routePush(
            isChat: true, isDirectMessage: false, alertId: "", href: "", conversation: "#office/design")
        XCTAssertEqual(room, .chat)
        XCTAssertEqual(chat.switched, [direct, "#office/design"])
    }

    @MainActor
    func testAnInboxPushReadsTheRowMarksItReadAndOpensItsRecord() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/notifications") { _ in inboxPage([issueRow], unread: 1) }
        http.on("POST", path: "/api/notifications") { _ in HTTPResponse(status: 200, json: ["ok": true]) }
        let (_, inbox) = makeInbox(http)
        let shell = HomeShellModel(chat: FakeChat(), notifications: inbox)
        let route = await shell.routePush(isChat: false, isDirectMessage: false, alertId: "a1", href: "")
        guard case .repo(let link)? = route else { return XCTFail("expected the issue, got \(String(describing: route))") }
        XCTAssertEqual(link.number, 7)
        XCTAssertEqual(link.initialTab, .issues)
        for _ in 0..<50 where !http.requests.contains(where: { $0.method == "POST" }) { await Task.yield() }
        XCTAssertTrue(http.requests.contains { $0.method == "POST" && $0.bodyText.contains("a1") })
        XCTAssertEqual(inbox.unreadCount, 0)

        let nothing = await shell.routePush(isChat: false, isDirectMessage: false, alertId: "", href: "")
        XCTAssertEqual(nothing, .notifications)
        let byHref = await shell.routePush(isChat: false, isDirectMessage: false, alertId: "", href: "/mona/forkmesh/pulls/2")
        XCTAssertEqual(byHref, .repo(NotificationDeepLink.parse("/mona/forkmesh/pulls/2")!))
    }

    @MainActor
    func testAPushOpensItsPingFromTheBannerWithoutWaitingOnTheInbox() async {
        let http = FakeHTTPClient()
        http.on("POST", path: "/api/notifications") { _ in HTTPResponse(status: 200, json: ["ok": true]) }
        let store = InMemoryKeyValueStore()
        let api = ApiService(settings: StaticApiSettings(), http: http, sleep: { _ in })
        api.sessionTokenProvider = { "session" }
        let inbox = NotificationService(api: api, nodeName: { "alice" }, snapshot: PingSnapshot(store: store))
        let shell = HomeShellModel(chat: FakeChat(), notifications: inbox)
        let open = PushOpen(arguments: [
            "alertId": "p9", "title": "Host offline", "subtitle": "by mona", "body": "fm-1 went quiet",
            "kind": "host_offline", "receivedMs": "1770000000000",
        ])
        let route = await shell.routePush(
            isChat: false, isDirectMessage: false, alertId: open.alertId, href: open.href, ping: open.ping)
        XCTAssertEqual(route, .ping("p9"))
        XCTAssertFalse(http.requests.contains { $0.method == "GET" })
        let row = try? XCTUnwrap(inbox.notifications.first)
        XCTAssertEqual(row?.title, "Host offline")
        XCTAssertEqual(row?.body, "fm-1 went quiet")
        XCTAssertEqual(row?.actor, "mona")
        XCTAssertEqual(row?.kind, "host_offline")
        XCTAssertEqual(row?.isUnread, false)
        XCTAssertEqual(PingSnapshot(store: store).pings(for: "alice").map(\.id), ["p9"])
        for _ in 0..<50 where !http.requests.contains(where: { $0.method == "POST" }) { await Task.yield() }
        XCTAssertTrue(http.requests.contains { $0.method == "POST" && $0.bodyText.contains("p9") })

        let withRecord = PushOpen(arguments: [
            "alertId": "p10", "title": "#7 Fix it", "subtitle": "mona/forkmesh · by bob", "href": "/mona/forkmesh/issues/7",
        ])
        XCTAssertEqual(withRecord.ping?.repo, "mona/forkmesh")
        XCTAssertEqual(withRecord.ping?.actor, "bob")
        let recordRoute = await shell.routePush(
            isChat: false, isDirectMessage: false, alertId: withRecord.alertId, href: withRecord.href, ping: withRecord.ping)
        XCTAssertEqual(recordRoute, .repo(NotificationDeepLink.parse("/mona/forkmesh/issues/7")!))
        XCTAssertNil(PushOpen(arguments: ["alertId": "c1", "title": "Chat", "topic": "chat"]).ping)
    }

    @MainActor
    func testTheInboxShowsTheLastKnownPingsBeforeTheNetworkAnswers() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/notifications") { _ in inboxPage([issueRow], unread: 1) }
        let store = InMemoryKeyValueStore()
        let api = ApiService(settings: StaticApiSettings(), http: http, sleep: { _ in })
        api.sessionTokenProvider = { "session" }
        let first = NotificationService(api: api, nodeName: { "alice" }, snapshot: PingSnapshot(store: store))
        await first.refresh()
        XCTAssertEqual(first.notifications.map(\.id), ["a1"])

        let relaunched = NotificationService(api: api, nodeName: { "alice" }, snapshot: PingSnapshot(store: store))
        relaunched.start()
        XCTAssertEqual(relaunched.notifications.map(\.id), ["a1"])
        XCTAssertEqual(relaunched.unreadCount, 1)
        let other = NotificationService(api: api, nodeName: { "bob" }, snapshot: PingSnapshot(store: store))
        other.start()
        XCTAssertTrue(other.notifications.isEmpty)
    }

    @MainActor
    func testAPushedPingSurvivesARefreshThatDoesNotHaveItYet() async {
        let snapshot = PingSnapshot(store: InMemoryKeyValueStore(), clock: FixedClock(epochMillis: 3_000))
        let pushed = ForkNotification(id: "new", kind: "mention", title: "New", ts: 2_000)
        XCTAssertTrue(snapshot.insert(pushed, for: "Alice"))
        XCTAssertFalse(snapshot.insert(pushed, for: "alice"))
        let old = ForkNotification(id: "old", kind: "mention", title: "Old", ts: 1_000)
        XCTAssertEqual(snapshot.save([old], for: "alice").map(\.id), ["new", "old"])
        XCTAssertEqual(snapshot.save([pushed, old], for: "alice").map(\.id), ["new", "old"])
        XCTAssertEqual(snapshot.save([old], for: "alice").map(\.id), ["old"])
        let many = (0..<150).map { ForkNotification(id: "n\($0)", kind: "mention", title: "x", ts: $0) }
        snapshot.save(many, for: "alice")
        XCTAssertEqual(snapshot.pings(for: "alice").count, PingSnapshot.maxEntries)
        XCTAssertEqual(snapshot.pings(for: "alice").first?.id, "n149")
    }

    @MainActor
    func testABannerForADeletedPingDoesNotBringItBack() async {
        let clock = FixedClock(epochMillis: 1_770_000_000_000)
        let snapshot = PingSnapshot(store: InMemoryKeyValueStore(), clock: clock)
        let old = ForkNotification(id: "old", kind: "mention", title: "Old", ts: 1_770_000_000_000)
        clock.advance(by: 11 * 60)
        snapshot.insert(old, for: "alice")
        XCTAssertEqual(snapshot.save([], for: "alice").map(\.id), [])
        let fresh = ForkNotification(id: "fresh", kind: "mention", title: "Fresh", ts: Int(clock.nowMillis) - 60_000)
        snapshot.insert(fresh, for: "alice")
        XCTAssertEqual(snapshot.save([], for: "alice").map(\.id), ["fresh"])
    }

    @MainActor
    func testADesktopAgentPushOpensTheAgentSession() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/notifications") { _ in inboxPage([desktopAgentRow], unread: 1) }
        http.on("POST", path: "/api/notifications") { _ in HTTPResponse(status: 200, json: ["ok": true]) }
        let (_, inbox) = makeInbox(http)
        let shell = HomeShellModel(chat: FakeChat(), notifications: inbox)
        let named = await shell.routePush(
            isChat: false, isDirectMessage: false, alertId: "", href: "/mona/forkmesh/agents/12", agent: "12")
        XCTAssertEqual(named, .agent("12"))
        let byRow = await shell.routePush(isChat: false, isDirectMessage: false, alertId: "d1", href: "")
        XCTAssertEqual(byRow, .agent("12"))
        for _ in 0..<50 where !http.requests.contains(where: { $0.method == "POST" }) { await Task.yield() }
        XCTAssertTrue(http.requests.contains { $0.method == "POST" && $0.bodyText.contains("d1") })
    }

    @MainActor
    func testNotificationsScreenFiltersDeletesAndOpens() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/notifications") { _ in inboxPage([issueRow, bareRow], unread: 2) }
        http.on("POST", path: "/api/notifications") { _ in HTTPResponse(status: 200, json: ["ok": true]) }
        http.on("DELETE", path: "/api/notifications") { _ in HTTPResponse(status: 403, json: ["error": "forbidden"]) }
        let (api, inbox) = makeInbox(http)
        let model = NotificationsScreenModel(service: inbox, api: api)
        await model.start()
        XCTAssertEqual(model.body.stateKey, "list-All")
        XCTAssertEqual(model.unreadLabel, "2 unread")
        XCTAssertTrue(model.canMarkAllRead)
        XCTAssertTrue(model.canDeleteAll)
        XCTAssertEqual(model.deleteAllTitle, "Delete all 2 notifications?")
        model.filter = "Mentions"
        guard case .items(_, let mentions) = model.body else { return XCTFail("expected items") }
        XCTAssertEqual(mentions.map(\.id), ["a1"])

        // A refused swipe puts the row back under a new identity.
        let row = inbox.notifications[1]
        XCTAssertEqual(model.rowKey(row), "ping-a2-0")
        await model.delete(row)
        XCTAssertEqual(inbox.notifications.map(\.id), ["a1", "a2"])
        XCTAssertEqual(model.rowKey(row), "ping-a2-1")
        XCTAssertEqual(model.notice?.hasPrefix("Could not delete notification: "), true)

        let first = await model.open(inbox.notifications[0])
        XCTAssertEqual(first, .repo(NotificationDeepLink.parse("/mona/forkmesh/issues/7")!))
        let web = await model.open(inbox.notifications[1])
        XCTAssertEqual(web, .web(URL(string: "https://example.com/x")!))
        model.webOpenFailed(URL(string: "https://example.com/x")!)
        XCTAssertEqual(model.notice, "Could not open https://example.com/x")

        await model.deleteAll()
        XCTAssertEqual(model.notice?.hasPrefix("Could not delete notifications: "), true)
    }
}
