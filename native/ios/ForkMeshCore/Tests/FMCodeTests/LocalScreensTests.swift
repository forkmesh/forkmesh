// The logic of clone to device, quick edit, the local repos section and the local repo screen,
// against the core view models the SwiftUI screens bind to.

import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMGit
import FMNet
import Foundation
import XCTest
@testable import FMCode

private func mobileSession(capabilities: [String] = ["browse", "publish_repo", "owner_sign"]) -> AuthSession {
    AuthSession(
        nodeName: "alice", email: "alice@example.com", status: "active", pubkey: "", emailVerified: true,
        isAdmin: false, avatarPng: "", avatarUpdatedAt: 0, createdAt: 1,
        savedAt: 1, sessionKind: "mobile_node", deviceKind: "mobile_node", capabilities: capabilities,
        sessionToken: "session-token")
}

/// Records what was asked of the engine; the "clone" initializes a repository
/// locally (or fails as scripted).
final class LocalScriptedCloneWorkspace: GitWorkspace, @unchecked Sendable {
    var cloneCredentials: ForkMeshRemoteCredentials?
    var cloneMaxPackBytes: Int?
    var cloneBranch: String?
    var cloneDepth: Int?
    var failure: Error?
    var failureFor: ((ForkMeshRemoteCredentials) -> Error?)?

    override func clone(
        credentials: ForkMeshRemoteCredentials, branch: String? = nil, depth: Int? = nil,
        maxPackBytes: Int = GitWorkspace.maxCloneBytes,
        onProgress: (@Sendable (GitTransferProgress) -> Void)? = nil
    ) async throws -> LocalCloneResult {
        cloneCredentials = credentials
        cloneDepth = depth
        cloneMaxPackBytes = maxPackBytes
        cloneBranch = branch
        onProgress?(GitTransferProgress(phase: "receivingPack", bytes: 4096))
        if let failure { throw failure }
        if let failure = failureFor?(credentials) { throw failure }
        try await initialize(files: ["README.md": "# cloned\n"], commitMessage: "Upstream\n", author: localTestAuthor)
        return LocalCloneResult(branch: "main", packBytes: 100, objects: 3)
    }
}

@MainActor
final class LocalScreensTests: XCTestCase {
    private func cloneHarness(
        session: AuthSession? = mobileSession(), failure: Error? = nil,
        mirrors: (@MainActor (String, String) async throws -> [RepoMirror])? = nil,
        failureFor: ((ForkMeshRemoteCredentials) -> Error?)? = nil,
        attempts: LocalCloneAttempts? = nil
    ) throws -> (CloneToDeviceModel, LocalRepoStore, () -> LocalScriptedCloneWorkspace?, Identity) {
        let temp = try localTempDirectory(self, "fm-clone")
        let store = LocalRepoStore(root: temp.appendingPathComponent("repos"))
        var last: LocalScriptedCloneWorkspace?
        store.workspaceFactory = { url in
            let workspace = LocalScriptedCloneWorkspace(worktree: url)
            workspace.failure = failure
            workspace.failureFor = { credentials in
                attempts?.urls.append(credentials.remoteUrl)
                return failureFor?(credentials)
            }
            last = workspace
            return workspace
        }
        let api = ApiService(settings: StaticApiSettings(serverUrl: "wss://forkmesh.test/ws"), http: FakeHTTPClient(), sleep: { _ in })
        let identity = try Identity.fromSeed((0..<32).map { UInt8($0 + 7) })
        let publisher = PublishService(api: api, identity: identity, prefs: InMemoryKeyValueStore())
        let model = CloneToDeviceModel(store: store, publisher: publisher, session: { session }, mirrors: mirrors)
        return (model, store, { last }, identity)
    }

    private static func mirror(_ node: String, latency: Int, healthy: Bool = true) -> RepoMirror {
        RepoMirror(json: [
            "node": .string(node), "owner": .string(node), "repo": "forkmesh", "status": healthy ? "online" : "offline",
            "lastSync": 1_790_000_000_000, "cloneAvailable": true, "integrity": "ok",
            "mirrorUrl": .string("https://\(node).forkmesh.com/forkmesh/forkmesh.git"),
            "endpointHealthy": .bool(healthy), "endpointFresh": .bool(healthy), "latencyMs": .int(Int64(latency)),
        ])
    }

    func testClonesFromTheFastestHealthyMirrorAndFailsOverToTheNext() async throws {
        let attempts = LocalCloneAttempts()
        let (model, store, last, _) = try cloneHarness(
            mirrors: { owner, name in
                XCTAssertEqual("\(owner)/\(name)", "forkmesh/forkmesh")
                return [Self.mirror("mirror36", latency: 90), Self.mirror("mirror35", latency: 40)]
            },
            failureFor: { credentials in
                credentials.host == "mirror35.forkmesh.com"
                    ? GitWorkspaceException("Could not reach ForkMesh.", code: .networkUnavailable) : nil
            },
            attempts: attempts)
        let repo = Repository(owner: "mirror35", name: "forkmesh", description: "The flagship", publicOwner: "forkmesh")

        let record = await model.clone(repo: repo, localName: "forkmesh")

        XCTAssertEqual(attempts.urls, [
            "https://mirror35.forkmesh.com/forkmesh/forkmesh.git",
            "https://mirror36.forkmesh.com/forkmesh/forkmesh.git",
        ])
        let credentials = try XCTUnwrap(last()?.cloneCredentials)
        XCTAssertFalse(credentials.canSign)
        XCTAssertEqual(credentials.relayUrl, "https://forkmesh.test/forkmesh/forkmesh")
        XCTAssertEqual(last()?.cloneDepth, GitWorkspace.meshCloneDepth)
        XCTAssertEqual(record?.publishedAs, "forkmesh/forkmesh")
        XCTAssertEqual(record?.description, "The flagship")
        XCTAssertEqual(model.notice?.message, "Cloned forkmesh/forkmesh to forkmesh from mirror36.forkmesh.com.")
        XCTAssertEqual(store.meshIdentities, ["forkmesh/forkmesh"])
        XCTAssertFalse(model.isBusy)
    }

    func testWhenNothingServesTheRepositoryTheErrorNamesEverySourceTried() async throws {
        let attempts = LocalCloneAttempts()
        let (model, store, _, _) = try cloneHarness(
            mirrors: { _, _ in [Self.mirror("mirror35", latency: 40)] },
            failureFor: { credentials in
                credentials.endpointUrl.isEmpty
                    ? GitWorkspaceException(
                        describeRemoteFailure(GitException(.remoteRejected, "remote returned HTTP 503"), host: "forkmesh.test").message,
                        code: .remoteRejected)
                    : GitWorkspaceException("The secure connection to ForkMesh was rejected.", code: .tlsRejected)
            },
            attempts: attempts)

        let record = await model.clone(owner: "forkmesh", name: "forkmesh", localName: "forkmesh")

        XCTAssertNil(record)
        XCTAssertEqual(attempts.urls.count, 2)
        let message = try XCTUnwrap(model.notice?.message)
        XCTAssertTrue(message.hasPrefix("Could not clone forkmesh/forkmesh from any of its 2 sources."))
        XCTAssertTrue(message.contains("• mirror35.forkmesh.com: The secure connection to ForkMesh was rejected."))
        XCTAssertTrue(message.contains("• the relay (forkmesh.test): Nothing on this relay (forkmesh.test) is serving this repository"))
        XCTAssertFalse(message.contains("offline"))
        let listed = try await store.list()
        XCTAssertTrue(listed.isEmpty)
    }

    func testAMirrorListThatCannotBeReadStillTriesTheRelayAndSaysWhy() async throws {
        let (model, _, _, _) = try cloneHarness(
            mirrors: { _, _ in throw ApiException("HTTP 500") },
            failureFor: { _ in GitWorkspaceException("Could not reach ForkMesh.", code: .networkUnavailable) })

        _ = await model.clone(owner: "forkmesh", name: "forkmesh", localName: "forkmesh")

        let message = try XCTUnwrap(model.notice?.message)
        XCTAssertTrue(message.hasPrefix("Could not list the mirrors of forkmesh/forkmesh ("))
        XCTAssertTrue(message.contains("• the relay (forkmesh.test): Could not reach ForkMesh."))
    }

    func testATooLargeRepositoryStopsAtTheFirstMirror() async throws {
        let attempts = LocalCloneAttempts()
        let (model, _, _, _) = try cloneHarness(
            mirrors: { _, _ in [Self.mirror("mirror35", latency: 40), Self.mirror("mirror36", latency: 50)] },
            failureFor: { _ in
                GitWorkspaceException("This repository is too large to clone on this device.", code: .resourceLimitExceeded)
            },
            attempts: attempts)

        _ = await model.clone(owner: "forkmesh", name: "forkmesh", localName: "forkmesh")

        XCTAssertEqual(attempts.urls.count, 1)
        XCTAssertEqual(model.notice?.message, "This repository is too large to clone on this device.")
    }

    // MARK: Clone to device

    func testClonesFromTheAppOriginAndLandsInTheLocalStore() async throws {
        let (model, store, last, identity) = try cloneHarness()
        let name = await model.suggestedName("catalog-repo")
        XCTAssertEqual(name, "catalog-repo")
        let record = await model.clone(owner: "net", name: "catalog-repo", localName: name, description: "A network repository")
        let credentials = try XCTUnwrap(last()?.cloneCredentials)
        XCTAssertEqual(credentials.remoteUrl, "https://forkmesh.test/net/catalog-repo")
        XCTAssertTrue(credentials.canSign)
        XCTAssertEqual(credentials.pubkeyB64url, identity.publicKeyB64url)
        XCTAssertEqual(credentials.seed.count, 32)
        XCTAssertEqual(last()?.cloneMaxPackBytes, GitWorkspace.maxCloneBytes)
        XCTAssertNil(last()?.cloneBranch)

        XCTAssertEqual(record?.name, "catalog-repo")
        XCTAssertEqual(record?.publishedAs, "net/catalog-repo")
        XCTAssertEqual(record?.origin, .cloned)
        XCTAssertEqual(record?.syncLabel, "Cloned")
        XCTAssertEqual(record?.description, "A network repository")
        let stored = try await store.list()
        XCTAssertEqual(stored.map(\.name), ["catalog-repo"])
        XCTAssertEqual(stored.first?.publishedAs, "net/catalog-repo")
        XCTAssertEqual(stored.first?.origin, .cloned)
        XCTAssertEqual(model.notice?.message, "Cloned net/catalog-repo to catalog-repo from the relay (forkmesh.test).")
        XCTAssertFalse(model.isBusy)
        XCTAssertFalse(store.isBusy)
        XCTAssertEqual(store.meshIdentities, ["net/catalog-repo"])
        // The name is taken now.
        let next = await model.suggestedName("catalog-repo")
        XCTAssertEqual(next, "catalog-repo-2")
    }

    func testClonesAnonymouslyWhenSignedOut() async throws {
        let (model, _, last, _) = try cloneHarness(session: nil)
        let record = await model.clone(owner: "net", name: "catalog-repo", localName: "catalog-repo")
        let credentials = try XCTUnwrap(last()?.cloneCredentials)
        XCTAssertFalse(credentials.canSign)
        XCTAssertTrue(credentials.seed.isEmpty)
        XCTAssertEqual(credentials.pubkeyB64url, "")
        XCTAssertNotNil(record)
    }

    func testAFailedCloneLeavesNothingBehindAndReadsAsASentence() async throws {
        let refused = GitWorkspaceException(
            describeRemoteFailure(GitException(.remoteRejected, "remote returned HTTP 401")).message,
            code: .remoteRejected, detail: "push_unauthorized")
        let (model, store, _, _) = try cloneHarness(failure: refused)
        let record = await model.clone(owner: "net", name: "catalog-repo", localName: "catalog-repo")
        XCTAssertNil(record)
        XCTAssertTrue(model.notice?.message.contains("refused this device") ?? false)
        XCTAssertFalse(model.notice?.message.contains("HTTP 401") ?? true)
        let listed = try await store.list()
        XCTAssertTrue(listed.isEmpty)
        XCTAssertEqual(EngineFS.entityType(store.root.appendingPathComponent("catalog-repo").path), .notFound)

        let (cancelModel, cancelStore, _, _) = try cloneHarness(failure: GitWorkspaceException("Operation cancelled.", code: .cancelled))
        _ = await cancelModel.clone(owner: "net", name: "x", localName: "x")
        XCTAssertEqual(cancelModel.notice?.message, "Operation cancelled.")
        let none = try await cancelStore.list()
        XCTAssertTrue(none.isEmpty)
    }

    func testSurfacesAStoreRefusalReadably() async throws {
        let (model, _, _, _) = try cloneHarness()
        _ = await model.clone(owner: "net", name: "catalog-repo", localName: "catalog-repo")
        let again = await model.clone(owner: "net", name: "catalog-repo", localName: "catalog-repo-2")
        XCTAssertNil(again)
        XCTAssertEqual(
            model.notice?.message, "net/catalog-repo is already on this device as catalog-repo. Open it and sync instead.")
        let clash = await model.clone(owner: "net", name: "other-repo", localName: "catalog-repo")
        XCTAssertNil(clash)
        XCTAssertEqual(model.notice?.message, "A local repository with that name already exists.")
        XCTAssertTrue(CloneToDeviceModel.sizeNote.contains("latest commit of the default branch"))
        XCTAssertFalse(CloneToDeviceModel.sizeNote.contains("refused"))
        XCTAssertEqual(GitWorkspace.meshCloneDepth, 1)
        XCTAssertEqual(GitWorkspace.maxCloneBytes, GitResourceLimits.hardMaxCompressedPackBytes)
        XCTAssertEqual(try GitWorkspace.deviceLimits().maxInflatedBytes, GitResourceLimits.hardMaxInflatedBytes)
        XCTAssertEqual(try GitWorkspace.deviceLimits().maxObjectBytes, GitResourceLimits.hardMaxObjectBytes)
        XCTAssertEqual(CloneToDeviceModel.suggest("my repo!", taken: []), "my-repo-")
    }

    // MARK: Quick edit

    func testQuickEditBlocker() {
        XCTAssertNil(QuickEdit.blocker(bytes: Array("hi\n".utf8)))
        XCTAssertEqual(QuickEdit.blocker(bytes: [0x00, 0x01]), .binary)
        XCTAssertEqual(QuickEdit.blocker(bytes: [0x68, 0xff]), .notUtf8)
        XCTAssertEqual(QuickEdit.blocker(bytes: [UInt8](repeating: 0, count: 2048), maxBytes: 1024), .tooLarge)
        // Size is judged before content, so a huge binary reads as huge.
        XCTAssertEqual(QuickEdit.blocker(bytes: [0, 0, 0], maxBytes: 2), .tooLarge)
        XCTAssertTrue(QuickEditBlocker.tooLarge.message.contains("512 KB"))
    }

    func testQuickEditHead() {
        let at = Date(epochMillis: 1_789_828_200_000) // 2026-09-19T14:30:00Z
        XCTAssertEqual(QuickEdit.head(account: "kaif", path: "docs/README.md", now: at), "kaif:mobile-edit/readme-md-0919-1430")
        XCTAssertEqual(QuickEdit.head(account: "", path: "README.md", now: at), "mobile-edit/readme-md-0919-1430")
        XCTAssertEqual(QuickEdit.head(account: "kaif", path: ".env", now: at), "kaif:mobile-edit/env-0919-1430")
        XCTAssertEqual(QuickEdit.head(account: "kaif", path: "---", now: at), "kaif:mobile-edit/file-0919-1430")
        XCTAssertNotEqual(
            QuickEdit.head(account: "k", path: "a.txt", now: at),
            QuickEdit.head(account: "k", path: "a.txt", now: at.addingTimeInterval(60)))
        XCTAssertTrue(QuickEdit.submitFailure("ApiException: inbox_full").contains("inbox is full"))
    }

    private func quickModel(_ bytes: [UInt8], status: Int = 200) -> (QuickEditModel, SubmittedPulls) {
        let submitted = SubmittedPulls()
        let model = QuickEditModel(
            repo: Repository(owner: "owner", name: "repo", defaultBranch: "main"),
            path: "README.md", ref: "main",
            readBytes: { _, _, path, _, _ in
                if status != 200 { throw ApiException("Could not read \(path) (HTTP \(status)).") }
                return Data(bytes)
            },
            submit: { owner, name, title, base, head, patch, description, author in
                submitted.values.append([owner, name, title, base, head, patch, description, author])
            })
        return (model, submitted)
    }

    func testQuickEditModelOpensTheServedTextAndProposes() async throws {
        let (model, submitted) = quickModel(Array("one\ntwo\n".utf8))
        await model.load()
        XCTAssertEqual(model.text, "one\ntwo\n")
        XCTAssertFalse(model.canPropose)
        XCTAssertEqual(model.base, "main")
        model.text = "one\nTWO\n"
        XCTAssertTrue(model.canPropose)
        let result = await model.propose(title: "Update README.md", description: "why", account: "kaif")
        XCTAssertEqual(result, .submitted("Submitted to the inbox - pending the repo owner applying it."))
        let sent = try XCTUnwrap(submitted.values.first)
        XCTAssertEqual(sent[0], "owner")
        XCTAssertEqual(sent[3], "main")
        XCTAssertTrue(sent[4].hasPrefix("kaif:mobile-edit/readme-md-"))
        XCTAssertTrue(sent[5].contains("-two\n+TWO\n"))
        XCTAssertTrue(sent[5].contains("index \(GitBlobId.of(Array("one\ntwo\n".utf8)).hex).."))
    }

    func testQuickEditModelExplainsBlockersAndFailures() async throws {
        let (binary, _) = quickModel([0x89, 0x50, 0x4e, 0x00])
        await binary.load()
        XCTAssertEqual(binary.blocker, .binary)
        let (missing, _) = quickModel([], status: 404)
        await missing.load()
        XCTAssertNil(missing.blocker)
        XCTAssertNotNil(missing.error)
        XCTAssertFalse(missing.loading)
        // Edited and undone by hand: nothing to propose.
        let (undone, submitted) = quickModel(Array("a\n".utf8))
        await undone.load()
        undone.text = "a\n"
        let result = await undone.propose(title: "t", description: "", account: "")
        XCTAssertEqual(result, .unchanged("Nothing changed in README.md."))
        XCTAssertTrue(submitted.values.isEmpty)
    }

    // MARK: Local repos section

    func testSectionSlugifyAndCreate() async throws {
        XCTAssertEqual(LocalReposSectionModel.slugify("My Project (copy)"), "My-Project-copy")
        XCTAssertEqual(LocalReposSectionModel.slugify("..hidden.."), "hidden")
        XCTAssertEqual(LocalReposSectionModel.slugify("!!!"), "imported")
        XCTAssertEqual(LocalReposSectionModel.zipBaseName("proj.ZIP"), "proj")
        let temp = try localTempDirectory(self, "fm-section")
        let store = LocalRepoStore(root: temp.appendingPathComponent("repos"))
        let model = LocalReposSectionModel(store: store, session: { mobileSession() })
        await model.load()
        XCTAssertTrue(model.isLoaded)
        XCTAssertTrue(model.records.isEmpty)
        let record = await model.create(name: "notes", description: "")
        XCTAssertEqual(record?.name, "notes")
        XCTAssertEqual(model.records.map(\.name), ["notes"])
        let dup = await model.create(name: "notes", description: "")
        XCTAssertNil(dup)
        XCTAssertEqual(model.notice?.message, "A local repository with that name already exists.")
        XCTAssertTrue(LocalRepoText.recordMeta(model.records[0]).hasPrefix("main · committed "))
    }

    func testSectionUsesLocalDefaultBranchAndAuthorOverride() async throws {
        let temp = try localTempDirectory(self, "fm-section-defaults")
        let store = LocalRepoStore(root: temp.appendingPathComponent("repos"))
        let prefs = InMemoryKeyValueStore()
        prefs.set("trunk", forKey: SettingsService.localDefaultBranchKey)
        prefs.set("Alice Laptop", forKey: SettingsService.localAuthorNameKey)
        prefs.set("laptop@example.com", forKey: SettingsService.localAuthorEmailKey)
        let model = LocalReposSectionModel(store: store, session: { mobileSession() }, prefs: prefs)
        XCTAssertEqual(model.author, GitAuthor(name: "Alice Laptop", email: "laptop@example.com"))
        XCTAssertEqual(model.defaultBranch, "trunk")
        XCTAssertTrue(model.createHintText.contains("initial commit on trunk."))
        await model.load()
        let record = await model.create(name: "notes", description: "")
        XCTAssertEqual(record?.branch, "trunk")
        let screen = LocalRepoScreenModel(record: record!, store: store, publisher: nil, session: { mobileSession() }, prefs: prefs)
        defer { screen.dispose() }
        await screen.refreshAll()
        XCTAssertEqual(screen.branchLabel, "trunk")
        XCTAssertEqual(screen.log.first?.authorName, "Alice Laptop")
        XCTAssertEqual(screen.log.first?.authorEmail, "laptop@example.com")

        prefs.set("bad email", forKey: SettingsService.localAuthorEmailKey)
        prefs.remove(SettingsService.localAuthorNameKey)
        XCTAssertEqual(screen.author, GitAuthor(name: "alice", email: "alice@example.com"))
        prefs.set("bad..branch", forKey: SettingsService.localDefaultBranchKey)
        XCTAssertEqual(model.defaultBranch, "main")
        XCTAssertEqual(LocalReposSectionModel(store: store, session: { nil }).author.name, "forkmesh")
    }

    // MARK: Local repo screen

    func testScreenModelFilesChangesBranchesAndDelete() async throws {
        let temp = try localTempDirectory(self, "fm-screen")
        let store = LocalRepoStore(root: temp.appendingPathComponent("repos"))
        let record = try await store.create(name: "demo", author: localTestAuthor)
        let model = LocalRepoScreenModel(record: record, store: store, publisher: nil, session: { mobileSession() })
        defer { model.dispose() }
        await model.refreshAll()
        XCTAssertNil(model.loadError)
        XCTAssertEqual(model.files?.map(\.path), ["README.md"])
        XCTAssertEqual(model.branchLabel, "main")
        XCTAssertEqual(model.log.count, 1)

        // New file in a folder, then navigate into it.
        let entry = await model.newFile("docs/notes.md")
        XCTAssertEqual(entry?.path, "docs/notes.md")
        await model.openDirectory("docs")
        XCTAssertEqual(model.files?.map(\.path), ["docs/notes.md"])
        XCTAssertEqual(model.newFilePrefill, "docs/")
        XCTAssertEqual(LocalRepoText.breadcrumbs("docs/a").map(\.label), ["Root", "docs", "a"])

        // Stage and commit.
        XCTAssertEqual(model.status.untracked.map(\.path), ["docs/notes.md"])
        await model.stageAll()
        XCTAssertEqual(model.status.staged.map(\.path), ["docs/notes.md"])
        XCTAssertFalse(model.canCommit)
        model.commitMessage = "Add notes"
        XCTAssertTrue(model.canCommit)
        await model.commit()
        XCTAssertTrue(model.notice?.message.hasPrefix("Committed ") ?? false)
        XCTAssertEqual(model.log.count, 2)
        XCTAssertEqual(model.commitMessage, "")

        // A dirty worktree blocks switching branches.
        await model.createBranch("feature/x", checkout: false)
        XCTAssertEqual(model.branches.map(\.name), ["feature/x", "main"])
        let editor = LocalFileEditorModel(workspace: model.workspace, path: "README.md")
        await editor.load()
        editor.text = "changed\n"
        XCTAssertTrue(editor.canSave)
        await editor.save()
        XCTAssertEqual(editor.notice?.message, "Saved")
        XCTAssertTrue(editor.changed)
        await model.refreshAll()
        await model.checkout(model.branches.first { $0.name == "feature/x" }!)
        XCTAssertEqual(model.notice?.message, "Commit or discard your changes before switching branches.")
        XCTAssertEqual(model.tab, .changes)
        await model.discard("README.md")
        await model.checkout(model.branches.first { $0.name == "feature/x" }!)
        XCTAssertEqual(model.branchLabel, "feature/x")

        // Publishing needs a node session.
        let guest = LocalRepoScreenModel(record: record, store: store, publisher: nil, session: { nil })
        defer { guest.dispose() }
        XCTAssertEqual(guest.beginPublish(), .notANode("Sign in to publish a repository to ForkMesh."))
        XCTAssertEqual(model.head().commitCount, 2)
        XCTAssertEqual(model.head().rootCommit, model.log.last?.id)

        let deleted = await model.deleteRepository()
        XCTAssertTrue(deleted)
        let listed = try await store.list()
        XCTAssertTrue(listed.isEmpty)
    }

    func testScreenLabels() {
        XCTAssertEqual(LocalRepoText.humanSize(512), "512 B")
        XCTAssertEqual(LocalRepoText.humanSize(1536), "1.5 KB")
        XCTAssertEqual(LocalRepoText.fileMeta(LocalFileEntry(name: "a", path: "a", kind: .file, size: 10, tracked: false)), "10 B · untracked")
        XCTAssertEqual(LocalRepoText.fileMeta(LocalFileEntry(name: "d", path: "d", kind: .directory, size: 0, tracked: true)), "Folder")
        XCTAssertEqual(LocalRepoText.changeLabel(LocalChange(path: "a", kind: .modified, staged: false)), "Modified")
        XCTAssertEqual(LocalRepoText.author(nil), GitAuthor(name: "forkmesh", email: "forkmesh@mobile.forkmesh"))
    }
}

final class SubmittedPulls: @unchecked Sendable {
    var values: [[String]] = []
}

final class LocalCloneAttempts: @unchecked Sendable {
    var urls = [String]()
}
