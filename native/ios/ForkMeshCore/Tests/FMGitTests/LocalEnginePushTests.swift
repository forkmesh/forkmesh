// Push orchestration (Kotlin: GitPushOrchestrationTest.kt) over the in-memory fake transport.

import Foundation
import XCTest
@testable import FMGit

final class LocalEnginePushTests: LocalEngineTestCase {
    private var root = ""
    private var repository: GitRepository!

    override func setUpWithError() throws {
        try super.setUpWithError()
        root = try makeTemp("forkmesh-push-")
        repository = try GitRepository.initialize(GitRepositoryInit(root))
        try repository.setRemote("origin", try GitRemoteUrl.parse("https://forkmesh.test/ada/notes"))
    }

    /// A receive-pack server advertising `refs` that must never see a POST.
    private func advertiseOnly(_ refs: [(String, GitObjectId)]) -> LocalEngineFakeTransport {
        LocalEngineFakeTransport { request in
            if request.method == "GET" { return leOkResponse(request, leReceivePackAdvertisementBody(refs)) }
            throw LocalEngineTestFailure(description: "this push must not POST")
        }
    }

    private func push(_ transport: LocalEngineFakeTransport, refspecs: [GitPushRefspec] = []) async throws -> GitPushResult {
        try await repository.push(try GitPushRequest(refspecs: refspecs, access: GitRemoteAccess(transport: transport)))
    }

    func testPushesAFastForwardWithCompareAndSwapCommandsAndADeltaPack() async throws {
        let base = try leCommit(repository, "a.txt", "a\n", "base\n")
        let tip = try leCommit(repository, "b.txt", "b\n", "next\n")
        let posted = LocalEngineBox()
        let transport = LocalEngineFakeTransport { request in
            if request.method == "GET" {
                XCTAssertNil(request.headers["git-protocol"])
                XCTAssertEqual(request.url.query, "service=git-receive-pack")
                return leOkResponse(request, leReceivePackAdvertisementBody([("refs/heads/main", base)]))
            }
            XCTAssertEqual(request.headers["content-type"], "application/x-git-receive-pack-request")
            posted.bytes = request.body
            return leOkResponse(request, leSidebandReportBody(["unpack ok", "ok refs/heads/main"]))
        }
        let progress = LocalEngineProgressLog()

        let result = try await repository.push(
            try GitPushRequest(access: GitRemoteAccess(transport: transport)), progress: progress.listener
        )

        XCTAssertEqual(result.refs.count, 1)
        XCTAssertEqual(result.refs[0].outcome, .updated)
        XCTAssertEqual(result.refs[0].oldId, base)
        XCTAssertEqual(result.refs[0].newId, tip)
        XCTAssertEqual(result.refs[0].localRef, "refs/heads/main")
        XCTAssertEqual(result.refs[0].remoteRef, "refs/heads/main")
        // Command line: old new ref NUL capabilities, then flush, then the pack.
        let body = try XCTUnwrap(posted.bytes)
        let packets = try GitPktLine.decodeAll(Array(body[0..<(body.count - result.packBytes)]))
        let command = leLatin1(packets[0].payload)
        XCTAssertTrue(command.hasPrefix("\(base.hex) \(tip.hex) refs/heads/main"))
        XCTAssertTrue(command.contains("report-status-v2"))
        XCTAssertFalse(command.contains("delete-refs"))
        // The pack contains the new commit, tree, and blob, but not the base
        // history the remote already has.
        let pack = try GitPackReader.parse(Array(body[(body.count - result.packBytes)...]), .standard)
        XCTAssertEqual(result.objects, pack.objects.count)
        XCTAssertTrue(pack.objects.map(\.objectId).contains(tip))
        XCTAssertFalse(pack.objects.map(\.objectId).contains(base))
        XCTAssertEqual(pack.objects.filter { $0.type == .blob }.count, 1)
        // The remote-tracking ref now reflects the accepted push.
        let tracking = try RefStore(repository.gitDirectory).readDirect(try GitRefName.parse("refs/remotes/origin/main"))
        XCTAssertEqual(tracking, tip)
        let phases = progress.phases
        for phase in [GitProgressPhase.connecting, .preparingPack, .sendingPack, .awaitingReport, .completed] {
            XCTAssertTrue(phases.contains(phase), "missing \(phase)")
        }
    }

    func testANonFastForwardPushIsRefusedBeforeAnyBytesAreSent() async throws {
        try leCommit(repository, "a.txt", "a\n", "base\n")
        try repository.createBranch(try GitBranchName("divergent"))
        let localTip = try leCommit(repository, "b.txt", "b\n", "local\n")
        try repository.checkout(.branch(try GitBranchName("divergent")))
        let divergent = try leCommit(repository, "c.txt", "c\n", "remote\n")
        try repository.checkout(.branch(try GitBranchName("main")))
        let transport = advertiseOnly([("refs/heads/main", divergent)])

        let error = await leAssertGitErrorAsync(.nonFastForward) { _ = try await self.push(transport) }

        XCTAssertEqual(error?.message, "pushing refs/heads/main to refs/heads/main would not fast-forward")
        XCTAssertEqual(transport.requests.count, 1)
        XCTAssertEqual(try repository.readHeadObjectId(), localTip)
    }

    func testAnUnknownRemoteTipIsTreatedAsNonFastForward() async throws {
        try leCommit(repository, "a.txt", "a\n", "base\n")
        let unknown = try GitObjectId.parseSha1(String(repeating: "9", count: 40))
        let transport = advertiseOnly([("refs/heads/main", unknown)])

        let error = await leAssertGitErrorAsync(.nonFastForward) { _ = try await self.push(transport) }

        XCTAssertEqual(
            error?.message,
            "remote refs/heads/main points at a commit that is not known locally; fetch first"
        )
        XCTAssertEqual(transport.requests.count, 1)
    }

    func testAnUpToDatePushSendsNothing() async throws {
        let tip = try leCommit(repository, "a.txt", "a\n", "base\n")
        let transport = advertiseOnly([("refs/heads/main", tip)])

        let result = try await push(transport)

        XCTAssertEqual(result.refs.map(\.outcome), [.upToDate])
        XCTAssertEqual(result.packBytes, 0)
        XCTAssertEqual(result.objects, 0)
    }

    func testARemoteNgReportBecomesATypedRejection() async throws {
        try leCommit(repository, "a.txt", "a\n", "base\n")
        let transport = LocalEngineFakeTransport { request in
            if request.method == "GET" { return leOkResponse(request, leReceivePackAdvertisementBody([])) }
            return leOkResponse(request, leSidebandReportBody(["unpack ok", "ng refs/heads/main hook declined"]))
        }

        let error = await leAssertGitErrorAsync(.remoteRejected) { _ = try await self.push(transport) }

        XCTAssertEqual(error?.message, "remote rejected refs/heads/main: hook declined")
        // A rejected ref does not move the remote-tracking ref.
        XCTAssertNil(try RefStore(repository.gitDirectory).readDirect(try GitRefName.parse("refs/remotes/origin/main")))
    }

    func testAnUnpackFailureAndAnIncompleteReportAreTypedErrors() async throws {
        try leCommit(repository, "a.txt", "a\n", "base\n")
        let unpack = LocalEngineFakeTransport { request in
            if request.method == "GET" { return leOkResponse(request, leReceivePackAdvertisementBody([])) }
            return leOkResponse(request, leSidebandReportBody(["unpack index-pack abnormal exit", "ng refs/heads/main x"]))
        }
        let unpackError = await leAssertGitErrorAsync(.remoteRejected) { _ = try await self.push(unpack) }
        XCTAssertEqual(unpackError?.message, "remote failed to unpack the push: index-pack abnormal exit")

        let omitting = LocalEngineFakeTransport { request in
            if request.method == "GET" { return leOkResponse(request, leReceivePackAdvertisementBody([])) }
            return leOkResponse(request, leSidebandReportBody(["unpack ok", "ok refs/heads/other"]))
        }
        let omitted = await leAssertGitErrorAsync(.protocolUnsupported) { _ = try await self.push(omitting) }
        XCTAssertEqual(omitted?.message, "remote report omitted a pushed ref")
    }

    func testPushTargetsTheConfiguredUpstreamBranch() async throws {
        let tip = try leCommit(repository, "a.txt", "a\n", "base\n")
        try repository.setBranchUpstream(
            try GitBranchName("main"), remote: "origin", merge: try GitRefName.branch("refs/heads/trunk")
        )
        let transport = LocalEngineFakeTransport { request in
            if request.method == "GET" { return leOkResponse(request, leReceivePackAdvertisementBody([])) }
            return leOkResponse(request, leSidebandReportBody(["unpack ok", "ok refs/heads/trunk"]))
        }

        let result = try await push(transport)

        XCTAssertEqual(result.refs.map(\.remoteRef), ["refs/heads/trunk"])
        XCTAssertNil(result.refs[0].oldId)
        XCTAssertEqual(
            try RefStore(repository.gitDirectory).readDirect(try GitRefName.parse("refs/remotes/origin/trunk")), tip
        )
    }

    func testPushValidatesItsRefspecsAndRemote() async throws {
        try leCommit(repository, "a.txt", "a\n", "base\n")
        let twice = await leAssertGitErrorAsync(.invalidRef) {
            _ = try await self.push(self.advertiseOnly([]), refspecs: [
                try GitPushRefspec(localBranch: "main", remoteBranch: "x"),
                try GitPushRefspec(localBranch: "main", remoteBranch: "x"),
            ])
        }
        XCTAssertEqual(twice?.message, "push refspecs target the same remote branch twice")
        let missing = await leAssertGitErrorAsync(.invalidRef) {
            _ = try await self.push(self.advertiseOnly([]), refspecs: [try GitPushRefspec(localBranch: "nope")])
        }
        XCTAssertEqual(missing?.message, "local branch nope does not exist")
        let unconfigured = await leAssertGitErrorAsync(.invalidRepository) {
            _ = try await self.repository.push(try GitPushRequest(remote: "upstream"))
        }
        XCTAssertEqual(unconfigured?.message, "remote upstream is not configured")
        leAssertGitError(.resourceLimitExceeded) {
            _ = try GitPushRequest(refspecs: Array(repeating: try GitPushRefspec(localBranch: "main"), count: 1001))
        }
    }

    func testThePublicApiCannotExpressARefDeletion() throws {
        try leCommit(repository, "a.txt", "a\n", "base\n")
        // Refspec forms that would delete or force are rejected at parse time.
        leAssertGitError { _ = try GitPushRefspec.parse(":refs/heads/main") }
        leAssertGitError { _ = try GitPushRefspec(localBranch: "") }
        // And the command encoder refuses a zero next object ID outright.
        leAssertGitError {
            _ = try GitReceivePack.commandPackets([
                try GitPushRefUpdate(
                    refName: "refs/heads/main",
                    expectedOld: try GitObjectId.parseSha1(String(repeating: "1", count: 40)),
                    next: GitReceivePack.zeroId
                ),
            ])
        }
    }
}

final class LocalEngineBox: @unchecked Sendable {
    private let lock = NSLock()
    private var stored: [UInt8]?

    var bytes: [UInt8]? {
        get {
            lock.lock()
            defer { lock.unlock() }
            return stored
        }
        set {
            lock.lock()
            stored = newValue
            lock.unlock()
        }
    }
}
