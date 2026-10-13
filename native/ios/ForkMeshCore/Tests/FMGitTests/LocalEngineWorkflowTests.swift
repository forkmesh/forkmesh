// The local workflow, authoring, branch checkout, untracked status, repository and refs, repository
// packs and the repository parts of journal recovery and quarantine (Kotlin:
// GitLocalWorkflowTest.kt, GitRepositoryAndRefsTest.kt, GitRepositoryPackTest.kt,
// GitRecoveryTest.kt).

import Foundation
import XCTest
@testable import FMGit

final class LocalEngineWorkflowTests: LocalEngineTestCase {
    private var root = ""
    private var repository: GitRepository!

    override func setUpWithError() throws {
        try super.setUpWithError()
        root = try makeTemp("forkmesh-local-")
        repository = try GitRepository.initialize(GitRepositoryInit(root))
    }

    // MARK: log

    func testLogWalksNewestFirstWithTypedIdentitiesAndALimit() throws {
        let first = try leCommit(repository, "a.txt", "a\n", "one\n", name: "Ada Lovelace")
        let second = try leCommit(repository, "b.txt", "b\n", "two\n", seconds: 100, name: "Ada Lovelace")
        let third = try leCommit(repository, "c.txt", "c\n", "three\n", seconds: 200, name: "Ada Lovelace")

        let entries = try repository.log(try GitLogRequest())
        XCTAssertEqual(entries.map(\.id), [third, second, first])
        let identity = try XCTUnwrap(entries.first?.commit.author)
        XCTAssertEqual(identity.name, "Ada Lovelace")
        XCTAssertEqual(identity.email, "ada@example.test")
        XCTAssertEqual(identity.timestampSeconds, 200)
        XCTAssertEqual(identity.timezoneOffsetMinutes, 0)
        XCTAssertEqual(identity.timeUtc, Date(timeIntervalSince1970: 200))

        XCTAssertEqual(try repository.log(try GitLogRequest(limit: 2)).count, 2)
        let fromRef = try repository.log(try GitLogRequest(ref: "refs/heads/main", limit: 1))
        XCTAssertEqual(fromRef.map(\.id), [third])
    }

    func testLogAnUnbornHeadYieldsAnEmptyLog() throws {
        XCTAssertTrue(try repository.log(try GitLogRequest()).isEmpty)
    }

    func testLogAMissingRefIsATypedInvalidRefError() throws {
        try leCommit(repository, "a.txt", "a\n", "one\n")
        let error = leAssertGitError(.invalidRef) { _ = try repository.log(try GitLogRequest(ref: "refs/heads/nope")) }
        XCTAssertEqual(error?.message, "ref does not exist")
    }

    func testLogRequestBoundsItsLimit() {
        XCTAssertThrowsError(try GitLogRequest(limit: 0)) { XCTAssertTrue($0 is GitArgumentError) }
        XCTAssertThrowsError(try GitLogRequest(limit: GitLogRequest.maxLimit + 1)) {
            XCTAssertTrue($0 is GitArgumentError)
        }
        leAssertGitError(.invalidRef) { _ = try GitLogRequest(ref: "HEAD") }
    }

    // MARK: ref enumeration

    func testRefEnumerationListsLooseAndPackedBranchesWithLooseShadowingPacked() throws {
        let tip = try leCommit(repository, "a.txt", "a\n", "one\n")
        try repository.createBranch(try GitBranchName("feature/x"))
        // A packed ref not present loose, plus a stale packed copy of main
        // that the loose value must shadow.
        let stale = String(repeating: "0", count: 39)
        try leWriteFile(
            "\(repository.gitDirectory)/packed-refs",
            "# pack-refs with: peeled fully-peeled sorted \n"
                + "\(stale)1 refs/heads/main\n"
                + "\(tip.hex) refs/heads/packed-only\n"
                + "\(tip.hex) refs/tags/v1\n"
        )

        let branches = try repository.listBranches()
        XCTAssertEqual(branches.map(\.name.value), ["refs/heads/feature/x", "refs/heads/main", "refs/heads/packed-only"])
        XCTAssertEqual(branches.map(\.shortName), ["feature/x", "main", "packed-only"])
        XCTAssertEqual(branches.first { $0.shortName == "main" }?.objectId, tip, "loose value shadows the stale packed one")

        let tags = try repository.listTags()
        XCTAssertEqual(tags.map(\.shortName), ["v1"])
        XCTAssertEqual(tags.first?.objectId, tip)
    }

    // MARK: resetPaths

    func testResetPathsRestoresStagedEditsToHeadAndDropsNewlyStagedPaths() throws {
        try leCommit(repository, "a.txt", "old\n", "one\n")
        try leWriteFile("\(root)/a.txt", "new\n")
        try leWriteFile("\(root)/b.txt", "b\n")
        try repository.add([try GitWorktreePath("a.txt"), try GitWorktreePath("b.txt")])
        XCTAssertEqual(try repository.status().staged.count, 2)

        try repository.resetPaths([try GitWorktreePath("a.txt"), try GitWorktreePath("b.txt")])

        let status = try repository.status()
        XCTAssertTrue(status.staged.isEmpty)
        // The worktree still has the edits; they are just unstaged again.
        let paths = Set(status.worktree.map(\.path.value))
        XCTAssertTrue(paths.isSuperset(of: ["a.txt", "b.txt"]))
        XCTAssertEqual(try leReadFile("\(root)/a.txt"), "new\n")
    }

    // MARK: checkout

    func testCheckoutFastPathSwitchesToASameCommitBranchWithoutACleanWorktree() throws {
        try leCommit(repository, "a.txt", "a\n", "one\n")
        try repository.createBranch(try GitBranchName("topic"))
        try leWriteFile("\(root)/a.txt", "dirty\n")

        try repository.checkout(.branch(try GitBranchName("topic")))

        XCTAssertEqual(try repository.readHead(), GitSymbolicRef("refs/heads/topic"))
        XCTAssertEqual(try leReadFile("\(root)/a.txt"), "dirty\n", "no materialization happens for a same-commit switch")
    }

    func testCheckoutStillRefusesADirtySwitchBetweenDifferentCommits() throws {
        try leCommit(repository, "a.txt", "a\n", "one\n")
        try repository.createBranch(try GitBranchName("topic"))
        try repository.checkout(.branch(try GitBranchName("topic")))
        try leCommit(repository, "b.txt", "b\n", "two\n")
        try leWriteFile("\(root)/a.txt", "dirty\n")

        let error = leAssertGitError(.operationInterrupted) {
            try repository.checkout(.branch(try GitBranchName("main")))
        }
        XCTAssertEqual(error?.message, "cannot checkout with local changes")
    }

    func testCheckoutOfAMissingBranchIsATypedError() throws {
        try leCommit(repository, "a.txt", "a\n", "one\n")
        let error = leAssertGitError(.invalidRef) { try repository.checkout(.branch(try GitBranchName("nope"))) }
        XCTAssertEqual(error?.message, "branch does not exist")
    }

    func testCreatesABranchAndSafelyMaterializesItsCommittedFilesOnCheckout() throws {
        try leCommit(repository, "note.txt", "main\n", "main commit\n", name: "Ada Lovelace")
        try repository.createBranch(try GitBranchName("feature"))
        try repository.checkout(.branch(try GitBranchName("feature")))
        try leCommit(repository, "note.txt", "feature\n", "feature commit\n", name: "Ada Lovelace")

        try repository.checkout(.branch(try GitBranchName("main")))

        XCTAssertEqual(try leReadFile("\(root)/note.txt"), "main\n")
        XCTAssertEqual(try repository.readHead(), GitSymbolicRef("refs/heads/main"))
    }

    func testCheckoutRemovesFilesAbsentFromTheTargetCommit() throws {
        try leCommit(repository, "a.txt", "a\n", "one\n")
        try repository.createBranch(try GitBranchName("topic"))
        try repository.checkout(.branch(try GitBranchName("topic")))
        try leCommit(repository, "dir/b.txt", "b\n", "two\n")
        XCTAssertTrue(leExists("\(root)/dir/b.txt"))

        try repository.checkout(.branch(try GitBranchName("main")))

        XCTAssertFalse(leExists("\(root)/dir/b.txt"))
        XCTAssertTrue(try repository.status().isClean)
    }

    func testCheckoutRefusesATreeThatWritesIntoTheGitDirectory() throws {
        let parent = try leCommit(repository, "a.txt", "a\n", "one\n")
        let config = try leReadFile("\(root)/.git/config")
        let head = try leReadFile("\(root)/.git/HEAD")
        let store = LooseObjectStore("\(root)/.git", .standard)
        let planted = try store.write(.blob, utf8("[core]\n\tbare = true\n"))
        let readme = try store.write(.blob, utf8("a\n"))
        for (index, name) in [".git", ".GIT", ".Git"].enumerated() {
            let inner = try store.write(
                .tree, try GitTree([try .regular(utf8("config"), planted), try .regular(utf8("HEAD"), planted)]).encode())
            let top = try store.write(
                .tree, try GitTree([try .directory(utf8(name), inner), try .regular(utf8("a.txt"), readme)]).encode())
            let commit = try store.write(
                .commit,
                utf8(
                    "tree \(top.hex)\nparent \(parent.hex)\n"
                        + "author Ada <ada@example.test> 0 +0000\ncommitter Ada <ada@example.test> 0 +0000\n\nPlant\n"))
            try leWriteFile("\(root)/.git/refs/heads/evil\(index)", "\(commit.hex)\n")

            leAssertGitError(.pathEscapesWorktree) { try repository.checkout(.branch(try GitBranchName("evil\(index)"))) }

            XCTAssertEqual(try leReadFile("\(root)/.git/config"), config, name)
            XCTAssertEqual(try leReadFile("\(root)/.git/HEAD"), head, name)
        }
    }

    func testBranchCreationNeedsACommitAndAFreshName() throws {
        let unborn = leAssertGitError(.invalidRef) { try repository.createBranch(try GitBranchName("topic")) }
        XCTAssertEqual(unborn?.message, "cannot create a branch from an unborn HEAD")
        try leCommit(repository, "a.txt", "a\n", "one\n")
        try repository.createBranch(try GitBranchName("topic"))
        let duplicate = leAssertGitError(.refLocked) { try repository.createBranch(try GitBranchName("topic")) }
        XCTAssertEqual(duplicate?.message, "branch already exists")
        leAssertGitError(.invalidRef) { _ = try GitBranchName("/x") }
        leAssertGitError(.invalidRef) { _ = try GitBranchName("x/") }
        leAssertGitError(.invalidRef) { _ = try GitBranchName("a..b") }
        leAssertGitError(.invalidRef) { _ = try GitBranchName("") }
    }

    // MARK: UTF-8 identity

    func testUtf8IdentityRoundTripsANonAsciiAuthorThroughCommitAndLog() throws {
        try leWriteFile("\(root)/a.txt", "a\n")
        try repository.add([try GitWorktreePath("a.txt")])
        let identity = try GitIdentity(
            name: "Ada Ærlig Ødegård", email: "ada@ærlig.example", timestampSeconds: 42, timezoneOffsetMinutes: 330
        )
        let id = try repository.commit(try GitCommitRequest(message: "unicode\n", author: identity, committer: identity))

        let author = try XCTUnwrap(try repository.readCommit(id).author)
        XCTAssertEqual(author.name, "Ada Ærlig Ødegård")
        XCTAssertEqual(author.email, "ada@ærlig.example")
        XCTAssertEqual(author.timestampSeconds, 42)
        XCTAssertEqual(author.timezoneOffsetMinutes, 330)
    }

    func testUtf8IdentityRejectsAngleBracketsControlCharactersAndEmptyFields() {
        func make(_ name: String, _ email: String) throws -> GitIdentity {
            try GitIdentity(name: name, email: email, timestampSeconds: 0, timezoneOffsetMinutes: 0)
        }
        leAssertGitError(.invalidObject) { _ = try make("Ada <x>", "a@b") }
        leAssertGitError(.invalidObject) { _ = try make("Ada", "a@b>") }
        leAssertGitError(.invalidObject) { _ = try make("Ada\nEve", "a@b") }
        leAssertGitError(.invalidObject) { _ = try make("Ada\u{7}", "a@b") }
        leAssertGitError(.invalidObject) { _ = try make("", "a@b") }
        XCTAssertNoThrow(try make("Ünïcode is fine", "a@b"))
        leAssertGitError(.invalidObject) {
            _ = try GitIdentity(name: "Ada", email: "a@b", timestampSeconds: 0, timezoneOffsetMinutes: 15 * 60)
        }
        leAssertGitError(.invalidObject) {
            _ = try GitCommitRequest(message: "a\u{0}b", author: leIdentity(), committer: leIdentity())
        }
    }

    func testIdentityGitTextFormatsTheOffset() throws {
        XCTAssertEqual(
            try GitIdentity(name: "Ada", email: "a@b", timestampSeconds: 7, timezoneOffsetMinutes: -330).gitText,
            "Ada <a@b> 7 -0530"
        )
        XCTAssertEqual(leIdentity(1).gitText, "Ada <ada@example.test> 1 +0000")
    }

    // MARK: stale locks

    func testStaleLocksRemovesOnlyLocksOlderThanTheThreshold() throws {
        try leCommit(repository, "a.txt", "a\n", "one\n")
        let gitPath = repository.gitDirectory
        let staleIndex = "\(gitPath)/index.lock"
        let staleRef = "\(gitPath)/refs/heads/main.lock"
        let freshHead = "\(gitPath)/HEAD.lock"
        for file in [staleIndex, staleRef, freshHead] { try leWriteFile(file, "") }
        let old = Date().addingTimeInterval(-3600)
        try FileManager.default.setAttributes([.modificationDate: old], ofItemAtPath: staleIndex)
        try FileManager.default.setAttributes([.modificationDate: old], ofItemAtPath: staleRef)

        let removed = try repository.recoverStaleLocks()

        XCTAssertEqual(removed, 2)
        XCTAssertFalse(leExists(staleIndex))
        XCTAssertFalse(leExists(staleRef))
        XCTAssertTrue(leExists(freshHead), "fresh locks survive")
        XCTAssertThrowsError(try repository.recoverStaleLocks(olderThan: 30)) { XCTAssertTrue($0 is GitArgumentError) }
    }

    func testStaleLocksOpenClearsStaleLocksAutomatically() throws {
        try leCommit(repository, "a.txt", "a\n", "one\n")
        let stale = "\(repository.gitDirectory)/index.lock"
        try leWriteFile(stale, "")
        try FileManager.default.setAttributes([.modificationDate: Date().addingTimeInterval(-3600)], ofItemAtPath: stale)

        let reopened = try GitRepository.open(GitRepositoryOpen(root))

        XCTAssertFalse(leExists(stale))
        // The lock no longer blocks index writes.
        try leWriteFile("\(root)/b.txt", "b\n")
        try reopened.add([try GitWorktreePath("b.txt")])
    }

    func testAFreshIndexLockSurfacesAsRefLocked() throws {
        try leCommit(repository, "a.txt", "a\n", "one\n")
        try leWriteFile("\(repository.gitDirectory)/index.lock", "")
        try leWriteFile("\(root)/b.txt", "b\n")
        leAssertGitError(.refLocked) { try repository.add([try GitWorktreePath("b.txt")]) }
    }

    // MARK: cancellation

    func testCancellationAddStatusCommitAndCheckoutHonorACancelledToken() throws {
        let tip = try leCommit(repository, "a.txt", "a\n", "one\n")
        try repository.createBranch(try GitBranchName("other"))
        // Move `main` somewhere else so checkout takes the slow path.
        try leCommit(repository, "b.txt", "b\n", "two\n", seconds: 1)
        let cancelled = GitCancellationToken()
        cancelled.cancel()

        leAssertGitError(.cancelled) { try repository.add([try GitWorktreePath("a.txt")], cancellation: cancelled) }
        leAssertGitError(.cancelled) { _ = try repository.status(cancellation: cancelled) }
        leAssertGitError(.cancelled) {
            _ = try repository.commit(
                try GitCommitRequest(message: "nope\n", author: leIdentity(2), committer: leIdentity(2)),
                cancellation: cancelled
            )
        }
        leAssertGitError(.cancelled) {
            try repository.checkout(.branch(try GitBranchName("other")), cancellation: cancelled)
        }
        leAssertGitError(.cancelled) { _ = try repository.log(try GitLogRequest(), cancellation: cancelled) }
        leAssertGitError(.cancelled) { try repository.resetPaths([try GitWorktreePath("a.txt")], cancellation: cancelled) }
        // The repository is still consistent afterwards.
        XCTAssertNotEqual(try repository.readHeadObjectId(), tip)
        XCTAssertTrue(try repository.status().isClean)
        // No journal is left behind by the cancelled operations.
        let journal = "\(repository.gitDirectory)/forkmesh-journal"
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: journal), [])
    }

    // MARK: authoring

    func testStagesANestedFileWritesACommitAndReturnsToCleanStatus() throws {
        try FileManager.default.createDirectory(atPath: "\(root)/docs", withIntermediateDirectories: true)
        try leWriteFile("\(root)/docs/readme.txt", "hello\n")

        try repository.add([try GitWorktreePath("docs/readme.txt")])
        let staged = try repository.status()

        XCTAssertEqual(staged.staged.map(\.path.value), ["docs/readme.txt"])
        XCTAssertEqual(staged.staged.first?.kind, .added)
        XCTAssertTrue(staged.worktree.isEmpty)

        let identity = try GitIdentity(
            name: "Ada Lovelace", email: "ada@example.test", timestampSeconds: 0, timezoneOffsetMinutes: 0
        )
        let commitId = try repository.commit(
            try GitCommitRequest(message: "Add a nested README\n", author: identity, committer: identity)
        )

        let commit = try repository.readCommit(commitId)
        let tree = try repository.readTree(commit.tree)
        XCTAssertEqual(tree.entries.count, 1)
        XCTAssertEqual(tree.entries.first?.mode, .directory)
        let nested = try repository.readTree(tree.entries[0].objectId)
        XCTAssertEqual(leText(try repository.readBlob(nested.entries[0].objectId)), "hello\n")
        XCTAssertEqual(try repository.readHeadObjectId(), commitId)
        XCTAssertTrue(try repository.status().isClean)
    }

    func testAddingADirectoryStagesItsNestedFilesAndExecutableBits() throws {
        try FileManager.default.createDirectory(atPath: "\(root)/tools/deep", withIntermediateDirectories: true)
        try leWriteFile("\(root)/tools/run.sh", "#!/bin/sh\n")
        try leWriteFile("\(root)/tools/deep/data.txt", "data\n")
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: "\(root)/tools/run.sh")
        try FileManager.default.createSymbolicLink(atPath: "\(root)/tools/link", withDestinationPath: "run.sh")

        try repository.add([try GitWorktreePath("tools")])

        let status = try repository.status()
        XCTAssertEqual(status.staged.map(\.path.value), ["tools/deep/data.txt", "tools/link", "tools/run.sh"])
        XCTAssertTrue(status.worktree.isEmpty)
        _ = try repository.commit(try GitCommitRequest(message: "tools\n", author: leIdentity(), committer: leIdentity()))
        let commit = try repository.readCommit(try XCTUnwrap(try repository.readHeadObjectId()))
        let tools = try repository.readTree(try repository.readTree(commit.tree).entries[0].objectId)
        let modes = Dictionary(uniqueKeysWithValues: tools.entries.map { ($0.nameText, $0.mode) })
        XCTAssertEqual(modes["run.sh"], .executable)
        XCTAssertEqual(modes["link"], .symlink)
        XCTAssertEqual(modes["deep"], .directory)
    }

    func testStatusReportsModifiedDeletedAndStagedChanges() throws {
        try leCommit(repository, "a.txt", "a\n", "one\n")
        try leCommit(repository, "b.txt", "b\n", "two\n")
        try leWriteFile("\(root)/a.txt", "changed\n")
        try FileManager.default.removeItem(atPath: "\(root)/b.txt")

        var status = try repository.status()
        XCTAssertTrue(status.staged.isEmpty)
        XCTAssertEqual(status.worktree.map(\.path.value), ["a.txt", "b.txt"])
        XCTAssertEqual(status.worktree.map(\.kind), [.modified, .deleted])

        try repository.remove([try GitWorktreePath("b.txt")])
        status = try repository.status()
        XCTAssertEqual(status.staged.map(\.path.value), ["b.txt"])
        XCTAssertEqual(status.staged.map(\.kind), [.deleted])
        XCTAssertEqual(status.worktree.map(\.kind), [.modified])
    }

    func testStagingRepositoryMetadataIsRefused() throws {
        let error = leAssertGitError(.pathEscapesWorktree) { try repository.add([try GitWorktreePath(".git/config")]) }
        XCTAssertEqual(error?.message, "cannot stage repository metadata")
        leAssertGitError(.invalidRepository) { try repository.add([try GitWorktreePath("missing.txt")]) }
    }

    // MARK: status

    func testReportsAnUntrackedFileWithoutTreatingGitMetadataAsWorktreeContent() throws {
        try leWriteFile("\(root)/draft.txt", "draft\n")

        let status = try repository.status()

        XCTAssertTrue(status.staged.isEmpty)
        XCTAssertEqual(status.worktree.count, 1)
        XCTAssertEqual(status.worktree.first?.path.value, "draft.txt")
        XCTAssertEqual(status.worktree.first?.kind, .untracked)
    }

    // MARK: repository and refs

    func testInitializesAndReopensASha1RepositoryWithSymbolicMainHead() throws {
        let id = try repository.writeBlob(leUtf8("hello\n"))
        let reopened = try GitRepository.open(GitRepositoryOpen(root))

        XCTAssertEqual(try reopened.readHead(), GitSymbolicRef("refs/heads/main"))
        XCTAssertEqual(leText(try reopened.readBlob(id)), "hello\n")
        XCTAssertEqual(try reopened.readHeadObjectId(), nil)
        XCTAssertEqual(reopened.worktree, root)
        XCTAssertEqual(reopened.gitDirectory, "\(root)/.git")
    }

    func testInitAndOpenRefuseTheWrongDirectories() throws {
        let again = leAssertGitError(.invalidRepository) { _ = try GitRepository.initialize(GitRepositoryInit(root)) }
        XCTAssertEqual(again?.message, "repository already exists")
        let empty = try makeTemp("forkmesh-empty-")
        let missing = leAssertGitError(.invalidRepository) { _ = try GitRepository.open(GitRepositoryOpen(empty)) }
        XCTAssertEqual(missing?.message, "missing .git directory")
        try leWriteFile("\(root)/.git/HEAD", "garbage\n")
        let head = leAssertGitError(.invalidRepository) { _ = try GitRepository.open(GitRepositoryOpen(root)) }
        XCTAssertEqual(head?.message, "HEAD is invalid")
    }

    func testReadersRejectTheWrongObjectType() throws {
        let blob = try repository.writeBlob(leUtf8("x"))
        leAssertGitError(.invalidObject) { _ = try repository.readTree(blob) }
        leAssertGitError(.invalidObject) { _ = try repository.readCommit(blob) }
        let commit = try leCommit(repository, "a.txt", "a\n", "one\n")
        leAssertGitError(.invalidObject) { _ = try repository.readBlob(commit) }
        leAssertGitError(.invalidObject) { _ = try repository.readBlob(leFixtureId(7)) }
    }

    func testStaleRefUpdateLeavesOriginalObjectIdUnchanged() throws {
        let refs = RefStore(try makeTemp("forkmesh-ref-"))
        let ref = try GitRefName.branch("refs/heads/main")
        _ = try refs.update(ref, expectedOld: nil, next: leFixtureId(3))

        let result = try refs.update(ref, expectedOld: leFixtureId(1), next: leFixtureId(2))

        XCTAssertEqual(result, .nonFastForward)
        XCTAssertEqual(try refs.readDirect(ref), leFixtureId(3))
    }

    // MARK: repository packs

    func testWalksACommitAndItsNestedObjectsIntoANonDeltaPack() throws {
        let commit = try leCommit(repository, "hello.txt", "hello\n", "fixture\n")

        let pack = try repository.writePack([commit])
        let objects = try GitPackReader.parse(pack.packBytes, GitResourceLimits.standard).objects

        XCTAssertTrue(objects.map(\.objectId).contains(commit))
        XCTAssertEqual(Set(objects.map(\.type.rawValue)), ["commit", "tree", "blob"])
        let blob = try XCTUnwrap(objects.first { $0.type == .blob })
        XCTAssertEqual(leText(try repository.readBlob(blob.objectId)), "hello\n")
    }

    func testPackWalkExcludesWhatTheRemoteAlreadyHas() throws {
        let base = try leCommit(repository, "a.txt", "a\n", "one\n")
        let baseObjects = Set(try GitPackReader.parse(try repository.writePack([base]).packBytes, .standard)
            .objects.map(\.objectId))
        let tip = try leCommit(repository, "b.txt", "b\n", "two\n")

        let delta = try GitPackReader.parse(try repository.writePack([tip], exclude: baseObjects).packBytes, .standard)

        XCTAssertEqual(delta.objects.count, 3, "commit, root tree and the new blob")
        XCTAssertTrue(delta.objects.allSatisfy { !baseObjects.contains($0.objectId) })
        let cancelled = GitCancellationToken()
        cancelled.cancel()
        leAssertGitError(.cancelled) { _ = try repository.writePack([tip], cancellation: cancelled) }
    }

    // MARK: recovery (journal, quarantine)

    func testRefusesToOpenWhenARecoveryJournalIsMalformed() throws {
        let journal = "\(root)/.git/forkmesh-journal"
        try FileManager.default.createDirectory(atPath: journal, withIntermediateDirectories: true)
        try leWriteFile("\(journal)/broken", "not a ForkMesh journal")

        leAssertGitError(.operationInterrupted) { _ = try GitRepository.open(GitRepositoryOpen(root)) }
    }

    func testOpenClearsAnInterruptedQuarantineBeforeExposingState() throws {
        let quarantine = try GitObjectQuarantine.begin(repository.gitDirectory)
        try quarantine.stage("interrupted.pack", [1, 2, 3])

        _ = try GitRepository.open(GitRepositoryOpen(root))

        XCTAssertFalse(leExists(quarantine.directory))
    }

    func testQuarantineRejectionLeavesHeadUnchanged() throws {
        let headBefore = try leReadFile("\(repository.gitDirectory)/HEAD")
        let quarantine = try GitObjectQuarantine.begin(repository.gitDirectory)
        try quarantine.stage("incoming.pack", [1, 2, 3])

        try quarantine.reject()

        XCTAssertFalse(leExists(quarantine.directory))
        XCTAssertEqual(try leReadFile("\(repository.gitDirectory)/HEAD"), headBefore)
    }
}
