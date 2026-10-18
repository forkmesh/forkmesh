import Foundation
import XCTest

@testable import FMApi

final class RouteParserTests: XCTestCase {
    let repo = RepoRef(owner: "forkmesh", name: "app")
    var root: Route { .repo(repo, section: nil) }

    private func stack(_ text: String, me: String = "") -> [Route]? {
        RouteParser.parse(URL(string: text)!, me: me)?.stack
    }

    private func web(_ path: String, me: String = "") -> [Route]? {
        stack("https://app.next.forkmesh.com\(path)", me: me)
    }

    func testEveryUniversalLinkHostOpensAndNamesItsProject() {
        let expected: [String: String] = [
            "app.next.forkmesh.com": "app.forkmesh.com",
            "next.forkmesh.com": "app.forkmesh.com",
            "app.forkmesh.com": "app.forkmesh.com",
            "forkmesh.com": "app.forkmesh.com",
        ]
        XCTAssertEqual(Set(RouteParser.universalLinkHosts), Set(expected.keys))
        for (host, project) in expected {
            let link = RouteParser.parse(URL(string: "https://\(host)/forkmesh/app")!)
            XCTAssertEqual(link?.project, project, host)
            XCTAssertEqual(link?.stack, [root], host)
        }
        XCTAssertEqual(RouteParser.parse(URL(string: "https://forkmesh.com/forkmesh/app")!)?.explicitProject, false)
        XCTAssertNil(RouteParser.parse(URL(string: "https://example.com/forkmesh/app")!))
        XCTAssertNil(RouteParser.parse(URL(string: "http://app.next.forkmesh.com/forkmesh/app")!))
        XCTAssertNil(RouteParser.parse(URL(string: "https://world.next.forkmesh.com/forkmesh/app")!))
    }

    func testProjectQueryWins() {
        let link = RouteParser.parse(URL(string: "https://next.forkmesh.com/forkmesh/app?project=App.Forkmesh.com")!)
        XCTAssertEqual(link?.project, "app.forkmesh.com")
        XCTAssertEqual(link?.explicitProject, true)
        XCTAssertNil(RouteParser.parse(URL(string: "forkmesh://forkmesh/app")!)?.project)
        XCTAssertEqual(RouteParser.parse(URL(string: "forkmesh://forkmesh/app?project=home.lan:8443")!)?.project, "home.lan:8443")
        XCTAssertEqual(
            RouteParser.parse(URL(string: "forkmesh://forkmesh/app?project=App.Next.Forkmesh.com")!)?.project, "app.forkmesh.com")
    }

    func testRepositoryRoot() {
        XCTAssertEqual(web("/forkmesh/app"), [root])
        XCTAssertEqual(web("/forkmesh/app/"), [root])
    }

    func testTreeAndBlob() {
        XCTAssertEqual(web("/forkmesh/app/tree/main/native/ios"), [root, .tree(repo, ref: "main", path: "native/ios")])
        XCTAssertEqual(web("/forkmesh/app/tree/main"), [root, .tree(repo, ref: "main", path: "")])
        XCTAssertEqual(
            web("/forkmesh/app/blob/main/native/ios/README.md"),
            [root, .tree(repo, ref: "main", path: "native/ios"), .file(repo, ref: "main", path: "native/ios/README.md")])
        XCTAssertEqual(
            web("/forkmesh/app/blob/v1/a%20b.txt"),
            [root, .tree(repo, ref: "v1", path: ""), .file(repo, ref: "v1", path: "a b.txt")])
        XCTAssertNil(web("/forkmesh/app/blob/main"))
    }

    func testIssuesPullsAndDiscussions() {
        XCTAssertEqual(web("/forkmesh/app/issues"), [root, .repo(repo, section: .issues)])
        XCTAssertEqual(web("/forkmesh/app/issues/7"), [root, .repo(repo, section: .issues), .issue(repo, number: 7)])
        XCTAssertEqual(web("/forkmesh/app/issues/new"), [root, .repo(repo, section: .issues), .newIssue(repo)])
        XCTAssertEqual(web("/forkmesh/app/pulls"), [root, .repo(repo, section: .pulls)])
        XCTAssertEqual(
            web("/forkmesh/app/pulls/12"),
            [root, .repo(repo, section: .pulls), .pull(repo, number: 12, tab: .conversation)])
        XCTAssertEqual(
            web("/forkmesh/app/pulls/12/files"), [root, .repo(repo, section: .pulls), .pull(repo, number: 12, tab: .files)])
        XCTAssertEqual(web("/forkmesh/app/discussions"), [root, .repo(repo, section: .discussions)])
        XCTAssertEqual(
            web("/forkmesh/app/discussions/3"), [root, .repo(repo, section: .discussions), .discussion(repo, number: 3)])
        XCTAssertNil(web("/forkmesh/app/issues/abc"))
    }

    func testRepositoryShortcutsOpenTheNavigatorPage() {
        XCTAssertEqual(web("/forkmesh/app/shortcuts"), [.shortcuts(repo)])
        XCTAssertNil(web("/forkmesh/app/shortcuts/deploy.md"))
        XCTAssertNil(Route.shortcuts(repo).homeTab)
        XCTAssertEqual(Route.shortcuts(repo).repoRef, repo)
    }

    func testTheNotificationDeepLinkRulesAgree() {
        for path in ["/forkmesh/app/issues/7", "/forkmesh/app/pulls/12", "/forkmesh/app/discussions/3",
                     "/forkmesh/app/commit/abcdef1", "/forkmesh/app/releases", "/forkmesh/app/mirrors"] {
            let link = NotificationDeepLink.parse(path)!
            XCTAssertEqual(link.routeStack, web(path), path)
        }
    }

    func testCommits() {
        XCTAssertEqual(
            web("/forkmesh/app/commit/abcdef1234"),
            [root, .repo(repo, section: .commits), .commit(repo, sha: "abcdef1234")])
        XCTAssertEqual(web("/forkmesh/app/commits/feature/x"), [root, .commits(repo, ref: "feature/x")])
        XCTAssertEqual(
            web("/forkmesh/app/commits/abcdef1"), [root, .repo(repo, section: .commits), .commit(repo, sha: "abcdef1")])
        XCTAssertEqual(web("/forkmesh/app/commits"), [root, .repo(repo, section: .commits)])
    }

    func testRepositorySections() {
        XCTAssertEqual(web("/forkmesh/app/actions"), [root, .repo(repo, section: .actions)])
        XCTAssertEqual(
            web("/forkmesh/app/actions/runs/99"), [root, .repo(repo, section: .actions), .run(repo, id: "99")])
        XCTAssertEqual(web("/forkmesh/app/releases"), [root, .repo(repo, section: .releases)])
        XCTAssertEqual(
            web("/forkmesh/app/releases/tag/v1.4.0"), [root, .repo(repo, section: .releases), .release(repo, tag: "v1.4.0")])
        XCTAssertEqual(web("/forkmesh/app/mirrors"), [root, .repo(repo, section: .mirrors)])
        XCTAssertEqual(web("/forkmesh/app/agents"), [root, .repo(repo, section: .agents)])
        XCTAssertEqual(web("/forkmesh/app/worktrees"), [root, .repo(repo, section: .worktrees)])
        XCTAssertEqual(web("/forkmesh/app/about"), [root, .repo(repo, section: .about)])
        XCTAssertEqual(
            web("/forkmesh/app/compare/main...feature"),
            [root, .repo(repo, section: .branches), .compare(repo, base: "main", head: "feature")])
        XCTAssertNil(web("/forkmesh/app/wiki"))
    }

    func testProfiles() {
        XCTAssertEqual(web("/@alice"), [.profile(name: "alice")])
        XCTAssertNil(web("/@alice/extra"))
        XCTAssertNil(web("/@"))
    }

    func testDashboardPages() {
        XCTAssertEqual(web("/dashboard/chat"), [.chat])
        XCTAssertEqual(web("/dashboard/notes"), [.notes])
        XCTAssertEqual(web("/dashboard/steps"), [.steps])
        XCTAssertEqual(web("/dashboard/sleep"), [.sleep])
        XCTAssertEqual(web("/dashboard/sugar-break"), [.sugarBreak])
        XCTAssertEqual(web("/dashboard/stretch"), [.stretch])
        XCTAssertEqual(web("/dashboard/food-guide"), [.foodGuide])
        XCTAssertEqual(web("/dashboard/water"), [.water])
        XCTAssertEqual(web("/dashboard/speech"), [.speech])
        XCTAssertEqual(web("/dashboard/glasses-alert"), [.glassesAlert])
        XCTAssertEqual(web("/dashboard/phone-security"), [.phoneSecurity])
        XCTAssertEqual(web("/dashboard/face-changer"), [.faceChanger])
        XCTAssertEqual(web("/dashboard/video-mix-dj-studio"), [.videoMixDjStudio])
        XCTAssertEqual(web("/dashboard/app-store-status"), [.appStoreStatus])
        XCTAssertNil(web("/dashboard/app-store-status/extra"))
        XCTAssertEqual(web("/dashboard/rss"), [.rssReader])
        XCTAssertEqual(web("/dashboard/rss-reader"), [.rssReader])
        XCTAssertNil(web("/dashboard/rss/extra"))
        XCTAssertEqual(web("/dashboard/secure-drive"), [.secureDrive])
        XCTAssertNil(web("/dashboard/secure-drive/extra"))
        XCTAssertEqual(web("/dashboard/people"), [.people])
        XCTAssertEqual(web("/dashboard/docs"), [.docs])
        XCTAssertEqual(web("/dashboard/calories"), [.calories])
        XCTAssertEqual(web("/dashboard/notes/n1"), [.notes, .note(id: "n1")])
        XCTAssertEqual(web("/dashboard/notes?note=n2"), [.notes, .note(id: "n2")])
        XCTAssertEqual(web("/dashboard/notes/n1?note=n2"), [.notes, .note(id: "n1")])
        XCTAssertEqual(web("/dashboard/notes?note="), [.notes])
        XCTAssertEqual(web("/dashboard/tasks"), [.tasks(filter: nil)])
        XCTAssertEqual(web("/dashboard/calendar"), [.calendar(date: nil, mode: .month)])
        XCTAssertEqual(web("/dashboard/calendar?date=2026-09-24&mode=week"), [.calendar(date: "2026-09-24", mode: .week)])
        XCTAssertEqual(web("/dashboard/settings"), [.settings(section: nil)])
        XCTAssertEqual(web("/dashboard/settings/keys"), [.settings(section: "keys")])
        XCTAssertEqual(web("/dashboard/network"), [.network(section: nil)])
        XCTAssertEqual(web("/dashboard/ads"), [.ads])
        XCTAssertEqual(web("/dashboard/apps"), [.settings(section: "apps")])
        XCTAssertNil(web("/dashboard/apps/build"))
        XCTAssertEqual(web("/dashboard/repos"), [.repos(.all)])
        XCTAssertEqual(web("/dashboard/profile", me: "alice"), [.profile(name: "alice")])
        XCTAssertEqual(web("/dashboard/profile"), [.settings(section: "profile")])
        XCTAssertNil(web("/dashboard"))
        XCTAssertNil(web("/dashboard/unknown"))
    }

    func testStaticPages() {
        XCTAssertEqual(web("/ledger.html"), [.ledger(section: nil)])
        XCTAssertEqual(web("/market.html"), [.market(filter: nil)])
        XCTAssertEqual(web("/status.html"), [.status])
    }

    func testMarketShareLinksOpenThePackageUnderTheMarket() {
        let expected: [Route] = [.market(filter: nil), .package(id: "crm")]
        for host in RouteParser.universalLinkHosts {
            XCTAssertEqual(stack("https://\(host)/market/crm"), expected, host)
            XCTAssertEqual(stack("https://\(host)/market/crm/"), expected, host)
        }
        XCTAssertEqual(web("/market/rss-reader"), [.market(filter: nil), .package(id: "rss-reader")])
        XCTAssertEqual(web("/market/acme.status-page2"), [.market(filter: nil), .package(id: "acme.status-page2")])
        XCTAssertEqual(stack("forkmesh://market/crm"), expected)
        let longest = String(repeating: "a", count: 96)
        XCTAssertEqual(web("/market/\(longest)"), [.market(filter: nil), .package(id: longest)])
    }

    func testMarketPathsThatAreNotPackagesFallThroughToSafari() {
        XCTAssertNil(web("/market"))
        XCTAssertNil(web("/market/"))
        XCTAssertNil(web("/market/previews"))
        XCTAssertNil(web("/market/previews/"))
        XCTAssertNil(web("/market/previews/crm.png"))
        XCTAssertNil(web("/market/crm/reviews"))
        XCTAssertNil(web("/market/CRM"))
        XCTAssertNil(web("/market/1crm"))
        XCTAssertNil(web("/market/crm-"))
        XCTAssertNil(web("/market/crm--x"))
        XCTAssertNil(web("/market/crm_x"))
        XCTAssertNil(web("/market/crm.png.."))
        XCTAssertNil(web("/market/" + String(repeating: "a", count: 97)))
        XCTAssertEqual(web("/market.html"), [.market(filter: nil)])
    }

    func testPackageIdsFollowTheMarketplaceRule() {
        XCTAssertTrue(RouteParser.isPackageId("crm"))
        XCTAssertTrue(RouteParser.isPackageId("video-mix-dj-studio"))
        XCTAssertTrue(RouteParser.isPackageId("acme.tools"))
        XCTAssertTrue(RouteParser.isPackageId(String(repeating: "a", count: 96)))
        XCTAssertFalse(RouteParser.isPackageId(String(repeating: "a", count: 97)))
        XCTAssertFalse(RouteParser.isPackageId(""))
        XCTAssertFalse(RouteParser.isPackageId("previews"))
        XCTAssertFalse(RouteParser.isPackageId("Crm"))
        XCTAssertFalse(RouteParser.isPackageId("-crm"))
        XCTAssertFalse(RouteParser.isPackageId("crm/x"))
        XCTAssertFalse(RouteParser.isPackageId("crm x"))
    }

    func testAnythingElseFallsThroughToSafari() {
        XCTAssertNil(web("/"))
        XCTAssertNil(web("/pricing.html"))
        XCTAssertNil(web("/docs/native"))
        XCTAssertNil(web("/api/repositories"))
        XCTAssertNil(web("/forkmesh"))
        XCTAssertNil(web("/fork%20mesh/app"))
        XCTAssertNil(web("/../app"))
        XCTAssertNil(web("/oauth/authorize?response_type=code&client_id=c&code_challenge=x"))
    }

    func testCustomSchemeTakesTheSamePaths() {
        XCTAssertEqual(
            stack("forkmesh://forkmesh/app/pulls/12"),
            [root, .repo(repo, section: .pulls), .pull(repo, number: 12, tab: .conversation)])
        XCTAssertEqual(stack("forkmesh://dashboard/notes"), [.notes])
    }

    func testAppOnlyRoutes() {
        XCTAssertEqual(stack("forkmesh://agents/42"), [.agents(.all), .agent(id: "42")])
        XCTAssertEqual(stack("forkmesh://agents"), [.agents(.all)])
        XCTAssertEqual(stack("forkmesh://agents?filter=running"), [.agents(.running)])
        XCTAssertEqual(stack("forkmesh://approvals/s1/a2"), [.inbox(.approvals), .approval(id: "s1/a2")])
        XCTAssertEqual(
            stack("forkmesh://compose?repo=forkmesh/app&text=fix%20it&issue=4"),
            [.compose(ComposeTarget(repo: repo, issue: 4, text: "fix it"))])
        XCTAssertEqual(stack("forkmesh://compose"), [.compose(ComposeTarget())])
        XCTAssertEqual(stack("forkmesh://search?q=router&scope=code"), [.search(query: "router", scope: .code)])
        XCTAssertEqual(stack("forkmesh://search?q=x&scope=all"), [.search(query: "x", scope: .everything)])
        XCTAssertEqual(stack("forkmesh://search?q=x&scope=chat"), [.search(query: "x", scope: .chat)])
        XCTAssertEqual(stack("forkmesh://inbox/mentions"), [.inbox(.mentions)])
        XCTAssertEqual(stack("forkmesh://rss-reader"), [.rssReader])
        XCTAssertEqual(stack("forkmesh://dashboard/rss"), [.rssReader])
        XCTAssertEqual(stack("forkmesh://secure-drive"), [.secureDrive])
        XCTAssertEqual(stack("forkmesh://dashboard/secure-drive"), [.secureDrive])
        XCTAssertNil(stack("forkmesh://ios-udid?udid=abc"))
        XCTAssertNil(stack("forkmesh://surface/open?route=eyJjb21wb3NlIjp7fX0"))
        XCTAssertNil(RouteParser.parse(URL(string: "forkmesh://surface/open?route=eyJjb21wb3NlIjp7fX0")!))
        XCTAssertNil(stack("forkmeshwatch://agents/1"))
    }

    func testStacksWalkUpToTheirParents() {
        XCTAssertEqual(
            RouteParser.stack(for: .review(repo, number: 5)),
            [root, .repo(repo, section: .pulls), .pull(repo, number: 5, tab: .conversation), .review(repo, number: 5)])
        XCTAssertEqual(RouteParser.stack(for: .thread(channel: "#dev", id: "t")), [.channel(id: "#dev"), .thread(channel: "#dev", id: "t")])
        XCTAssertEqual(RouteParser.stack(for: .note(id: "1")), [.notes, .note(id: "1")])
        XCTAssertEqual(RouteParser.stack(for: .approval(id: "s/a")), [.inbox(.approvals), .approval(id: "s/a")])
        XCTAssertEqual(RouteParser.stack(for: .agent(id: "1")), [.agent(id: "1")])
        XCTAssertEqual(RouteParser.stack(for: root), [root])
    }

    func testApprovalIdsSplitIntoSessionAndApproval() {
        guard case let .approval(id)? = stack("forkmesh://approvals/s1/a2")?.last,
              let parts = InboxRoutes.approvalParts(id) else {
            return XCTFail("no approval")
        }
        XCTAssertEqual(parts.sessionId, "s1")
        XCTAssertEqual(parts.approvalId, "a2")
    }
}
