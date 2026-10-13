import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMDiff
import FMGit
import FMNet
import Foundation
import XCTest
@testable import FMCode

private func session(nodeName: String = "alice", capabilities: [String] = [
    "browse", "comment", "submit_issue", "submit_pr", "publish_repo", "owner_sign",
]) -> AuthSession {
    AuthSession(
        nodeName: nodeName, email: "alice@example.com", status: "active", pubkey: "", emailVerified: true,
        isAdmin: false, avatarPng: "", avatarUpdatedAt: 0, createdAt: 1,
        savedAt: 1, sessionKind: "mobile_node", deviceKind: "mobile_node", capabilities: capabilities,
        sessionToken: "session-token")
}

/// A `GitWorkspace` whose network operations are scripted.
final class LocalFakeRemoteWorkspace: GitWorkspace, @unchecked Sendable {
    var pushes: [(owner: String, repo: String, branch: String)] = []
    var fetches: [(owner: String, repo: String)] = []
    var pushResult = LocalPushResult(
        refs: [LocalPushRef(remoteRef: "refs/heads/main", newId: String(repeating: "a", count: 40), outcome: "updated")],
        packBytes: 512, objects: 4)
    var pushError: Error?

    override func push(
        credentials: ForkMeshRemoteCredentials, branch: String, remote: String = "origin",
        onProgress: (@Sendable (GitTransferProgress) -> Void)? = nil
    ) async throws -> LocalPushResult {
        pushes.append((credentials.owner, credentials.repo, branch))
        if let pushError { throw pushError }
        return pushResult
    }

    override func fetch(
        credentials: ForkMeshRemoteCredentials, remote: String = "origin", route: GitRemoteRoute? = nil,
        onProgress: (@Sendable (GitTransferProgress) -> Void)? = nil
    ) async throws -> LocalFetchResult {
        fetches.append((credentials.owner, credentials.repo))
        return LocalFetchResult(updatedRefs: ["refs/remotes/origin/main"], packBytes: 128, objects: 2, remoteHead: "refs/heads/main")
    }
}

@MainActor
final class LocalPublishServiceTests: XCTestCase {
    struct Harness {
        let publisher: PublishService
        let identity: Identity
        let workspace: LocalFakeRemoteWorkspace
        let http: FakeHTTPClient
        var posted: [JSONObject] {
            http.requests.filter { $0.method == "POST" }.map { request in
                var body = JSON.parseObject(request.body ?? Data())
                body["path"] = .string(request.url.path)
                return body
            }
        }
    }

    private func harness(_ respond: (@Sendable (HTTPRequest) throws -> HTTPResponse)? = nil) throws -> Harness {
        let http = FakeHTTPClient()
        http.on(where: { $0.url.path == "/api/repositories" }) { request in
            try respond?(request) ?? HTTPResponse(status: 200, json: ["ok": true])
        }
        let api = ApiService(settings: StaticApiSettings(serverUrl: "wss://forkmesh.test/ws"), http: http, sleep: { _ in })
        let identity = try Identity.fromSeed((0..<32).map { UInt8(200 - $0) })
        let temp = try localTempDirectory(self, "fm-publish")
        return Harness(
            publisher: PublishService(
                api: api, identity: identity, prefs: InMemoryKeyValueStore(),
                clock: FixedClock(epochMillis: 1_750_000_000_000)),
            identity: identity,
            workspace: LocalFakeRemoteWorkspace(worktree: temp),
            http: http)
    }

    private func verifies(_ body: JSONObject, _ identity: Identity) throws -> Bool {
        var record = body
        for key in ["path", "catalogSig", "catalogSigVersion", "publishIntent", "signature"] { record.removeValue(forKey: key) }
        let canonical = CatalogCanonical.v2Canonical(try catalogValues(record))
        let signature = [UInt8](try B64.decodeUnpadded(body["catalogSig"]?.stringValue ?? ""))
        return Ed25519.verify(publicKey: identity.publicKeyBytes, message: Array(canonical.utf8), signature: signature)
    }

    // MARK: Session gate

    func testSessionGate() async throws {
        XCTAssertEqual(PublishService.describeSession(nil), "Sign in to publish a repository to ForkMesh.")
        let refusal = PublishService.describeSession(session(capabilities: ["browse", "comment"]))
        XCTAssertTrue(refusal?.contains("not a mobile node") ?? false)
        XCTAssertNotNil(PublishService.describeSession(session(capabilities: ["browse", "owner_sign"])))
        XCTAssertNotNil(PublishService.describeSession(session(capabilities: ["browse", "publish_repo"])))
        XCTAssertNil(PublishService.describeSession(session()))
    }

    func testPublishRefusesWithoutPushingWhenTheSessionCannotPublish() async throws {
        let h = try harness()
        let outcome = try await h.publisher.publish(
            session: session(capabilities: ["browse"]), workspace: h.workspace, repo: "demo", branch: "main", isPublic: true)
        XCTAssertEqual(outcome.status, .unauthorized)
        XCTAssertTrue(h.workspace.pushes.isEmpty)
        XCTAssertTrue(h.posted.isEmpty)
    }

    // MARK: Names

    func testNameValidation() {
        XCTAssertNil(PublishService.validatePublishName("demo-repo_1.x"))
        XCTAssertNotNil(PublishService.validatePublishName(""))
        XCTAssertNotNil(PublishService.validatePublishName("has space"))
        XCTAssertNotNil(PublishService.validatePublishName("a/b"))
        XCTAssertTrue(PublishService.validatePublishName(String(repeating: "a", count: 81))?.contains("at most 80") ?? false)
        XCTAssertNotNil(PublishService.validatePublishName(String(repeating: "a", count: 100)))
    }

    // MARK: Publish

    func testPushesTheBranchThenPostsAV2SignedCatalogRecord() async throws {
        let h = try harness()
        let outcome = try await h.publisher.publish(
            session: session(), workspace: h.workspace, repo: "demo", branch: "main", isPublic: true,
            description: "Notes",
            head: PublishHead(
                commit: String(repeating: "a", count: 40), subject: "Initial commit", authorName: "Alice",
                at: Date(epochMillis: 1_750_000_000_000), commitCount: 1, branchCount: 1))
        XCTAssertEqual(outcome.status, .published)
        XCTAssertEqual(outcome.publishedAs, "alice/demo")
        XCTAssertEqual(h.workspace.pushes.count, 1)
        XCTAssertEqual(h.workspace.pushes.first?.owner, "alice")
        XCTAssertEqual(h.workspace.pushes.first?.repo, "demo")
        XCTAssertEqual(h.workspace.pushes.first?.branch, "main")

        let body = try XCTUnwrap(h.posted.first)
        XCTAssertEqual(h.posted.count, 1)
        XCTAssertEqual(body["path"], "/api/repositories")
        XCTAssertEqual(body["catalogSigVersion"], .int(2))
        XCTAssertEqual(body["publishIntent"], "user")
        XCTAssertEqual(body["source"], "local-node")
        XCTAssertEqual(body["platform"], "mobile")
        XCTAssertEqual(body["visibility"], "public")
        XCTAssertEqual(body["maintainer"]?.stringValue, h.identity.publicKeyB64url)
        XCTAssertEqual(body["cloneUrl"], "https://forkmesh.test/alice/demo")
        XCTAssertTrue(PublishService.isNodeId(body["nodeId"]?.stringValue ?? ""))
        XCTAssertEqual(body["commitAt"], "1750000000")
        XCTAssertEqual(body["updatedAt"], "1750000000000")
        XCTAssertEqual(body["commitSubject"], "Initial commit")
        XCTAssertTrue(try verifies(body, h.identity))
    }

    func testTheNodeIdIsStableAcrossPublications() async throws {
        let h = try harness()
        for _ in 0..<2 {
            _ = await h.publisher.publishCatalogRecord(session: session(), repo: "demo", branch: "main", isPublic: true)
        }
        XCTAssertEqual(h.posted.count, 2)
        XCTAssertEqual(h.posted[0]["nodeId"], h.posted[1]["nodeId"])
    }

    func testARejectedRefStopsThePublishBeforeTheCatalogPost() async throws {
        let h = try harness()
        h.workspace.pushResult = LocalPushResult(
            refs: [LocalPushRef(remoteRef: "refs/heads/main", newId: "", outcome: "rejected", message: "non-fast-forward")],
            packBytes: 0, objects: 0)
        let outcome = try await h.publisher.publish(
            session: session(), workspace: h.workspace, repo: "demo", branch: "main", isPublic: true)
        XCTAssertEqual(outcome.status, .failed)
        XCTAssertTrue(outcome.message.contains("non-fast-forward"))
        XCTAssertTrue(h.posted.isEmpty)
    }

    // MARK: Worker answers

    private func answer(_ status: Int, _ json: JSONValue) async throws -> PublishOutcome {
        let h = try harness { _ in HTTPResponse(status: status, json: json) }
        return await h.publisher.publishCatalogRecord(session: session(), repo: "demo", branch: "main", isPublic: true)
    }

    func testWorkerAnswers() async throws {
        let mirrored = try await answer(202, ["mirror": true, "canonicalNodeId": "desktop-node-1"])
        XCTAssertEqual(mirrored.status, .mirrored)
        XCTAssertFalse(mirrored.isSuccess)
        XCTAssertEqual(mirrored.canonicalNodeId, "desktop-node-1")
        XCTAssertEqual(mirrored.publishedAs, "alice/demo")
        XCTAssertTrue(mirrored.message.contains("different name"))

        let stale = try await answer(409, ["error": "stale_update"])
        XCTAssertEqual(stale.status, .stale)
        XCTAssertTrue(stale.message.contains("newer record"))

        let accountRequired = try await answer(403, ["error": "account_required"])
        XCTAssertEqual(accountRequired.status, .unauthorized)
        XCTAssertTrue(accountRequired.message.contains("no key that may publish"))

        let mismatch = try await answer(403, ["error": "maintainer_mismatch"])
        XCTAssertEqual(mismatch.status, .unauthorized)
        XCTAssertTrue(mismatch.message.contains("alice"))

        let upgrade = try await answer(426, ["error": "client_upgrade_required"])
        XCTAssertEqual(upgrade.status, .upgradeRequired)
        XCTAssertTrue(upgrade.message.contains("owner-sealed"))

        let limited = try await answer(429, ["error": "rate_limited"])
        XCTAssertEqual(limited.status, .limited)

        let deleted = try await answer(410, ["error": "deleted"])
        XCTAssertEqual(deleted.status, .failed)
        XCTAssertTrue(deleted.message.contains("deleted on ForkMesh"))

        let boom = try await answer(500, ["error": "boom"])
        XCTAssertEqual(boom.status, .failed)
        XCTAssertTrue(boom.message.contains("boom"))
    }

    func testAnUnreachableWorkerFailsThePublishNotTheApp() async throws {
        let h = try harness { _ in throw HTTPError.transport("down") }
        let outcome = await h.publisher.publishCatalogRecord(session: session(), repo: "demo", branch: "main", isPublic: true)
        XCTAssertEqual(outcome.status, .failed)
        XCTAssertTrue(outcome.message.contains("Could not reach ForkMesh"))
    }

    // MARK: Push and fetch

    func testPushSignsForTheMeshRemoteAndNeverForces() async throws {
        let h = try harness()
        let result = try await h.publisher.push(workspace: h.workspace, owner: "alice", repo: "demo", branch: "main")
        XCTAssertEqual(result.refs.first?.branch, "main")
        XCTAssertEqual(h.workspace.pushes.first?.owner, "alice")
        XCTAssertTrue(h.posted.isEmpty)
    }

    func testCredentialsCarryTheDeviceSeedAndPubkeyByDefault() throws {
        let h = try harness()
        let signed = h.publisher.credentials(owner: "alice", repo: "demo")
        XCTAssertTrue(signed.canSign)
        XCTAssertEqual(signed.pubkeyB64url, h.identity.publicKeyB64url)
        XCTAssertEqual(signed.remoteUrl, "https://forkmesh.test/alice/demo")
        let anonymous = h.publisher.credentials(owner: "alice", repo: "demo", signed: false)
        XCTAssertFalse(anonymous.canSign)
        XCTAssertEqual(anonymous.pubkeyB64url, "")
    }

    func testFetchReportsTheUpdatedRefs() async throws {
        let h = try harness()
        let result = try await h.publisher.fetch(workspace: h.workspace, owner: "alice", repo: "demo")
        XCTAssertFalse(result.isUpToDate)
        XCTAssertEqual(result.updatedRefs, ["refs/remotes/origin/main"])
        XCTAssertEqual(h.workspace.fetches.first?.owner, "alice")
    }

    private func pushFailure(_ code: GitErrorCode, _ message: String, detail: String) -> GitWorkspaceException {
        GitWorkspaceException(describeRemoteFailure(GitException(code, message), push: true).message, detail: detail)
    }

    func testPushFailuresSurfaceAndNeverWriteACatalogRecord() async throws {
        let h = try harness()
        h.workspace.pushError = pushFailure(.remoteRejected, "remote returned HTTP 413", detail: "push_too_large")
        await localAssertThrows(try await h.publisher.publish(
            session: session(), workspace: h.workspace, repo: "demo", branch: "main", isPublic: true)
        ) { error in
            let failure = error as? GitWorkspaceException
            XCTAssertEqual(failure?.detail, "push_too_large")
            XCTAssertTrue(failure?.message.contains("8 MB") ?? false)
        }
        h.workspace.pushError = pushFailure(.remoteRejected, "remote returned HTTP 401", detail: "push_unauthorized")
        await localAssertThrows(try await h.publisher.publish(
            session: session(), workspace: h.workspace, repo: "demo", branch: "main", isPublic: true)
        ) { error in
            XCTAssertEqual((error as? GitWorkspaceException)?.detail, "push_unauthorized")
        }
        XCTAssertTrue(h.posted.isEmpty)

        h.workspace.pushError = pushFailure(.remoteRejected, "remote returned HTTP 503", detail: "push_unavailable")
        await localAssertThrows(try await h.publisher.push(workspace: h.workspace, owner: "alice", repo: "demo", branch: "main")) { error in
            XCTAssertEqual((error as? GitWorkspaceException)?.detail, "push_unavailable")
            XCTAssertTrue((error as? GitWorkspaceException)?.message.contains("try again") ?? false)
        }
        h.workspace.pushError = pushFailure(.nonFastForward, "", detail: "non_fast_forward")
        await localAssertThrows(try await h.publisher.push(workspace: h.workspace, owner: "alice", repo: "demo", branch: "main")) { error in
            XCTAssertEqual((error as? GitWorkspaceException)?.message, "Someone else pushed first — fetch and retry.")
        }
    }

    // MARK: forkedFrom

    private func normalize(_ value: String, owner: String = "kaif", name: String = "forkmesh") -> String? {
        normalizeForkedFrom(value, owner: owner, name: name)
    }

    func testForkedFromMatchesTheWorkerOnValuesItKeeps() {
        XCTAssertEqual(normalize("mainnode/forkmesh"), "mainnode/forkmesh")
        XCTAssertEqual(normalize("MainNode/ForkMesh"), "mainnode/forkmesh")
        XCTAssertEqual(normalize("  mainnode/forkmesh  "), "mainnode/forkmesh")
        XCTAssertEqual(normalize("main.node/fork-mesh_1"), "main.node/fork-mesh_1")
        XCTAssertEqual(normalize("mainnode/forkmesh", name: "FORKMESH"), "mainnode/forkmesh")
    }

    func testForkedFromMatchesTheWorkerOnValuesItDrops() {
        XCTAssertNil(normalize("kaif/forkmesh"))
        XCTAssertNil(normalize("KAIF/ForkMesh"))
        XCTAssertNil(normalize(""))
        XCTAssertNil(normalize("noslash"))
        XCTAssertNil(normalize("a/b/c"))
        XCTAssertNil(normalize("main node/forkmesh"))
        XCTAssertNil(normalize("mainnode/"))
        XCTAssertNil(normalize("/forkmesh"))
        XCTAssertNil(normalize("mainnode/fork mesh"))
        XCTAssertNil(normalize("\(String(repeating: "m", count: 81))/forkmesh"))
    }

    func testForkedFromIsAbsentNeverEmptyWhenNotAFork() {
        let plain = buildCatalogRecord(
            owner: "kaif", name: "demo", description: "", isPublic: true, branch: "main", maintainer: "k",
            nodeId: String(repeating: "0", count: 32), updatedAt: Date(epochMillis: 1_789_776_000_000))
        XCTAssertFalse(plain.contains("forkedFrom"))
        let fork = buildCatalogRecord(
            owner: "kaif", name: "demo", description: "", isPublic: true, branch: "main", maintainer: "k",
            nodeId: String(repeating: "0", count: 32), updatedAt: Date(epochMillis: 1_789_776_000_000),
            forkedFrom: "MainNode/Demo")
        XCTAssertEqual(fork["forkedFrom"], "mainnode/demo")
        XCTAssertEqual(fork["channel"], "#kaif-demo")
        XCTAssertEqual(fork["activityWeeks"]?.arrayValue?.count, 52)
    }

    func testPublishingAForkPostsTheEdgeAndStillVerifies() async throws {
        let h = try harness()
        let outcome = try await h.publisher.publish(
            session: session(), workspace: h.workspace, repo: "demo", branch: "main", isPublic: true,
            forkedFrom: "mainnode/demo")
        XCTAssertEqual(outcome.status, .published)
        let body = try XCTUnwrap(h.posted.first)
        XCTAssertEqual(body["forkedFrom"], "mainnode/demo")
        XCTAssertTrue(try verifies(body, h.identity), "the fork edge must be inside the signed record")
    }

    func testPublishingUnderTheParentAddressPostsNoEdge() async throws {
        let h = try harness()
        _ = try await h.publisher.publish(
            session: session(), workspace: h.workspace, repo: "demo", branch: "main", isPublic: true,
            forkedFrom: "alice/demo")
        XCTAssertFalse(h.posted.first?.contains("forkedFrom") ?? true)
    }
}
