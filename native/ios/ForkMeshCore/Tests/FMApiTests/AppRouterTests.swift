import Foundation
import XCTest

@testable import FMApi

final class AppRouterTests: XCTestCase {
    let repo = RepoRef(owner: "forkmesh", name: "app")

    @MainActor func testStartsOnAgents() async {
        let router = AppRouter()
        XCTAssertEqual(router.selectedTab, .agents)
        XCTAssertTrue(router.currentPath.isEmpty)
    }

    @MainActor func testScreenTitleNamesTheTabAndItsStack() async {
        let router = AppRouter()
        XCTAssertEqual(router.screenTitle, "Agents")
        router.select(.inbox)
        router.setPath([.repo(repo, section: nil), .shortcuts(nil)], for: .inbox)
        XCTAssertEqual(router.screenTitle, "Inbox > forkmesh/app > Shortcuts")
    }

    @MainActor func testReselectPopsThenScrollsToTop() async {
        let router = AppRouter()
        router.select(.code)
        router.setPath([.repo(repo, section: nil)], for: .code)
        router.select(.code)
        XCTAssertTrue(router.path(for: .code).isEmpty)
        XCTAssertEqual(router.reselects[.code] ?? 0, 0)
        router.select(.code)
        XCTAssertEqual(router.reselects[.code], 1)
    }

    @MainActor func testSwitchingTabsOpensTheTabsRoot() async {
        let router = AppRouter()
        router.select(.code)
        router.setPath([.repo(repo, section: nil), .pull(repo, number: 3, tab: .files)], for: .code)
        router.select(.inbox)
        router.setPath([.notes], for: .inbox)
        router.select(.code)
        XCTAssertEqual(router.selectedTab, .code)
        XCTAssertTrue(router.path(for: .code).isEmpty)
        XCTAssertEqual(router.path(for: .inbox), [.notes], "the tab being left keeps its stack until it is tapped")
        XCTAssertEqual(router.reselects[.code] ?? 0, 0)
    }

    @MainActor func testOpenChoosesTheHomeTabOrTheCurrentTab() async {
        let router = AppRouter()
        router.select(.chat)
        router.open(.pull(repo, number: 3, tab: .files))
        XCTAssertEqual(router.selectedTab, .code)
        XCTAssertEqual(router.path(for: .code), [.pull(repo, number: 3, tab: .files)])
        router.open(.calendar(date: nil, mode: .week))
        XCTAssertEqual(router.selectedTab, .code, "hub routes open on the current tab")
        XCTAssertEqual(router.path(for: .code).last, .calendar(date: nil, mode: .week))
        router.open(.calendar(date: nil, mode: .week))
        XCTAssertEqual(router.path(for: .code).count, 2, "the same route is not pushed twice")
        router.open(.inbox(.all))
        XCTAssertEqual(router.selectedTab, .inbox)
        XCTAssertTrue(router.path(for: .inbox).isEmpty, "a tab's root route selects the tab")
        router.open(.agent(id: "7"), placement: .currentTab)
        XCTAssertEqual(router.path(for: .inbox), [.agent(id: "7")])
    }

    @MainActor func testHubOpensOnTheCurrentTabAndRemembers() async {
        let router = AppRouter()
        router.select(.chat)
        for route: Route in [.notes, .timers, .hosts, .stats, .vault(section: nil), .notes, .activity] {
            router.openFromHub(route)
        }
        XCTAssertEqual(router.selectedTab, .chat)
        XCTAssertEqual(router.recents, [.activity, .notes, .vault(section: nil), .stats, .hosts])
        XCTAssertEqual(router.path(for: .chat).last, .activity)
    }

    @MainActor func testHubPageAlreadyOpenPopsBackToItself() async {
        let router = AppRouter()
        router.select(.chat)
        router.openFromHub(.notes)
        router.open(.agent(id: "1"), placement: .currentTab)
        router.openFromHub(.timers)
        router.openFromHub(.notes)
        XCTAssertEqual(router.path(for: .chat), [.notes])
        router.openFromHub(.notes)
        XCTAssertEqual(router.path(for: .chat), [.notes])
    }

    @MainActor func testModalRoutesArePresented() async {
        let router = AppRouter()
        let target = ComposeTarget(repo: repo, text: "hi")
        router.open(.compose(target))
        XCTAssertEqual(router.takeComposeRequest(), target)
        XCTAssertNil(router.takeComposeRequest())
        router.open(.updateRequired)
        XCTAssertEqual(router.presented, .updateRequired)
        router.dismissPresented()
        XCTAssertNil(router.presented)
        XCTAssertTrue(router.currentPath.isEmpty)
    }

    @MainActor func testDeepLinksReplaceTheHomeTabStack() async {
        let router = AppRouter(project: "app.forkmesh.com")
        router.open(.hosts)
        let link = RouteParser.parse(URL(string: "https://next.forkmesh.com/forkmesh/app/pulls/12")!)!
        XCTAssertEqual(router.open(link: link), .opened)
        XCTAssertEqual(router.selectedTab, .code)
        XCTAssertEqual(router.path(for: .code), [
            .repo(repo, section: nil), .repo(repo, section: .pulls), .pull(repo, number: 12, tab: .conversation),
        ])
        XCTAssertEqual(router.path(for: .agents), [.hosts])
        XCTAssertEqual(router.landings, 1)
        router.show(stack: [.agents(.all), .agent(id: "9")])
        XCTAssertEqual(router.selectedTab, .agents)
        XCTAssertEqual(router.path(for: .agents), [.agent(id: "9")], "the tab root is not pushed")
    }

    @MainActor func testNotificationRoutesOpenWithTheirParents() async {
        let router = AppRouter()
        router.show(.issue(repo, number: 4))
        XCTAssertEqual(router.path(for: .code), [.repo(repo, section: nil), .repo(repo, section: .issues), .issue(repo, number: 4)])
        router.show(.approval(id: "s/a"))
        XCTAssertEqual(router.selectedTab, .inbox)
        XCTAssertEqual(router.path(for: .inbox), [.inbox(.approvals), .approval(id: "s/a")])
        router.show(.note(id: "n"))
        router.show(.note(id: "n"))
        XCTAssertEqual(router.path(for: .inbox).suffix(2), [.notes, .note(id: "n")])
        XCTAssertEqual(router.path(for: .inbox).count, 4, "a repeated hub link is not stacked twice")
    }

    @MainActor func testRoutesWaitForSignIn() async {
        let router = AppRouter(project: "p", accountReady: false)
        let link = ParsedLink(project: nil, stack: [.agents(.all), .agent(id: "1")])
        XCTAssertEqual(router.open(link: link), .waitingForSignIn)
        XCTAssertEqual(router.pendingLink, link)
        XCTAssertTrue(router.path(for: .agents).isEmpty)
        router.accountReady = true
        XCTAssertNil(router.pendingLink)
        XCTAssertEqual(router.path(for: .agents), [.agent(id: "1")])
    }

    @MainActor func testOtherProjectsSwitchFirstAndUnknownOnesAreRefused() async {
        let router = AppRouter(project: "app.next.forkmesh.com")
        let link = ParsedLink(project: "app.forkmesh.com", stack: [.repo(repo, section: nil)])
        XCTAssertEqual(router.open(link: link, knownProjects: []), .unknownProject("app.forkmesh.com"))
        let hinted = ParsedLink(project: "app.forkmesh.com", stack: [.notes], explicitProject: false)
        XCTAssertEqual(router.open(link: hinted, knownProjects: []), .opened, "a host the phone has no project for opens here")
        XCTAssertEqual(router.path(for: .agents), [.notes])
        XCTAssertNil(router.pendingLink)
        XCTAssertEqual(router.open(link: link, knownProjects: ["app.forkmesh.com"]), .switchProject("app.forkmesh.com"))
        XCTAssertEqual(router.pendingLink, link)
        router.activate(project: "app.forkmesh.com")
        XCTAssertNil(router.pendingLink)
        XCTAssertEqual(router.path(for: .code), [.repo(repo, section: nil)])
        XCTAssertEqual(router.open(link: ParsedLink(stack: [])), .ignored)
    }

    @MainActor func testPathsArePerProject() async {
        let router = AppRouter(project: "a")
        router.open(.notes)
        router.select(.chat)
        router.activate(project: "b")
        XCTAssertEqual(router.selectedTab, .agents)
        XCTAssertTrue(router.path(for: .agents).isEmpty)
        router.activate(project: "a")
        XCTAssertEqual(router.selectedTab, .chat)
        XCTAssertEqual(router.path(for: .agents), [.notes])
    }

    @MainActor func testRestorationWithinEightHours() async throws {
        var clock = Date(timeIntervalSince1970: 1_000_000)
        let router = AppRouter(project: "p", now: { clock })
        router.select(.code)
        router.open(.repo(repo, section: .issues))
        router.open(.agent(id: "3"))
        router.select(.inbox)
        router.openFromHub(.notes)
        let data = router.archiveData()

        clock = clock.addingTimeInterval(7 * 60 * 60)
        let warm = AppRouter(project: "p", now: { clock })
        warm.load(data)
        XCTAssertTrue(warm.restored)
        XCTAssertEqual(warm.selectedTab, .inbox)
        XCTAssertEqual(warm.path(for: .inbox), [.notes])
        XCTAssertEqual(warm.path(for: .agents), [.agent(id: "3")])
        XCTAssertEqual(warm.path(for: .code), [.repo(repo, section: .issues)])
        XCTAssertEqual(warm.recents, [.notes])

        clock = clock.addingTimeInterval(2 * 60 * 60)
        let cold = AppRouter(project: "p", now: { clock })
        cold.load(data)
        XCTAssertEqual(cold.selectedTab, .agents, "after eight hours the app opens on the Agents root")
        XCTAssertTrue(cold.path(for: .agents).isEmpty)
        XCTAssertEqual(cold.path(for: .code), [.repo(repo, section: .issues)], "the other tabs are still restored")
    }

    @MainActor func testRestorationSurvivesGarbage() async {
        let router = AppRouter(project: "p")
        router.load(Data("not json".utf8))
        XCTAssertEqual(router.selectedTab, .agents)
        router.load(nil)
        XCTAssertTrue(router.currentPath.isEmpty)
    }

    @MainActor func testEveryRouteRoundTripsThroughTheArchive() async throws {
        let routes: [Route] = [
            .launchedAgent(repo, sessionId: "s"), .search(query: "q", scope: .chat), .approval(id: "a/b"),
            .teammateEditor(id: nil), .coves(nil), .tasks(filter: nil),
        ]
        let router = AppRouter(project: "p")
        router.setPath(routes, for: .agents)
        let copy = AppRouter(project: "p")
        copy.load(router.archiveData())
        XCTAssertEqual(copy.path(for: .agents), routes)
    }

    @MainActor func testPathsAreBounded() async {
        let router = AppRouter()
        router.setPath((0..<100).map { Route.agent(id: "\($0)") }, for: .agents)
        XCTAssertEqual(router.path(for: .agents).count, AppRouter.maxDepth)
        XCTAssertEqual(router.path(for: .agents).last, .agent(id: "99"))
    }

    @MainActor func testFocusNeverPops() async {
        let router = AppRouter()
        router.open(.agent(id: "1"))
        router.focus(.agents)
        XCTAssertEqual(router.path(for: .agents), [.agent(id: "1")])
        router.focus(.chat)
        XCTAssertEqual(router.selectedTab, .chat)
    }

    @MainActor func testBack() async {
        let router = AppRouter()
        XCTAssertFalse(router.back())
        router.open(.agent(id: "1"))
        XCTAssertTrue(router.back())
        XCTAssertTrue(router.currentPath.isEmpty)
    }

    @MainActor func testRemoveDropsARouteFromEveryTabAndKeepsTheRest() async {
        let router = AppRouter()
        router.setPath([.videoMixDjStudio, .repo(repo, section: nil)], for: .agents)
        router.setPath([.shortcuts(nil), .videoMixDjStudio], for: .inbox)
        router.focus(.inbox)
        router.remove(.videoMixDjStudio)
        XCTAssertEqual(router.path(for: .agents), [.repo(repo, section: nil)])
        XCTAssertEqual(router.path(for: .inbox), [.shortcuts(nil)])
        XCTAssertEqual(router.selectedTab, .inbox)
    }

    @MainActor func testInterceptTakesRoutes() async {
        let router = AppRouter()
        var taken: [Route] = []
        router.intercept = { route in
            if case .dm = route {
                taken.append(route)
                return true
            }
            return false
        }
        router.open(.dm(user: "bob"))
        XCTAssertEqual(taken, [.dm(user: "bob")])
        XCTAssertTrue(router.path(for: .chat).isEmpty)
    }

    @MainActor func testComposeContextFollowsTheScreen() async {
        XCTAssertEqual(
            ComposeContext.target(path: [.repo(repo, section: nil), .pull(repo, number: 2, tab: .files)], tab: .code),
            ComposeTarget(repo: repo, pull: 2))
        XCTAssertEqual(ComposeContext.target(path: [.issue(repo, number: 5)], tab: .code), ComposeTarget(repo: repo, issue: 5))
        XCTAssertEqual(ComposeContext.target(path: [.agent(id: "9")], tab: .agents), ComposeTarget(agentId: "9"))
        XCTAssertEqual(ComposeContext.target(path: [.repo(repo, section: .actions)], tab: .code), ComposeTarget(repo: repo))
        XCTAssertEqual(ComposeContext.target(path: [], tab: .chat, conversation: "#dev"), ComposeTarget(channel: "#dev"))
        XCTAssertEqual(ComposeContext.target(path: [.dm(user: "bob")], tab: .chat), ComposeTarget(channel: "@bob"))
        XCTAssertEqual(ComposeContext.target(path: [.notes], tab: .agents), ComposeTarget())
        XCTAssertFalse(ComposeContext.startsAgent(ComposeTarget(channel: "#dev")))
    }
}
