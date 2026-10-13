import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMGit
import FMNet
import Foundation
import XCTest
@testable import FMCode

private let syncOrigin = "https://forkmesh.test"

private func syncCredentials(_ repo: String = "demo") -> ForkMeshRemoteCredentials {
    ForkMeshRemoteCredentials(origin: syncOrigin, owner: "forkmesh", repo: repo)
}

private func syncSession() -> AuthSession {
    AuthSession(
        nodeName: "alice", email: "alice@example.com", status: "active", pubkey: "", emailVerified: true,
        isAdmin: false, avatarPng: "", avatarUpdatedAt: 0, createdAt: 1,
        savedAt: 1, sessionKind: "mobile_node", deviceKind: "mobile_node",
        capabilities: ["browse", "publish_repo", "owner_sign"], sessionToken: "session-token")
}

@MainActor
final class LocalSyncStoreTests: XCTestCase {
    private func harness(
        repos: [String] = ["demo"]
    ) throws -> (LocalGitOrigin, LocalRepoStore, PublishService, LocalMirrorLookups) {
        let origin = try LocalGitOrigin.make(self, commits: 2)
        let server = LocalGitSmartServer(git: origin.git)
        for repo in repos { server.serve("\(syncOrigin)/forkmesh/\(repo)", origin.bare) }
        let temp = try localTempDirectory(self, "fm-sync-store")
        let store = LocalRepoStore(root: temp.appendingPathComponent("repos"))
        store.workspaceFactory = { url in
            let workspace = GitWorkspace(worktree: url)
            workspace.transportFactory = { server }
            return workspace
        }
        let api = ApiService(settings: StaticApiSettings(serverUrl: "wss://forkmesh.test/ws"), http: FakeHTTPClient(), sleep: { _ in })
        let identity = try Identity.fromSeed((0..<32).map { UInt8($0 + 5) })
        let publisher = PublishService(api: api, identity: identity, prefs: InMemoryKeyValueStore())
        let lookups = LocalMirrorLookups()
        publisher.mirrorLookup = { _, _ in
            lookups.count += 1
            return []
        }
        return (origin, store, publisher, lookups)
    }

    func testPullResultsSpeakOfSyncing() {
        let fetch = LocalFetchResult(updatedRefs: [], packBytes: 0, objects: 0, remoteHead: nil)
        func result(_ outcome: LocalPullOutcome) -> LocalPullResult {
            LocalPullResult(branch: "main", fetch: fetch, outcome: outcome)
        }
        XCTAssertEqual(result(.upToDate).title, "Up to date")
        XCTAssertEqual(result(.ahead).title, "Up to date")
        XCTAssertEqual(result(.fastForwarded(from: "a", to: "b")).title, "Synced")
        XCTAssertEqual(result(.fastForwarded(from: "a", to: "b")).message, "Synced main with ForkMesh.")
        XCTAssertTrue(result(.fastForwarded(from: "a", to: "b")).changedWorktree)
        XCTAssertEqual(result(.diverged).title, "Sync needs attention")
        XCTAssertTrue(result(.diverged).needsAttention)
        XCTAssertTrue(result(.undetermined).needsAttention)
        XCTAssertFalse(result(.ahead).needsAttention)
        XCTAssertEqual(result(.noRemoteBranch).title, "Nothing to sync")
        XCTAssertEqual(
            result(.ahead).message, "Nothing to sync: main here already contains ForkMesh's. Push to share your commits.")
    }

    func testARepositoryOnTheDeviceIsFoundAndNeverClonedTwice() async throws {
        let (_, store, _, _) = try harness()
        let record = try await store.cloneFromMesh(name: "demo", credentials: syncCredentials())
        XCTAssertEqual(record.publishedAs, "forkmesh/demo")
        XCTAssertEqual(store.record(meshIdentity: " ForkMesh/Demo ")?.name, "demo")
        XCTAssertEqual(store.record(for: Repository(owner: "forkmesh", name: "demo"))?.name, "demo")
        XCTAssertEqual(store.record(for: Repository(owner: "mirror-node", name: "demo", publicOwner: "forkmesh"))?.name, "demo")
        XCTAssertNil(store.record(for: Repository(owner: "forkmesh", name: "other")))
        XCTAssertNil(store.record(meshIdentity: ""))

        do {
            _ = try await store.cloneFromMesh(name: "demo-2", credentials: syncCredentials())
            XCTFail("a second clone of the same repository was allowed")
        } catch let error as LocalRepoStoreException {
            XCTAssertEqual(error.message, "forkmesh/demo is already on this device as demo. Open it and sync instead.")
        }
        XCTAssertEqual(store.records.map(\.name), ["demo"])
        XCTAssertEqual(EngineFS.entityType(store.root.appendingPathComponent("demo-2").path), .notFound)
    }

    func testStoreSyncPullsTheCurrentBranchAndRefreshesTheRecord() async throws {
        let (origin, store, publisher, _) = try harness()
        let record = try await store.cloneFromMesh(name: "demo", credentials: syncCredentials())
        try origin.commit("step.txt", "later\n", "Later upstream")
        try origin.publish("main")

        let result = try await store.sync(record, publisher: publisher)

        guard case .fastForwarded = result.outcome else { return XCTFail("\(result.outcome)") }
        XCTAssertEqual(store.records.first?.lastCommitSubject, "Later upstream")
        XCTAssertFalse(store.isBusy)
        let solo = LocalRepoRecord(name: "solo", path: "/tmp/solo", createdAt: Date())
        do {
            _ = try await store.sync(solo, publisher: publisher)
            XCTFail("an unpublished repository was synced")
        } catch let error as LocalRepoStoreException {
            XCTAssertEqual(error.message, "solo is only on this device. Publish it before syncing.")
        }
    }

    func testTheSectionSyncsEveryCloneAndSummarises() async throws {
        let (origin, store, publisher, _) = try harness(repos: ["demo", "second"])
        _ = try await store.cloneFromMesh(name: "demo", credentials: syncCredentials())
        _ = try await store.cloneFromMesh(name: "second", credentials: syncCredentials("second"))
        _ = try await store.create(name: "scratch", author: localTestAuthor)
        let model = LocalReposSectionModel(store: store, session: { syncSession() }, publisher: publisher)
        XCTAssertTrue(model.canSync)
        XCTAssertEqual(model.cloneCount, 2)
        XCTAssertEqual(Set(model.syncableRecords.map(\.name)), ["demo", "second"])
        try origin.commit("step.txt", "later\n", "Later upstream")
        try origin.publish("main")

        await model.syncAll()

        XCTAssertEqual(model.notice?.message, "Synced 2 of 2 repositories · 2 updated")
        XCTAssertNil(model.busyLabel)
        let demo = try XCTUnwrap(store.records.first { $0.name == "demo" })
        await model.sync(demo)
        XCTAssertEqual(model.notice?.message, "demo: main is already up to date with ForkMesh.")
        XCTAssertFalse(LocalReposSectionModel(store: store, session: { nil }).canSync)
    }

    func testTheCloneButtonOpensOrSyncsTheCopyAlreadyHere() async throws {
        let (origin, store, publisher, lookups) = try harness()
        let model = CloneToDeviceModel(
            store: store, publisher: publisher, session: { syncSession() }, mirrors: { _, _ in
                lookups.count += 1
                return []
            })
        let repo = Repository(owner: "forkmesh", name: "demo")
        XCTAssertNil(model.existing(repo))
        let cloned = await model.clone(repo: repo, localName: "demo")
        let record = try XCTUnwrap(cloned)
        XCTAssertEqual(model.existing(repo)?.name, "demo")
        let lookupsAfterClone = lookups.count

        let again = await model.clone(repo: repo, localName: "demo-2")

        XCTAssertNil(again)
        XCTAssertEqual(model.notice?.message, "forkmesh/demo is already on this device as demo. Open it and sync instead.")
        XCTAssertEqual(lookups.count, lookupsAfterClone)
        try origin.commit("step.txt", "later\n", "Later upstream")
        try origin.publish("main")

        let result = await model.sync(record)

        guard case .fastForwarded = result?.outcome else { return XCTFail("\(String(describing: result))") }
        XCTAssertEqual(model.notice?.message, "Synced main with ForkMesh.")
        XCTAssertFalse(model.wasCancelled)
        XCTAssertFalse(model.isBusy)
    }
}

@MainActor
final class LocalMirrorLookups {
    var count = 0
}
