// Repository config and remote URLs (Kotlin: GitRepositoryConfigTest.kt, GitRemoteUrlTest.kt).

import Foundation
import XCTest
@testable import FMGit

final class LocalEngineConfigTests: LocalEngineTestCase {
    private func parse(_ text: String) throws -> RepositoryConfig { try RepositoryConfig.parse(leUtf8(text)) }

    func testAcceptsOnlyTheSupportedNonBareSha1CoreConfiguration() throws {
        let config = try parse(
            "[core]\nrepositoryformatversion = 0\nbare = false\nfilemode = true\n[extensions]\nobjectformat = sha1\n"
        )
        XCTAssertTrue(config.fileMode)
        XCTAssertFalse(try parse("[core]\n\tfilemode = false\n").fileMode)
    }

    func testRejectsIncludeDirectivesAndExternalWorktrees() {
        leAssertGitError(.unsupportedRepositoryFormat) { _ = try parse("[include]\npath = /tmp/x\n") }
        leAssertGitError(.unsupportedRepositoryFormat) { _ = try parse("[core]\nworktree = /tmp/x\n") }
    }

    func testRejectsEveryUnsupportedOrDangerousShape() {
        let cases: [(String, String)] = [
            ("[core]\nrepositoryformatversion = 1\n", "repository format is unsupported"),
            ("[core]\nbare = true\n", "bare repository is unsupported"),
            ("[extensions]\nobjectformat = sha256\n", "object format is unsupported"),
            ("[core]\nfilemode = yes\n", "core.filemode is invalid"),
            ("[includeIf \"gitdir:/x\"]\npath = y\n", "config includes are unsupported"),
            ("[alias]\nco = checkout\n", "config section is unsupported"),
            ("[credential]\nhelper = store\n", "config section is unsupported"),
            ("[core]\nhooksPath = /tmp\n", "executable or transport-overriding config is unsupported"),
            ("[core]\nsshCommand = evil\n", "executable or transport-overriding config is unsupported"),
            ("[http]\nproxy = http://x\n", "executable or transport-overriding config is unsupported"),
            ("[core]\nfoo = \"quoted\"\n", "config value quoting or continuation is unsupported"),
            ("[core]\nfoo = a\\\n", "config value quoting or continuation is unsupported"),
            ("key = value\n", "config line is invalid"),
            ("[core\n", "config section header is malformed or uses unsupported quoting"),
            ("[remote \"a\\\"b\"]\n", "config section header is malformed or uses unsupported quoting"),
        ]
        for (text, message) in cases {
            let error = leAssertGitError(.unsupportedRepositoryFormat) { _ = try parse(text) }
            XCTAssertEqual(error?.message, message, text)
        }
        leAssertGitError(.unsupportedRepositoryFormat) { _ = try RepositoryConfig.parse([0x5b, 0xff, 0x5d]) }
        leAssertGitError(.resourceLimitExceeded) {
            _ = try RepositoryConfig.parse([UInt8](repeating: 0x23, count: 1024 * 1024 + 1))
        }
    }

    func testParsesOneValidatedRemoteAndBranchUpstream() throws {
        let config = try parse(
            "[core]\nrepositoryformatversion = 0\n"
                + "[remote \"origin\"]\n\turl = https://forkmesh.test/ada/notes\n"
                + "\tfetch = +refs/heads/*:refs/remotes/origin/*\n"
                + "[branch \"main\"]\n\tremote = origin\n\tmerge = refs/heads/main\n"
        )

        let remote = try XCTUnwrap(config.remotes["origin"])
        XCTAssertEqual(remote.url.value, "https://forkmesh.test/ada/notes")
        XCTAssertTrue(remote.fetch.isWildcard)
        let branch = try XCTUnwrap(config.branches["main"])
        XCTAssertEqual(branch.remote, "origin")
        XCTAssertEqual(branch.merge.value, "refs/heads/main")
    }

    func testARemoteWithoutFetchGetsTheStandardRefspec() throws {
        let config = try parse("[remote \"up\"]\nurl = https://forkmesh.test/r\n")
        XCTAssertEqual(config.remotes["up"]?.fetch.value, "+refs/heads/*:refs/remotes/up/*")
        XCTAssertTrue(try parse("[branch \"main\"]\n").branches.isEmpty)
        let error = leAssertGitError(.unsupportedRepositoryFormat) { _ = try parse("[branch \"main\"]\nremote = origin\n") }
        XCTAssertEqual(error?.message, "branch must have one remote and one merge ref")
    }

    func testRejectsNonHttpsRemoteUrlsAndMultipleFetchRefspecs() {
        leAssertGitError(.protocolUnsupported) { _ = try parse("[remote \"origin\"]\nurl = ssh://host/repo\n") }
        let multiple = leAssertGitError(.unsupportedRepositoryFormat) {
            _ = try parse(
                "[remote \"origin\"]\nurl = https://forkmesh.test/r\n"
                    + "fetch = +refs/heads/*:refs/remotes/origin/*\nfetch = +refs/heads/*:refs/remotes/other/*\n"
            )
        }
        XCTAssertEqual(multiple?.message, "remote must have one url and at most one fetch refspec")
        leAssertGitError(.protocolUnsupported) {
            _ = try parse("[remote \"origin\"]\nurl = https://user:pass@forkmesh.test/r\n")
        }
    }

    func testRejectsAFetchRefspecThatTargetsAnotherRemoteNamespace() {
        let error = leAssertGitError(.unsupportedRepositoryFormat) {
            _ = try parse(
                "[remote \"origin\"]\nurl = https://forkmesh.test/r\nfetch = +refs/heads/*:refs/remotes/other/*\n"
            )
        }
        XCTAssertEqual(error?.message, "remote fetch refspec must target its own remote namespace")
    }

    func testWritesRemoteAndBranchSectionsAtomicallyAndRevalidates() throws {
        let root = try makeTemp("forkmesh-config-")
        let file = "\(root)/config"
        try leWriteFile(file, "[core]\n\trepositoryformatversion = 0\n\tbare = false\n")

        let url = try GitRemoteUrl.parse("https://forkmesh.test/ada/notes")
        try RepositoryConfig.writeRemote(file, "origin", url: url, fetch: try GitFetchRefspec.standard("origin"))
        try RepositoryConfig.writeBranchUpstream(
            file, "main", remote: "origin", merge: try GitRefName.branch("refs/heads/main")
        )
        // Rewriting the same remote must replace, not duplicate, the section.
        try RepositoryConfig.writeRemote(file, "origin", url: url, fetch: try GitFetchRefspec.standard("origin"))

        let config = try RepositoryConfig.load(file)
        XCTAssertEqual(Array(config.remotes.keys), ["origin"])
        XCTAssertEqual(Array(config.branches.keys), ["main"])
        let text = try leReadFile(file)
        XCTAssertEqual(text.components(separatedBy: "[remote \"origin\"]").count - 1, 1)
        XCTAssertTrue(text.contains("[core]"))
        XCTAssertEqual(
            text,
            "[core]\n\trepositoryformatversion = 0\n\tbare = false\n"
                + "[branch \"main\"]\n\tremote = origin\n\tmerge = refs/heads/main\n"
                + "[remote \"origin\"]\n\turl = https://forkmesh.test/ada/notes\n"
                + "\tfetch = +refs/heads/*:refs/remotes/origin/*\n"
        )
        XCTAssertFalse(leExists("\(file).lock"))
    }

    func testConfigWritesFailClosed() throws {
        let root = try makeTemp("forkmesh-config-lock-")
        let file = "\(root)/config"
        try leWriteFile(file, "[core]\n\tbare = false\n")
        let url = try GitRemoteUrl.parse("https://forkmesh.test/r")
        try leWriteFile("\(file).lock", "")
        let locked = leAssertGitError(.refLocked) {
            try RepositoryConfig.writeRemote(file, "origin", url: url, fetch: try GitFetchRefspec.standard("origin"))
        }
        XCTAssertEqual(locked?.message, "config lock already exists")
        try FileManager.default.removeItem(atPath: "\(file).lock")
        leAssertGitError(.invalidRef) {
            try RepositoryConfig.writeRemote(file, "bad name", url: url, fetch: try GitFetchRefspec.standard("origin"))
        }
        // A malformed file is never silently fixed by a rewrite.
        try leWriteFile(file, "[alias]\nx = y\n")
        leAssertGitError(.unsupportedRepositoryFormat) {
            try RepositoryConfig.writeRemote(file, "origin", url: url, fetch: try GitFetchRefspec.standard("origin"))
        }
    }

    func testRepositorySetRemoteAndUpstreamRoundTrip() throws {
        let root = try makeTemp("forkmesh-config-repo-")
        let repository = try GitRepository.initialize(GitRepositoryInit(root))
        try repository.setRemote("origin", try GitRemoteUrl.parse("https://forkmesh.test/ada/notes"))
        try repository.setBranchUpstream(
            try GitBranchName("main"), remote: "origin", merge: try GitRefName.branch("refs/heads/trunk")
        )
        let config = try repository.readConfig()
        XCTAssertEqual(config.remotes["origin"]?.url.value, "https://forkmesh.test/ada/notes")
        XCTAssertEqual(config.branches["main"]?.merge.value, "refs/heads/trunk")
        // The rewritten config still opens.
        _ = try GitRepository.open(GitRepositoryOpen(root))
    }

    // MARK: remote URLs

    func testAcceptsOnlyCredentialFreeHttpsGitRemoteUrls() throws {
        let remote = try GitRemoteUrl.parse("https://example.test/group/repo.git")

        XCTAssertEqual(remote.url.scheme, "https")
        XCTAssertEqual(remote.redactedOrigin, "https://example.test")
        XCTAssertEqual(remote.value, "https://example.test/group/repo.git")
    }

    func testRejectsNonHttpsSchemesUrlCredentialsAndFragments() {
        for value in [
            "http://example.test/repo.git",
            "file:///tmp/repo.git",
            "ssh://example.test/repo.git",
            "https://user:secret@example.test/repo.git",
            "https://example.test/repo.git#fragment",
            "https://example.test/repo.git?x=1",
            "https:///repo.git",
            "not a url",
        ] {
            let error = leAssertGitError(.protocolUnsupported) { _ = try GitRemoteUrl.parse(value) }
            XCTAssertEqual(error?.message, "remote URL must be a credential-free HTTPS URL", value)
        }
    }

    func testNormalizesLikeDartUri() throws {
        XCTAssertEqual(try GitRemoteUrl.parse("HTTPS://Example.TEST:443/a/./b/../repo.git").value, "https://example.test/a/repo.git")
        let ported = try GitRemoteUrl.parse("https://example.test:8443/repo")
        XCTAssertEqual(ported.value, "https://example.test:8443/repo")
        XCTAssertEqual(ported.redactedOrigin, "https://example.test:8443")
    }
}
