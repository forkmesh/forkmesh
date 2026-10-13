// A scriptable RepoClient, the stand-in for ApiService in the repo screen tests.

import FMAccount
import FMApi
import FMBase
import FMCrypto
import Foundation
@testable import FMCode

struct ReposTestError: Error, CustomStringConvertible {
    let text: String
    var description: String { text }
}

@MainActor
final class FakeRepoClient: RepoClient {
    var repositoriesResult: () throws -> [Repository] = { [] }
    var repositoriesCalls = 0

    var treeRefs: [String] = []
    var treePaths: [String] = []
    var treeResult: (String, String) throws -> RepoTree = { path, _ in RepoTree(path: path, entries: []) }

    var blobRefs: [String] = []
    var nextBlob = RepoBlob(path: "README.md", content: "void main() {}", source: "qt-node-a")

    var branchesResult: () throws -> [RepoBranch] = { [] }
    var branchesCalls = 0

    var searchRefs: [String] = []
    var searchQueries: [String] = []
    var searchResult: [RepoCodeSearchMatch] = []

    var commitsResult: () throws -> [JSONObject] = { [] }
    var commitsCalls = 0
    var historyRefs: [String] = []
    var historyResult: ((String) throws -> RepoHistoryRead)?
    var commitDetailHashes: [String] = []
    var commitDetailFallbacks: [JSONObject] = []
    var commitDetailResult: (String, JSONObject) throws -> RepoCommitDetail = { _, fallback in
        RepoCommitDetail(commit: fallback)
    }

    var mirrorSetResult: () throws -> RepoMirrorSet = { RepoMirrorSet(mirrors: []) }
    var releasesResult: () throws -> [RepoRelease] = { [] }

    var actionsCalls: [(ownerAccount: String, ts: String, sig: String)] = []
    var actionsResult: () throws -> RepoActions = { RepoActions() }
    var actionLogCalls: [Int] = []
    var actionLogResult: () throws -> ActionLog = { ActionLog(id: 0) }

    var desktopCommands: [(command: String, target: String, ownerAccount: String, ts: String, sig: String, payload: JSONObject)] = []
    var desktopResult: () throws -> DesktopCommandResult = { DesktopCommandResult(ok: true, queued: 1) }

    var agentSessionAccounts: [String] = []
    var agentSessionsResult: () throws -> [AgentSession] = { [] }
    var transcriptCalls: [(id: Int, ownerAccount: String)] = []
    var transcriptResult: () throws -> AgentTranscript = { AgentTranscript(status: "", transcript: "") }

    var imports: [(sourceUrl: String, targetOwner: String, mode: String, providerToken: String)] = []
    var importResult: () throws -> ApiJsonResult = { ApiJsonResult(200, ["ok": true, "created": true]) }

    var clearedCaches = 0

    func repositories() async throws -> [Repository] {
        repositoriesCalls += 1
        return try repositoriesResult()
    }

    func tree(_ owner: String, _ name: String, path: String, ref: String) async throws -> RepoTree {
        treeRefs.append(ref)
        treePaths.append(path)
        return try treeResult(path, ref)
    }

    func blob(_ owner: String, _ name: String, _ path: String, ref: String) async throws -> RepoBlob {
        blobRefs.append(ref)
        return RepoBlob(
            path: path, content: nextBlob.content, encoding: nextBlob.encoding, size: nextBlob.size, source: nextBlob.source)
    }

    func rawURL(_ owner: String, _ name: String, _ path: String, ref: String) -> URL {
        var text = "https://forkmesh.test/api/repo/\(owner)/\(name)/raw/\(path)"
        if !ref.isEmpty { text += "?ref=\(ref)" }
        return URL(string: text)!
    }

    func cloneURL(_ owner: String, _ name: String) -> URL {
        URL(string: "https://forkmesh.test/\(owner)/\(name)")!
    }

    func clearRepoCache(_ owner: String, _ name: String) {
        clearedCaches += 1
    }

    func branches(_ owner: String, _ name: String) async throws -> [RepoBranch] {
        branchesCalls += 1
        return try branchesResult()
    }

    func searchRepo(_ owner: String, _ name: String, _ query: String, ref: String) async throws -> RepoSearchResults {
        searchRefs.append(ref)
        searchQueries.append(query)
        return RepoSearchResults(code: searchResult)
    }

    func commits(_ owner: String, _ name: String) async throws -> [JSONObject] {
        commitsCalls += 1
        return try commitsResult()
    }

    func history(_ owner: String, _ name: String, ref: String) async throws -> RepoHistoryRead {
        historyRefs.append(ref)
        if let historyResult { return try historyResult(ref) }
        return RepoHistoryRead(commits: try commitsResult())
    }

    func commitDetail(_ owner: String, _ name: String, _ hash: String, fallbackCommit: JSONObject) async throws -> RepoCommitDetail {
        commitDetailHashes.append(hash)
        commitDetailFallbacks.append(fallbackCommit)
        return try commitDetailResult(hash, fallbackCommit)
    }

    func mirrorSet(_ owner: String, _ name: String) async throws -> RepoMirrorSet {
        try mirrorSetResult()
    }

    func releases(_ owner: String, _ name: String) async throws -> [RepoRelease] {
        try releasesResult()
    }

    func repoActions(_ owner: String, _ name: String, ownerAccount: String, ts: String, sig: String) async throws -> RepoActions {
        actionsCalls.append((ownerAccount, ts, sig))
        return try actionsResult()
    }

    func actionLog(_ owner: String, _ name: String, _ id: Int, ownerAccount: String, ts: String, sig: String) async throws -> ActionLog {
        actionLogCalls.append(id)
        return try actionLogResult()
    }

    func desktopCommand(
        _ owner: String, _ name: String, ownerAccount: String, command: String, target: String, ts: String, sig: String,
        payload: JSONObject
    ) async throws -> DesktopCommandResult {
        desktopCommands.append((command, target, ownerAccount, ts, sig, payload))
        return try desktopResult()
    }

    func agentSessions(_ owner: String, _ name: String, ownerAccount: String) async throws -> [AgentSession] {
        agentSessionAccounts.append(ownerAccount)
        return try agentSessionsResult()
    }

    func agentTranscript(_ owner: String, _ name: String, _ id: Int, ownerAccount: String) async throws -> AgentTranscript {
        transcriptCalls.append((id, ownerAccount))
        return try transcriptResult()
    }

    func createRepositoryImport(sourceUrl: String, targetOwner: String, mode: String, providerToken: String) async throws -> ApiJsonResult {
        imports.append((sourceUrl, targetOwner, mode, providerToken))
        return try importResult()
    }
}

/// A signed-in `mobile_node` session.
func repoTestSession(nodeName: String = "alice", pubkey: String = "") -> AuthSession {
    AuthSession(
        nodeName: nodeName, email: "\(nodeName)@example.com", status: "active", pubkey: pubkey, emailVerified: true,
        isAdmin: false, avatarPng: "", avatarUpdatedAt: 0, createdAt: 1,
        savedAt: 1, sessionKind: "mobile_node", deviceKind: "mobile_node",
        capabilities: ["browse", "comment", "submit_issue", "submit_pr", "publish_repo", "owner_sign"],
        sessionToken: "session-token")
}

/// A fixed device key.
func repoTestIdentity() throws -> Identity {
    try Identity.fromSeed(Array(repeating: 7, count: 32))
}
