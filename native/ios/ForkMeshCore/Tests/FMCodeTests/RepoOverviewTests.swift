import FMApi
import FMBase
import XCTest
@testable import FMCode

@MainActor
final class FakeOverviewClient: RepoOverviewClient {
    var failing = false

    func publishedIssues(_ owner: String, _ name: String) async throws -> [Issue] {
        if failing { throw ReposTestError(text: "down") }
        return [Issue(json: ["number": .int(1), "status": .string("open")]), Issue(json: ["number": .int(2), "status": .string("closed")])]
    }

    func publishedPulls(_ owner: String, _ name: String) async throws -> [PublishedPull] {
        [PublishedPull(number: 1, title: "a"), PublishedPull(number: 2, title: "b", status: "merged"), PublishedPull(number: 3, title: "c")]
    }

    func branches(_ owner: String, _ name: String) async throws -> [RepoBranch] {
        if failing { throw ReposTestError(text: "down") }
        return [RepoBranch(name: "main")]
    }

    func releases(_ owner: String, _ name: String) async throws -> [RepoRelease] { [] }

    func commits(_ owner: String, _ name: String) async throws -> [JSONObject] {
        [["hash": .string("a")], ["hash": .string("b")], ["hash": .string("c")], ["hash": .string("d")]]
    }
}

@MainActor
final class RepoOverviewTests: XCTestCase {
    func testCountsOpenItemsAndLists() async {
        let model = RepoOverviewModel(repo: Repository(owner: "o", name: "r", mirrors: 3), client: FakeOverviewClient())
        await model.load()
        XCTAssertEqual(model.counts[.issues], 1)
        XCTAssertEqual(model.counts[.pulls], 2)
        XCTAssertEqual(model.counts[.branches], 1)
        XCTAssertEqual(model.counts[.releases], 0)
        XCTAssertEqual(model.counts[.commits], 4)
        XCTAssertEqual(model.counts[.mirrors], 3)
        XCTAssertFalse(model.loading)
    }

    func testFailedReadsLeaveNoCount() async {
        let client = FakeOverviewClient()
        client.failing = true
        let model = RepoOverviewModel(repo: Repository(owner: "o", name: "r"), client: client)
        await model.load()
        XCTAssertNil(model.counts[.issues])
        XCTAssertNil(model.counts[.branches])
        XCTAssertEqual(model.counts[.pulls], 2)
    }

    func testTilesHideSettingsWithoutRights() async {
        let model = RepoOverviewModel(repo: Repository(owner: "o", name: "r"), client: nil)
        XCTAssertFalse(model.tiles(canEditSettings: false).contains { $0.section == .settings })
        XCTAssertEqual(model.tiles(canEditSettings: true).last?.section, .settings)
        XCTAssertEqual(model.tiles(canEditSettings: true).first?.section, .code)
        XCTAssertFalse(model.tiles(canEditSettings: true).contains { $0.title == "Shortcuts" })
    }

    func testAdoptTakesCatalogRecord() async {
        let model = RepoOverviewModel(repo: Repository(owner: "O", name: "R"), client: nil)
        model.adopt([Repository(owner: "o", name: "r", description: "hello", mirrors: 5)])
        XCTAssertEqual(model.repo.description, "hello")
        XCTAssertEqual(model.counts[.mirrors], 5)
    }

    func testCurrentRepoMemory() async {
        let memory = CurrentRepoMemory(store: InMemoryKeyValueStore())
        XCTAssertNil(memory.ref)
        memory.remember(owner: "", name: "x")
        XCTAssertNil(memory.ref)
        memory.remember(owner: "o", name: "r")
        XCTAssertEqual(memory.ref, RepoRef(owner: "o", name: "r"))
    }
}
