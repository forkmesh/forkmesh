// Interoperability of the engine's repository layer with the real `git` (as in the Kotlin port's
// GitInteroperabilityTest.kt): git fsck accepts what the engine writes, and the engine reads,
// reports clean, and extends what git writes. Skipped when git is not installed.

import Foundation
import XCTest
@testable import FMGit

final class LocalEngineInteropTests: LocalEngineTestCase {
    private func git() throws -> LocalGitBinary {
        guard let git = LocalGitBinary.locate() else { throw XCTSkip("git is not installed") }
        return git
    }

    /// A repository created by git with one committed fixture.
    private func gitRepository(_ git: LocalGitBinary, _ prefix: String) throws -> String {
        let root = try makeTemp(prefix)
        try git.run(["init", "--initial-branch=main", "-q", root])
        return root
    }

    // system git reads what the engine writes

    func testSystemGitReadsAForkMeshCommitWithCorePathAndModeVariants() throws {
        let git = try git()
        let root = try makeTemp("forkmesh-g2-owned-")
        let repository = try GitRepository.initialize(GitRepositoryInit(root))
        let fixture = "\(root)/fixtures"
        try FileManager.default.createDirectory(atPath: fixture, withIntermediateDirectories: true)
        try leWriteFile("\(fixture)/plain.txt", "plain\n")
        try Data([0, 1, 2, 0xff]).write(to: URL(fileURLWithPath: "\(fixture)/binary.bin"))
        try leWriteFile("\(fixture)/run.sh", "#!/bin/sh\n")
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: "\(fixture)/run.sh")
        try FileManager.default.createSymbolicLink(atPath: "\(fixture)/link", withDestinationPath: "plain.txt")
        try leWriteFile("\(fixture)/café.txt", "accent\n")

        try repository.add([try GitWorktreePath("fixtures")])
        let commit = try repository.commit(
            try GitCommitRequest(message: "ForkMesh G2 fixture\n", author: leIdentity(), committer: leIdentity())
        )

        try git.run(["-C", root, "fsck", "--no-dangling"])
        let tree = try git.run(["-C", root, "-c", "core.quotePath=false", "ls-tree", "-r", "--full-tree", commit.hex])
        XCTAssertTrue(tree.contains("100755 blob"), tree)
        XCTAssertTrue(tree.contains("120000 blob"), tree)
        XCTAssertTrue(tree.contains("fixtures/café.txt"), tree)
        // git agrees the worktree is clean and on main.
        XCTAssertEqual(try git.run(["-C", root, "status", "--porcelain"]), "")
        XCTAssertEqual(try git.run(["-C", root, "rev-parse", "HEAD"]).trimmingCharacters(in: .whitespacesAndNewlines), commit.hex)
        XCTAssertEqual(try git.run(["-C", root, "log", "-1", "--format=%an <%ae> %s"]),
                       "Ada <ada@example.test> ForkMesh G2 fixture\n")
    }

    func testForkMeshReadsASystemGitIndexAndCleanCommittedWorktree() throws {
        let git = try git()
        let root = try gitRepository(git, "forkmesh-g2-oracle-")
        let nested = "\(root)/nested"
        try FileManager.default.createDirectory(atPath: nested, withIntermediateDirectories: true)
        try Data([0, 0xff, 1]).write(to: URL(fileURLWithPath: "\(nested)/binary.bin"))
        try leWriteFile("\(nested)/café.txt", "accent\n")
        try FileManager.default.createSymbolicLink(atPath: "\(nested)/link", withDestinationPath: "café.txt")
        try git.run(["-C", root, "add", "."])
        try git.run(["-C", root, "commit", "-q", "-m", "oracle fixture"])

        let repository = try GitRepository.open(GitRepositoryOpen(root))
        let status = try repository.status()

        XCTAssertTrue(status.isClean, "staged=\(status.staged.map(\.path.value)) worktree=\(status.worktree.map(\.path.value))")
        let head = try XCTUnwrap(try repository.readHeadObjectId())
        XCTAssertTrue(try repository.readCommit(head).parents.isEmpty)
        XCTAssertEqual(try repository.log(try GitLogRequest()).map(\.commit.messageText), ["oracle fixture\n"])
    }

    func testTheEngineExtendsAGitHistoryThatGitThenVerifies() throws {
        let git = try git()
        let root = try gitRepository(git, "forkmesh-g2-extend-")
        try leWriteFile("\(root)/a.txt", "a\n")
        try git.run(["-C", root, "add", "."])
        try git.run(["-C", root, "commit", "-q", "-m", "from git"])
        let gitHead = try git.run(["-C", root, "rev-parse", "HEAD"]).trimmingCharacters(in: .whitespacesAndNewlines)

        let repository = try GitRepository.open(GitRepositoryOpen(root))
        let tip = try leCommit(repository, "b.txt", "b\n", "from ForkMesh\n", seconds: 1_700_000_100)
        try repository.createBranch(try GitBranchName("topic"))

        try git.run(["-C", root, "fsck", "--no-dangling"])
        XCTAssertEqual(try git.run(["-C", root, "log", "--format=%s"]), "from ForkMesh\nfrom git\n")
        XCTAssertEqual(try git.run(["-C", root, "rev-parse", "HEAD~1"]).trimmingCharacters(in: .whitespacesAndNewlines), gitHead)
        XCTAssertEqual(try git.run(["-C", root, "rev-parse", "topic"]).trimmingCharacters(in: .whitespacesAndNewlines), tip.hex)
        XCTAssertEqual(try git.run(["-C", root, "status", "--porcelain"]), "")
    }

    func testGitCheckoutAndEngineCheckoutAgree() throws {
        let git = try git()
        let root = try makeTemp("forkmesh-g2-checkout-")
        let repository = try GitRepository.initialize(GitRepositoryInit(root))
        try leCommit(repository, "note.txt", "main\n", "main\n")
        try repository.createBranch(try GitBranchName("feature"))
        try repository.checkout(.branch(try GitBranchName("feature")))
        try leCommit(repository, "dir/extra.txt", "extra\n", "feature\n")

        try repository.checkout(.branch(try GitBranchName("main")))
        XCTAssertEqual(try git.run(["-C", root, "status", "--porcelain"]), "")
        XCTAssertEqual(try git.run(["-C", root, "symbolic-ref", "HEAD"]), "refs/heads/main\n")

        try git.run(["-C", root, "checkout", "-q", "feature"])
        let reopened = try GitRepository.open(GitRepositoryOpen(root))
        XCTAssertTrue(try reopened.status().isClean)
        XCTAssertEqual(try reopened.readHead(), GitSymbolicRef("refs/heads/feature"))
    }

    // packs system git writes

    func testOpensAPackedSystemGitRepositoryThroughTheRepositoryApi() throws {
        let git = try git()
        let root = try gitRepository(git, "forkmesh-g3-repository-")
        try leWriteFile("\(root)/packed.txt", "packed\n")
        try git.run(["-C", root, "add", "."])
        try git.run(["-C", root, "commit", "-q", "-m", "packed fixture"])
        try git.run(["-C", root, "branch", "side"])
        try git.run(["-C", root, "tag", "v1"])
        let blob = try git.run(["-C", root, "rev-parse", "HEAD:packed.txt"]).trimmingCharacters(in: .whitespacesAndNewlines)
        try git.run(["-C", root, "repack", "-adq"])
        // (No `git pack-refs`: RefStore.readDirect reads loose refs only, so
        // HEAD's branch must stay loose.)

        let repository = try GitRepository.open(GitRepositoryOpen(root))

        XCTAssertEqual(leText(try repository.readBlob(try GitObjectId.parseSha1(blob))), "packed\n")
        XCTAssertEqual(try repository.listBranches().map(\.shortName), ["main", "side"])
        XCTAssertEqual(try repository.listTags().map(\.shortName), ["v1"])
        XCTAssertTrue(try repository.status().isClean)
    }

    // packs the engine writes

    func testSystemGitVerifiesAndReadsAForkMeshGeneratedPack() throws {
        let git = try git()
        let source = try makeTemp("forkmesh-g4-source-")
        let repository = try GitRepository.initialize(GitRepositoryInit(source))
        try leCommit(repository, "a.txt", "owned\n", "one\n")
        let tip = try leCommit(repository, "dir/b.bin", "\u{0}\u{1}", "two\n")
        let written = try repository.writePack([tip])

        let root = try gitRepository(git, "forkmesh-g4-pack-")
        let base = "\(root)/.git/objects/pack/pack-forkmesh"
        try Data(written.packBytes).write(to: URL(fileURLWithPath: "\(base).pack"))
        try Data(written.indexBytes).write(to: URL(fileURLWithPath: "\(base).idx"))

        try git.run(["verify-pack", "-v", "\(base).idx"])
        XCTAssertEqual(try git.run(["-C", root, "log", "--format=%s", tip.hex]), "two\none\n")
        XCTAssertEqual(try git.run(["-C", root, "show", "\(tip.hex):a.txt"]), "owned\n")
    }
}
