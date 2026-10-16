import FMBase
import Foundation
import XCTest

@testable import FMApi

final class NavigationCatalogTests: XCTestCase {
    let repo = RepoRef(owner: "forkmesh", name: "app")

    func testEveryKindHasATitleAndSymbol() {
        for kind in Route.Kind.allCases {
            XCTAssertFalse(RouteCatalog.title(kind).isEmpty, "\(kind)")
            XCTAssertFalse(RouteCatalog.symbol(kind).isEmpty, "\(kind)")
        }
    }

    func testHubGroupsFollowNavigationSectionFour() {
        let groups = HubCatalog.groups(guest: false) { _ in false }
        XCTAssertEqual(groups.map(\.title), ["Workspace", "Operate", "Business", "Personal", "Insights", "App"])
        let routes = groups.flatMap(\.items).compactMap(\.route)
        XCTAssertFalse(routes.contains(.allAgents), "the Agents tab lists every agent")
        XCTAssertTrue(routes.contains(.hosts))
        XCTAssertEqual(
            groups.first { $0.id == "operate" }?.items.map(\.id), ["desktops", "teammates", "shortcuts", "hosts", "network", "logs", "diagnostics"])
        XCTAssertTrue(routes.contains(.logs))
        XCTAssertTrue(routes.contains(.diagnostics))
        XCTAssertTrue(routes.contains(.market(filter: nil)))
        XCTAssertFalse(routes.contains(.crm(pipeline: nil)), "unbuilt business packages stay out of the hub")
        XCTAssertFalse(routes.contains(.admin(section: nil)))
        XCTAssertEqual(groups.last?.items.last?.action, .signOut)
        XCTAssertEqual(
            groups.last?.items.first { $0.action == .designSystem }?.id, "designSystem",
            "the App group carries the design system link")
        XCTAssertEqual(
            groups.last?.items.first { $0.action == .workflows }?.id, "workflows",
            "the App group opens Workflows on the web")
        XCTAssertEqual(HubItem.workflowsPath, "/dashboard/workflows")
        let app = groups.last?.items.map(\.id) ?? []
        XCTAssertEqual(app.firstIndex(of: "docs"), app.firstIndex(of: "people").map { $0 + 1 }, "Docs sits next to People")
        XCTAssertEqual(RouteCatalog.title(.docs), "Docs")
        XCTAssertEqual(PackageGate.packageId(for: Route.docs), "docs")

        XCTAssertFalse(
            HubCatalog.groups(guest: false) { _ in true }.flatMap(\.items).contains { $0.route == .admin(section: nil) },
            "admin is for admins only")
        let registered = HubCatalog.groups(guest: true, admin: true) { $0 == .crm || $0 == .admin }
        let all = registered.flatMap(\.items)
        XCTAssertTrue(all.contains { $0.route == .crm(pipeline: nil) })
        XCTAssertTrue(all.contains { $0.route == .admin(section: nil) })
        XCTAssertEqual(all.last?.action, .signIn)
        XCTAssertEqual(Set(all.map(\.id)).count, all.count)
    }

    func testGuestActions() {
        XCTAssertEqual(HubCatalog.guestAction(for: .notes), "use notes")
        XCTAssertEqual(HubCatalog.guestAction(for: .tasks(filter: "x")), "use tasks")
        XCTAssertEqual(HubCatalog.guestAction(for: .vault(section: nil)), "open your password vault")
        XCTAssertEqual(HubCatalog.guestAction(for: .hosts), "manage your hosts")
        XCTAssertNil(HubCatalog.guestAction(for: .calories))
    }

    func testTabs() {
        XCTAssertEqual(AppTab.barOrder, [.agents, .inbox, .code, .chat])
        XCTAssertEqual(AppTab.barOrder.map(\.root), [.agents(.all), .inbox(.all), .repos(.all), .chat])
        XCTAssertEqual(Route.launchedAgent(repo, sessionId: "s").homeTab, .agents)
        XCTAssertEqual(Route.launchedAgent(repo, sessionId: "s").repoRef, repo)
        XCTAssertEqual(Route.pull(repo, number: 3, tab: .files).title, "forkmesh/app #3")
        XCTAssertEqual(Route.repo(repo, section: .pulls).title, "Pull requests · forkmesh/app")
    }

    func testPaletteSuggestionsFollowTheContext() {
        let context = PaletteContext(target: ComposeTarget(repo: repo), tab: .code, pendingApprovals: 2)
        let suggestions = CommandPalette.suggestions(context)
        XCTAssertEqual(suggestions.first?.title, "New agent here")
        XCTAssertTrue(suggestions.contains { $0.command == .open(.newIssue(repo)) })
        XCTAssertTrue(suggestions.contains { $0.title == "Review 2 pending approvals" })
        XCTAssertFalse(CommandPalette.suggestions(PaletteContext()).contains { $0.id == "newIssue" })
        XCTAssertFalse(CommandPalette.suggestions(PaletteContext(guest: true)).contains { $0.id == "newNote" })
        XCTAssertEqual(
            CommandPalette.suggestions(PaletteContext(target: ComposeTarget(channel: "#dev"))).first?.title, "Message #dev")
    }

    func testPaletteSections() {
        let empty = CommandPalette.sections(query: "", context: PaletteContext(recents: [.notes]))
        XCTAssertEqual(empty.map(\.id), ["suggested", "recent", "tabs"])
        let typed = CommandPalette.sections(query: "calendar", context: PaletteContext())
        XCTAssertEqual(typed.map(\.id), ["destinations", "everywhere"])
        XCTAssertEqual(typed.first?.entries.first?.command, .open(.calendar(date: nil, mode: .month)))
        XCTAssertEqual(typed.last?.entries.first?.command, .searchEverywhere("calendar"))
        let tab = CommandPalette.sections(query: "inbox", context: PaletteContext())
        XCTAssertTrue(tab.first?.entries.contains { $0.command == .selectTab(.inbox) } ?? false)
    }

    func testPaletteOpensSlugsDirectly() {
        XCTAssertEqual(CommandPalette.directEntries("forkmesh/app").map(\.command), [.open(.repo(repo, section: nil))])
        XCTAssertEqual(
            CommandPalette.directEntries("forkmesh/app#12").map(\.command),
            [.open(.issue(repo, number: 12)), .open(.pull(repo, number: 12, tab: .conversation))])
        XCTAssertEqual(CommandPalette.directEntries("@bob").map(\.command), [.open(.profile(name: "bob"))])
        XCTAssertTrue(CommandPalette.directEntries("hello").isEmpty)
    }

    func testQuickActions() {
        XCTAssertEqual(ShellQuickAction.staticActions.count, 4)
        XCTAssertEqual(ShellQuickAction.newAgent.route(), .compose(ComposeTarget()))
        XCTAssertEqual(ShellQuickAction.inbox.route(), .inbox(.all))
        XCTAssertEqual(ShellQuickAction.search.route(), .search(query: "", scope: .everything))
        XCTAssertEqual(ShellQuickAction.newNote.route(), .note(id: ""))
        XCTAssertEqual(ShellQuickAction.recentRepo.route(payload: "forkmesh/app"), .repo(repo, section: nil))
        XCTAssertNil(ShellQuickAction.recentRepo.route(payload: "nope"))
        let dynamic = ShellQuickAction.dynamicItems(recents: [.notes, .pull(repo, number: 1, tab: .files), .agent(id: "5")])
        XCTAssertEqual(dynamic.map(\.action), [.recentRepo, .recentAgent])
        XCTAssertEqual(dynamic.map(\.payload), ["forkmesh/app", "5"])
    }

    func testVersionGate() {
        XCTAssertTrue(ClientVersionGate.isTooOld(running: "1.3.9", minimum: "1.4.0"))
        XCTAssertFalse(ClientVersionGate.isTooOld(running: "1.4.0", minimum: "1.4"))
        XCTAssertFalse(ClientVersionGate.isTooOld(running: "v1.4.1+17", minimum: "1.4.0"))
        XCTAssertTrue(ClientVersionGate.isTooOld(running: "1.4.0-rc1", minimum: "1.4.1"))
        XCTAssertFalse(ClientVersionGate.isTooOld(running: "dev", minimum: "9.0.0"))
        XCTAssertFalse(ClientVersionGate.isTooOld(running: "1.0.0", minimum: ""))

        var body = JSONObject()
        XCTAssertNil(ClientVersionGate.requirement(body: body, running: "1.0.0"))
        var clients = JSONObject()
        clients["ios"] = .string("1.5.0")
        body["minimumClientVersions"] = .object(clients)
        let required = ClientVersionGate.requirement(body: body, running: "1.4.0")
        XCTAssertEqual(required?.minimum, "1.5.0")
        XCTAssertEqual(required?.storeURL, ClientVersionGate.defaultStoreURL)
        XCTAssertEqual(required?.summary, "ForkMesh 1.4.0 is older than the minimum supported version 1.5.0")
        XCTAssertNil(ClientVersionGate.requirement(body: body, running: "1.5.0"))

        var flat = JSONObject()
        flat["minimumIosVersion"] = .string("2.0")
        flat["appStoreUrl"] = .string("https://example.com/app")
        XCTAssertEqual(ClientVersionGate.requirement(body: flat, running: "1.9")?.storeURL, URL(string: "https://example.com/app"))
        XCTAssertNotNil(ClientVersionGate.requirement(status: 426, running: "1.0"))
        XCTAssertNil(ClientVersionGate.requirement(status: 200, running: "1.0"))

        var release = JSONObject()
        release["version"] = .string("1.3.0")
        release["trackViewUrl"] = .string("https://apps.apple.com/us/app/forkmesh/id6803060735")
        var lookup = JSONObject()
        lookup["results"] = .array([.object(release)])
        let update = ClientVersionGate.availableUpdate(lookup: lookup, running: "1.2.5+14")
        XCTAssertEqual(update?.version, "1.3.0")
        XCTAssertEqual(update?.label, "v1.3.0")
        XCTAssertEqual(update?.storeURL, URL(string: "https://apps.apple.com/us/app/forkmesh/id6803060735"))
        XCTAssertNil(ClientVersionGate.availableUpdate(lookup: lookup, running: "1.3.0+2"))
        XCTAssertNil(ClientVersionGate.availableUpdate(lookup: JSONObject(), running: "1.0.0"))
    }
}
