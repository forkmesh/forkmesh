// Gate 2: the Route contract round-trips through Codable (state restoration stores paths), every
// kind names an owner, and the registry falls back to the placeholder.

import Foundation
import XCTest

@testable import FMApi

final class RouteContractTests: XCTestCase {
    let repo = RepoRef(owner: "forkmesh", name: "app")

    @MainActor func testRoutesRoundTripThroughCodable() async throws {
        let routes: [Route] = [
            .agents(.needsYou), .agent(id: "42"), .compose(ComposeTarget(repo: repo, issue: 7, text: "fix")),
            .repo(repo, section: nil), .repo(repo, section: .pulls), .pull(repo, number: 12, tab: .files),
            .file(repo, ref: "main", path: "a/b.swift"), .calendar(date: "2026-09-24", mode: .week),
            .coves(nil), .coves(repo), .shortcuts(nil), .shortcuts(repo), .settings(section: "keys"), .search(query: "q", scope: .code),
        ]
        let data = try JSONEncoder().encode(routes)
        XCTAssertEqual(try JSONDecoder().decode([Route].self, from: data), routes)
    }

    @MainActor func testEveryKindHasAnOwner() async {
        for kind in Route.Kind.allCases {
            XCTAssertFalse(kind.owner.isEmpty, "\(kind)")
        }
    }

    @MainActor func testHomeTabs() async {
        XCTAssertEqual(Route.pull(repo, number: 1, tab: .conversation).homeTab, .code)
        XCTAssertEqual(Route.approval(id: "a").homeTab, .inbox)
        XCTAssertEqual(Route.dm(user: "bob").homeTab, .chat)
        XCTAssertEqual(Route.agent(id: "1").homeTab, .agents)
        XCTAssertNil(Route.calendar(date: nil, mode: .month).homeTab, "hub routes open on the current tab")
        XCTAssertTrue(Route.compose(ComposeTarget()).isModal)
    }

    @MainActor func testRepoSlug() async {
        XCTAssertEqual(RepoRef(slug: "forkmesh/app"), repo)
        XCTAssertNil(RepoRef(slug: "forkmesh"))
        XCTAssertNil(RepoRef(slug: "a/b/c"))
        XCTAssertNil(RepoRef(slug: "/b"))
    }

    @MainActor func testRegistryFallsBackToThePlaceholder() async {
        let registry = RouteRegistry<String> { "soon:\($0.kind.owner)" }
        XCTAssertEqual(registry.destination(for: .calendar(date: nil, mode: .day)), "soon:WS15")
        registry.register(.calendar, by: "WS15") { route in "calendar:\(route.kind.rawValue)" }
        XCTAssertEqual(registry.destination(for: .calendar(date: nil, mode: .day)), "calendar:calendar")
        XCTAssertFalse(registry.unregisteredKinds.contains(.calendar))
        registry.register(.calendar, by: "WS8") { _ in "other" }
        XCTAssertEqual(registry.conflicts.count, 1)
        registry.unregister(.calendar)
        XCTAssertFalse(registry.isRegistered(.calendar))
    }
}
