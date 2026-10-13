import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMGit
import FMNet
import Foundation
import XCTest
@testable import FMCode

@MainActor
final class LocalRemoteFailoverTests: XCTestCase {
    private let relayBase = "https://forkmesh.test/forkmesh/demo"

    private func mirrorBase(_ node: String) -> String { "https://\(node).test/forkmesh/demo.git" }

    private func credentials(signed: Bool = true) -> ForkMeshRemoteCredentials {
        ForkMeshRemoteCredentials(
            origin: "https://forkmesh.test", owner: "forkmesh", repo: "demo",
            seed: signed ? (0..<32).map { UInt8($0 + 3) } : nil, pubkeyB64url: signed ? "device-key" : "")
    }

    private func makeWorkspace(_ server: LocalGitSmartServer) throws -> GitWorkspace {
        let directory = try localTempDirectory(self, "fm-remote").appendingPathComponent("demo")
        let workspace = GitWorkspace(worktree: directory)
        workspace.transportFactory = { server }
        addTeardownBlock { workspace.dispose() }
        return workspace
    }

    private func publisher(mirrors: @escaping @MainActor (String, String) async throws -> [RepoMirror]) throws -> PublishService {
        let api = ApiService(settings: StaticApiSettings(serverUrl: "wss://forkmesh.test/ws"), http: FakeHTTPClient(), sleep: { _ in })
        let identity = try Identity.fromSeed((0..<32).map { UInt8($0 + 11) })
        let publisher = PublishService(api: api, identity: identity, prefs: InMemoryKeyValueStore())
        publisher.mirrorLookup = mirrors
        return publisher
    }

    private static func mirror(_ node: String, latency: Int) -> RepoMirror {
        RepoMirror(json: [
            "node": .string(node), "owner": .string(node), "repo": "demo", "status": "online",
            "lastSync": 1_790_000_000_000, "cloneAvailable": true, "integrity": "ok",
            "mirrorUrl": .string("https://\(node).test/forkmesh/demo.git"),
            "endpointHealthy": true, "endpointFresh": true, "latencyMs": .int(Int64(latency)),
        ])
    }

    private func config(_ workspace: GitWorkspace) throws -> String {
        try localRead(workspace.worktree.appendingPathComponent(".git/config"))
    }

    func testFetchFollowsTheRelaysRedirectToAMirrorUnsignedAndKeepsTheRelayAsOrigin() async throws {
        let origin = try LocalGitOrigin.make(self, commits: 3)
        let server = LocalGitSmartServer(git: origin.git)
        server.serve(mirrorBase("mirror35"), origin.bare)
        server.redirect(relayBase, to: mirrorBase("mirror35"))
        let workspace = try makeWorkspace(server)
        _ = try await workspace.clone(credentials: credentials())
        XCTAssertTrue(try config(workspace).contains("url = \(relayBase)"))
        let tip = try origin.commit("next.txt", "next\n", "Next")
        try origin.publish("main")
        let before = server.requests.count

        let route = GitRemoteRoute()
        let result = try await workspace.fetch(credentials: credentials(), route: route)

        XCTAssertEqual(result.updatedRefs, ["refs/remotes/origin/main"])
        XCTAssertEqual(result.servedBy, "mirror35.test")
        XCTAssertEqual(route.mirrorUrl, mirrorBase("mirror35"))
        let fetchRequests = Array(server.requests.dropFirst(before))
        XCTAssertEqual(fetchRequests.first?.url.absoluteString, "\(relayBase)/info/refs?service=git-upload-pack")
        XCTAssertNotNil(fetchRequests.first?.headers["x-forkmesh-sig"])
        let mirrored = fetchRequests.dropFirst()
        XCTAssertFalse(mirrored.isEmpty)
        XCTAssertTrue(mirrored.allSatisfy { $0.url.host == "mirror35.test" })
        XCTAssertTrue(mirrored.allSatisfy { $0.headers["x-forkmesh-sig"] == nil && $0.headers["x-forkmesh-pubkey"] == nil })
        let sync = try await workspace.syncState(branch: "main")
        XCTAssertEqual(sync.remoteTip, tip)
        XCTAssertTrue(try config(workspace).contains("url = \(relayBase)"))
    }

    func testFetchFailsOverFromTheRedirectedMirrorToTheNextOne() async throws {
        let origin = try LocalGitOrigin.make(self, commits: 2)
        let server = LocalGitSmartServer(git: origin.git)
        server.serve(mirrorBase("mirror36"), origin.bare)
        server.redirect(relayBase, to: mirrorBase("mirror35"))
        server.takeDown("mirror35.test")
        let workspace = try makeWorkspace(server)
        _ = try await workspace.clone(credentials: credentials().at(endpoint: mirrorBase("mirror36")))
        let tip = try origin.commit("next.txt", "next\n", "Next")
        try origin.publish("main")
        let publisher = try publisher(mirrors: { owner, name in
            XCTAssertEqual("\(owner)/\(name)", "forkmesh/demo")
            return [Self.mirror("mirror35", latency: 10), Self.mirror("mirror36", latency: 50)]
        })

        let result = try await publisher.fetch(workspace: workspace, owner: "forkmesh", repo: "demo")

        XCTAssertEqual(result.servedBy, "mirror36.test")
        XCTAssertEqual(result.updatedRefs, ["refs/remotes/origin/main"])
        XCTAssertEqual(server.requested(host: "mirror35.test").count, 1)
        let sync = try await workspace.syncState(branch: "main")
        XCTAssertEqual(sync.remoteTip, tip)
        XCTAssertEqual(LocalRepoScreenModel.fetchMessage(result), "Fetched 1 branch from mirror36.test.")
    }

    func testWhenEverySourceIsDownTheFetchNamesEachOne() async throws {
        let origin = try LocalGitOrigin.make(self, commits: 1)
        let server = LocalGitSmartServer(git: origin.git)
        server.serve(mirrorBase("mirror37"), origin.bare)
        let workspace = try makeWorkspace(server)
        _ = try await workspace.clone(credentials: credentials().at(endpoint: mirrorBase("mirror37")))
        server.answer(relayBase, status: 503)
        server.takeDown("mirror35.test")
        server.takeDown("mirror36.test")
        let publisher = try publisher(mirrors: { _, _ in
            [Self.mirror("mirror35", latency: 10), Self.mirror("mirror36", latency: 50)]
        })

        await localAssertThrows(try await publisher.fetch(workspace: workspace, owner: "forkmesh", repo: "demo")) { error in
            guard let failure = error as? CloneFailure else { return XCTFail("expected CloneFailure, got \(error)") }
            let message = failure.message
            XCTAssertTrue(message.hasPrefix("Could not fetch forkmesh/demo from any of its 3 sources."), message)
            XCTAssertTrue(message.contains("• the relay (forkmesh.test): Nothing on this relay (forkmesh.test) is serving"), message)
            XCTAssertTrue(message.contains("• mirror35.test: Could not reach ForkMesh."), message)
            XCTAssertTrue(message.contains("• mirror36.test: Could not reach ForkMesh."), message)
        }
    }

    func testAMirrorListThatCannotBeReadIsReportedWithTheRelayFailure() async throws {
        let origin = try LocalGitOrigin.make(self, commits: 1)
        let server = LocalGitSmartServer(git: origin.git)
        server.serve(mirrorBase("mirror37"), origin.bare)
        let workspace = try makeWorkspace(server)
        _ = try await workspace.clone(credentials: credentials().at(endpoint: mirrorBase("mirror37")))
        server.redirect(relayBase, to: mirrorBase("mirror35"))
        server.takeDown("mirror35.test")
        let publisher = try publisher(mirrors: { _, _ in throw ApiException("HTTP 500") })

        await localAssertThrows(try await publisher.pull(workspace: workspace, owner: "forkmesh", repo: "demo", branch: "main")) { error in
            let message = (error as? CloneFailure)?.message ?? ""
            XCTAssertTrue(message.hasPrefix("Could not pull forkmesh/demo from any of its 1 sources."), message)
            XCTAssertTrue(message.contains("• mirror35.test: Could not reach ForkMesh."), message)
        }
    }

    func testRedirectTargetsMustBeCredentialFreeHttpsUploadPackAdvertisements() throws {
        let request = try GitHttpRequest(
            method: "GET", url: URL(string: "\(relayBase)/info/refs?service=git-upload-pack")!,
            maxResponseBytes: 1024, expectedContentType: nil)
        let good = GitRedirectTarget.resolve(
            request, location: "https://mirror35.test/forkmesh/demo.git/info/refs?service=git-upload-pack", relayBase: relayBase)
        XCTAssertEqual(good?.base, "https://mirror35.test/forkmesh/demo.git")
        let relative = GitRedirectTarget.resolve(
            request, location: "/mirror/forkmesh/demo/info/refs?service=git-upload-pack", relayBase: relayBase)
        XCTAssertEqual(relative?.base, "https://forkmesh.test/mirror/forkmesh/demo")
        for location in [
            "http://mirror35.test/forkmesh/demo.git/info/refs?service=git-upload-pack",
            "https://user:secret@mirror35.test/forkmesh/demo.git/info/refs?service=git-upload-pack",
            "https://mirror35.test/forkmesh/demo.git/info/refs?service=git-receive-pack",
            "https://mirror35.test/forkmesh/demo.git/git-upload-pack",
            "\(relayBase)/info/refs?service=git-upload-pack",
            "",
        ] {
            XCTAssertNil(GitRedirectTarget.resolve(request, location: location, relayBase: relayBase), location)
        }
        let push = try GitHttpRequest(
            method: "GET", url: URL(string: "\(relayBase)/info/refs?service=git-receive-pack")!,
            maxResponseBytes: 1024, expectedContentType: nil)
        XCTAssertNil(GitRedirectTarget.resolve(
            push, location: "https://mirror35.test/forkmesh/demo.git/info/refs?service=git-upload-pack", relayBase: relayBase))
    }

    func testASecondRedirectIsNeverFollowed() async throws {
        let origin = try LocalGitOrigin.make(self, commits: 1)
        let server = LocalGitSmartServer(git: origin.git)
        server.redirect(relayBase, to: mirrorBase("mirror35"))
        server.redirect(mirrorBase("mirror35"), to: mirrorBase("mirror36"))
        server.serve(mirrorBase("mirror36"), origin.bare)
        let workspace = try makeWorkspace(server)

        await localAssertThrows(try await workspace.clone(credentials: credentials())) { error in
            XCTAssertEqual((error as? GitWorkspaceException)?.code, .remoteRejected)
        }
        XCTAssertTrue(server.requested(host: "mirror36.test").isEmpty)
    }

    func testPullFastForwardsTheCheckedOutBranchAndItsFiles() async throws {
        let origin = try LocalGitOrigin.make(self, commits: 2)
        let server = LocalGitSmartServer(git: origin.git)
        server.serve(relayBase, origin.bare)
        let workspace = try makeWorkspace(server)
        _ = try await workspace.clone(credentials: credentials())
        let tip = try origin.commit("step.txt", "pulled\n", "Upstream change")
        try origin.publish("main")

        let result = try await workspace.pull(credentials: credentials(), branch: "main")

        guard case .fastForwarded(_, let to) = result.outcome else { return XCTFail("\(result.outcome)") }
        XCTAssertEqual(to, tip)
        XCTAssertEqual(result.message, "Synced main with ForkMesh.")
        XCTAssertEqual(try localRead(workspace.worktree.appendingPathComponent("step.txt")), "pulled\n")
        let status = try await workspace.status()
        XCTAssertTrue(status.isClean)
        let sync = try await workspace.syncState(branch: "main")
        XCTAssertTrue(sync.isSynced)
        let again = try await workspace.pull(credentials: credentials(), branch: "main")
        XCTAssertEqual(again.outcome, .upToDate)
    }

    func testPullRefusesLocalChangesAndReportsAheadAndDiverged() async throws {
        let origin = try LocalGitOrigin.make(self, commits: 2)
        let server = LocalGitSmartServer(git: origin.git)
        server.serve(relayBase, origin.bare)
        let workspace = try makeWorkspace(server)
        _ = try await workspace.clone(credentials: credentials())
        try await workspace.writeFile("local.txt", "mine\n")
        try await workspace.stageAll()
        _ = try await workspace.commit(message: "Local\n", author: localTestAuthor)

        let ahead = try await workspace.pull(credentials: credentials(), branch: "main")
        XCTAssertEqual(ahead.outcome, .ahead)

        try origin.commit("step.txt", "theirs\n", "Upstream change")
        try origin.publish("main")
        let diverged = try await workspace.pull(credentials: credentials(), branch: "main")
        XCTAssertEqual(diverged.outcome, .diverged)
        XCTAssertTrue(diverged.message.contains("changed both here and on ForkMesh"))

        let fresh = try makeWorkspace(server)
        _ = try await fresh.clone(credentials: credentials())
        try origin.commit("step.txt", "later\n", "Later")
        try origin.publish("main")
        try await fresh.writeFile("step.txt", "edited\n")
        await localAssertThrows(try await fresh.pull(credentials: credentials(), branch: "main")) { error in
            XCTAssertEqual((error as? GitWorkspaceException)?.message, "Commit or discard your changes before syncing main.")
        }
    }

    func testAShallowPushDeepensUntilTheFastForwardIsProven() async throws {
        let origin = try LocalGitOrigin.make(self, commits: 40)
        let deep = try origin.revParse("HEAD~30", in: origin.work)
        try origin.branch("old", at: deep)
        try origin.publish("old")
        let server = LocalGitSmartServer(git: origin.git)
        server.serve(relayBase, origin.bare)
        let workspace = try makeWorkspace(server)
        _ = try await workspace.clone(credentials: credentials(), depth: 5)
        try await workspace.createBranch("old", checkout: true)
        try await workspace.writeFile("phone.txt", "from the phone\n")
        try await workspace.stageAll()
        let local = try await workspace.commit(message: "Phone work\n", author: localTestAuthor)

        let result = try await workspace.push(credentials: credentials(), branch: "old")

        XCTAssertEqual(result.refs.map(\.outcome), ["updated"])
        XCTAssertEqual(try origin.tip("refs/heads/old"), local)
        XCTAssertTrue(server.fetchBodies.contains { $0.contains("deepen 50\n") && $0.contains("deepen-relative\n") })
    }

    func testAShallowPushOntoADivergedBranchSaysWhatToDoWithoutDeepening() async throws {
        let origin = try LocalGitOrigin.make(self, commits: 10)
        let server = LocalGitSmartServer(git: origin.git)
        server.serve(relayBase, origin.bare)
        let workspace = try makeWorkspace(server)
        _ = try await workspace.clone(credentials: credentials(), depth: 3)
        try origin.commit("step.txt", "theirs\n", "Upstream change")
        try origin.publish("main")
        try await workspace.writeFile("phone.txt", "mine\n")
        try await workspace.stageAll()
        _ = try await workspace.commit(message: "Phone work\n", author: localTestAuthor)
        let pushedBefore = try origin.tip("refs/heads/main")
        let cloneFetches = server.fetchBodies.count

        await localAssertThrows(try await workspace.push(credentials: credentials(), branch: "main")) { error in
            let failure = error as? GitWorkspaceException
            XCTAssertEqual(failure?.detail, "push_diverged")
            XCTAssertEqual(failure?.code, .nonFastForward)
            XCTAssertTrue(failure?.message.contains("Push your work to a new branch") ?? false)
        }
        XCTAssertEqual(try origin.tip("refs/heads/main"), pushedBefore)
        let proofFetches = server.fetchBodies.dropFirst(cloneFetches)
        XCTAssertFalse(proofFetches.isEmpty)
        XCTAssertFalse(proofFetches.contains { $0.contains("deepen") })
    }

    func testTheProofStopsAtItsBoundWithAClearError() async throws {
        let origin = try LocalGitOrigin.make(self, commits: 30)
        try origin.branch("old", at: try origin.revParse("HEAD~20", in: origin.work))
        try origin.publish("old")
        let server = LocalGitSmartServer(git: origin.git)
        server.serve(relayBase, origin.bare)
        let workspace = try makeWorkspace(server)
        _ = try await workspace.clone(credentials: credentials(), depth: 2)
        try await workspace.createBranch("old", checkout: true)
        let repo = try WorkspaceOps.open(workspace.path)
        let token = GitCancellationToken()
        let creds = credentials()
        var steps = [GitDeepen?]()

        await localAssertThrows(try await ShallowPushProof.prove(
            repo, branch: "old", remote: "origin", schedule: [.relative(1)],
            fetch: { refspec, deepen in
                steps.append(deepen)
                _ = try await WorkspaceOps.fetchInto(
                    repo, creds, "origin", refspec: refspec, deepen: deepen, route: nil,
                    inner: server, token: token, progress: nil)
            },
            token: token)
        ) { error in
            let failure = error as? GitWorkspaceException
            XCTAssertEqual(failure?.detail, "shallow_history_limit")
            XCTAssertTrue(failure?.message.contains("push from a full clone on a computer") ?? false)
        }
        XCTAssertEqual(steps, [nil, .relative(1)])

        await localAssertThrows(try await ShallowPushProof.prove(
            repo, branch: "old", remote: "origin",
            fetch: { _, _ in throw GitException(.resourceLimitExceeded, "pack exceeds the compressed byte limit") },
            token: token)
        ) { error in
            XCTAssertEqual((error as? GitWorkspaceException)?.detail, "shallow_history_limit")
        }
    }
}
