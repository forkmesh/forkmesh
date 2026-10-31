// View models of the collaboration detail screens: issue detail (with the
// bounty funding refresh), thread subscription, pull detail (with desktop
// command requests), the pull review composer, discussion detail and federated
// replies, plus CollabWriter for the signed inbox writes of the issue and pull
// actions, new pulls, new discussions, new issues and the discussion reply.
//
// The native screens are opened by number (a notification deep link has no
// record at hand), so each detail model reads the published list and finds
// its record; a caller that already holds the record passes it as `initial`
// and nothing is shown loading.

import FMAccount
import FMApi
import FMBase
import FMDiff
import FMCrypto
import Foundation
import Observation

/// "Issue #N" not in the published list.
public func collabNotPublished(_ kind: String, _ number: Int) -> String {
    "\(kind) #\(number) has not been published yet."
}

// MARK: - Issue

/// `_IssueDetailScreenState`.
@MainActor
@Observable
public final class IssueDetailModel {
    public let repo: Repository
    public let number: Int
    public private(set) var issue: Issue?
    /// Every issue in the repository: parents, sub-issues and mentions are
    /// relationships between issues, so they are read across the whole set.
    public private(set) var all: [Issue] = []
    public private(set) var loadError: String?
    public private(set) var pull: PublishedPull?
    public private(set) var bountyBusy = false
    public private(set) var bountyMessage = ""

    @ObservationIgnored private let api: CollabAPI
    @ObservationIgnored private var started = false

    public init(repo: Repository, number: Int, initial: Issue? = nil, api: CollabAPI) {
        self.repo = repo
        self.number = initial?.number ?? number
        self.issue = initial
        self.api = api
    }

    public var title: String { IssueDetail.title(number) }

    public var isLoading: Bool { issue == nil && pull == nil && loadError == nil }

    public func start() async {
        if started { return }
        started = true
        do {
            let issues = try await api.publishedIssues(repo.owner, repo.name)
            all = issues
            if issue == nil {
                issue = issues.first { $0.number == number }
                if issue == nil {
                    pull = try? await api.publishedPull(repo.owner, repo.name, number)
                    if pull == nil { loadError = collabNotPublished("Issue", number) }
                }
            }
        } catch {
            // `_allIssues` falls back to no relationships; only a screen with
            // nothing to show reports the failure.
            all = []
            if issue == nil { loadError = dartErrorText(error) }
        }
    }

    public func find(_ number: Int) -> Issue? { all.first { $0.number == number } }

    public var parents: [Int] { issue.map(IssueDetail.parents) ?? [] }

    public var children: [Int] { issue.map { subIssueNumbersOf(all, $0) } ?? [] }

    public var mentions: [IssueMention] { issueMentionsOf(all, number) }

    public var timeline: [IssueTimelineEntry] { IssueTimeline.entries(issue?.events ?? [], mentions) }

    public var chips: [CollabChip] { issue.map { IssueDetail.chips($0, repoFullName: repo.fullName) } ?? [] }

    public var body: String { issue.map(IssueDetail.body) ?? "" }

    public var subIssuesTitle: String { SubIssues.title(children, find: find) }

    /// Private repositories have no ActivityPub actor to ask about.
    public var showsFederatedReplies: Bool { !repo.isPrivate }

    public var bountyRows: [BountyCopyRow] { issue.map(BountyFunding.rows) ?? [] }

    public var bountyMessageIsError: Bool { IssueDetail.bountyMessageIsError(bountyMessage) }

    /// `_refreshBounty`.
    public func refreshBounty() async {
        guard !bountyBusy, let current = issue else { return }
        bountyBusy = true
        bountyMessage = ""
        do {
            let bounty = try await api.issueBountyStatus(repo.owner, repo.name, current.number)
            let (next, message) = IssueDetail.bountyRefreshed(issue ?? current, bounty)
            issue = next
            bountyMessage = message
        } catch {
            bountyMessage = IssueDetail.bountyRefreshFailed(error)
        }
        bountyBusy = false
    }
}

// MARK: - Thread subscription

/// `_ThreadSubscriptionButtonState`.
@MainActor
@Observable
public final class ThreadSubscriptionModel {
    public let repo: Repository
    /// "issue" or "pull".
    public let source: String
    public let number: Int
    public private(set) var subscribed = false
    public private(set) var busy = false

    @ObservationIgnored private let inbox: CollabInbox?
    @ObservationIgnored private let account: @MainActor () -> CollabAccount
    @ObservationIgnored private let identityPublicKey: String?

    public init(
        repo: Repository, source: String, number: Int, inbox: CollabInbox?,
        account: @escaping @MainActor () -> CollabAccount, identityPublicKey: String?
    ) {
        self.repo = repo
        self.source = source
        self.number = number
        self.inbox = inbox
        self.account = account
        self.identityPublicKey = identityPublicKey
    }

    /// `_signedAccountNode`.
    public var node: String { ThreadSubscription.signedAccountNode(account(), identityPublicKey: identityPublicKey) }

    public var canSign: Bool { !node.isEmpty }

    public func label(guest: Bool) -> String {
        ThreadSubscription.label(guest: guest, canSign: canSign, subscribed: subscribed)
    }

    public func icon(guest: Bool) -> String {
        ThreadSubscription.icon(guest: guest, canSign: canSign, subscribed: subscribed)
    }

    /// Whether the button is live: a guest may tap to be asked to sign in.
    public func enabled(guest: Bool) -> Bool { guest || (!busy && canSign) }

    /// `_toggle`: nil when nothing was sent.
    public func toggle() async -> CollabNotice? {
        let node = self.node
        if busy || node.isEmpty { return nil }
        let next = !subscribed
        busy = true
        defer { busy = false }
        do {
            guard let inbox else { throw CollabInboxUnavailable() }
            try await inbox.setThreadSubscription(
                repo.owner, repo.name, node: node, source: source, number: number, subscribed: next)
            subscribed = next
            return CollabNotice(ThreadSubscription.toggled(next))
        } catch {
            return CollabNotice(CollabCopy.runFailure(error), isError: true)
        }
    }
}

// MARK: - Signed inbox writes

/// The signed writes of the collaboration screens, each answering with
/// `_run`'s notice.
@MainActor
public final class CollabWriter {
    public let repo: Repository
    private let inbox: CollabInbox?

    public init(repo: Repository, inbox: CollabInbox?) {
        self.repo = repo
        self.inbox = inbox
    }

    private func run(_ okMessage: String, _ write: (CollabInbox) async throws -> Void) async -> CollabNotice {
        let inbox = self.inbox
        return await collabRun(okMessage) {
            guard let inbox else { throw CollabInboxUnavailable() }
            try await write(inbox)
        }
    }

    /// `openNewIssueComposer`'s submit.
    public func submitIssue(_ draft: NewIssueDraft) async -> CollabNotice {
        await run(CollabCopy.pendingNote) { inbox in
            try await inbox.submitNewIssue(
                self.repo.owner, self.repo.name, title: draft.title, body: draft.body, labels: draft.labels,
                milestone: draft.milestone, priority: draft.priority, assignees: draft.assignees)
        }
    }

    public func vote(_ issue: Issue) async -> CollabNotice {
        await run(IssueActions.voteSubmitted) { inbox in
            try await inbox.voteOnIssue(self.repo.owner, self.repo.name, issue.number)
        }
    }

    /// nil when the comment is blank (nothing is sent).
    public func comment(on issue: Issue, _ body: String) async -> CollabNotice? {
        let text = body.dartTrimmed
        if text.isEmpty { return nil }
        return await run(CollabCopy.pendingNote) { inbox in
            try await inbox.commentOnIssue(self.repo.owner, self.repo.name, issue.number, text)
        }
    }

    /// Close an open issue, reopen a closed one.
    public func toggleStatus(_ issue: Issue) async -> CollabNotice {
        await run(CollabCopy.pendingNote) { inbox in
            try await inbox.setIssueStatus(self.repo.owner, self.repo.name, issue.number, IssueActions.nextStatus(issue))
        }
    }

    /// nil when the comment is blank.
    public func commentOnPull(_ number: Int, _ body: String) async -> CollabNotice? {
        let text = body.dartTrimmed
        if text.isEmpty { return nil }
        return await run(CollabCopy.pendingNote) { inbox in
            try await inbox.commentOnPull(self.repo.owner, self.repo.name, number, text)
        }
    }

    /// `_reviewPublishedPull` / `_review` after the composer answered.
    public func review(_ number: Int, state: String, body: String) async -> CollabNotice {
        await run(CollabCopy.pendingNote) { inbox in
            try await inbox.reviewPull(self.repo.owner, self.repo.name, number, state, body)
        }
    }

    /// `showNewPullDialog`: nil when the title or the patch is blank.
    public func submitPull(title: String, base: String, head: String, patch: String) async -> CollabNotice? {
        if !NewPullComposer.ready(title: title, patch: patch) { return nil }
        return await run(CollabCopy.pendingNote) { inbox in
            try await inbox.submitNewPull(
                self.repo.owner, self.repo.name, title: title.dartTrimmed, base: base.dartTrimmed,
                head: head.dartTrimmed, patch: patch, commits: "", description: "", authorName: "")
        }
    }

    /// `showNewDiscussionDialog`: nil when the title or the body is blank.
    public func submitDiscussion(title: String, body: String, category: String) async -> CollabNotice? {
        if !NewDiscussionComposer.ready(title: title, body: body) { return nil }
        return await run(CollabCopy.pendingNote) { inbox in
            try await inbox.submitNewDiscussion(
                self.repo.owner, self.repo.name, title: title.dartTrimmed, body: body.dartTrimmed, category: category)
        }
    }

    /// The discussion reply: nil when blank.
    public func reply(to number: Int, _ body: String) async -> CollabNotice? {
        let text = body.dartTrimmed
        if text.isEmpty { return nil }
        return await run(CollabCopy.pendingNote) { inbox in
            try await inbox.commentOnDiscussion(self.repo.owner, self.repo.name, number, text)
        }
    }

    public func markAnswer(_ number: Int, target: String, sessionToken: String) async -> CollabNotice {
        await run(target.isEmpty ? DiscussionDetail.answerCleared : DiscussionDetail.answerSent) { inbox in
            try await inbox.markDiscussionAnswer(
                self.repo.owner, self.repo.name, number, target: target, sessionToken: sessionToken)
        }
    }
}

// MARK: - Pull request

/// `_requestDesktopCommand` after the guest gate: the owner checks, the
/// signature and the request; the notice text to show.
@MainActor
public func requestDesktopCommand(
    api: CollabAPI, repo: Repository, command: String, target: String, payload: JSONObject, successMessage: String,
    failureMessage: String = DesktopCommand.failureMessage, account: CollabAccount, identity: DeviceIdentity?,
    clock: Clock
) async -> String {
    guard DesktopCommandContract.isValid(command: command, target: target) else {
        return DesktopCommandContract.unsupportedMessage
    }
    do {
        let owner = try DesktopCommand.ownerAccount(account, identityPublicKey: identity?.publicKeyB64url)
        guard let identity else { throw ApiException("Sign in with the repo owner account to request desktop control.") }
        let repoOwner = RepoOwnerSigning.desktopCommandOwner(repo, account: owner)
        let ts = String(clock.nowMillis)
        let canonical = DesktopCommand.canonical(
            owner: repoOwner, name: repo.name, command: command, target: target, ts: ts)
        let sig = identity.sign(Array(canonical.utf8))
        let result = try await api.desktopCommand(
            repoOwner, repo.name, ownerAccount: owner, command: command, target: target, ts: ts, sig: sig,
            payload: payload)
        return result.ok ? successMessage : failureMessage
    } catch {
        return dartErrorText(error)
    }
}

/// `_PullDetailScreen`.
@MainActor
@Observable
public final class PullDetailModel {
    public let repo: Repository
    public let number: Int
    public private(set) var pull: PublishedPull?
    public private(set) var patchFiles: [PatchFile] = []
    public private(set) var diffFiles: [DiffFile] = []
    public private(set) var commits: [PatchCommit] = []
    public private(set) var loadError: String?
    public private(set) var refreshing = false

    @ObservationIgnored private let api: CollabAPI
    @ObservationIgnored private let account: @MainActor () -> CollabAccount
    @ObservationIgnored private let identity: DeviceIdentity?
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored private let retryDelays: [TimeInterval]
    @ObservationIgnored private var started = false

    public init(
        repo: Repository, number: Int, initial: PublishedPull? = nil, api: CollabAPI,
        account: @escaping @MainActor () -> CollabAccount = { .signedOut }, identity: DeviceIdentity? = nil,
        clock: Clock = SystemClock(), retryDelays: [TimeInterval] = [1.5, 3, 6, 12, 24]
    ) {
        self.repo = repo
        self.number = initial?.number ?? number
        self.api = api
        self.account = account
        self.identity = identity
        self.clock = clock
        self.retryDelays = retryDelays
        if let initial { show(initial) }
    }

    public var title: String { PullDetail.title(number) }

    public var isLoading: Bool { pull == nil && loadError == nil }

    public func start() async {
        if started { return }
        started = true
        if pull != nil {
            await reload()
            return
        }
        do {
            for delay in [0] + retryDelays {
                if delay > 0 { try await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000)) }
                if let found = try await api.publishedPull(repo.owner, repo.name, number) {
                    show(found)
                    return
                }
            }
            loadError = collabNotPublished("Pull request", number)
        } catch is CancellationError {
            started = false
        } catch {
            loadError = dartErrorText(error)
        }
    }

    public func reload() async {
        if refreshing { return }
        refreshing = true
        defer { refreshing = false }
        if let found = try? await api.publishedPull(repo.owner, repo.name, number) {
            show(found)
        }
    }

    private func show(_ next: PublishedPull) {
        var merged = next
        if let current = pull {
            if merged.patch.dartTrimmed.isEmpty { merged.patch = current.patch }
            if merged.commits.dartTrimmed.isEmpty { merged.commits = current.commits }
        }
        pull = merged
        patchFiles = parsePatchFiles(merged.patch)
        diffFiles = UnifiedDiff.parse(merged.patch)
        commits = UnifiedDiff.parseFormatPatchSeries(merged.commits)
    }

    public var isOpen: Bool { pull.map { $0.status.dartTrimmed.lowercased() == "open" } ?? false }

    public var reviewScope: String { "\(repo.owner)/\(repo.name)/pull/\(number)" }

    public var diffAdditions: Int { patchFileTotals(patchFiles).adds }

    public var diffDeletions: Int { patchFileTotals(patchFiles).dels }

    public var showsFederatedReplies: Bool { !repo.isPrivate }

    public var canRequestMerge: Bool { pull.map(PullDetail.canRequestMerge) ?? false }

    public var shipCommands: [PullShipCommand] { pull.map(PullShipCommands.all) ?? [] }

    public func isEnabled(_ command: PullShipCommand) -> Bool { !command.needsOpen || canRequestMerge }

    public var offeredShipCommands: [PullShipCommand] { pull.map(PullShipCommands.offered) ?? [] }

    /// `_requestPullCommand` after the guest gate.
    public func request(_ command: PullShipCommand) async -> String {
        guard let pull else { return DesktopCommand.failureMessage }
        return await requestDesktopCommand(
            api: api, repo: repo, command: command.command, target: PullDetail.commandTarget(pull),
            payload: PullDetail.commandPayload(pull), successMessage: command.successMessage, account: account(),
            identity: identity, clock: clock)
    }
}

/// `_showPullReviewComposer`'s dialog state.
@MainActor
@Observable
public final class PullReviewComposerModel {
    public let number: Int
    /// "approve", "request-changes" or "comment".
    public let state: String
    public var text = ""
    public private(set) var errorText = ""

    public init(number: Int, state: String) {
        self.number = number
        self.state = state
    }

    public var title: String { PullReview.composerTitle(state, number) }

    public var helper: String { PullReview.composerHelper(state) }

    /// The body to submit, or nil (with `errorText` set) when a
    /// request-changes review has no note.
    public func confirm() -> String? {
        if let error = PullReview.validationError(state, text) {
            errorText = error
            return nil
        }
        return text.dartTrimmed
    }
}

// MARK: - Discussion

/// `_DiscussionDetailScreen`.
@MainActor
@Observable
public final class DiscussionDetailModel {
    public let repo: Repository
    public let number: Int
    public private(set) var discussion: RepoDiscussion?
    public private(set) var loadError: String?
    public var reply = ""

    @ObservationIgnored private let api: CollabAPI
    @ObservationIgnored private var started = false

    public init(repo: Repository, number: Int, initial: RepoDiscussion? = nil, api: CollabAPI) {
        self.repo = repo
        self.number = initial?.number ?? number
        self.discussion = initial
        self.api = api
    }

    public var title: String { DiscussionDetail.title(number) }

    public var isLoading: Bool { discussion == nil && loadError == nil }

    public func start() async {
        if started || discussion != nil { return }
        started = true
        do {
            let discussions = try await api.publishedDiscussions(repo.owner, repo.name)
            discussion = discussions.first { $0.number == number }
            if discussion == nil { loadError = collabNotPublished("Discussion", number) }
        } catch {
            loadError = dartErrorText(error)
        }
    }

    public var chips: [CollabChip] {
        discussion.map { DiscussionDetail.chips($0, repoFullName: repo.fullName) } ?? []
    }

    public var showsFederatedReplies: Bool { !repo.isPrivate }

    /// The reply button acts only on a non-blank reply.
    public var hasReply: Bool { !reply.dartTrimmed.isEmpty }

    public func showAnswer(_ target: String) {
        guard var current = discussion else { return }
        current.answer = target.isEmpty ? nil : DiscussionEvent(type: "answer", target: target)
        discussion = current
    }
}

// MARK: - Fediverse

/// `_FederatedRepliesCard`'s future.
@MainActor
@Observable
public final class FederatedRepliesModel {
    public let repo: Repository
    /// "issue", "pull" or "discussion".
    public let kind: String
    public let number: Int
    public private(set) var state: CollabLoad<[FederatedReply]> = .loading

    @ObservationIgnored private let api: CollabAPI
    @ObservationIgnored private var started = false

    public init(repo: Repository, kind: String, number: Int, api: CollabAPI) {
        self.repo = repo
        self.kind = kind
        self.number = number
        self.api = api
    }

    public func start() async {
        if started { return }
        started = true
        do {
            state = .ready(try await api.federatedReplies(repo.owner, repo.name, kind: kind, number: number))
        } catch {
            state = .failed(dartErrorText(error))
        }
    }
}

// MARK: - Composers

/// `_NewIssueScreenState`.
@MainActor
@Observable
public final class NewIssueModel {
    public let repo: Repository
    public let parentNumber: Int
    public var title = "" {
        didSet {
            // The error clears as soon as a title is typed.
            if titleError != nil && !title.dartTrimmed.isEmpty { titleError = nil }
        }
    }
    public var body: String
    public var labels = ""
    public var priority = ""
    public var milestone = ""
    public var assignees = ""
    public private(set) var titleError: String?

    public init(repo: Repository, parentNumber: Int = 0) {
        self.repo = repo
        self.parentNumber = parentNumber
        self.body = NewIssueComposer.initialBody(parentNumber)
    }

    public var pageTitle: String { NewIssueComposer.title(parentNumber) }

    /// `_submit`: the draft, or nil with `titleError` set.
    public func submit() -> NewIssueDraft? {
        guard let draft = NewIssueComposer.draft(
            title: title, body: body, labels: labels, priority: priority, milestone: milestone, assignees: assignees)
        else {
            titleError = NewIssueComposer.titleRequired
            return nil
        }
        return draft
    }
}

/// `showNewDiscussionDialog`'s fields.
@MainActor
@Observable
public final class NewDiscussionModel {
    public let repo: Repository
    public var title = ""
    public var category = NewDiscussionComposer.defaultCategory
    public var body = ""

    public init(repo: Repository) {
        self.repo = repo
    }

    public var ready: Bool { NewDiscussionComposer.ready(title: title, body: body) }
}

// MARK: - Merge from the phone

/// `_PullMergeCardState`: merge an open pull request through the relay at the
/// exact commits the mirror reported.
@MainActor
@Observable
public final class PullMergeModel {
    public let repo: Repository
    public let pull: PublishedPull
    public private(set) var loading = true
    public private(set) var authorized = false
    public private(set) var busy = false
    public private(set) var merged = false
    public private(set) var plan: PullMergePlan?
    public private(set) var message = ""
    /// Set when the relay refused for want of an independent review: the
    /// owner is asked whether to waive it.
    public var offerReviewWaiver = false

    @ObservationIgnored private let api: CollabAPI

    public init(repo: Repository, pull: PublishedPull, api: CollabAPI) {
        self.repo = repo
        self.pull = pull
        self.api = api
    }

    public var isOpen: Bool { pull.status.dartTrimmed.lowercased() == "open" }

    /// The card is shown for an open pull, and stays to say it merged.
    public var visible: Bool { isOpen || merged }

    public var state: PullState { merged ? .merged : PullState(pull) }

    public var canMerge: Bool { !busy && plan?.isReady == true }

    /// `_load`.
    public func load() async {
        if !isOpen {
            loading = false
            return
        }
        let owner = await api.canMergePulls(repo.owner, repo.name)
        if !owner {
            authorized = false
            loading = false
            return
        }
        let next = await api.pullMergePlan(repo.owner, repo.name, pull)
        authorized = true
        plan = next
        loading = false
    }

    /// `_merge` after the guest gate: the notice text, or nil when the
    /// merge was refused (the reason is `message`).
    public func merge(overrideReview: Bool = false, deleteHead: Bool = false) async -> String? {
        guard let plan, !busy else { return nil }
        busy = true
        message = ""
        do {
            let outcome = try await api.mergePull(
                repo.owner, repo.name, plan, overrideReview: overrideReview, deleteHead: deleteHead)
            busy = false
            merged = outcome.merged
            message = outcome.message
            return outcome.merged ? PullMerge.mergedNotice(pull.number, deletedHead: deleteHead) : outcome.message
        } catch let error as AgentLaunchException {
            busy = false
            message = error.message
            // A review gate is the one refusal an owner can answer here, so
            // it is offered rather than just reported.
            if error.code == "review_required" { offerReviewWaiver = true }
            return nil
        } catch {
            busy = false
            message = dartErrorText(error)
            return nil
        }
    }
}
