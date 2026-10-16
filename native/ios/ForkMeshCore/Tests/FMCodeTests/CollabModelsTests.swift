// Repo collaboration and the issue filters on the view models the native screens bind to, over
// a fake collaboration API and a recording inbox; taps become model calls.

import FMApi
import FMBase
import FMCrypto
import FMDiff
import XCTest
@testable import FMCode

/// A fake collaboration API (plus the compare answer of the branch diff tests).
@MainActor
final class FakeCollabAPI: CollabAPI {
    var issues: [Issue] = [collabFixtureIssue]
    var pulls: [PublishedPull] = [collabFixturePull]
    var discussions: [RepoDiscussion] = [
        RepoDiscussion(
            number: 3, title: "Mobile maintainer workflow", body: "How should maintainers review from phones?",
            author: "alice", category: "ideas",
            events: [
                DiscussionEvent(
                    type: "comment", body: "Keep desktop node as the canonical apply surface.", authorName: "Mona"),
            ]),
    ]
    var replies: [FederatedReply] = []
    var statusBounty = IssueBounty(
        address: "escrow-fixture", status: "funded", amountUsd: 150, requiredLamports: 1_500_000_000,
        receivedLamports: 1_500_000_000, amountSol: 1.5, payUri: "solana:escrow-fixture?amount=1.5",
        payee: "maintainer-node")
    var comparison = RepoComparison()
    var failure: Error?

    var commandRequests: [JSONObject] = []
    var bountyRequests: [JSONObject] = []
    var compareRequests: [String] = []
    var federatedRequests: [String] = []
    var issueReads = 0
    var pullReads = 0
    var pullsHiddenForReads = 0

    @MainActor private func check() throws {
        if let failure { throw failure }
    }

    func publishedIssues(_ owner: String, _ name: String) async throws -> [Issue] {
        issueReads += 1
        try check()
        return issues
    }

    func publishedPulls(_ owner: String, _ name: String) async throws -> [PublishedPull] {
        pullReads += 1
        try check()
        return pullReads > pullsHiddenForReads ? pulls : []
    }

    func publishedDiscussions(_ owner: String, _ name: String) async throws -> [RepoDiscussion] {
        try check()
        return discussions
    }

    func federatedReplies(_ owner: String, _ name: String, kind: String, number: Int) async throws -> [FederatedReply] {
        federatedRequests.append("\(owner)/\(name) \(kind) \(number)")
        try check()
        return replies
    }

    func issueBountyStatus(_ owner: String, _ repo: String, _ number: Int) async throws -> IssueBounty {
        bountyRequests.append(["action": "status", "owner": .string(owner), "repo": .string(repo), "number": .int(Int64(number))])
        try check()
        return statusBounty
    }

    func desktopCommand(
        _ owner: String, _ name: String, ownerAccount: String, command: String, target: String, ts: String, sig: String,
        payload: JSONObject
    ) async throws -> DesktopCommandResult {
        commandRequests.append([
            "owner": .string(owner), "name": .string(name), "ownerAccount": .string(ownerAccount),
            "command": .string(command), "target": .string(target), "ts": .string(ts), "sig": .string(sig),
            "payload": .object(payload),
        ])
        try check()
        return DesktopCommandResult(ok: true, queued: 1)
    }

    func compare(_ owner: String, _ name: String, base: String, head: String) async throws -> RepoComparison {
        compareRequests.append("\(owner)/\(name) \(base)...\(head)")
        try check()
        return comparison
    }

    var mergeOwner = true
    var mergePlan: PullMergePlan? = PullMergePlan(
        number: 7, expectedBaseOid: "aaaa", expectedHeadOid: "bbbb", expectedPullsOid: "cccc")
    var mergeResults: [Result<PullMergeOutcome, Error>] = []
    var mergeRequests: [Bool] = []
    var mergeDeletes: [Bool] = []

    func canMergePulls(_ owner: String, _ repo: String) async -> Bool { mergeOwner }

    func pullMergePlan(_ owner: String, _ repo: String, _ pull: PublishedPull) async -> PullMergePlan? { mergePlan }

    func mergePull(_ owner: String, _ repo: String, _ plan: PullMergePlan, overrideReview: Bool, deleteHead: Bool) async throws
        -> PullMergeOutcome {
        mergeRequests.append(overrideReview)
        mergeDeletes.append(deleteHead)
        if mergeResults.isEmpty { return PullMergeOutcome(merged: true) }
        return try mergeResults.removeFirst().get()
    }
}

/// `RecordingInboxService`.
@MainActor
final class RecordingInbox: CollabInbox {
    var calls: [String] = []
    var draft: NewIssueDraft?
    var reviewNumber = 0
    var reviewState = ""
    var reviewBody = ""
    var discussion: (title: String, body: String, category: String)?
    var discussionComment: (number: Int, body: String)?
    var subscription: (node: String, source: String, number: Int, subscribed: Bool)?
    var subscriptionCount = 0
    var failure: Error?

    @MainActor private func record(_ call: String) throws {
        calls.append(call)
        if let failure { throw failure }
    }

    func submitNewIssue(
        _ owner: String, _ name: String, title: String, body: String, labels: [String], milestone: String,
        priority: Int, assignees: [String]
    ) async throws {
        draft = NewIssueDraft(
            title: title, body: body, labels: labels, milestone: milestone, priority: priority, assignees: assignees)
        try record("issue \(owner)/\(name)")
    }

    func commentOnIssue(_ owner: String, _ name: String, _ number: Int, _ body: String) async throws {
        try record("comment #\(number) \(body)")
    }

    func voteOnIssue(_ owner: String, _ name: String, _ number: Int) async throws {
        try record("vote #\(number)")
    }

    func setIssueStatus(_ owner: String, _ name: String, _ number: Int, _ status: String) async throws {
        try record("status #\(number) \(status)")
    }

    func submitNewPull(
        _ owner: String, _ name: String, title: String, base: String, head: String, patch: String, commits: String,
        description: String, authorName: String
    ) async throws {
        try record("pull \(title) \(base)...\(head) \(patch.count)")
    }

    func commentOnPull(_ owner: String, _ name: String, _ number: Int, _ body: String) async throws {
        try record("pull comment #\(number) \(body)")
    }

    func reviewPull(_ owner: String, _ name: String, _ number: Int, _ state: String, _ body: String) async throws {
        reviewNumber = number
        reviewState = state
        reviewBody = body
        try record("review #\(number) \(state)")
    }

    func submitNewDiscussion(_ owner: String, _ name: String, title: String, body: String, category: String) async throws {
        discussion = (title, body, category)
        try record("discussion \(title)")
    }

    func commentOnDiscussion(_ owner: String, _ name: String, _ number: Int, _ body: String) async throws {
        discussionComment = (number, body)
        try record("discussion comment #\(number)")
    }

    func markDiscussionAnswer(
        _ owner: String, _ name: String, _ number: Int, target: String, sessionToken: String
    ) async throws {
        try record("answer #\(number) \(target) \(sessionToken.isEmpty ? "unsigned-in" : "session")")
    }

    func setThreadSubscription(
        _ owner: String, _ name: String, node: String, source: String, number: Int, subscribed: Bool
    ) async throws {
        subscription = (node, source, number, subscribed)
        subscriptionCount += 1
        try record("subscribe \(source) #\(number) \(subscribed)")
    }
}

let collabRepo = Repository(owner: "owner", name: "repo", defaultBranch: "main")

final class CollabModelsTests: XCTestCase {
    private let identity = try! Identity.fromSeed([UInt8](repeating: 7, count: 32))

    override func setUp() async throws {
        await MainActor.run { IssueFilterMemory.forgetAll() }
    }

    /// `authenticatePreview()`: the preview node, bound to this device's key.
    @MainActor private var previewAccount: CollabAccount { CollabAccount(nodeName: "preview-node", pubkey: identity.publicKeyB64url) }

    /// `_ownerAuth`: the repo owner, bound to this device's key.
    @MainActor private var ownerAccount: CollabAccount { CollabAccount(nodeName: "owner", pubkey: identity.publicKeyB64url) }

    // MARK: Issues tab

    @MainActor private func issuesTab(account: CollabAccount = .signedOut) async -> (RepoIssuesModel, FakeCollabAPI) {
        let api = FakeCollabAPI()
        api.issues = filterFixtureIssues
        let model = RepoIssuesModel(repo: collabRepo, api: api, account: { account })
        await model.start()
        return (model, api)
    }

    @MainActor private func titles(_ model: RepoIssuesModel) -> [String] { model.visible.map(\.title) }

    @MainActor private func pick(_ model: RepoIssuesModel, _ field: IssueFilterField, _ label: String) {
        guard let option = model.options(field).last(where: { $0.label == label }) else {
            XCTFail("\(label) is not offered for \(field.rawValue)")
            return
        }
        model.select(field, option.value)
    }

    @MainActor func testIssuesTabFiltersByAuthorAndBack() async {
        let (model, _) = await issuesTab()
        XCTAssertEqual(model.countLabel, "3 of 4")
        pick(model, .author, "kai")
        XCTAssertEqual(titles(model), ["World scene leaks textures"])
        XCTAssertTrue(model.isActive(.author))
        XCTAssertEqual(model.pillLabel(.author), "kai")
        XCTAssertEqual(model.pillLabel(.label), "Label")
        pick(model, .author, "Any author")
        XCTAssertEqual(titles(model), ["Signed inbox drops replies", "World scene leaks textures", "Mirror roster goes stale"])
        XCTAssertFalse(model.isActive(.author))
    }

    @MainActor func testAssignedToMeUsesTheSignedInAccount() async {
        let (model, _) = await issuesTab(account: previewAccount)
        pick(model, .assignee, "Assigned to me")
        XCTAssertEqual(titles(model), ["World scene leaks textures"])
    }

    @MainActor func testAssignedToMeIsHiddenWhileSignedOut() async {
        let (model, _) = await issuesTab()
        let labels = model.options(.assignee).map(\.label)
        XCTAssertTrue(labels.contains("Unassigned"))
        XCTAssertFalse(labels.contains("Assigned to me"))
    }

    @MainActor func testStatusSwitchAndClearAll() async {
        let (model, _) = await issuesTab()
        XCTAssertFalse(titles(model).contains("Relay retries forever"))
        model.setStatus(.closed)
        XCTAssertEqual(titles(model), ["Relay retries forever"])
        model.setStatus(.all)
        XCTAssertEqual(titles(model).count, 4)
        model.setStatus(.open)

        pick(model, .author, "alice")
        pick(model, .label, "world")
        XCTAssertEqual(titles(model), [])
        XCTAssertTrue(model.hasMetadataFilter)
        model.clearMetadata()
        XCTAssertFalse(model.hasMetadataFilter)
        XCTAssertEqual(titles(model), ["Signed inbox drops replies", "World scene leaks textures", "Mirror roster goes stale"])
        XCTAssertEqual(model.status, .open)
    }

    @MainActor func testFilterSelectionsSurviveLeavingAndReturningToTheTab() async {
        let (model, api) = await issuesTab()
        pick(model, .author, "kai")
        model.setStatus(.all)
        XCTAssertEqual(titles(model), ["World scene leaks textures", "Relay retries forever"])

        // A rebuilt tab for the same repository reads back what was picked.
        let again = RepoIssuesModel(repo: collabRepo, api: api, account: { .signedOut })
        await again.start()
        XCTAssertEqual(again.pillLabel(.author), "kai")
        XCTAssertEqual(titles(again), ["World scene leaks textures", "Relay retries forever"])
        // Another repository starts clean.
        let other = RepoIssuesModel(repo: Repository(owner: "owner", name: "other"), api: api)
        XCTAssertEqual(other.filters, IssueFilters())
    }

    @MainActor func testIssuesTabLoadStates() async {
        let api = FakeCollabAPI()
        api.failure = ApiException("HTTP 503 for /api/repo/owner/repo/issues")
        let model = RepoIssuesModel(repo: collabRepo, api: api)
        XCTAssertTrue(model.state.isLoading)
        await model.start()
        XCTAssertEqual(model.state.error, "Exception: HTTP 503 for /api/repo/owner/repo/issues")
        api.failure = nil
        api.issues = []
        await model.load()
        XCTAssertEqual(model.state.value, [])
        XCTAssertEqual(RepoIssuesModel.emptyList, "No published issues yet.")
    }

    @MainActor func testPullsAndDiscussionsTabsLoad() async {
        let api = FakeCollabAPI()
        let pulls = RepoPullsModel(repo: collabRepo, api: api)
        await pulls.start()
        XCTAssertEqual(pulls.state.value?.map(\.title), ["Add signed review flow"])
        XCTAssertEqual(pulls.status, .open)
        XCTAssertEqual(pulls.visible.map(\.number), [7])
        pulls.status = .closed
        XCTAssertEqual(pulls.visible, [])
        XCTAssertEqual(pulls.noMatch, "No closed or merged pull requests.")
        XCTAssertEqual(PullStatusFilter.allCases.map(pulls.count), [1, 0, 1])
        api.pulls = []
        await pulls.start()
        XCTAssertEqual(pulls.state.value, [])
        let discussions = RepoDiscussionsModel(repo: collabRepo, api: api)
        await discussions.start()
        XCTAssertEqual(discussions.state.value?.map(\.title), ["Mobile maintainer workflow"])
        XCTAssertEqual(RepoPullsModel.emptyList, "No published pull requests yet.")
        XCTAssertEqual(RepoDiscussionsModel.emptyList, "No discussions have been published yet.")
    }

    // MARK: Issue detail

    @MainActor func testIssueDetailOpensByNumberAndSurfacesSignedCollaborationState() async {
        let api = FakeCollabAPI()
        let model = IssueDetailModel(repo: collabRepo, number: 12, api: api)
        XCTAssertTrue(model.isLoading)
        await model.start()
        XCTAssertEqual(model.issue?.title, "Stabilize signed inbox")
        XCTAssertEqual(model.title, "#12")
        let labels = model.chips.map(\.label)
        for expected in ["mobile", "signed", "2 votes", "$150 bounty", "priority 3", "v2 mobile", "mona", "kai"] {
            XCTAssertTrue(labels.contains(expected), expected)
        }
        XCTAssertEqual(model.timeline.count, 1)
        XCTAssertEqual(model.subIssuesTitle, "Sub-issues")
        XCTAssertTrue(model.showsFederatedReplies)

        let missing = IssueDetailModel(repo: collabRepo, number: 99, api: api)
        await missing.start()
        XCTAssertEqual(missing.loadError, "Issue #99 has not been published yet.")
        XCTAssertNil(missing.pull)

        let pull = IssueDetailModel(repo: collabRepo, number: 7, api: api)
        await pull.start()
        XCTAssertEqual(pull.pull?.title, "Add signed review flow")
        XCTAssertNil(pull.loadError)
        XCTAssertFalse(pull.isLoading)
    }

    @MainActor func testIssueDetailReadsRelationshipsAcrossTheWholeSet() async {
        let api = FakeCollabAPI()
        // The body is also the open event.
        func issue(_ number: Int, _ title: String, _ body: String, status: String = "open") -> Issue {
            Issue(
                number: number, title: title, status: status, body: body,
                events: [IssueEvent(type: "open", id: "open-\(number)", body: body, ts: number)])
        }
        api.issues = [
            issue(20, "Parent", "- [ ] Row (sub-issue #24)"),
            issue(21, "Child", "Parent issue: #20", status: "closed"),
            issue(22, "Other", "Depends on: #23, #20"),
        ]
        let model = IssueDetailModel(repo: collabRepo, number: 20, initial: api.issues[0], api: api)
        XCTAssertFalse(model.isLoading)
        await model.start()
        XCTAssertEqual(model.children, [21, 22, 24])
        XCTAssertEqual(model.subIssuesTitle, "Sub-issues · 1 of 3 closed")
        XCTAssertEqual(model.mentions.map(\.sourceNumber), [21, 22])
        let child = IssueDetailModel(repo: collabRepo, number: 21, initial: api.issues[1], api: api)
        await child.start()
        XCTAssertEqual(child.parents, [20])
        XCTAssertEqual(child.find(20)?.title, "Parent")

        // A failed relationship read leaves the issue on screen.
        api.failure = ApiException("offline")
        let offline = IssueDetailModel(repo: collabRepo, number: 20, initial: api.issues[0], api: api)
        await offline.start()
        XCTAssertNil(offline.loadError)
        XCTAssertEqual(offline.all, [])
    }

    @MainActor func testBountyRefreshWithoutALegacyRecordKeepsCustodialFundingDisabled() async {
        let api = FakeCollabAPI()
        api.statusBounty = IssueBounty()
        let model = IssueDetailModel(repo: collabRepo, number: 12, api: api)
        await model.start()
        await model.refreshBounty()
        XCTAssertEqual(model.bountyMessage, "No historical bounty migration record was found.")
        XCTAssertFalse(model.bountyMessageIsError)
        XCTAssertEqual(api.bountyRequests, [["action": "status", "owner": "owner", "repo": "repo", "number": 12]])
        XCTAssertFalse(model.bountyBusy)
    }

    @MainActor func testBountyRefreshAppliesAHistoricalRecordOrReportsTheFailure() async {
        let api = FakeCollabAPI()
        var plain = collabFixtureIssue
        plain.bountyAddress = ""
        plain.bountyStatus = ""
        let model = IssueDetailModel(repo: collabRepo, number: 12, initial: plain, api: api)
        await model.refreshBounty()
        XCTAssertEqual(model.bountyMessage, "Historical migration status refreshed.")
        XCTAssertEqual(model.issue?.bountyAddress, "escrow-fixture")

        api.failure = ApiException("HTTP 500")
        await model.refreshBounty()
        XCTAssertEqual(model.bountyMessage, "Refresh failed: Exception: HTTP 500")
        XCTAssertTrue(model.bountyMessageIsError)
    }

    @MainActor func testIssueDetailSkipsTheFediverseForPrivateRepositories() async {
        var secret = collabRepo
        secret.isPrivate = true
        let model = IssueDetailModel(repo: secret, number: 12, initial: collabFixtureIssue, api: FakeCollabAPI())
        XCTAssertFalse(model.showsFederatedReplies)
    }

    // MARK: Thread alerts

    @MainActor private func subscription(
        source: String, number: Int, account: CollabAccount, inbox: RecordingInbox
    ) -> ThreadSubscriptionModel {
        ThreadSubscriptionModel(
            repo: collabRepo, source: source, number: number, inbox: inbox, account: { account },
            identityPublicKey: identity.publicKeyB64url)
    }

    @MainActor func testIssueDetailCanSubscribeAndUnsubscribeFromThreadAlerts() async {
        let inbox = RecordingInbox()
        let model = subscription(source: "issue", number: 12, account: previewAccount, inbox: inbox)
        XCTAssertEqual(model.label(guest: false), "Subscribe thread")
        let first = await model.toggle()
        XCTAssertEqual(first, CollabNotice("Subscribed to thread notifications."))
        XCTAssertEqual(inbox.subscription?.node, "preview-node")
        XCTAssertEqual(inbox.subscription?.source, "issue")
        XCTAssertEqual(inbox.subscription?.number, 12)
        XCTAssertEqual(inbox.subscription?.subscribed, true)
        XCTAssertEqual(model.label(guest: false), "Unsubscribe thread")

        let second = await model.toggle()
        XCTAssertEqual(second, CollabNotice("Unsubscribed from thread notifications."))
        XCTAssertEqual(inbox.subscriptionCount, 2)
        XCTAssertEqual(inbox.subscription?.subscribed, false)
        XCTAssertEqual(model.label(guest: false), "Subscribe thread")
    }

    @MainActor func testPullDetailCanSubscribeAndUnsubscribeFromThreadAlerts() async {
        let inbox = RecordingInbox()
        let model = subscription(source: "pull", number: 7, account: previewAccount, inbox: inbox)
        _ = await model.toggle()
        XCTAssertEqual(inbox.subscription?.source, "pull")
        XCTAssertEqual(inbox.subscription?.number, 7)
        XCTAssertEqual(inbox.subscription?.subscribed, true)
        _ = await model.toggle()
        XCTAssertEqual(inbox.subscriptionCount, 2)
        XCTAssertEqual(inbox.subscription?.subscribed, false)
    }

    @MainActor func testThreadAlertControlIsDisabledWithoutAccountSigningKey() async {
        let inbox = RecordingInbox()
        let model = subscription(source: "issue", number: 12, account: .signedOut, inbox: inbox)
        XCTAssertEqual(model.label(guest: false), "Thread alerts unavailable")
        XCTAssertFalse(model.enabled(guest: false))
        let sent = await model.toggle()
        XCTAssertNil(sent)
        XCTAssertEqual(inbox.subscriptionCount, 0)
        // A guest is offered sign-in instead.
        XCTAssertEqual(model.label(guest: true), "Sign in for thread alerts")
        XCTAssertTrue(model.enabled(guest: true))
    }

    @MainActor func testAThreadAlertFailureKeepsTheState() async {
        let inbox = RecordingInbox()
        inbox.failure = ApiException("bad_signature")
        let model = subscription(source: "issue", number: 12, account: previewAccount, inbox: inbox)
        let notice = await model.toggle()
        XCTAssertEqual(notice, CollabNotice("Failed: Exception: bad_signature", isError: true))
        XCTAssertFalse(model.subscribed)
        XCTAssertFalse(model.busy)
    }

    // MARK: Pull detail

    @MainActor private func pullDetail(_ api: FakeCollabAPI, account: CollabAccount) async -> PullDetailModel {
        let model = PullDetailModel(
            repo: collabRepo, number: 7, api: api, account: { account }, identity: identity,
            clock: FixedClock(epochMillis: 1_700_000_000_000))
        await model.start()
        return model
    }

    @MainActor func testPullDetailQueuesSignedDesktopMergeRequest() async throws {
        let api = FakeCollabAPI()
        let model = await pullDetail(api, account: ownerAccount)
        XCTAssertEqual(model.pull?.title, "Add signed review flow")
        XCTAssertEqual(model.patchFiles, [PatchFile("lib/a.dart", .modified, adds: 1, dels: 0)])
        XCTAssertTrue(model.canRequestMerge)

        let wanted = ["pull.merge", "pull.merge_delete", "pull.fix_agent", "pull.close"]
        for command in model.shipCommands where wanted.contains(command.command) {
            XCTAssertTrue(model.isEnabled(command))
            let message = await model.request(command)
            XCTAssertEqual(message, command.successMessage)
        }
        XCTAssertEqual(api.commandRequests.map { $0["command"]?.stringValue ?? "" }, wanted)
        XCTAssertEqual(api.commandRequests.map { $0["target"]?.stringValue ?? "" }, Array(repeating: "pull:7", count: 4))
        let first = try XCTUnwrap(api.commandRequests.first)
        XCTAssertEqual(first["ownerAccount"], "owner")
        XCTAssertEqual(first["ts"], "1700000000000")
        for request in api.commandRequests {
            XCTAssertEqual(request["payload"], .object(["pullNumber": 7, "base": "main", "head": "review/mobile"]))
        }
        // The signature verifies against this device's key over the canonical string.
        let sig = try B64.decodeUnpadded(first["sig"]?.stringValue ?? "")
        let canonical = "forkmesh-desktop-command-v1\nowner\nrepo\npull.merge\npull:7\n1700000000000"
        XCTAssertTrue(Ed25519.verify(
            publicKey: identity.publicKeyBytes, message: Array(canonical.utf8), signature: Array(sig)))
    }

    @MainActor func testDesktopRequestsFromAnotherAccountAreSignedAsThatAccount() async {
        let api = FakeCollabAPI()
        let model = await pullDetail(api, account: previewAccount)
        let merge = model.shipCommands[0]
        let message = await model.request(merge)
        XCTAssertEqual(message, merge.successMessage)
        XCTAssertEqual(api.commandRequests.first?["owner"], "owner")
        XCTAssertEqual(api.commandRequests.first?["ownerAccount"], "preview-node")

        let signedOut = await pullDetail(api, account: .signedOut)
        let refused = await signedOut.request(merge)
        XCTAssertEqual(refused, "Exception: Sign in with the repo owner account to request desktop control.")
    }

    @MainActor func testMergeRequestsWaitForAnOpenPull() async {
        let api = FakeCollabAPI()
        api.pulls[0].status = "merged"
        let model = await pullDetail(api, account: ownerAccount)
        XCTAssertFalse(model.canRequestMerge)
        XCTAssertEqual(model.shipCommands.map(model.isEnabled), [false, false, true, true, true, true])
    }

    @MainActor func testPullDetailRendersSignedCommentAndReviewTimeline() async {
        let model = await pullDetail(FakeCollabAPI(), account: .signedOut)
        XCTAssertEqual(model.pull?.events.map(\.displayTitle), ["Comment", "Approved", "Requested changes"])
        XCTAssertEqual(
            model.pull?.events.map(\.body),
            ["Review changes before merge.", "LGTM from mobile.", "Please add a regression test."])
        let api = FakeCollabAPI()
        let missing = PullDetailModel(repo: collabRepo, number: 70, api: api, retryDelays: [0.001, 0.001])
        await missing.start()
        XCTAssertEqual(missing.loadError, "Pull request #70 has not been published yet.")
        XCTAssertEqual(api.pullReads, 3)
    }

    @MainActor func testPullDetailOpenedFromTheListReadsTheFullRecord() async {
        let api = FakeCollabAPI()
        let patch = "diff --git a/lib/b.dart b/lib/b.dart\n--- a/lib/b.dart\n+++ b/lib/b.dart\n@@ -1 +1,2 @@\n one\n+two\n"
        let mbox = "From 1111111111111111111111111111111111111111 Mon Sep 17 00:00:00 2001\nFrom: Kai <kai@example.com>\n"
            + "Date: Mon, 5 Oct 2026 10:00:00 +0000\nSubject: [PATCH] Add two\n\n---\n" + patch
        api.pulls[0].patch = patch
        api.pulls[0].commits = mbox
        var listed = collabFixturePull
        listed.patch = ""
        let model = PullDetailModel(repo: collabRepo, number: 7, initial: listed, api: api)
        XCTAssertTrue(model.diffFiles.isEmpty)
        await model.start()
        XCTAssertEqual(api.pullReads, 1)
        XCTAssertEqual(model.patchFiles, [PatchFile("lib/b.dart", .modified, adds: 1, dels: 0)])
        XCTAssertEqual(model.diffFiles.map(\.path), ["lib/b.dart"])
        XCTAssertEqual(model.commits.map(\.subject), ["Add two"])
        XCTAssertEqual(model.reviewScope, "owner/repo/pull/7")

        api.pulls[0].status = "merged"
        api.pulls[0].patch = ""
        api.pulls[0].commits = ""
        await model.reload()
        XCTAssertEqual(model.pull?.status, "merged")
        XCTAssertEqual(model.diffFiles.map(\.path), ["lib/b.dart"])
        XCTAssertEqual(model.commits.count, 1)
        XCTAssertEqual(model.offeredShipCommands.map(\.command), ["pull.delete"])
    }

    @MainActor func testPullDetailWaitsForAPullStillOnItsWay() async {
        let api = FakeCollabAPI()
        api.pullsHiddenForReads = 2
        let model = PullDetailModel(repo: collabRepo, number: 7, api: api, retryDelays: [0.001, 0.001, 0.001])
        await model.start()
        XCTAssertNil(model.loadError)
        XCTAssertEqual(model.pull?.number, 7)
        XCTAssertEqual(api.pullReads, 3)
    }

    @MainActor func testMergeFromThePhoneOffersAReviewWaiver() async {
        let api = FakeCollabAPI()
        api.mergeResults = [
            .failure(AgentLaunchException("An independent review is required.", code: "review_required", status: 409)),
            .success(PullMergeOutcome(merged: true, message: "Merged at abc1234.")),
        ]
        let model = PullMergeModel(repo: collabRepo, pull: collabFixturePull, api: api)
        XCTAssertTrue(model.loading)
        await model.load()
        XCTAssertFalse(model.loading)
        XCTAssertTrue(model.authorized)
        XCTAssertTrue(model.canMerge)
        XCTAssertTrue(PullMerge.planCopy(try! XCTUnwrap(model.plan), collabFixturePull).hasPrefix("Merges review/mobile into main"))

        let refused = await model.merge()
        XCTAssertNil(refused)
        XCTAssertEqual(model.message, "An independent review is required.")
        XCTAssertTrue(model.offerReviewWaiver)

        let merged = await model.merge(overrideReview: true)
        XCTAssertEqual(merged, "Merged pull request #7.")
        XCTAssertTrue(model.merged)
        XCTAssertEqual(api.mergeRequests, [false, true])
    }

    @MainActor func testMergeAndDeleteFromThePhoneUsesThePinnedMerge() async {
        let api = FakeCollabAPI()
        let model = PullMergeModel(repo: collabRepo, pull: collabFixturePull, api: api)
        await model.load()

        let merged = await model.merge(deleteHead: true)

        XCTAssertEqual(merged, "Merged pull request #7 and deleted its branch.")
        XCTAssertEqual(api.mergeDeletes, [true])
    }

    @MainActor func testMergeIsOfferedOnlyToOwnersOfOpenPulls() async {
        let api = FakeCollabAPI()
        api.mergeOwner = false
        let stranger = PullMergeModel(repo: collabRepo, pull: collabFixturePull, api: api)
        await stranger.load()
        XCTAssertFalse(stranger.authorized)
        XCTAssertFalse(stranger.canMerge)
        XCTAssertTrue(PullMerge.notOwner("forkmesh").contains("not an owner of forkmesh."))

        var closed = collabFixturePull
        closed.status = "closed"
        let done = PullMergeModel(repo: collabRepo, pull: closed, api: api)
        XCTAssertFalse(done.visible)
        await done.load()
        XCTAssertFalse(done.loading)

        api.mergeOwner = true
        api.mergePlan = PullMergePlan(
            number: 7, expectedBaseOid: "aaaa", expectedHeadOid: "bbbb", expectedPullsOid: "cccc", clean: false,
            conflicts: (1...10).map { "f\($0)" })
        let conflicted = PullMergeModel(repo: collabRepo, pull: collabFixturePull, api: api)
        await conflicted.load()
        XCTAssertFalse(conflicted.canMerge)
        XCTAssertEqual(
            PullMerge.conflicts(try! XCTUnwrap(conflicted.plan)), "Conflicts: f1, f2, f3, f4, f5, f6, f7, f8 and 2 more")
        XCTAssertEqual(
            PullMerge.planCopy(try! XCTUnwrap(conflicted.plan), collabFixturePull),
            "The branches conflict, so this cannot be merged as it stands. Resolve the conflict and refresh.")
        XCTAssertEqual(PullMerge.mergeLabel(busy: true), "Merging…")
        XCTAssertEqual(PullMerge.mergeDeleteLabel, "Merge & delete")
    }

    @MainActor func testApproveReviewComposerSubmitsApproveState() async {
        let composer = PullReviewComposerModel(number: 7, state: "approve")
        XCTAssertEqual(composer.title, "Approve PR #7")
        XCTAssertTrue(composer.helper.contains("Approval is signed and sent to the pull inbox"))
        composer.text = "Looks good."
        let body = composer.confirm()
        XCTAssertEqual(body, "Looks good.")

        let inbox = RecordingInbox()
        let notice = await CollabWriter(repo: collabRepo, inbox: inbox).review(7, state: composer.state, body: body ?? "")
        XCTAssertEqual(inbox.reviewNumber, 7)
        XCTAssertEqual(inbox.reviewState, "approve")
        XCTAssertEqual(inbox.reviewBody, "Looks good.")
        XCTAssertEqual(inbox.calls.count, 1)
        XCTAssertTrue(notice.message.contains("pending the repo owner applying"))
    }

    @MainActor func testRequestChangesComposerRequiresAReviewNote() async {
        let composer = PullReviewComposerModel(number: 7, state: "request-changes")
        XCTAssertEqual(composer.title, "Request changes on PR #7")
        XCTAssertNil(composer.confirm())
        XCTAssertEqual(composer.errorText, "A note is required to request changes.")
        composer.text = "  Please cover the empty state before owner apply. "
        XCTAssertEqual(composer.confirm(), "Please cover the empty state before owner apply.")
    }

    // MARK: Signed writes

    @MainActor func testNewIssueSubmitsCollaborationMetadata() async {
        let inbox = RecordingInbox()
        let model = NewIssueModel(repo: collabRepo)
        model.title = "Polish mobile issues"
        model.body = "Add the same metadata controls mobile maintainers expect."
        model.labels = "mobile, signed"
        model.priority = "4"
        model.milestone = "v2 mobile"
        model.assignees = "mona, kai"
        guard let draft = model.submit() else { return XCTFail("no draft") }
        let notice = await CollabWriter(repo: collabRepo, inbox: inbox).submitIssue(draft)
        XCTAssertEqual(inbox.draft, NewIssueDraft(
            title: "Polish mobile issues", body: "Add the same metadata controls mobile maintainers expect.",
            labels: ["mobile", "signed"], milestone: "v2 mobile", priority: 4, assignees: ["mona", "kai"]))
        XCTAssertEqual(notice, CollabNotice("Submitted to the inbox - pending the repo owner applying it."))
    }

    @MainActor func testIssueActionsVoteCommentAndStatus() async {
        let inbox = RecordingInbox()
        let writer = CollabWriter(repo: collabRepo, inbox: inbox)
        let vote = await writer.vote(collabFixtureIssue)
        XCTAssertEqual(vote.message, "Vote submitted to the inbox.")
        let blank = await writer.comment(on: collabFixtureIssue, "   ")
        XCTAssertNil(blank)
        let comment = await writer.comment(on: collabFixtureIssue, " Repro on iOS too. ")
        XCTAssertEqual(comment?.message, CollabCopy.pendingNote)
        _ = await writer.toggleStatus(collabFixtureIssue)
        XCTAssertEqual(inbox.calls, ["vote #12", "comment #12 Repro on iOS too.", "status #12 closed"])

        inbox.failure = ApiException("bad_signature")
        let failed = await writer.vote(collabFixtureIssue)
        XCTAssertEqual(failed, CollabNotice("Failed: Exception: bad_signature", isError: true))
        let noIdentity = await CollabWriter(repo: collabRepo, inbox: nil).vote(collabFixtureIssue)
        XCTAssertEqual(noIdentity.message, "Failed: Exception: This device has no signing identity.")
    }

    @MainActor func testNewDiscussionComposerSubmitsSelectedCategory() async {
        let inbox = RecordingInbox()
        let model = NewDiscussionModel(repo: collabRepo)
        XCTAssertEqual(model.category, "general")
        XCTAssertFalse(model.ready)
        model.title = "Mobile review rituals"
        model.category = "ideas"
        model.body = "Let maintainers review without pretending the phone applies Git changes."
        XCTAssertTrue(model.ready)
        let notice = await CollabWriter(repo: collabRepo, inbox: inbox)
            .submitDiscussion(title: model.title, body: model.body, category: model.category)
        XCTAssertEqual(inbox.discussion?.title, "Mobile review rituals")
        XCTAssertEqual(inbox.discussion?.category, "ideas")
        XCTAssertEqual(
            inbox.discussion?.body, "Let maintainers review without pretending the phone applies Git changes.")
        XCTAssertTrue(notice?.message.contains("pending the repo owner applying") == true)
        let skipped = await CollabWriter(repo: collabRepo, inbox: inbox).submitDiscussion(title: "t", body: "", category: "help")
        XCTAssertNil(skipped)
    }

    @MainActor func testDiscussionDetailShowsCategoryAndSignedReplyComposer() async {
        let api = FakeCollabAPI()
        let inbox = RecordingInbox()
        let model = DiscussionDetailModel(repo: collabRepo, number: 3, api: api)
        await model.start()
        XCTAssertEqual(model.title, "Discussion #3")
        XCTAssertTrue(model.chips.map(\.label).contains("ideas"))
        XCTAssertEqual(model.discussion?.events.first?.body, "Keep desktop node as the canonical apply surface.")
        XCTAssertFalse(model.hasReply)
        model.reply = "Replies go through the discussion inbox."
        XCTAssertTrue(model.hasReply)
        _ = await CollabWriter(repo: collabRepo, inbox: inbox).reply(to: model.number, model.reply)
        XCTAssertEqual(inbox.discussionComment?.number, 3)
        XCTAssertEqual(inbox.discussionComment?.body, "Replies go through the discussion inbox.")
    }

    @MainActor func testNewPullDefaultsToTheDefaultBranchAndNeedsAPatch() async {
        let inbox = RecordingInbox()
        let model = NewPullModel(repo: collabRepo)
        XCTAssertEqual(model.base, "main")
        model.title = "Fix relay"
        XCTAssertFalse(model.ready)
        let skipped = await CollabWriter(repo: collabRepo, inbox: inbox)
            .submitPull(title: model.title, base: model.base, head: model.head, patch: model.patch)
        XCTAssertNil(skipped)
        model.head = " fix/relay "
        model.patch = "diff --git a/a b/a\n+x\n"
        let notice = await CollabWriter(repo: collabRepo, inbox: inbox)
            .submitPull(title: model.title, base: model.base, head: model.head, patch: model.patch)
        XCTAssertEqual(notice?.message, CollabCopy.pendingNote)
        XCTAssertEqual(inbox.calls, ["pull Fix relay main...fix/relay 22"])
    }

    @MainActor func testFederatedRepliesLoadForTheThread() async {
        let api = FakeCollabAPI()
        api.replies = [FederatedReply(remoteId: "r", author: "bob@lemmy.example", body: "hello from Lemmy")]
        let model = FederatedRepliesModel(repo: collabRepo, kind: "discussion", number: 9, api: api)
        await model.start()
        XCTAssertEqual(model.state.value?.first?.body, "hello from Lemmy")
        XCTAssertEqual(api.federatedRequests, ["owner/repo discussion 9"])
        api.failure = ApiException("offline")
        let failed = FederatedRepliesModel(repo: collabRepo, kind: "issue", number: 1, api: api)
        await failed.start()
        XCTAssertNotNil(failed.state.error)
    }
}
