// The repo catalog's search, filters, sort and refresh, plus the row text.

import FMApi
import FMBase
import XCTest
@testable import FMCode

@MainActor
final class ReposCatalogTests: XCTestCase {
    private func catalog() -> [Repository] {
        [
            Repository(
                owner: "beta", name: "relay", description: "Mainnode catalog relay", language: "Python", mirrors: 1,
                isPrivate: true, updatedMs: 1000),
            Repository(
                owner: "alpha", name: "mobile", description: "Phone command center", language: "Dart", mirrors: 8,
                cloneOnline: true, updatedMs: 2000),
            Repository(
                owner: "gamma", name: "desktop", description: "Qt canonical host", language: "C++", mirrors: 3,
                liveHost: true, updatedMs: 3000),
        ]
    }

    private func loadedModel() async -> (ReposScreenModel, FakeRepoClient) {
        let client = FakeRepoClient()
        let repos = catalog()
        client.repositoriesResult = { repos }
        let model = ReposScreenModel(client: client)
        await model.start()
        return (model, client)
    }

    func testRefreshReloadsRepositories() async {
        let client = FakeRepoClient()
        client.repositoriesResult = { [Repository(owner: "owner", name: "repo\(client.repositoriesCalls)")] }
        let model = ReposScreenModel(client: client)
        XCTAssertEqual(model.stateKey, "loading")
        await model.start()
        XCTAssertEqual(model.visibleRepos.map(\.fullName), ["owner/repo1"])
        // A second start does not read again; Refresh does.
        await model.start()
        XCTAssertEqual(client.repositoriesCalls, 1)
        await model.reload()
        XCTAssertEqual(client.repositoriesCalls, 2)
        XCTAssertEqual(model.visibleRepos.map(\.fullName), ["owner/repo2"])
    }

    func testSearchMatchesDescriptionAndLanguage() async {
        let (model, _) = await loadedModel()
        model.queryText = "mainnode"
        XCTAssertEqual(model.visibleRepos.map(\.fullName), ["beta/relay"])
        model.queryText = "  DART "
        XCTAssertEqual(model.visibleRepos.map(\.fullName), ["alpha/mobile"])
    }

    func testFiltersAndSortRefineTheCatalog() async {
        let (model, _) = await loadedModel()
        // Recently updated first.
        XCTAssertEqual(model.visibleRepos.map(\.fullName), ["gamma/desktop", "alpha/mobile", "beta/relay"])
        let mobile = model.visibleRepos[1]
        XCTAssertTrue(ReposCatalog.metaItems(mobile).contains(RepoMetaItem(icon: "circle", text: "Dart", isLanguage: true)))
        XCTAssertTrue(ReposCatalog.metaItems(mobile).map(\.text).contains("8 mirrors"))

        model.filter = .private
        XCTAssertEqual(model.visibleRepos.map(\.fullName), ["beta/relay"])

        model.filter = .online
        XCTAssertEqual(model.visibleRepos.map(\.fullName), ["gamma/desktop", "alpha/mobile"])

        model.sort = .mirrors
        XCTAssertEqual(model.visibleRepos.map(\.fullName), ["alpha/mobile", "gamma/desktop"])

        model.filter = .offline
        model.sort = .name
        XCTAssertEqual(model.visibleRepos.map(\.fullName), ["beta/relay"])
        model.filter = .public
        XCTAssertEqual(model.visibleRepos.map(\.fullName), ["alpha/mobile", "gamma/desktop"])
    }

    func testLabelsMatchTheDartControls() {
        XCTAssertEqual(RepoFilter.allCases.map(\.label), ["All", "Online", "Offline", "Public", "Private"])
        XCTAssertEqual(RepoSort.allCases.map(\.buttonLabel), ["Updated", "Name", "Mirrors"])
        XCTAssertEqual(
            RepoSort.allCases.map(\.menuLabel), ["Recently updated first", "Name A-Z", "Mirrors high-low"])
    }

    func testEmptyStatesDependOnActiveControls() async {
        let client = FakeRepoClient()
        let model = ReposScreenModel(client: client)
        await model.start()
        XCTAssertEqual(model.emptyTitle, "No repositories")
        XCTAssertEqual(model.emptyMessage, "Repository mirrors will appear here when available.")
        XCTAssertEqual(model.stateKey, "list-all-recentlyUpdated--0")
        model.queryText = "x"
        XCTAssertEqual(model.emptyTitle, "No matching repositories")
        XCTAssertEqual(model.emptyMessage, "Try another search term or filter.")
        model.queryText = ""
        model.filter = .private
        XCTAssertEqual(model.stateKey, "list-private-recentlyUpdated--0")
        XCTAssertTrue(model.hasActiveControls)
    }

    func testLoadFailureShowsTheErrorAndNoRows() async {
        let client = FakeRepoClient()
        client.repositoriesResult = { throw ApiException("HTTP 500 for /api/repositories") }
        let model = ReposScreenModel(client: client)
        await model.start()
        XCTAssertEqual(model.catalog.error, "Exception: HTTP 500 for /api/repositories")
        XCTAssertEqual(model.stateKey, "error")
        XCTAssertTrue(model.visibleRepos.isEmpty)
    }

    func testRowMetaHidesZeroCountsAndDatesTheUpdate() {
        let quiet = Repository(owner: "o", name: "r", stars: 0, forks: 0, mirrors: 0)
        XCTAssertEqual(ReposCatalog.metaItems(quiet).map(\.text), ["Public"])
        let busy = Repository(
            owner: "o", name: "r", language: "Go", stars: 3, forks: 1, mirrors: 1, isPrivate: true,
            updatedMs: 1_700_000_000_000)
        let items = ReposCatalog.metaItems(busy, nowMillis: 1_700_000_000_000 + 2 * 3_600_000)
        XCTAssertEqual(items.map(\.text), ["Go", "3", "1", "1 mirror", "Private", "Updated 2h ago"])
        XCTAssertEqual(items.map(\.icon), ["circle", "star_border", "call_split", "dns_outlined", "lock_outline", "update"])
        XCTAssertEqual(items.last?.tooltip, "Updated 2023-11-14")
        XCTAssertTrue(ReposCatalog.isOnline(Repository(owner: "o", name: "r", liveHost: true)))
        XCTAssertFalse(ReposCatalog.isOnline(Repository(owner: "o", name: "r")))
    }
}
