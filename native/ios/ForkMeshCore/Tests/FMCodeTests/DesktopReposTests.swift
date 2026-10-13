import FMApi
import FMBase
import FMChat
import FMControl
import Foundation
import XCTest
@testable import FMCode

@MainActor
final class FakeReposChannel: DesktopControlCalling {
    var methods: [String] = []
    var reply: JSONObject = [:]
    var failure: Error?
    private var taps: [UUID: @MainActor (String, JSONObject) -> Void] = [:]

    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        methods.append(method)
        if let failure { throw failure }
        return reply
    }

    func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID {
        let id = UUID()
        taps[id] = tap
        return id
    }

    func removeEventTap(_ id: UUID) { taps.removeValue(forKey: id) }

    var tapCount: Int { taps.count }
}

@MainActor
final class DesktopReposTests: XCTestCase {
    func testListsTheDesktopRepositories() async {
        let channel = FakeReposChannel()
        channel.reply = [
            "desktop": .string("studio"),
            "repos": .array([
                .object(["repo": .string("me/zeta"), "branch": .string("main"), "checkout": .bool(true), "published": .bool(true)]),
                .object([
                    "owner": .string("me"), "name": .string("alpha"), "private": .bool(true),
                    "agents": .object(["running": .int(2), "total": .int(5)]),
                ]),
                .object(["repo": .string("ME/Zeta")]),
                .object(["repo": .string("broken")]),
            ]),
        ]
        let model = DesktopReposModel(channel: channel)
        await model.start()
        XCTAssertEqual(channel.methods, ["repos.list"])
        XCTAssertEqual(model.desktopName, "studio")
        XCTAssertEqual(model.repos.map(\.slug), ["me/alpha", "me/zeta"])
        XCTAssertEqual(model.repos[0].runningAgents, 2)
        XCTAssertEqual(model.repos[0].detail, "No checkout · 2 running · Private · Not published")
        XCTAssertEqual(model.repos[1].detail, "main · Public")
        XCTAssertNil(model.problem)
        XCTAssertEqual(channel.tapCount, 1)
        model.stop()
        XCTAssertEqual(channel.tapCount, 0)
    }

    func testOlderDesktopFallsBackToTheAgentsSnapshot() async {
        let channel = FakeReposChannel()
        channel.failure = LocalControlException("unknown_method")
        let model = DesktopReposModel(channel: channel, fallback: { [LocalAgentRepository("me", "old")] })
        await model.load()
        XCTAssertEqual(model.repos.map(\.slug), ["me/old"])
        XCTAssertNil(model.problem)
    }

    func testUnreachableDesktopExplains() async {
        let channel = FakeReposChannel()
        channel.failure = LocalControlException("not_connected")
        let model = DesktopReposModel(channel: channel)
        await model.load()
        XCTAssertTrue(model.repos.isEmpty)
        XCTAssertEqual(model.problem, "The desktop is offline. Its repositories show here when it is back.")
    }

    func testNoHubMeansNothingToShow() async {
        let model = DesktopReposModel(channel: nil)
        await model.start()
        XCTAssertFalse(model.available)
        XCTAssertTrue(model.repos.isEmpty)
        XCTAssertNil(model.problem)
    }

    func testRepositoryCarriesTheRowFields() async {
        let repo = DesktopRepo(owner: "me", name: "app", description: "d", isPrivate: true).repository
        XCTAssertEqual(repo.fullName, "me/app")
        XCTAssertTrue(repo.isPrivate)
        XCTAssertTrue(repo.liveHost)
    }
}
