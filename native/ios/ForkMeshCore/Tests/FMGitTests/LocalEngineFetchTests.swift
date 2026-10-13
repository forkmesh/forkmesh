// Fetch orchestration (Kotlin: GitFetchOrchestrationTest.kt) over the in-memory fake transport.

import Foundation
import XCTest
@testable import FMGit

/// Progress samples, collected across threads.
final class LocalEngineProgressLog: @unchecked Sendable {
    private let lock = NSLock()
    private var samples = [GitProgress]()

    var listener: GitProgressListener {
        { [self] progress in
            lock.lock()
            samples.append(progress)
            lock.unlock()
        }
    }

    var phases: [GitProgressPhase] { all.map(\.phase) }

    var all: [GitProgress] {
        lock.lock()
        defer { lock.unlock() }
        return samples
    }
}

final class LocalEngineFetchTests: LocalEngineTestCase {
    private var serverRoot = ""
    private var serverRepository: GitRepository!
    private let remoteUrl = try! GitRemoteUrl.parse("https://forkmesh.test/ada/notes")

    override func setUpWithError() throws {
        try super.setUpWithError()
        serverRoot = try makeTemp("forkmesh-fake-remote-")
        serverRepository = try GitRepository.initialize(GitRepositoryInit(serverRoot))
    }

    /// The upload-pack side of a fake server advertising `records` and
    /// answering every fetch with `pack` (after `onFetch` inspects the body).
    private func uploadPackServer(
        _ records: [String],
        _ pack: [UInt8],
        onFetch: ((String) throws -> [UInt8]?)? = nil
    ) -> LocalEngineFakeTransport {
        LocalEngineFakeTransport { request in
            if request.method == "GET" { return leOkResponse(request, leUploadPackAdvertisementBody()) }
            let body = leLatin1(request.body)
            if body.contains("command=ls-refs") { return leOkResponse(request, leLsRefsBody(records)) }
            if let custom = try onFetch?(body) { return leOkResponse(request, custom) }
            return leOkResponse(request, lePackfileBody(pack))
        }
    }

    private func clone(
        _ destination: String,
        _ transport: LocalEngineFakeTransport,
        detector: GitSha1CollisionDetector? = LocalEngineClearDetector(),
        progress: GitProgressListener? = nil
    ) async throws -> GitCloneResult {
        try await GitRepository.clone(
            try GitCloneRequest(
                url: remoteUrl,
                worktree: destination,
                access: GitRemoteAccess(transport: transport),
                detector: detector
            ),
            progress: progress
        )
    }

    func testCloneFetchesThePackUpdatesRefsHeadAndTheWorktree() async throws {
        let tip = try leCommit(serverRepository, "hello.txt", "hello\n", "one\n")
        let pack = try serverRepository.writePack([tip])
        let transport = uploadPackServer(
            ["\(tip.hex) HEAD symref-target:refs/heads/main", "\(tip.hex) refs/heads/main"],
            pack.packBytes
        ) { body in
            guard body.contains("want \(tip.hex)"), body.contains("done") else {
                throw LocalEngineTestFailure(description: "unexpected fetch body \(body)")
            }
            return nil
        }
        let destination = "\(serverRoot)/clone-target"
        let progress = LocalEngineProgressLog()

        let result = try await clone(destination, transport, progress: progress.listener)

        XCTAssertEqual(result.checkedOutBranch, "main")
        XCTAssertEqual(result.fetch.updates.map(\.ref), ["refs/remotes/origin/main"])
        XCTAssertTrue(result.fetch.updates[0].isNew)
        XCTAssertEqual(result.fetch.objects, 3)
        XCTAssertEqual(result.fetch.remoteHead, "refs/heads/main")
        let cloned = result.repository
        XCTAssertEqual(try cloned.readHead(), GitSymbolicRef("refs/heads/main"))
        XCTAssertEqual(try cloned.readHeadObjectId(), tip)
        XCTAssertEqual(try leReadFile("\(destination)/hello.txt"), "hello\n")
        XCTAssertTrue(try cloned.status().isClean)
        let config = try cloned.readConfig()
        XCTAssertEqual(config.remotes["origin"]?.url.url.host, "forkmesh.test")
        XCTAssertEqual(config.branches["main"]?.remote, "origin")
        // GET info/refs, ls-refs, one fetch round.
        XCTAssertEqual(transport.requests.count, 3)
        XCTAssertEqual(transport.requests[0].headers["git-protocol"], "version=2")
        XCTAssertEqual(transport.requests[0].url.absoluteString,
                       "https://forkmesh.test/ada/notes/info/refs?service=git-upload-pack")
        XCTAssertEqual(transport.requests[1].url.absoluteString, "https://forkmesh.test/ada/notes/git-upload-pack")
        XCTAssertEqual(transport.requests[1].headers["content-type"], "application/x-git-upload-pack-request")
        XCTAssertFalse(transport.closed, "a caller-supplied transport is not closed")
        // A clone writes no FETCH_HEAD and leaves no journal behind.
        XCTAssertFalse(leExists("\(destination)/.git/FETCH_HEAD"))
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: "\(destination)/.git/forkmesh-journal"), [])
        let phases = progress.phases
        XCTAssertEqual(phases.first, .connecting)
        XCTAssertEqual(phases.last, .completed)
        for phase in [GitProgressPhase.listingRefs, .negotiating, .receivingPack, .indexingPack, .updatingRefs, .checkingOut] {
            XCTAssertTrue(phases.contains(phase), "missing \(phase)")
        }
    }

    func testFetchNegotiatesWithHavesAndInstallsOnlyTheDeltaOfHistory() async throws {
        let base = try leCommit(serverRepository, "a.txt", "a\n", "base\n")
        let basePack = try serverRepository.writePack([base])
        let seed = uploadPackServer(
            ["\(base.hex) HEAD symref-target:refs/heads/main", "\(base.hex) refs/heads/main"], basePack.packBytes
        )
        let local = try await clone("\(serverRoot)/local", seed)

        // The server advances by one commit.
        let tip = try leCommit(serverRepository, "b.txt", "b\n", "next\n")
        let baseObjects = Set(try GitPackReader.parse(basePack.packBytes, .standard).objects.map(\.objectId))
        let deltaPack = try serverRepository.writePack([tip], exclude: baseObjects)

        let sawAckRound = LocalEngineFlag()
        let transport = uploadPackServer(
            ["\(tip.hex) HEAD symref-target:refs/heads/main", "\(tip.hex) refs/heads/main"], deltaPack.packBytes
        ) { body in
            guard body.contains("want \(tip.hex)") else { throw LocalEngineTestFailure(description: "no want") }
            if body.contains("have \(base.hex)") && !body.contains("done") {
                // First negotiation round: acknowledge the common base and send
                // the pack in the same response, as a `ready` server does.
                sawAckRound.set()
                return lePackfileBody(deltaPack.packBytes, acknowledgments: ["ACK \(base.hex)", "ready"])
            }
            return nil
        }

        let result = try await local.repository.fetch(try GitFetchRequest(access: GitRemoteAccess(transport: transport)))

        XCTAssertTrue(sawAckRound.value)
        XCTAssertEqual(result.negotiationRounds, 1)
        XCTAssertEqual(result.objects, 3)
        XCTAssertEqual(result.updates.map(\.ref), ["refs/remotes/origin/main"])
        XCTAssertEqual(result.updates[0].newId, tip)
        XCTAssertEqual(result.updates[0].oldId, base)
        XCTAssertTrue(result.updates[0].isChanged)
        let repository = local.repository
        let tree = try repository.readTree(try repository.readCommit(tip).tree)
        let blob = try XCTUnwrap(tree.entries.first { $0.nameText == "b.txt" })
        XCTAssertEqual(leText(try repository.readBlob(blob.objectId)), "b\n")
        let fetchHead = try leReadFile("\(repository.gitDirectory)/FETCH_HEAD")
        XCTAssertTrue(fetchHead.contains(tip.hex))
        XCTAssertTrue(fetchHead.contains("branch 'main' of https://forkmesh.test"))
        XCTAssertEqual(fetchHead, "\(tip.hex)\t\tbranch 'main' of https://forkmesh.test/ada/notes\n")
        // Local branches and the worktree are untouched by a fetch.
        XCTAssertEqual(try repository.readHeadObjectId(), base)
        XCTAssertFalse(leExists("\(repository.worktree)/b.txt"))
    }

    func testFetchIsANoOpWhenTheRemoteHasNothingNew() async throws {
        let tip = try leCommit(serverRepository, "a.txt", "a\n", "base\n")
        let pack = try serverRepository.writePack([tip])
        let transport = uploadPackServer(
            ["\(tip.hex) HEAD symref-target:refs/heads/main", "\(tip.hex) refs/heads/main"], pack.packBytes
        )
        let local = try await clone("\(serverRoot)/local2", transport)
        let requestsAfterClone = transport.requests.count

        let result = try await local.repository.fetch(try GitFetchRequest(access: GitRemoteAccess(transport: transport)))

        XCTAssertEqual(result.packBytes, 0)
        XCTAssertEqual(result.objects, 0)
        XCTAssertEqual(result.negotiationRounds, 0)
        XCTAssertFalse(result.updates[0].isChanged)
        // Only GET info/refs and ls-refs; no fetch command was needed.
        XCTAssertEqual(transport.requests.count, requestsAfterClone + 2)
    }

    func testFetchWritesTagsOnlyWhenAbsent() async throws {
        let tip = try leCommit(serverRepository, "a.txt", "a\n", "base\n")
        let pack = try serverRepository.writePack([tip])
        let transport = uploadPackServer(
            ["\(tip.hex) HEAD symref-target:refs/heads/main", "\(tip.hex) refs/heads/main", "\(tip.hex) refs/tags/v1"],
            pack.packBytes
        )
        let local = try await clone("\(serverRoot)/tags", transport)
        XCTAssertEqual(local.fetch.updates.map(\.ref), ["refs/remotes/origin/main", "refs/tags/v1"])
        XCTAssertEqual(try local.repository.listTags().map(\.shortName), ["v1"])

        let again = try await local.repository.fetch(try GitFetchRequest(access: GitRemoteAccess(transport: transport)))
        XCTAssertEqual(again.updates.map(\.ref), ["refs/remotes/origin/main"])
    }

    func testAPackThatOmitsAWantedObjectIsRejectedAndNothingIsInstalled() async throws {
        let tip = try leCommit(serverRepository, "a.txt", "a\n", "base\n")
        let bogusPack = try GitPackWriter.write([try GitPackWriteObject(.blob, leUtf8("unrelated\n"))])
        let transport = uploadPackServer(["\(tip.hex) refs/heads/main"], bogusPack.packBytes)
        let destination = "\(serverRoot)/broken-clone"

        let error = await leAssertGitErrorAsync(.packCorrupt) { _ = try await self.clone(destination, transport) }

        XCTAssertEqual(error?.message, "remote pack does not contain a required object")
        // The failed clone removed everything it created.
        XCTAssertFalse(leExists(destination))
    }

    func testAFailedCloneIntoAnExistingEmptyDirectoryEmptiesItAgain() async throws {
        let tip = try leCommit(serverRepository, "a.txt", "a\n", "base\n")
        let bogusPack = try GitPackWriter.write([try GitPackWriteObject(.blob, leUtf8("unrelated\n"))])
        let transport = uploadPackServer(["\(tip.hex) refs/heads/main"], bogusPack.packBytes)
        let destination = "\(serverRoot)/existing"
        try FileManager.default.createDirectory(atPath: destination, withIntermediateDirectories: true)

        await leAssertGitErrorAsync(.packCorrupt) { _ = try await self.clone(destination, transport) }

        XCTAssertTrue(leExists(destination))
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: destination), [])
    }

    func testCloneRefusesANonEmptyDestination() async throws {
        let destination = "\(serverRoot)/busy"
        try FileManager.default.createDirectory(atPath: destination, withIntermediateDirectories: true)
        try leWriteFile("\(destination)/file", "x")
        let transport = LocalEngineFakeTransport { _ in throw LocalEngineTestFailure(description: "must not connect") }

        let error = await leAssertGitErrorAsync(.invalidRepository) { _ = try await self.clone(destination, transport) }

        XCTAssertEqual(error?.message, "clone destination is not empty")
        XCTAssertTrue(leExists("\(destination)/file"))
    }

    func testCloningAnEmptyRemoteLeavesAnUnbornMain() async throws {
        // Without the `unborn` ls-refs argument a server omits an unborn HEAD.
        let transport = uploadPackServer([], [])
        let result = try await clone("\(serverRoot)/empty", transport)
        XCTAssertNil(result.checkedOutBranch)
        XCTAssertNil(try result.repository.readHeadObjectId())
        XCTAssertEqual(transport.requests.count, 2)
    }

    func testShallowCloneDepthOutOfRangeIsRejectedWithATypedError() {
        let error = leAssertGitError(.unsupportedRepositoryFormat) {
            _ = try GitCloneRequest(url: remoteUrl, worktree: "unused", depth: 0)
        }
        XCTAssertEqual(error?.message, "shallow clone depth must be in 1..\(GitResourceLimits.hardMaxCommitWalk)")
        XCTAssertNoThrow(try GitCloneRequest(url: remoteUrl, worktree: "unused", depth: 1))
        leAssertGitError(.invalidRef) { _ = try GitCloneRequest(url: remoteUrl, worktree: "unused", remoteName: "bad name") }
        leAssertGitError(.invalidRef) { _ = try GitCloneRequest(url: remoteUrl, worktree: "unused", branch: "a..b") }
        leAssertGitError(.invalidRef) { _ = try GitFetchRequest(remote: "..") }
    }

    func testUntrustedPacksWithoutACollisionDetectorFailClosed() async throws {
        let tip = try leCommit(serverRepository, "a.txt", "a\n", "base\n")
        let pack = try serverRepository.writePack([tip])
        let transport = uploadPackServer(["\(tip.hex) refs/heads/main"], pack.packBytes)
        let destination = "\(serverRoot)/unscreened-clone"

        await leAssertGitErrorAsync(.hashScreeningUnavailable) {
            _ = try await self.clone(destination, transport, detector: nil)
        }
        XCTAssertFalse(leExists(destination))
    }

    func testFetchFromAnUnconfiguredRemoteIsATypedError() async throws {
        let error = await leAssertGitErrorAsync(.invalidRepository) {
            _ = try await self.serverRepository.fetch(try GitFetchRequest(remote: "upstream"))
        }
        XCTAssertEqual(error?.message, "remote upstream is not configured")
    }

    func testRemoteFailuresAndCancellationSurfaceAsTypedErrors() async throws {
        try serverRepository.setRemote("origin", remoteUrl)
        let rejecting = LocalEngineFakeTransport { request in
            GitHttpResponse(statusCode: 200, headers: ["content-type": request.expectedContentType ?? ""],
                            body: lePackets([leDataLine("version 2\n"), leDataLine("agent=x\n"), .flush()]))
        }
        let unsupported = await leAssertGitErrorAsync(.protocolUnsupported) {
            _ = try await self.serverRepository.fetch(try GitFetchRequest(access: GitRemoteAccess(transport: rejecting)))
        }
        XCTAssertEqual(unsupported?.message, "remote does not advertise protocol v2 ls-refs and fetch")

        let cancelled = GitCancellationToken()
        cancelled.cancel()
        let transport = uploadPackServer([], [])
        await leAssertGitErrorAsync(.cancelled) {
            _ = try await self.serverRepository.fetch(
                try GitFetchRequest(access: GitRemoteAccess(transport: transport), cancellation: cancelled)
            )
        }
        XCTAssertTrue(transport.requests.isEmpty)

        // A non-Git failure inside the operation is reported redacted.
        let crashing = LocalEngineFakeTransport { _ in throw LocalEngineTestFailure(description: "secret detail") }
        let crash = await leAssertGitErrorAsync(.operationInterrupted) {
            _ = try await self.serverRepository.fetch(try GitFetchRequest(access: GitRemoteAccess(transport: crashing)))
        }
        XCTAssertEqual(crash?.message, "unexpected failure: LocalEngineTestFailure")
        // No journal or quarantine is left behind by the failures.
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: "\(serverRepository.gitDirectory)/forkmesh-journal"), [])
    }

    func testProgressSinkCapsTheEventCount() {
        let log = LocalEngineProgressLog()
        let sink = GitProgressSink(log.listener)
        for _ in 0..<(GitProgressSink.maxEvents + 5) { sink.add(GitProgress(.negotiating)) }
        XCTAssertEqual(log.phases.count, GitProgressSink.maxEvents)
        XCTAssertEqual(GitProgressPhase.allCases.count, 11)
    }
    func testReceivingPackProgressIsReportedWhileTheBodyStreams() async throws {
        let tip = try leCommit(serverRepository, "a.txt", "a\n", "base\n")
        let pack = try serverRepository.writePack([tip])
        let step = 256 * 1024
        let transport = LocalEngineFakeTransport { request in
            if request.method == "GET" { return leOkResponse(request, leUploadPackAdvertisementBody()) }
            let body = leLatin1(request.body)
            if body.contains("command=ls-refs") {
                return leOkResponse(request, leLsRefsBody(["\(tip.hex) HEAD symref-target:refs/heads/main", "\(tip.hex) refs/heads/main"]))
            }
            for bytes in [step + 10, step + 20, 2 * step + 30] { request.onBodyBytes?(bytes) }
            return leOkResponse(request, lePackfileBody(pack.packBytes))
        }
        let progress = LocalEngineProgressLog()

        _ = try await clone("\(serverRoot)/progress", transport, progress: progress.listener)

        let received = progress.all.filter { $0.phase == .receivingPack }.map(\.bytes)
        XCTAssertEqual(Array(received.prefix(2)), [step + 10, 2 * step + 30])
        XCTAssertEqual(received.count, 3)
    }

    private func shallowPackBody(_ packBytes: [UInt8], shallow: [GitObjectId]) -> [UInt8] {
        let pack = lePackfileBody(packBytes)
        var lines = [leDataLine("shallow-info\n")]
        for id in shallow { lines.append(leDataLine("shallow \(id.hex)\n")) }
        lines.append(.delimiter())
        return lePackets(lines) + pack
    }

    private func shallowClone(
        _ destination: String,
        _ transport: LocalEngineFakeTransport,
        depth: Int = 1
    ) async throws -> GitCloneResult {
        try await GitRepository.clone(
            try GitCloneRequest(
                url: remoteUrl,
                worktree: destination,
                depth: depth,
                singleBranch: true,
                access: GitRemoteAccess(transport: transport),
                detector: LocalEngineClearDetector()
            )
        )
    }

    func testShallowSingleBranchCloneRecordsTheBoundaryAndWalksStopThere() async throws {
        let base = try leCommit(serverRepository, "a.txt", "a\n", "base\n")
        let tip = try leCommit(serverRepository, "b.txt", "b\n", "tip\n")
        let pack = try serverRepository.writePack([tip], exclude: [base])
        let fetchBodies = LocalEngineBodies()
        let transport = uploadPackServer(
            [
                "\(tip.hex) HEAD symref-target:refs/heads/main",
                "\(tip.hex) refs/heads/main",
                "\(base.hex) refs/heads/old",
                "\(base.hex) refs/tags/v1",
            ],
            pack.packBytes
        ) { body in
            guard body.contains("command=fetch") else { return nil }
            fetchBodies.append(body)
            return self.shallowPackBody(pack.packBytes, shallow: [tip])
        }
        let destination = "\(serverRoot)/shallow"

        let result = try await shallowClone(destination, transport)

        XCTAssertEqual(result.checkedOutBranch, "main")
        XCTAssertEqual(result.fetch.updates.map(\.ref), ["refs/remotes/origin/main"])
        let body = try XCTUnwrap(fetchBodies.all.first)
        XCTAssertTrue(body.contains("deepen 1\n"))
        XCTAssertTrue(body.contains("want \(tip.hex)"))
        XCTAssertFalse(body.contains("want \(base.hex)"))
        let repository = result.repository
        XCTAssertTrue(repository.isShallow)
        XCTAssertEqual(try repository.shallowCommits(), [tip])
        XCTAssertEqual(try leReadFile("\(destination)/.git/shallow"), "\(tip.hex)\n")
        XCTAssertEqual(try repository.log(GitLogRequest(limit: 10)).map(\.id), [tip])
        XCTAssertEqual(try repository.reachableCommits([tip], nil), [tip])
        XCTAssertFalse(try repository.isAncestor(base, tip, nil))
        XCTAssertEqual(try leReadFile("\(destination)/b.txt"), "b\n")
        XCTAssertTrue(try repository.status().isClean)
        XCTAssertTrue(try repository.listTags().isEmpty)
        XCTAssertEqual(try repository.readConfig().remotes["origin"]?.fetch.source, "refs/heads/main")
        XCTAssertEqual(try repository.readConfig().remotes["origin"]?.fetch.destination, "refs/remotes/origin/main")
    }

    func testAFetchInAShallowCloneTellsTheServerItsBoundary() async throws {
        let base = try leCommit(serverRepository, "a.txt", "a\n", "base\n")
        let tip = try leCommit(serverRepository, "b.txt", "b\n", "tip\n")
        let pack = try serverRepository.writePack([tip], exclude: [base])
        let seed = uploadPackServer(
            ["\(tip.hex) HEAD symref-target:refs/heads/main", "\(tip.hex) refs/heads/main"], pack.packBytes
        ) { body in
            body.contains("command=fetch") ? self.shallowPackBody(pack.packBytes, shallow: [tip]) : nil
        }
        let local = try await shallowClone("\(serverRoot)/shallow-fetch", seed)

        let next = try leCommit(serverRepository, "c.txt", "c\n", "next\n")
        let objects = Set(try GitPackReader.parse(pack.packBytes, .standard).objects.map(\.objectId))
        let delta = try serverRepository.writePack([next], exclude: objects.union([base]))
        let fetchBodies = LocalEngineBodies()
        let transport = uploadPackServer(
            [
                "\(next.hex) HEAD symref-target:refs/heads/main",
                "\(next.hex) refs/heads/main",
                "\(base.hex) refs/heads/old",
                "\(base.hex) refs/tags/v1",
            ],
            delta.packBytes
        ) { body in
            if body.contains("command=fetch") { fetchBodies.append(body) }
            return nil
        }

        let fetched = try await local.repository.fetch(try GitFetchRequest(access: GitRemoteAccess(transport: transport)))

        XCTAssertEqual(fetched.updates.first?.newId, next)
        let body = try XCTUnwrap(fetchBodies.all.last)
        XCTAssertTrue(body.contains("shallow \(tip.hex)\n"))
        XCTAssertFalse(body.contains("deepen"))
        XCTAssertFalse(body.contains("want \(base.hex)"))
        XCTAssertEqual(fetched.updates.map(\.ref), ["refs/remotes/origin/main"])
        XCTAssertEqual(try local.repository.shallowCommits(), [tip])
        XCTAssertEqual(
            try local.repository.log(GitLogRequest(ref: "refs/remotes/origin/main", limit: 10)).map(\.id), [next, tip])
    }

    func testAShallowCloneFromARemoteWithoutShallowSupportFailsCleanly() async throws {
        let tip = try leCommit(serverRepository, "a.txt", "a\n", "base\n")
        let transport = LocalEngineFakeTransport { request in
            if request.method == "GET" {
                return leOkResponse(request, leUploadPackAdvertisementBody(
                    capabilities: ["agent=git/test", "ls-refs=unborn", "fetch=wait-for-done", "object-format=sha1"]))
            }
            return leOkResponse(request, leLsRefsBody(["\(tip.hex) refs/heads/main"]))
        }
        let destination = "\(serverRoot)/no-shallow"

        let error = await leAssertGitErrorAsync(.protocolUnsupported) { _ = try await self.shallowClone(destination, transport) }

        XCTAssertEqual(error?.message, "remote does not support shallow fetch")
        XCTAssertFalse(leExists(destination))
    }
}

final class LocalEngineFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var flag = false

    func set() {
        lock.lock()
        flag = true
        lock.unlock()
    }

    var value: Bool {
        lock.lock()
        defer { lock.unlock() }
        return flag
    }
}

final class LocalEngineBodies: @unchecked Sendable {
    private let lock = NSLock()
    private var bodies = [String]()

    func append(_ body: String) {
        lock.lock()
        bodies.append(body)
        lock.unlock()
    }

    var all: [String] {
        lock.lock()
        defer { lock.unlock() }
        return bodies
    }
}
