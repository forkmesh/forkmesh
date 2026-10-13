// The node-sync adapter over a real LocalRepoStore and the Git engine: the applier cases that
// need a worktree (FMChatTests runs the applier over the in-memory workspace), plus the
// per-project roots and keys the scope shim gives the store.

import FMApi
import FMBase
import FMChat
import FMCrypto
import FMNet
import FMGit
import Foundation
import XCTest
@testable import FMCode

@MainActor
final class LocalNodeSyncAdapterTests: XCTestCase {
    private func makeStore() throws -> LocalRepoStore {
        let temp = try localTempDirectory(self, "fm-node-sync")
        return LocalRepoStore(root: temp.appendingPathComponent("docs/forkmesh/repos"))
    }

    private func issueOpen(id: Int = 11, owner: String = "alice", repo: String = "notes") -> NodeSyncItem {
        NodeSyncItem(owner: owner, repo: repo, kind: .issues, id: id, data: [
            "titleIfNew": "Crash on launch",
            "event": [
                "type": "open",
                "title": "Crash on launch",
                "body": "It crashes every time.",
                "author": "F4rlozoS0ls2-Evs6Q7mLsSsZR1TVwMAsan0tdgaJoM",
                "authorName": "bob",
                "ts": 1_783_000_000_000,
            ],
        ])
    }

    private func applier(_ store: LocalRepoStore, _ prefs: KeyValueStore, scope: LocalRepoScope = LegacyLocalRepoScope())
        -> LocalNodeSyncApplier
    {
        LocalNodeSyncApplier.forLocalRepos(store, prefs: prefs, scope: scope) { ("alice", "alice@example.com") }
    }

    func testAppliesAnIssueIntoAPublishedRepositoryAndCommitsIt() async throws {
        let store = try makeStore()
        _ = try await store.create(name: "notes", author: localTestAuthor)
        try await store.markPublished("notes", "alice/notes")
        let prefs = InMemoryKeyValueStore([:])
        let applier = applier(store, prefs)
        defer { applier.dispose() }

        let ok = await applier.call(issueOpen())
        XCTAssertTrue(ok)
        XCTAssertEqual(applier.appliedTotal, 1)

        let found = try await store.find("notes")
        let record = try XCTUnwrap(found)
        let workspace = store.workspace(record)
        defer { workspace.dispose() }
        let open = try await workspace.listFiles(".forkmesh/issues/open")
        let number = try XCTUnwrap(open.first?.name)
        let issue = try await workspace.readFile(".forkmesh/issues/open/\(number)/issue-\(number).json")
        XCTAssertTrue(issue.text.contains("Crash on launch"))

        // Committed as the account, and nothing is left unstaged.
        let log = try await workspace.log(limit: 5)
        XCTAssertEqual(log.count, 2)
        XCTAssertEqual(log.first?.authorName, "alice")
        let status = try await workspace.status()
        XCTAssertTrue(status.staged.isEmpty && status.unstaged.isEmpty && status.untracked.isEmpty)
        // The store's index follows the new commit.
        XCTAssertNotEqual(store.records.first?.lastCommitSubject, "Initial commit")

        // A re-offered row is recognised from the ledger, not applied twice.
        XCTAssertEqual(prefs.stringList(forKey: LocalNodeSyncApplier.appliedPrefsKey), ["alice/notes|issues|row:11"])
        let again = await applier.call(issueOpen())
        XCTAssertTrue(again)
        let after = try await workspace.log(limit: 5)
        XCTAssertEqual(after.count, 2)
    }

    func testLeavesItemsForRepositoriesThisPhoneDoesNotHold() async throws {
        let store = try makeStore()
        _ = try await store.create(name: "notes", author: localTestAuthor)
        let applier = applier(store, InMemoryKeyValueStore([:]))
        defer { applier.dispose() }

        // Not published: nothing matches alice/notes.
        let ok = await applier.call(issueOpen())
        XCTAssertFalse(ok)
        XCTAssertEqual(applier.leftTotal, 1)
    }

    func testTheWorkspaceAdapterMeetsTheContract() async throws {
        let store = try makeStore()
        let record = try await store.create(name: "notes", author: localTestAuthor)
        let source = LocalNodeSyncRepositories(store: store)
        await source.ensureLoaded()
        XCTAssertEqual(source.records, [NodeSyncRepoRecord(name: "notes", path: record.path, publishedAs: nil)])
        let workspace = source.workspace(for: source.records[0])
        defer { workspace.dispose() }

        let summary = try await workspace.summary()
        XCTAssertEqual(summary.branch, "main")
        do {
            _ = try await workspace.readFile("missing/file.md")
            XCTFail("a missing path must throw")
        } catch {}

        try await workspace.writeFile("deep/er/file.md", "hello\n")
        let read = try await workspace.readFile("deep/er/file.md")
        XCTAssertEqual(read.text, "hello\n")
        let listed = try await workspace.listFiles("deep")
        XCTAssertEqual(listed, [NodeSyncFileEntry(name: "er", path: "deep/er", isDirectory: true)])

        // Nothing staged: the applier's success marker.
        do {
            _ = try await workspace.commit(message: "empty", author: NodeSyncAuthor(name: "a", email: "a@example.com"))
            XCTFail("an empty commit must throw")
        } catch {
            XCTAssertTrue(String(describing: error).contains("Nothing is staged"))
        }

        try await workspace.stage(["deep/er/file.md"])
        let id = try await workspace.commit(message: "add", author: NodeSyncAuthor(name: "a", email: "a@example.com"))
        XCTAssertEqual(id.count, 40)

        // A deletion is staged by naming the path.
        try await workspace.deleteFile("deep/er/file.md")
        try await workspace.stage(["deep/er/file.md"])
        _ = try await workspace.commit(message: "remove", author: NodeSyncAuthor(name: "a", email: "a@example.com"))
        do {
            _ = try await workspace.readFile("deep/er/file.md")
            XCTFail("a deleted path must throw")
        } catch {}
        try await source.refresh("notes")
        XCTAssertEqual(store.records.first?.lastCommitSubject, "remove")
    }

    func testScopesTheLedgerRootAndCatalogNodeIdPerProject() async throws {
        let legacy = LegacyLocalRepoScope()
        let pinned = LegacyLocalRepoScope.project("App.Next.ForkMesh.com:8443")
        let documents = URL(fileURLWithPath: "/var/docs", isDirectory: true)

        XCTAssertEqual(LocalRepoStore.root(for: legacy, documents: documents).path, "/var/docs/forkmesh/repos")
        XCTAssertEqual(
            LocalRepoStore.root(for: pinned, documents: documents).path,
            "/var/docs/projects/app.next.forkmesh.com_8443/forkmesh/repos")
        XCTAssertEqual(pinned.namespace, "p/App.Next.ForkMesh.com:8443/")
        XCTAssertEqual(pinned.key("nodeSync/applied"), "p/App.Next.ForkMesh.com:8443/nodeSync/applied")
        XCTAssertEqual(legacy.key("nodeSync/applied"), "nodeSync/applied")
        XCTAssertEqual(LegacyLocalRepoScope.slugFor("[::1]:80"), "___1__80")

        let store = try makeStore()
        _ = try await store.create(name: "notes", author: localTestAuthor)
        try await store.markPublished("notes", "alice/notes")
        let prefs = InMemoryKeyValueStore([:])
        let applier = applier(store, prefs, scope: pinned)
        defer { applier.dispose() }
        let ok = await applier.call(issueOpen())
        XCTAssertTrue(ok)
        XCTAssertNil(prefs.stringList(forKey: "nodeSync/applied"))
        XCTAssertEqual(prefs.stringList(forKey: pinned.key("nodeSync/applied")), ["alice/notes|issues|row:11"])

        // One catalog node id per instance.
        let api = ApiService(settings: StaticApiSettings(serverUrl: "wss://forkmesh.test/ws"), http: FakeHTTPClient(), sleep: { _ in })
        let identity = try Identity.fromSeed((0..<32).map { UInt8($0 + 3) })
        let shared = InMemoryKeyValueStore()
        let first = PublishService(api: api, identity: identity, prefs: shared).nodeId()
        let second = PublishService(api: api, identity: identity, prefs: shared, scope: pinned).nodeId()
        XCTAssertEqual(shared.string(forKey: "node/catalogNodeId"), first)
        XCTAssertEqual(shared.string(forKey: "p/App.Next.ForkMesh.com:8443/node/catalogNodeId"), second)
        XCTAssertNotEqual(first, second)
    }
}
