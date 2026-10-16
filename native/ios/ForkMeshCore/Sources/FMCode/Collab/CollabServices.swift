// What the collaboration screens read: ApiService's collaboration calls and
// InboxService's signed writes, as protocols so the view models can be driven by fakes in
// tests (both services are final classes). ApiService and InboxService
// conform as they are.

import FMApi
import FMBase
import Foundation

/// The ApiService reads and requests the collaboration screens make.
@MainActor
public protocol CollabAPI: AnyObject {
    func publishedIssues(_ owner: String, _ name: String) async throws -> [Issue]
    func publishedPulls(_ owner: String, _ name: String) async throws -> [PublishedPull]
    func publishedPull(_ owner: String, _ name: String, _ number: Int) async throws -> PublishedPull?
    func publishedDiscussions(_ owner: String, _ name: String) async throws -> [RepoDiscussion]
    func federatedReplies(_ owner: String, _ name: String, kind: String, number: Int) async throws -> [FederatedReply]
    func issueBountyStatus(_ owner: String, _ repo: String, _ number: Int) async throws -> IssueBounty
    func desktopCommand(
        _ owner: String, _ name: String, ownerAccount: String, command: String, target: String, ts: String, sig: String,
        payload: JSONObject
    ) async throws -> DesktopCommandResult
    func compare(_ owner: String, _ name: String, base: String, head: String) async throws -> RepoComparison
    func canMergePulls(_ owner: String, _ repo: String) async -> Bool
    func pullMergePlan(_ owner: String, _ repo: String, _ pull: PublishedPull) async -> PullMergePlan?
    func mergePull(_ owner: String, _ repo: String, _ plan: PullMergePlan, overrideReview: Bool, deleteHead: Bool) async throws
        -> PullMergeOutcome
}

extension CollabAPI {
    public func publishedPull(_ owner: String, _ name: String, _ number: Int) async throws -> PublishedPull? {
        try await publishedPulls(owner, name).first { $0.number == number }
    }
}

extension ApiService: CollabAPI {}

/// The InboxService writes the collaboration screens sign and send.
@MainActor
public protocol CollabInbox: AnyObject {
    func submitNewIssue(
        _ owner: String, _ name: String, title: String, body: String, labels: [String], milestone: String,
        priority: Int, assignees: [String]
    ) async throws
    func commentOnIssue(_ owner: String, _ name: String, _ number: Int, _ body: String) async throws
    func voteOnIssue(_ owner: String, _ name: String, _ number: Int) async throws
    func setIssueStatus(_ owner: String, _ name: String, _ number: Int, _ status: String) async throws
    func submitNewPull(
        _ owner: String, _ name: String, title: String, base: String, head: String, patch: String, commits: String,
        description: String, authorName: String
    ) async throws
    func commentOnPull(_ owner: String, _ name: String, _ number: Int, _ body: String) async throws
    func reviewPull(_ owner: String, _ name: String, _ number: Int, _ state: String, _ body: String) async throws
    func submitNewDiscussion(_ owner: String, _ name: String, title: String, body: String, category: String) async throws
    func commentOnDiscussion(_ owner: String, _ name: String, _ number: Int, _ body: String) async throws
    func setThreadSubscription(
        _ owner: String, _ name: String, node: String, source: String, number: Int, subscribed: Bool
    ) async throws
}

extension InboxService: CollabInbox {}

/// A FutureBuilder snapshot: waiting, `snap.hasError` (with `'${snap.error}'`)
/// or data.
public enum CollabLoad<Value> {
    case loading
    case failed(String)
    case ready(Value)

    public var value: Value? {
        if case .ready(let value) = self { return value }
        return nil
    }

    public var isLoading: Bool {
        if case .loading = self { return true }
        return false
    }

    public var error: String? {
        if case .failed(let message) = self { return message }
        return nil
    }
}

extension CollabLoad: Equatable where Value: Equatable {}

/// Thrown when a write is attempted on a device without a signing identity
/// (no InboxService was built).
public struct CollabInboxUnavailable: Error, CustomStringConvertible, Equatable {
    public init() {}
    public var description: String { "Exception: This device has no signing identity." }
}

/// `_run`: a signed inbox write, then `okMessage` or "Failed: $e".
@MainActor
public func collabRun(_ okMessage: String, _ action: () async throws -> Void) async -> CollabNotice {
    do {
        try await action()
        return CollabNotice(okMessage)
    } catch {
        return CollabNotice(CollabCopy.runFailure(error), isError: true)
    }
}
