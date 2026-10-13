// Parsing contracts for the models, including action sessions, agent sessions
// and federated threads, and native/android ModelsTest.kt. The Worker and
// desktop hosts answer in a few historical shapes; these pin the tolerant
// parsing (key aliases, string numbers, nesting) the screens rely on.

import XCTest
import FMBase
@testable import FMApi

final class ModelsTests: XCTestCase {
    // Issue #717: the catalog lists each serving copy under its node, so the
    // flagship read as mirror8/forkmesh, mirror23/forkmesh, ... in the app.
    func testCollapseRepositoryMirrorsNamesMirrorGroupsByOrganization() {
        let repos = collapseRepositoryMirrors([
            Repository(json: [
                "owner": "mirror8", "name": "forkmesh", "rootCommit": "root-a", "source": "remote-clone",
                "cloneUrl": "https://forkmesh.com/forkmesh/forkmesh", "liveHost": true, "updatedAt": "300",
                "logicalOwners": [["kind": "user", "owner": "ops"]],
            ]),
            Repository(json: [
                "owner": "jett", "name": "forkmesh", "rootCommit": "root-a", "description": "The flagship",
                "stars": 4, "updatedAt": "200",
                "logicalOwners": [["kind": "user", "owner": "jett"], ["kind": "organization", "owner": "forkmesh"]],
            ]),
            // No root commit: recognised by the route its clone URL copies.
            Repository(json: [
                "owner": "mirror23", "name": "forkmesh", "source": "remote-clone",
                "cloneUrl": "https://forkmesh.com/jett/forkmesh.git",
            ]),
            // Same root, but a fork: it keeps its own name.
            Repository(json: [
                "owner": "bob", "name": "forkmesh", "rootCommit": "root-a", "forkedFrom": "forkmesh/forkmesh",
            ]),
            Repository(json: ["owner": "alice", "name": "notes"]),
        ])

        XCTAssertEqual(repos.map(\.fullName), ["forkmesh/forkmesh", "bob/forkmesh", "alice/notes"])
        let flagship = repos[0]
        // Reads and owner-signed actions still address the aliased node's row.
        XCTAssertEqual(flagship.owner, "jett")
        XCTAssertEqual(flagship.description, "The flagship")
        XCTAssertEqual(flagship.stars, 4)
        XCTAssertEqual(flagship.mirrors, 3)
        XCTAssertTrue(flagship.liveHost)
        XCTAssertEqual(flagship.updatedMs, 300)
        XCTAssertEqual(repos[1].publicOwner, "")
        XCTAssertEqual(repos[2].owner, "alice")
    }

    func testCollapseRepositoryMirrorsKeepsUnrelatedSameNameReposApart() {
        let repos = collapseRepositoryMirrors([
            Repository(json: ["owner": "alice", "name": "notes"]),
            Repository(json: ["owner": "bob", "name": "notes"]),
            Repository(json: [
                "owner": "carol", "name": "notes", "isPrivate": true, "ownerKind": "organization",
                "logicalOwner": "acme",
            ]),
        ])
        XCTAssertEqual(repos.map(\.fullName), ["alice/notes", "bob/notes", "acme/notes"])
        XCTAssertEqual(repos.last?.owner, "carol")
    }

    func testCloneUrlRouteReadsHttpsAndScpForms() {
        XCTAssertEqual(cloneUrlRoute("https://forkmesh.com/jett/forkmesh.git")?.owner, "jett")
        XCTAssertEqual(cloneUrlRoute("git@forkmesh.com:jett/forkmesh.git")?.name, "forkmesh")
        XCTAssertNil(cloneUrlRoute("https://forkmesh.com/only"))
        XCTAssertNil(cloneUrlRoute("https://forkmesh.com/a%20b/c"))
        XCTAssertNil(cloneUrlRoute("  "))
    }

    func testRepositoryReadsWorkerCatalogFieldsWithDefaults() {
        let repo = Repository(json: [
            "owner": "alice", "name": "project",
            "stars": "12", // string numbers arrive from D1-backed JSON
            "mirrors": 3, "liveHost": true, "updatedAt": 1_700_000_000_000,
        ])
        XCTAssertEqual(repo.fullName, "alice/project")
        XCTAssertEqual(repo.stars, 12)
        XCTAssertEqual(repo.mirrors, 3)
        XCTAssertEqual(repo.defaultBranch, "main")
        XCTAssertFalse(repo.isPrivate)
        XCTAssertTrue(repo.liveHost)
        XCTAssertEqual(repo.updatedMs, 1_700_000_000_000)
        XCTAssertEqual(Repository(json: ["owner": "a", "name": "b"]).mirrors, 1)
        XCTAssertEqual(Repository(json: ["owner": "a", "name": "b", "mirrors": nil]).mirrors, 1)
        XCTAssertEqual(Repository(json: ["publishedAt": 5]).updatedMs, 5)
        XCTAssertEqual(Repository(json: ["forkedFrom": " Alice/X "]).forkedFrom, "alice/x")
    }

    func testIssueParsesNestedEventsAndToleratesMissingFields() {
        let issue = Issue(json: [
            "number": "7", "title": "Crash on open",
            "events": [["type": "comment", "body": "me too"], "not-an-event"],
            "labels": ["bug", 42], "milestone": "v2 mobile", "priority": "4", "assignees": ["mona", "kai"],
        ])
        XCTAssertEqual(issue.number, 7)
        XCTAssertTrue(issue.isOpen)
        XCTAssertEqual(issue.events.count, 1)
        XCTAssertEqual(issue.events.first?.body, "me too")
        XCTAssertEqual(issue.labels, ["bug", "42"])
        XCTAssertEqual(issue.milestone, "v2 mobile")
        XCTAssertEqual(issue.priority, 4)
        XCTAssertEqual(issue.assignees, ["mona", "kai"])
        XCTAssertEqual(Issue(json: ["progress": 250]).progress, 100)
        XCTAssertEqual(Issue(json: ["progress": -3]).progress, 0)
        // A non-list reads as empty.
        XCTAssertEqual(Issue(json: ["labels": "bug"]).labels, [])
        XCTAssertFalse(Issue(json: ["status": "closed"]).isOpen)
    }

    func testRepoTreeAcceptsListAndWrappedMapPayloads() {
        // Desktop hosts answer a bare entry list; the worker wraps it and names
        // the serving mirror.
        let bare = RepoTree(json: [["name": "src", "type": "tree"], ["name": "README.md", "type": "blob", "size": 120]])
        XCTAssertEqual(bare.entries.count, 2)

        let wrapped = RepoTree(json: [
            "entries": [
                ["name": "zeta.txt", "type": "blob"], ["name": "alpha", "type": "tree"],
                ["name": "beta.md", "type": "blob"],
            ],
            "servedBy": "mirror-node", "path": "docs",
        ])
        XCTAssertEqual(wrapped.source, "mirror-node")
        XCTAssertEqual(wrapped.path, "docs")
        // Directories sort first, then names case-insensitively.
        XCTAssertEqual(wrapped.entries.map(\.name), ["alpha", "beta.md", "zeta.txt"])
        // Bare child names are qualified with the requested path.
        XCTAssertEqual(wrapped.entries.last?.path, "docs/zeta.txt")
    }

    func testRepoBlobAcceptsRawStringsAndWrappedMaps() {
        let raw = RepoBlob(json: "hello world", path: "README.md")
        XCTAssertEqual(raw.content, "hello world")
        XCTAssertEqual(raw.path, "README.md")
        XCTAssertEqual(raw.size, 11)

        let wrapped = RepoBlob(json: ["content": "line1\n", "size": "6", "servedBy": "mirror-node"], path: "a.txt")
        XCTAssertEqual(wrapped.content, "line1\n")
        XCTAssertEqual(wrapped.size, 6)
        XCTAssertEqual(wrapped.source, "mirror-node")
        XCTAssertEqual(wrapped.path, "a.txt")
        XCTAssertEqual(RepoBlob(json: 42, path: "x").content, "")
    }

    func testRepoMirrorMapsTheMirrorListKeyAliases() {
        let mirror = RepoMirror(json: [
            "node": "backup-node", "repo": "project", "status": "online", "lastSeen": "1700000000000",
        ])
        XCTAssertEqual(mirror.label, "backup-node/project")
        XCTAssertTrue(mirror.online)
        XCTAssertEqual(mirror.lastSeenMs, 1_700_000_000_000)

        let offline = RepoMirror(json: ["name": "solo-node"])
        XCTAssertFalse(offline.online)
        XCTAssertEqual(offline.label, "solo-node/solo-node")
    }

    func testRepoMirrorReadsTheCloneEndpointAndHealthFlags() {
        var row: JSONObject = [
            "node": "mirror38", "owner": "mirror38", "repo": "forkmesh", "status": "online",
            "lastSync": 1_790_240_239_189, "cloneAvailable": true,
            "mirrorUrl": "https://mirror38.forkmesh.com/forkmesh/forkmesh.git",
            "endpoint": "https://mirror38.forkmesh.com", "integrity": "ok",
            "endpointHealthy": true, "endpointFresh": true, "abuseBlocked": false,
            "behind": false, "latencyMs": 60, "directLatencyMs": 0,
        ]
        let mirror = RepoMirror(json: row)
        XCTAssertEqual(mirror.node, "mirror38")
        XCTAssertEqual(mirror.mirrorUrl, "https://mirror38.forkmesh.com/forkmesh/forkmesh.git")
        XCTAssertEqual(mirror.endpoint, "https://mirror38.forkmesh.com")
        XCTAssertEqual(mirror.lastSyncMs, 1_790_240_239_189)
        XCTAssertEqual(mirror.latencyMs, 60)
        XCTAssertTrue(mirror.isHealthy)

        row["endpointFresh"] = false
        XCTAssertFalse(RepoMirror(json: row).isHealthy)
        row["endpointFresh"] = true
        row["directLatencyMs"] = 25
        XCTAssertEqual(RepoMirror(json: row).latencyMs, 25)
        XCTAssertFalse(RepoMirror(json: ["node": "old-node", "status": "online"]).isHealthy)
    }

    func testNetworkStatsReadsBothStatsEndpointShapes() {
        let modern = NetworkStats(json: [
            "onlineNodes": ["mainnode-a", "mainnode-b", "mainnode-c", "mainnode-d"], "hostsOnline": 2, "repos": 9,
        ])
        XCTAssertEqual(modern.nodesOnline, 4)
        XCTAssertEqual(modern.hostsOnline, 2)
        XCTAssertEqual(modern.repos, 9)

        let worker = NetworkStats(json: ["clients": 19, "hosts": 2, "repos": 9, "onlineNodes": ["mainnode-a", "mainnode-b"]])
        XCTAssertEqual(worker.nodesOnline, 2)

        let legacy = NetworkStats(json: ["clients": "3", "hosts": 1, "repositories": 5])
        XCTAssertEqual(legacy.nodesOnline, 3)
        XCTAssertEqual(legacy.hostsOnline, 1)
        XCTAssertEqual(legacy.repos, 5)
    }

    func testNetworkLeaderboardsParsesFundsReceivedBoards() {
        let boards = NetworkLeaderboards(json: [
            "fundsMainnodes": [["name": "mainnode-a", "lamports": "1250000000", "sol": "1.25"]],
            "fundsContributors": [["name": "alice", "lamports": 500_000_000, "sol": 0.5]],
            "fundsProjects": [["name": "forkmesh/mobile", "lamports": "42"]],
        ])
        XCTAssertEqual(boards.fundsMainnodes.first?.name, "mainnode-a")
        XCTAssertEqual(boards.fundsMainnodes.first?.lamports, 1_250_000_000)
        XCTAssertEqual(boards.fundsMainnodes.first?.sol, 1.25)
        XCTAssertEqual(boards.fundsMainnodes.first?.amountLabel, "1.25 SOL (1250000000 lamports)")
        XCTAssertEqual(boards.fundsContributors.first?.amountLabel, "0.5 SOL (500000000 lamports)")
        XCTAssertEqual(boards.fundsProjects.first?.amountLabel, "42 lamports")
        XCTAssertFalse(boards.isEmpty)
        XCTAssertTrue(NetworkLeaderboards(json: [:]).isEmpty)
    }

    func testForkNotificationParsesWorkerNotificationPayloads() {
        let notification = ForkNotification(json: [
            "id": "dedupe-1", "kind": "pull_submitted", "title": "Pull request submitted for mona/forkmesh",
            "body": "Mobile review flow is waiting in your desktop inbox.", "repo": "mona/forkmesh",
            "href": "/mona/forkmesh/pulls/7", "actor": "kai", "source": "pull", "ts": "1770000000000",
            "readAt": 0, "meta": ["number": 7],
        ])
        XCTAssertEqual(notification.id, "dedupe-1")
        XCTAssertEqual(notification.kind, "pull_submitted")
        XCTAssertEqual(notification.kindLabel, "Pull request")
        XCTAssertEqual(notification.title, "Pull request submitted for mona/forkmesh")
        XCTAssertEqual(notification.repo, "mona/forkmesh")
        XCTAssertEqual(notification.actor, "kai")
        XCTAssertEqual(notification.ts, 1_770_000_000_000)
        XCTAssertTrue(notification.isUnread)
        XCTAssertEqual(notification.meta["number"], .int(7))
        XCTAssertEqual(notification.filterGroup, "Repo")

        let defaults = ForkNotification(json: ["read_at": 5])
        XCTAssertEqual(defaults.kind, "pending_inbox")
        XCTAssertEqual(defaults.title, "Notification")
        XCTAssertEqual(defaults.kindLabel, "Inbox")
        XCTAssertEqual(defaults.readAt, 5)
        XCTAssertFalse(defaults.isUnread)
        XCTAssertEqual(notification.copyWith(readAt: 99).readAt, 99)
        XCTAssertEqual(notification.copyWith().readAt, 0)
        XCTAssertEqual(ForkNotification(id: "x", kind: "host_online", title: "t").filterGroup, "System")
        XCTAssertEqual(ForkNotification(id: "x", kind: "mention", title: "t").filter, .mentions)
    }

    func testNotificationPageReadsListAndUnreadCount() {
        let page = NotificationPage(json: [
            "ok": true, "unread": "2",
            "notifications": [
                ["id": "n1", "kind": "mention", "title": "Mention", "ts": 2],
                ["id": "n2", "kind": "host_offline", "title": "Host offline"],
            ],
        ])
        XCTAssertEqual(page.unread, 2)
        XCTAssertEqual(page.notifications.count, 2)
        XCTAssertEqual(page.notifications.first?.kindLabel, "Mention")
        XCTAssertEqual(page.notifications.last?.kindLabel, "Host")
        // No 'total' in the body: fall back to the page length rather than 0, so a
        // whole-inbox action never quotes an empty inbox.
        XCTAssertEqual(page.total, 2)
        XCTAssertEqual(NotificationPage(json: ["items": [["id": "i"]]]).notifications.count, 1)
    }

    func testNotificationPageKeepsTheInboxTotalPastThePage() {
        let page = NotificationPage(json: [
            "ok": true, "unread": 1, "total": "137",
            "notifications": [["id": "n1", "kind": "mention", "title": "Mention", "ts": 2]],
        ])
        XCTAssertEqual(page.notifications.count, 1)
        XCTAssertEqual(page.total, 137)
    }

    // MARK: - ApiService model cases (via ModelsTest.kt)

    func testBountyWalletParsesWalletFieldsAndBuildsLabelsAndLinks() {
        let wallet = BountyWallet(json: [
            "address": "OwnerBountyWallet111111111111111111111111111", "balanceLamports": "1234567890",
            "balanceSol": "1.23456789", "uri": "solana:OwnerBountyWallet111111111111111111111111111",
        ])
        XCTAssertTrue(wallet.hasWallet)
        XCTAssertEqual(wallet.address, "OwnerBountyWallet111111111111111111111111111")
        XCTAssertEqual(wallet.balanceLamports, 1_234_567_890)
        XCTAssertEqual(wallet.balanceSol, 1.23456789)
        XCTAssertEqual(wallet.payUri, "solana:OwnerBountyWallet111111111111111111111111111")
        XCTAssertEqual(wallet.balanceLabel, "1.23456789 SOL (1234567890 lamports)")
        XCTAssertEqual(wallet.shortAddress, "OwnerB...111111")
        XCTAssertEqual(
            wallet.explorerAddressUrl,
            "https://explorer.solana.com/address/OwnerBountyWallet111111111111111111111111111")
        XCTAssertEqual(solanaExplorerSignatureUrl("payout-signature"), "https://explorer.solana.com/tx/payout-signature")
        XCTAssertEqual(solanaExplorerAddressUrl("  "), "")
        XCTAssertEqual(solanaExplorerSignatureUrl(" a b/c "), "https://explorer.solana.com/tx/a%20b%2Fc")
        XCTAssertEqual(BountyWallet(json: "not a map"), BountyWallet())
        XCTAssertEqual(BountyWallet().balanceLabel, "0 lamports")
    }

    func testIssueParsesPublicBountyFundingFields() {
        let issue = Issue(json: [
            "number": 8, "title": "Pay a maintainer", "bountyUsd": 75, "bountyAddress": "escrow111",
            "bountyStatus": "funded", "bountyRequiredLamports": 9000, "bountyReceivedLamports": 7000,
            "bountyAmountSol": 0.000009, "bountyPayUri": "solana:escrow111?amount=0.000009",
            "bountyPayee": "maintainer-node", "bountyPayoutSig": "payout-sig",
        ])
        XCTAssertEqual(issue.bountyUsd, 75)
        XCTAssertEqual(issue.bountyAddress, "escrow111")
        XCTAssertEqual(issue.bountyStatus, "funded")
        XCTAssertEqual(issue.bountyRequiredLamports, 9000)
        XCTAssertEqual(issue.bountyReceivedLamports, 7000)
        XCTAssertEqual(issue.bountyAmountSol, 0.000009)
        XCTAssertEqual(issue.bountyPayUri, "solana:escrow111?amount=0.000009")
        XCTAssertEqual(issue.bountyPayee, "maintainer-node")
        XCTAssertEqual(issue.bountyPayoutSig, "payout-sig")
        XCTAssertEqual(issue.bountyProgressLabel, "7000 / 9000 lamports")
        XCTAssertTrue(issue.hasBountyFunding)
        XCTAssertEqual(issue.bountyStatusLabel, "funded")
        XCTAssertEqual(Issue(json: ["bountyUsd": "12.5"]).bountyUsd, 12.5, "a string falls back to the bounty record")
        XCTAssertEqual(Issue(json: ["address": "legacy"]).bountyAddress, "legacy")
    }

    func testIssueBountyReadsNestedRecordsAndDerivesStatus() {
        let nested = Issue(json: ["number": 3, "title": "Nested", "bounty": ["funding": ["address": "x"]]])
        XCTAssertEqual(nested.bountyAddress, "x", "a bounty record may wrap its own funding object")
        let funding = IssueBounty(json: ["funding": ["bountyAddress": "f1", "bountyStatus": " "]])
        XCTAssertEqual(funding.address, "f1")
        XCTAssertEqual(funding.status, "open")
        XCTAssertEqual(IssueBounty(status: "", payoutSig: "sig").statusLabel, "paid")
        XCTAssertEqual(IssueBounty(status: "", requiredLamports: 5, receivedLamports: 5).statusLabel, "funded")
        XCTAssertEqual(IssueBounty(receivedLamports: 3).progressLabel, "3 lamports received")
        XCTAssertFalse(IssueBounty().hasFunding)
        let updated = Issue(number: 1, title: "t", bountyUsd: 10).copyWithBounty(
            IssueBounty(address: "a", status: "", payoutSig: "p"))
        XCTAssertEqual(updated.bountyUsd, 10)
        XCTAssertEqual(updated.bountyStatus, "paid")
        XCTAssertEqual(updated.bountyAddress, "a")
    }

    // MARK: - agent sessions

    func testAgentSessionParsesWorkerAgentSessionPayloads() {
        let session = AgentSession(json: [
            "id": "42", "issueNumber": "12", "issueTitle": "Stabilize signed inbox", "status": "running",
            "provider": "openai", "model": "gpt-5.5", "branchName": "agent/fix-inbox", "lastError": "",
            "createdAtMs": "1700000000000", "startedAtMs": 1_700_000_001_000, "finishedAtMs": 0, "numTurns": "7",
            "durationMs": 125_000, "costUsd": "1.25", "createPr": true, "prNumber": "7", "baseRef": "abc123",
            "baseBranch": "main", "merged": true, "mergedAtMs": "1700000100000", "promptTokens": "1000",
            "completionTokens": 500, "totalTokens": "1500", "contextTokens": "12000", "contextWindow": "32000",
            "maxOutputTokens": "4000", "estimatedCredits": "6", "spendBeforeUsd": "2.00", "spendAfterUsd": "3.25",
            "diffStats": ["files": "3", "ahead": "2", "behind": "1", "conflicted": true],
        ])
        XCTAssertEqual(session.id, 42)
        XCTAssertEqual(session.issueNumber, 12)
        XCTAssertEqual(session.issueTitle, "Stabilize signed inbox")
        XCTAssertEqual(session.statusLabel, "Merged")
        XCTAssertEqual(session.providerLabel, "OpenAI")
        XCTAssertEqual(session.displayTitle, "Stabilize signed inbox")
        XCTAssertEqual(session.branchName, "agent/fix-inbox")
        XCTAssertEqual(session.numTurns, 7)
        XCTAssertEqual(session.durationLabel, "2m 5s")
        XCTAssertEqual(session.costLabel, "$1.25")
        XCTAssertFalse(session.isActive)
        XCTAssertTrue(session.createPr)
        XCTAssertEqual(session.prNumber, 7)
        XCTAssertEqual(session.prLabel, "PR #7")
        XCTAssertEqual(session.baseRef, "abc123")
        XCTAssertEqual(session.baseBranch, "main")
        XCTAssertEqual(session.diffBase, "main")
        XCTAssertTrue(session.merged)
        XCTAssertEqual(session.mergedAtMs, 1_700_000_100_000)
        XCTAssertEqual(session.promptTokens, 1000)
        XCTAssertEqual(session.completionTokens, 500)
        XCTAssertEqual(session.totalTokens, 1500)
        XCTAssertEqual(session.tokenLabel, "1500 tokens")
        XCTAssertEqual(session.contextTokens, 12000)
        XCTAssertEqual(session.contextWindow, 32000)
        XCTAssertEqual(session.maxOutputTokens, 4000)
        XCTAssertEqual(session.estimatedCredits, 6)
        XCTAssertEqual(session.spendBeforeUsd, 2.0)
        XCTAssertEqual(session.spendAfterUsd, 3.25)
        XCTAssertEqual(session.filesChanged, 3)
        XCTAssertEqual(session.ahead, 2)
        XCTAssertEqual(session.behind, 1)
        XCTAssertTrue(session.conflicted)
        XCTAssertEqual(session.diffLabel, "3 files · ↑2 ↓1")
        XCTAssertEqual(session.latestStatusSummary(), "Done · 7 turns · 2m 5s · $1.25 · 3 files · ↑2 ↓1")
    }

    func testAgentSessionLabelsFollowDartFormatting() {
        XCTAssertEqual(AgentSession(id: 1, costUsd: 0.004).costLabel, "<$0.01")
        XCTAssertEqual(AgentSession(id: 1).costLabel, "")
        XCTAssertEqual(AgentSession(id: 1, costUsd: 0.125).costLabel, "$0.13")
        XCTAssertEqual(AgentSession(id: 1, durationMs: 45_900).durationLabel, "45s")
        XCTAssertEqual(AgentSession(id: 9).displayTitle, "Agent session #9")
        XCTAssertEqual(AgentSession(id: 9, issueNumber: 4).displayTitle, "Issue #4")
        XCTAssertEqual(AgentSession(id: 1, status: "waiting_for_input").statusLabel, "Waiting")
        XCTAssertEqual(AgentSession(id: 1, status: "needs_review").statusLabel, "Needs Review")
        XCTAssertEqual(AgentSession(id: 1).statusLabel, "Unknown")
        XCTAssertEqual(AgentSession(id: 1, provider: "Cloudflare-AI").providerLabel, "Cloudflare AI")
        XCTAssertEqual(AgentSession(id: 1, provider: "local_llama").providerLabel, "Local Llama")
        XCTAssertEqual(AgentSession(id: 1).providerLabel, "Agent")
        XCTAssertEqual(AgentSession(json: ["id": 1]).filesChanged, -1)
        XCTAssertEqual(AgentSession(json: ["id": 1, "changedFiles": "x"]).filesChanged, -1)
        XCTAssertEqual(AgentSession(id: 1, filesChanged: 1).diffLabel, "1 file")
        XCTAssertEqual(AgentSession(id: 1, ahead: 0, behind: 0).diffLabel, "")
        XCTAssertEqual(AgentSession(id: 1, baseRef: "abc").diffBase, "abc")
        XCTAssertTrue(AgentSession(id: 1, status: "Waiting").isActive)

        let failed = AgentSession(id: 1, status: "error", lastError: String(repeating: "x", count: 200))
        XCTAssertEqual(failed.latestStatusSummary(), "Failed: " + String(repeating: "x", count: 117) + "...")
        XCTAssertEqual(failed.statusFailureReason(3), "...")
        XCTAssertEqual(AgentSession(id: 1, status: "failed").latestStatusSummary(), "Failed")
        XCTAssertEqual(AgentSession(id: 1, status: "running").latestStatusSummary(), "Running")

        let now = Date(epochMillis: 1_800_000_000_000)
        let limited = AgentSession(id: 1, rateLimitResetAtMs: 1_800_000_000_000 + 3_725_000)
        XCTAssertEqual(limited.rateLimitCountdownLabel(now: now), "Limit resets in 1h 2m")
        XCTAssertEqual(
            AgentSession(id: 1, rateLimitResetAtMs: 1_800_000_000_000 + 172_800_000).rateLimitCountdownLabel(now: now),
            "Limit resets in 2d 0h")
        XCTAssertEqual(
            AgentSession(id: 1, rateLimitResetAtMs: 1_800_000_000_000 + 30 * 86_400_000).rateLimitCountdownLabel(now: now),
            "Limit resets in 10d 0h")
        XCTAssertEqual(AgentSession(id: 1, rateLimitResetAtMs: 1_799_999_999_999).rateLimitCountdownLabel(now: now), "")

        let seconds = AgentSession(json: ["id": 1, "rateLimitResetAt": 1_800_000_000])
        XCTAssertEqual(seconds.rateLimitResetAtMs, 1_800_000_000_000)
        let after = AgentSession(json: ["id": 1, "x-ratelimit-reset-after": "90"], now: now)
        XCTAssertEqual(after.rateLimitResetAfterMs, 90_000)
        XCTAssertEqual(after.rateLimitResetAtMs, 1_800_000_090_000)
        XCTAssertEqual(AgentSession(json: ["id": 1, "retryAfterMs": 1500]).rateLimitResetAfterMs, 1500)
        XCTAssertEqual(AgentSession(json: ["id": 1, "reason": " quota "]).statusReason, "quota")
    }

    func testAgentTranscriptDefaults() {
        let transcript = AgentTranscript(json: ["status": "done", "transcript": "log"])
        XCTAssertEqual(transcript.status, "done")
        XCTAssertEqual(transcript.transcript, "log")
        XCTAssertFalse(transcript.sealedForOtherDevice)
        XCTAssertEqual(AgentSessionPage().sealedForOthers, 0)
    }

    // MARK: - org model cases (via ModelsTest.kt)

    func testOrgModelsParseFromJson() {
        let summary = OrgSummary(json: ["name": "acme", "role": "OWNER"])
        XCTAssertEqual(summary.name, "acme")
        XCTAssertEqual(summary.role, "owner")
        XCTAssertTrue(summary.canManage)

        let profile = OrgProfile(json: [
            "org": "acme", "displayName": "Acme Inc", "description": "We make things",
            "createdAt": 1_700_000_000_000, "members": 3, "teams": 2, "viewerRole": "admin",
            "repos": [["repo": "widget", "node": "alice"], ["repo": "", "node": "ignored"]],
        ])
        XCTAssertEqual(profile.title, "Acme Inc")
        XCTAssertTrue(profile.canManage)
        XCTAssertFalse(profile.isOwner)
        XCTAssertEqual(profile.repos.count, 1)
        XCTAssertEqual(profile.repos.first?.repo, "widget")
        XCTAssertEqual(profile.repos.first?.node, "alice")
        XCTAssertEqual(profile.createdMs, 1_700_000_000_000)
        XCTAssertEqual(profile.members, 3)
        XCTAssertEqual(OrgProfile(name: "x").title, "x")

        let team = OrgTeam(json: ["team": "Core", "permission": "Write", "members": 4])
        XCTAssertEqual(team.team, "core")
        XCTAssertEqual(team.permission, "write")
        XCTAssertEqual(team.members, 4)

        // node_owner is the wire name used inside the org profile repo list.
        let repo = OrgRepo(json: ["repo": "API", "node_owner": "Bob"])
        XCTAssertEqual(repo.repo, "api")
        XCTAssertEqual(repo.node, "Bob")

        let member = OrgMember(json: ["name": " Alice ", "role": "Member", "since": "7"])
        XCTAssertEqual(member.name, "alice")
        XCTAssertEqual(member.role, "member")
        XCTAssertEqual(member.sinceMs, 7)
        XCTAssertEqual(orgRoles, ["owner", "admin", "maintainer", "member", "auditor", "guest"])
        XCTAssertEqual(orgTeamPermissions, ["read", "write", "maintain", "admin"])
    }

    func testOrgErrorMessageMapsKnownCodesAndFallsBackForUnknown() {
        XCTAssertEqual(orgErrorMessage("org_name_taken"), "That name is already taken.")
        XCTAssertEqual(orgErrorMessage("last_owner"), "An organization must keep at least one owner.")
        XCTAssertEqual(orgErrorMessage("mystery"), "Request failed (mystery).")
        XCTAssertEqual(orgErrorMessage(""), "Request failed.")
        let error = OrgApiException("forbidden", orgErrorMessage("forbidden"))
        XCTAssertEqual(error.description, "You do not have permission to do that.")
        XCTAssertEqual(error.code, "forbidden")
    }

    // MARK: - Labels

    func testReleaseLabels() {
        let sha = "1938916325d5839850fbc39db05a3a1f836b10615ac4467725b9f49e864884fb"
        let release = RepoRelease(
            json: [
                "tag": "v1.2.3", "channel": "latest", "tag_commit": "abcdef1234567890", "created_at": 1_700_000_000_000,
                "assets": [
                    [
                        "name": "forkmesh-linux-x86_64", "blob_sha256": .string(sha), "size": 14_949_672, "os": "linux",
                        "arch": "x86_64",
                    ],
                    ["name": "forkmesh-darwin-arm64", "sha256": "b2", "size": "2048", "os": "darwin"],
                    ["size": 5],
                ],
            ],
            downloads: [sha: 3, "b2": 1])
        XCTAssertEqual(release.shortCommit, "abcdef12")
        XCTAssertEqual(release.assetSummary, "2 artifacts")
        let first = release.assets[0]
        XCTAssertEqual(first.sizeLabel, "14.3 MB")
        XCTAssertEqual(first.downloads, 3)
        XCTAssertEqual(first.downloadLabel, "3 downloads")
        XCTAssertEqual(first.shortSha, "1938916325d5")
        XCTAssertEqual(first.platformLabel, "linux / x86_64")
        let second = release.assets[1]
        XCTAssertEqual(second.sizeLabel, "2.0 KB")
        XCTAssertEqual(second.downloadLabel, "1 download")
        XCTAssertEqual(second.platformLabel, "darwin")
        XCTAssertEqual(RepoReleaseAsset(name: "a", sha256: "", size: 512).sizeLabel, "512 B")
        XCTAssertEqual(RepoReleaseAsset(name: "a", sha256: "").sizeLabel, "unknown size")
        XCTAssertEqual(RepoReleaseAsset(name: "a", sha256: "", os: " ").platformLabel, "artifact")
        XCTAssertEqual(RepoRelease(tag: "v0").assetSummary, "No published artifacts yet")
        let utc = TimeZone(identifier: "UTC")!
        XCTAssertEqual(release.createdDate(timeZone: utc), "2023-11-14")
        XCTAssertEqual(release.createdDate(timeZone: TimeZone(identifier: "Pacific/Kiritimati")!), "2023-11-15")
        XCTAssertEqual(RepoRelease(tag: "v0").createdDate, "")
    }

    func testPullEventDisplayTitles() {
        XCTAssertEqual(PullEvent(type: "review", state: "approve").displayTitle, "Approved")
        XCTAssertEqual(PullEvent(type: "review", state: "changes_requested").displayTitle, "Requested changes")
        XCTAssertEqual(PullEvent(type: "review", state: "comment").displayTitle, "Review comment")
        XCTAssertEqual(PullEvent(type: "review", state: "other").displayTitle, "Review")
        XCTAssertEqual(PullEvent(type: "line-comment").displayTitle, "Line comment")
        XCTAssertEqual(PullEvent(type: "thread-reply").displayTitle, "Thread reply")
        XCTAssertEqual(PullEvent(type: "suggestion-state").displayTitle, "Suggestion state")
        XCTAssertEqual(PullEvent(type: "comment").displayTitle, "Comment")
        XCTAssertEqual(PullEvent(type: "comment", author: "key", authorName: "Kai").displayAuthor, "Kai")
        XCTAssertEqual(PullEvent(type: "comment", author: "key").displayAuthor, "key")

        let pull = PullRequest(json: ["number": 7, "title": "t", "baseBranch": "main", "head": "feature", "submittedAt": 5])
        XCTAssertEqual(pull.baseBranch, "main")
        XCTAssertEqual(pull.headBranch, "feature")
        XCTAssertEqual(pull.createdMs, 5)
        XCTAssertEqual(pull.status, "open")
        XCTAssertEqual(PullRequest(number: 1, title: "t").createdDate, "")
        XCTAssertEqual(
            PullRequest(number: 1, title: "t", createdMs: 1_700_000_000_000).createdDate(timeZone: TimeZone(identifier: "UTC")!),
            "2023-11-14")
    }

    func testActionWorkflowRunAndLogParseWorkerPayloads() {
        let actions = RepoActions(json: [
            "workflows": [[
                "path": ".forkmesh/test.yml", "name": "Test suite", "on": ["push", "workflow_dispatch"],
                "stepCount": "2", "manual": true, "valid": true,
            ]],
            "runs": [[
                "id": "7", "workflowPath": ".forkmesh/test.yml", "workflowName": "Test suite",
                "commit": "abcdef123456", "ref": "refs/heads/main", "status": "running",
                "createdAtMs": "1700000000000", "startedAtMs": 1_700_000_001_000, "finishedAtMs": 0,
            ]],
        ])
        XCTAssertEqual(actions.workflows.first?.name, "Test suite")
        XCTAssertEqual(actions.workflows.first?.triggers, ["push", "workflow_dispatch"])
        XCTAssertEqual(actions.workflows.first?.stepCount, 2)
        XCTAssertEqual(actions.workflows.first?.manual, true)
        XCTAssertEqual(actions.runs.first?.id, 7)
        XCTAssertEqual(actions.runs.first?.workflowName, "Test suite")
        XCTAssertEqual(actions.runs.first?.statusLabel, "Running")
        XCTAssertEqual(actions.runs.first?.isActive, true)
        XCTAssertEqual(actions.runs.first?.refLabel, "main")
        XCTAssertEqual(actions.runs.first?.shortCommit, "abcdef12")

        let log = ActionLog(json: ["id": "7", "status": "success", "log": "tests passed"])
        XCTAssertEqual(log.id, 7)
        XCTAssertEqual(log.statusLabel, "Success")
        XCTAssertEqual(log.log, "tests passed")
    }

    func testActionRunLabels() {
        let run = ActionRun(
            id: 7, commit: "abcdef1234567890", ref: "refs/heads/main", status: "awaiting_approval",
            startedAtMs: 1_000, finishedAtMs: 126_000)
        XCTAssertEqual(run.statusLabel, "Awaiting approval")
        XCTAssertTrue(run.isActive)
        XCTAssertEqual(run.refLabel, "main")
        XCTAssertEqual(run.shortCommit, "abcdef12")
        XCTAssertEqual(run.durationLabel, "2m 5s")
        XCTAssertEqual(run.displayName, "Action run #7")
        XCTAssertEqual(ActionRun(id: 1, status: "failure").statusLabel, "Failed")
        XCTAssertEqual(ActionRun(id: 1, status: "Canceled").statusLabel, "Cancelled")
        XCTAssertEqual(ActionRun(id: 1, status: "weird_state").statusLabel, "Weird State")
        XCTAssertEqual(ActionRun(id: 1).statusLabel, "Unknown")
        XCTAssertEqual(ActionRun(id: 1, ref: "refs/tags/v1").refLabel, "v1")
        XCTAssertEqual(ActionRun(id: 1, startedAtMs: 1_000, finishedAtMs: 10_999).durationLabel, "9s")
        XCTAssertEqual(ActionRun(id: 1, startedAtMs: 5, finishedAtMs: 5).durationLabel, "")
        XCTAssertEqual(ActionLog(json: ["id": "7", "status": "passed"]).statusLabel, "Success")

        let workflow = ActionWorkflow(json: ["path": ".forkmesh/ci.yml", "on": "push", "steps": "3", "valid": nil])
        XCTAssertEqual(workflow.triggers, ["push"])
        XCTAssertEqual(workflow.triggerLabel, "push")
        XCTAssertEqual(workflow.stepCount, 3)
        XCTAssertTrue(workflow.valid)
        XCTAssertEqual(workflow.displayName, ".forkmesh/ci.yml")
        XCTAssertFalse(ActionWorkflow(json: ["valid": false]).valid)
        XCTAssertEqual(ActionWorkflow().triggerLabel, "No triggers")
        XCTAssertEqual(ActionWorkflow(json: ["triggers": ["a", "b"]]).triggers, ["a", "b"])

        let actions = RepoActions(json: ["workflows": [["name": "CI"]], "runs": "nope"])
        XCTAssertEqual(actions.workflows.first?.name, "CI")
        XCTAssertEqual(actions.runs.count, 0)
        let command = DesktopCommandResult(json: ["ok": true, "queued": "2"])
        XCTAssertTrue(command.ok)
        XCTAssertEqual(command.queued, 2)
    }

    func testRepoTreeEntryEpochAndDateParsing() {
        func updated(_ json: JSONObject) -> Int { RepoTreeEntry(json: json).updatedMs }
        XCTAssertEqual(updated(["path": "a", "date": 1_700_000_000]), 1_700_000_000_000)
        XCTAssertEqual(updated(["path": "a", "updatedAt": 1_700_000_000_000]), 1_700_000_000_000)
        XCTAssertEqual(updated(["path": "a", "mtime": " 1700000000000 "]), 1_700_000_000_000)
        XCTAssertEqual(updated(["path": "a", "date": "1700000000"]), 1_700_000_000_000)
        XCTAssertEqual(updated(["path": "a", "date": "2026-07-04T12:00:00Z"]), 1_783_166_400_000)
        XCTAssertEqual(updated(["path": "a", "date": "2026-07-04 12:30:00+02:00"]), 1_783_161_000_000)
        XCTAssertEqual(updated(["path": "a", "date": "yesterday"]), 0)
        XCTAssertNil(RepoTreeEntry(json: ["path": "a"]).updatedAt)
        XCTAssertEqual(
            RepoTreeEntry(json: ["path": "a", "date": "2026-07-04T12:00:00"], timeZone: TimeZone(identifier: "UTC")!).updatedMs,
            1_783_166_400_000, "a stamp without a zone is local time")

        let entry = RepoTreeEntry(json: [
            "path": "lib/src/main.dart", "mode": "100644", "oid": "abc", "bytes": "12",
            "commitMessage": "  Fix bug  \nbody",
        ])
        XCTAssertEqual(entry.name, "main.dart")
        XCTAssertEqual(entry.path, "lib/src/main.dart")
        XCTAssertEqual(entry.type, "file")
        XCTAssertEqual(entry.sha, "abc")
        XCTAssertEqual(entry.size, 12)
        XCTAssertEqual(entry.lastMessage, "Fix bug")
        XCTAssertTrue(RepoTreeEntry(json: ["name": "d", "kind": "Directory"]).isDirectory)
        XCTAssertTrue(RepoTreeEntry(json: ["name": "d", "type": "subtree"]).isDirectory)

        let nested = RepoTree(json: ["tree": [["name": "x", "path": "a/x"], ["name": ""]], "ts": "9"], path: "b")
        XCTAssertEqual(nested.entries.map(\.path), ["a/x"], "paths that already contain '/' are kept")
        XCTAssertEqual(nested.cachedAt, 9)
        XCTAssertEqual(nested.path, "b")
        XCTAssertEqual(RepoTree(json: ["commit": nil, "analysis": ["commit": "c1"]]).commit, "c1")
        XCTAssertEqual(RepoTree(json: ["commit": "c0", "analysis": ["commit": "c1"]]).commit, "c0")
    }

    func testRepoCommitBranchAndSearchParsing() {
        let detail = RepoCommitDetail(
            json: [
                "files": [["file": "a.txt", "additions": 3, "deletions": 1], ["adds": "1"]], "diff": "diff --git",
                "truncated": true, "servedBy": "mirror",
            ],
            fallbackCommit: ["hash": "abc"])
        XCTAssertEqual(detail.commit["hash"], .string("abc"))
        XCTAssertEqual(detail.files.count, 1)
        XCTAssertEqual(detail.files.first?.adds, "3")
        XCTAssertTrue(detail.truncated)
        XCTAssertEqual(detail.source, "mirror")
        XCTAssertEqual(RepoCommitDetail(json: [1], fallbackCommit: ["hash": "z"]).commit, ["hash": "z"])

        let branch = RepoBranch(json: ["branch": "main", "commit": "abc", "default": true, "worktree": "/w"])
        XCTAssertEqual(branch.name, "main")
        XCTAssertEqual(branch.sha, "abc")
        XCTAssertTrue(branch.isDefault)
        XCTAssertTrue(branch.hasWorktree)

        let search = RepoSearchResults(json: [
            "code": [["file": "a.kt", "lineNumber": "4", "snippet": "val x"], ["text": "no path"]],
        ])
        XCTAssertEqual(search.code.count, 1)
        XCTAssertEqual(search.code.first?.line, 4)
        XCTAssertEqual(search.code.first?.text, "val x")
        XCTAssertEqual(RepoSearchResults(json: nil).code.count, 0)

        let comparison = RepoComparison(json: [
            "baseOid": "a", "headOid": "b", "mergeBaseOid": "m", "commitCount": "3", "patch": "p", "commits": "c",
        ])
        XCTAssertEqual(comparison.commitCount, 3)
        XCTAssertEqual(comparison.mergeBaseOid, "m")
        XCTAssertEqual(RepoComparison(json: "nope"), RepoComparison())
    }

    // ModelsTest.kt
    func testFederatedReplyPreservesLifecycleAndFailsClosedAsNonNative() {
        let reply = FederatedReply(json: [
            "remoteId": "https://lemmy.example/comment/7", "parentRemoteId": "https://lemmy.example/comment/6",
            "author": "alice@lemmy.example", "body": "remote body", "url": "https://lemmy.example/comment/7",
            "sourceInstance": "lemmy.example", "sourceSoftware": "lemmy", "lifecycle": "edited", "depth": 2,
            // A compromised or old server response cannot promote this projection.
            "nativeEvent": true,
        ])
        XCTAssertEqual(reply.remoteId, "https://lemmy.example/comment/7")
        XCTAssertEqual(reply.parentRemoteId, "https://lemmy.example/comment/6")
        XCTAssertTrue(reply.edited)
        XCTAssertEqual(reply.depth, 2)
        XCTAssertFalse(reply.nativeEvent)
    }

    func testTombstonesAndModerationNeverExposeAProjectedBody() {
        let tombstone = FederatedReply(json: ["remoteId": "https://lemmy.example/comment/8", "body": "", "lifecycle": "tombstoned"])
        let moderated = FederatedReply(json: ["remoteId": "https://lemmy.example/comment/9", "body": "", "lifecycle": "moderated"])
        XCTAssertEqual(tombstone.displayBody, "Deleted on the remote instance")
        XCTAssertEqual(moderated.displayBody, "Hidden by remote moderation")
    }

    func testFederatedReplyLifecycleAndProvenance() {
        let tombstone = FederatedReply(json: ["id": "r1", "tombstone": true, "depth": 12, "ts": "5", "body": "gone"])
        XCTAssertEqual(tombstone.remoteId, "r1")
        XCTAssertEqual(tombstone.lifecycle, "tombstoned")
        XCTAssertTrue(tombstone.tombstone)
        XCTAssertEqual(tombstone.displayBody, "Deleted on the remote instance")
        XCTAssertEqual(tombstone.depth, 8)
        XCTAssertEqual(tombstone.timestamp, 5)
        XCTAssertEqual(tombstone.backlink, "")
        XCTAssertEqual(tombstone.sourceSoftware, "activitypub")

        let reply = FederatedReply(json: [
            "remoteId": "https://social.example/notes/1", "body": "hello", "depth": -3, "moderated": true,
            "edited": true, "nativeEvent": true,
            "provenance": ["instance": "social.example", "software": "mastodon"],
        ])
        XCTAssertEqual(reply.backlink, "https://social.example/notes/1")
        XCTAssertEqual(reply.sourceInstance, "social.example")
        XCTAssertEqual(reply.sourceSoftware, "mastodon")
        XCTAssertEqual(reply.lifecycle, "moderated")
        XCTAssertEqual(reply.displayBody, "Hidden by remote moderation")
        XCTAssertEqual(reply.depth, 0)
        XCTAssertFalse(reply.nativeEvent, "a remote projection is never native")
        XCTAssertEqual(FederatedReply(json: ["lifecycle": "edited", "body": "hello"]).displayBody, "hello")
        XCTAssertTrue(FederatedReply(json: ["lifecycle": "awaiting-redelivery"]).moderated)
    }

    func testChatMemberGroup() {
        let group = MemberGroup(
            name: "alice",
            members: [
                Member(id: "k1", name: "alice"),
                Member(id: "k2", name: "alice", online: true, userStatus: UserStatus(away: true)),
            ])
        XCTAssertTrue(group.online)
        XCTAssertEqual(group.id, "k1")
        XCTAssertEqual(group.nodeCount, 2)
        XCTAssertEqual(group.userStatus, UserStatus(away: true), "the first node that announced anything speaks")
        XCTAssertEqual(MemberGroup(name: "b", members: [Member(id: "x", name: "b")]).userStatus, .empty)
    }
}
