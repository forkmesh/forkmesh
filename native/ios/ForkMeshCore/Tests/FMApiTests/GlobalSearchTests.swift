import FMBase
import Foundation
import XCTest

@testable import FMApi

private struct FailingSource: GlobalSearchSource {
    let kind: SearchBucketKind = .code
    struct Failure: Error {}
    func search(_ query: String) async throws -> [SearchHit] { throw Failure() }
}

final class GlobalSearchTests: XCTestCase {
    let repo = RepoRef(owner: "forkmesh", name: "app")

    func testMatchRanksExactThenPrefixThenWordThenSubstring() {
        let ranked = SearchMatch.rank(["unrelated", "my notes", "notes", "notebook", "keynotes"], "notes") { [$0] }
        XCTAssertEqual(ranked, ["notes", "my notes", "keynotes"])
        XCTAssertNil(SearchMatch.score("x", "  "))
        XCTAssertEqual(SearchMatch.score("my-cool_repo", "my cool repo"), 4)
        XCTAssertEqual(SearchMatch.score("MyCoolRepo", "my-cool"), 4)
        XCTAssertNil(SearchMatch.score("abc", "--"))
    }

    func testDestinationsFindHubRoutesByTitle() async throws {
        let hits = try await DestinationsSearchSource().search("cal")
        XCTAssertEqual(hits.first?.route, .calendar(date: nil, mode: .month))
        let hosts = try await DestinationsSearchSource().search("hosts")
        XCTAssertEqual(hosts.first?.route, .hosts)
    }

    func testRepositoriesIssuesPeopleAndCode() async throws {
        let repos = RepositoriesSearchSource {
            [Repository(owner: "forkmesh", name: "app"), Repository(owner: "alice", name: "notes")]
        }
        let r1 = try await repos.search("app")
        XCTAssertEqual(r1.map(\.route), [.repo(repo, section: nil)])
        XCTAssertEqual(r1.first?.title, "app")
        XCTAssertEqual(r1.first?.subtitle, "forkmesh")

        let items: [SearchableItem] = [
            SearchableItem(number: 12, title: "Router restores tabs", isPull: true),
            SearchableItem(number: 4, title: "Router crash", isPull: false),
        ]
        let repoList: @Sendable (String) async -> [RepoRef] = { _ in [RepoRef(owner: "forkmesh", name: "app")] }
        let issues = IssuesSearchSource(pulls: false, repos: repoList) { _ in items }
        let pulls = IssuesSearchSource(pulls: true, repos: repoList) { _ in items }
        let r2 = try await issues.search("router")
        XCTAssertEqual(r2.map(\.route), [.issue(repo, number: 4)])
        let r3 = try await pulls.search("#12")
        XCTAssertEqual(r3.map(\.route), [.pull(repo, number: 12, tab: .conversation)])

        let people = PeopleSearchSource(known: { ["alice", "alfred"] }, accounts: { _ in ["albert", "Alice"] })
        let r4 = try await people.search("@al")
        XCTAssertEqual(r4.map(\.route), [.profile(name: "alice"), .profile(name: "alfred"), .profile(name: "albert")])
        let r5 = try await people.search("a b")
        XCTAssertEqual(r5.map(\.route), [])
        let r5b = try await PeopleSearchSource(known: { ["alice"] }).search("myrepo")
        XCTAssertEqual(r5b.map(\.route), [])

        let code = CodeSearchSource(repos: repoList) { _, _ in [CodeMatchItem(path: "src/a.swift", line: 3, text: "router")] }
        let r6 = try await code.search("r")
        XCTAssertEqual(r6.count, 0)
        let r7 = try await code.search("router")
        XCTAssertEqual(r7.map(\.route), [.file(repo, ref: "", path: "src/a.swift")])

        let chat = ChatSearchSource { ["#general", "@bob"] }
        let r8 = try await chat.search("bob")
        XCTAssertEqual(r8.map(\.route), [.dm(user: "bob")])
    }

    func testRepositoriesIncludeOnDeviceReposAndSurviveCatalogFailure() async throws {
        struct Offline: Error {}
        let device: @Sendable () async -> [LocalRepositoryItem] = {
            [
                LocalRepositoryItem(name: "my-garden", description: "plants"),
                LocalRepositoryItem(name: "app", publishedAs: "forkmesh/app"),
            ]
        }
        let mixed = RepositoriesSearchSource(load: { [Repository(owner: "forkmesh", name: "app")] }, local: device)
        let garden = try await mixed.search("my garden")
        XCTAssertEqual(garden.map(\.route), [.localRepo(id: "my-garden")])
        XCTAssertEqual(garden.first?.subtitle, "On this device · plants")
        let app = try await mixed.search("app")
        XCTAssertEqual(app.map(\.route), [.repo(repo, section: nil)])

        let offline = RepositoriesSearchSource(load: { throw Offline() }, local: device)
        let fallback = try await offline.search("garden")
        XCTAssertEqual(fallback.map(\.route), [.localRepo(id: "my-garden")])
        do {
            _ = try await RepositoriesSearchSource(load: { throw Offline() }).search("garden")
            XCTFail("expected the catalog failure")
        } catch {}
    }

    func testCommitsMatchSubjectAuthorAndSha() async throws {
        let commits = CommitsSearchSource(repos: { _ in [RepoRef(owner: "forkmesh", name: "app")] }) { _ in
            [
                CommitItem(sha: "abc1234def", subject: "Fix router crash", author: "alice"),
                CommitItem(sha: "fedcba98", subject: "Add search", author: "bob"),
                CommitItem(sha: "", subject: "router without sha"),
            ]
        }
        let byText = try await commits.search("router")
        XCTAssertEqual(byText.map(\.route), [.commit(repo, sha: "abc1234def")])
        XCTAssertEqual(byText.first?.subtitle, "forkmesh/app abc1234 · alice")
        let bySha = try await commits.search("fedc")
        XCTAssertEqual(bySha.map(\.route), [.commit(repo, sha: "fedcba98")])
        let byAuthor = try await commits.search("bob")
        XCTAssertEqual(byAuthor.first?.title, "Add search")
        let tooShort = try await commits.search("a")
        XCTAssertTrue(tooShort.isEmpty)
    }

    func testRepositoriesAndCommitsBelongToTheCodeScope() {
        XCTAssertEqual(SearchBucketKind.allCases.first, .repositories)
        XCTAssertEqual(SearchBucketKind.repositories.scope, .code)
        XCTAssertEqual(SearchBucketKind.commits.scope, .code)
        XCTAssertEqual(SearchBucketKind.people.scope, .people)
    }

    @MainActor func testModelBucketsScopesAndFailures() async {
        let repoList: @Sendable (String) async -> [RepoRef] = { _ in [RepoRef(owner: "forkmesh", name: "app")] }
        let items = (1...8).map { SearchableItem(number: $0, title: "router \($0)", isPull: false) }
        let sources: [GlobalSearchSource] = [
            DestinationsSearchSource(),
            IssuesSearchSource(pulls: false, repos: repoList) { _ in items },
            FailingSource(),
        ]
        let model = GlobalSearchModel(sources: sources, saved: SavedSearchStore(store: InMemoryKeyValueStore()))
        model.query = "router"
        await model.run()
        XCTAssertEqual(model.buckets.map(\.kind), [.issues])
        XCTAssertEqual(model.buckets.first?.hits.count, GlobalSearchModel.bucketLimit)
        XCTAssertEqual(model.buckets.first?.total, 8)
        XCTAssertTrue(model.buckets.first?.hasMore ?? false)
        XCTAssertEqual(model.failures, [.code])

        await model.expand(.issues)
        XCTAssertEqual(model.buckets.first?.hits.count, 8)

        model.scope = .people
        await model.expand(nil)
        XCTAssertTrue(model.buckets.isEmpty)
        XCTAssertEqual(model.sources(for: .issues).map(\.kind), [.issues])
        XCTAssertEqual(model.sources(for: .all).count, 3)
    }

    @MainActor func testModelDebouncesAndClearsOnEmpty() async throws {
        let model = GlobalSearchModel(
            sources: [DestinationsSearchSource()], saved: SavedSearchStore(store: nil), debounce: .milliseconds(20))
        model.query = "notes"
        model.queryChanged()
        XCTAssertTrue(model.buckets.isEmpty)
        try await Task.sleep(for: .milliseconds(200))
        XCTAssertEqual(model.buckets.first?.kind, .destinations)
        model.query = " "
        model.queryChanged()
        XCTAssertTrue(model.buckets.isEmpty)
    }

    @MainActor func testSavedAndRecentSearchesPersist() async {
        let store = InMemoryKeyValueStore()
        var clock = Date(timeIntervalSince1970: 100)
        let saved = SavedSearchStore(store: store, now: { clock })
        saved.save(query: "router", scope: .code, count: 3)
        saved.save(query: "Router", scope: .code, count: 4)
        XCTAssertEqual(saved.saved.count, 1)
        XCTAssertEqual(saved.saved.first?.lastCount, 4)
        clock = Date(timeIntervalSince1970: 200)
        saved.recordRun(query: "router", scope: .code, count: 9)
        saved.recordRun(query: "other", scope: .code, count: 1)
        XCTAssertEqual(saved.saved.first?.lastCount, 9)
        XCTAssertEqual(saved.saved.first?.lastRunAt, 200)
        XCTAssertEqual(saved.saved.first?.route, .search(query: "Router", scope: .code))
        for index in 0..<15 { saved.commit(query: "q\(index)", scope: .all) }
        saved.commit(query: "q3", scope: .all)
        XCTAssertEqual(saved.recent.count, SavedSearchStore.maxRecent)
        XCTAssertEqual(saved.recent.first?.query, "q3")

        let reopened = SavedSearchStore(store: store)
        XCTAssertEqual(reopened.saved, saved.saved)
        XCTAssertEqual(reopened.recent, saved.recent)
        reopened.remove(reopened.saved[0])
        reopened.clearRecent()
        XCTAssertTrue(SavedSearchStore(store: store).saved.isEmpty)
    }

    func testScopesMapOntoTheRouteScope() {
        for scope in GlobalSearchScope.allCases {
            XCTAssertEqual(GlobalSearchScope(scope.routeScope), scope)
        }
        XCTAssertEqual(GlobalSearchScope.allCases.count, 5)
    }
}
