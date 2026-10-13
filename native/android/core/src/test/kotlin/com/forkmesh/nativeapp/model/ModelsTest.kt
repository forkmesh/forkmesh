package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.json.jsonOf
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.time.Instant

class ModelsTest {
    fun testRepositoryReadsWorkerCatalogFieldsWithDefaults() {
        val repo = Repository.fromJson(
            jsonOf(
                "owner" to "alice",
                "name" to "project",
                "stars" to "12",
                "mirrors" to 3L,
                "liveHost" to true,
                "updatedAt" to 1700000000000L,
            ),
        )
        assertEq("alice/project", repo.fullName)
        assertEq(12, repo.stars)
        assertEq(3, repo.mirrors)
        assertEq("main", repo.defaultBranch)
        expect(!repo.isPrivate)
        expect(repo.liveHost)
        assertEq(1700000000000L, repo.updatedMs)
        assertEq(1, Repository.fromJson(jsonOf("owner" to "a", "name" to "b")).mirrors)
    }

    fun testIssueParsesNestedEventsAndToleratesMissingFields() {
        val issue = Issue.fromJson(
            jsonOf(
                "number" to "7",
                "title" to "Crash on open",
                "events" to listOf(jsonOf("type" to "comment", "body" to "me too"), "not-an-event"),
                "labels" to listOf("bug", 42L),
                "milestone" to "v2 mobile",
                "priority" to "4",
                "assignees" to listOf("mona", "kai"),
            ),
        )
        assertEq(7, issue.number)
        expect(issue.isOpen)
        assertEq(1, issue.events.size)
        assertEq("me too", issue.events.single().body)
        assertEq(listOf("bug", "42"), issue.labels)
        assertEq("v2 mobile", issue.milestone)
        assertEq(4, issue.priority)
        assertEq(listOf("mona", "kai"), issue.assignees)
        assertEq(100, Issue.fromJson(jsonOf("progress" to 250L)).progress)
        assertFails<ClassCastException> { Issue.fromJson(jsonOf("labels" to "bug")) }
    }

    fun testRepoTreeAcceptsListAndWrappedMapPayloads() {
        val bare = RepoTree.fromJson(
            listOf(
                jsonOf("name" to "src", "type" to "tree"),
                jsonOf("name" to "README.md", "type" to "blob", "size" to 120L),
            ),
        )
        assertEq(2, bare.entries.size)

        val wrapped = RepoTree.fromJson(
            jsonOf(
                "entries" to listOf(
                    jsonOf("name" to "zeta.txt", "type" to "blob"),
                    jsonOf("name" to "alpha", "type" to "tree"),
                    jsonOf("name" to "beta.md", "type" to "blob"),
                ),
                "servedBy" to "mirror-node",
                "path" to "docs",
            ),
        )
        assertEq("mirror-node", wrapped.source)
        assertEq("docs", wrapped.path)
        assertEq(listOf("alpha", "beta.md", "zeta.txt"), wrapped.entries.map { it.name })
        assertEq("docs/zeta.txt", wrapped.entries.last().path)
    }

    fun testRepoBlobAcceptsRawStringsAndWrappedMaps() {
        val raw = RepoBlob.fromJson("hello world", path = "README.md")
        assertEq("hello world", raw.content)
        assertEq("README.md", raw.path)
        assertEq(11L, raw.size)

        val wrapped = RepoBlob.fromJson(
            jsonOf("content" to "line1\n", "size" to "6", "servedBy" to "mirror-node"),
            path = "a.txt",
        )
        assertEq("line1\n", wrapped.content)
        assertEq(6L, wrapped.size)
        assertEq("mirror-node", wrapped.source)
        assertEq("a.txt", wrapped.path)
        assertEq("", RepoBlob.fromJson(42L, path = "x").content)
    }

    fun testRepoMirrorMapsTheMirrorListKeyAliases() {
        val mirror = RepoMirror.fromJson(
            jsonOf("node" to "backup-node", "repo" to "project", "status" to "online", "lastSeen" to "1700000000000"),
        )
        assertEq("backup-node/project", mirror.label)
        expect(mirror.online)
        assertEq(1700000000000L, mirror.lastSeenMs)

        val offline = RepoMirror.fromJson(jsonOf("name" to "solo-node"))
        expect(!offline.online)
        assertEq("solo-node/solo-node", offline.label)
    }

    fun testNetworkStatsReadsBothStatsEndpointShapes() {
        val modern = NetworkStats.fromJson(
            jsonOf(
                "onlineNodes" to listOf("mainnode-a", "mainnode-b", "mainnode-c", "mainnode-d"),
                "hostsOnline" to 2L,
                "repos" to 9L,
            ),
        )
        assertEq(4, modern.nodesOnline)
        assertEq(2, modern.hostsOnline)
        assertEq(9, modern.repos)

        val worker = NetworkStats.fromJson(
            jsonOf("clients" to 19L, "hosts" to 2L, "repos" to 9L, "onlineNodes" to listOf("mainnode-a", "mainnode-b")),
        )
        assertEq(2, worker.nodesOnline)

        val legacy = NetworkStats.fromJson(jsonOf("clients" to "3", "hosts" to 1L, "repositories" to 5L))
        assertEq(3, legacy.nodesOnline)
        assertEq(1, legacy.hostsOnline)
        assertEq(5, legacy.repos)
    }

    fun testNetworkStatsReadsPayoutReadinessNodes() {
        val stats = NetworkStats.fromJson(
            jsonOf(
                "nodesOnline" to 2L,
                "payoutNodes" to listOf(
                    jsonOf(
                        "name" to "mainnode-a",
                        "wallet" to "Wallet11111111111111111111111111111111",
                        "balanceLamports" to "1250000000",
                        "balanceSol" to "1.25",
                        "online" to true,
                        "payoutEligible" to true,
                        "eligibilityReason" to "eligible",
                        "relay" to "us-east",
                    ),
                    jsonOf("name" to "mainnode-b", "online" to false, "eligibilityReason" to "missing wallet"),
                ),
            ),
        )
        assertEq(2, stats.payoutNodes.size)
        val first = stats.payoutNodes.first()
        assertEq("mainnode-a", first.name)
        assertEq("Wallet11111111111111111111111111111111", first.wallet)
        assertEq("Wallet...111111", first.shortWallet)
        assertEq(1250000000L, first.balanceLamports)
        assertEq(1.25, first.balanceSol)
        expect(first.online)
        expect(first.payoutEligible)
        assertEq("eligible", first.eligibilityLabel)
        assertEq("1.25 SOL (1250000000 lamports)", first.balanceLabel)
        assertEq("us-east", first.relay)
        val last = stats.payoutNodes.last()
        assertEq("mainnode-b", last.name)
        assertEq("", last.wallet)
        assertEq(0L, last.balanceLamports)
        assertEq(0.0, last.balanceSol)
        expect(!last.payoutEligible)
        assertEq("missing wallet", last.eligibilityLabel)
        assertEq("not eligible", PayoutNode(payoutEligible = false).eligibilityLabel)
        expect(PayoutNode.fromJson(jsonOf("online" to "YES")).online, "string booleans are accepted")
    }

    fun testNetworkLeaderboardsParsesFundsReceivedBoards() {
        val boards = NetworkLeaderboards.fromJson(
            jsonOf(
                "fundsMainnodes" to listOf(jsonOf("name" to "mainnode-a", "lamports" to "1250000000", "sol" to "1.25")),
                "fundsContributors" to listOf(jsonOf("name" to "alice", "lamports" to 500000000L, "sol" to 0.5)),
                "fundsProjects" to listOf(jsonOf("name" to "forkmesh/mobile", "lamports" to "42")),
            ),
        )
        assertEq("mainnode-a", boards.fundsMainnodes.single().name)
        assertEq(1250000000L, boards.fundsMainnodes.single().lamports)
        assertEq(1.25, boards.fundsMainnodes.single().sol)
        assertEq("1.25 SOL (1250000000 lamports)", boards.fundsMainnodes.single().amountLabel)
        assertEq("0.5 SOL (500000000 lamports)", boards.fundsContributors.single().amountLabel)
        assertEq("42 lamports", boards.fundsProjects.single().amountLabel)
        expect(!boards.isEmpty)
        expect(NetworkLeaderboards.fromJson(emptyMap()).isEmpty)
    }

    fun testForkNotificationParsesWorkerNotificationPayloads() {
        val notification = ForkNotification.fromJson(
            jsonOf(
                "id" to "dedupe-1",
                "kind" to "pull_submitted",
                "title" to "Pull request submitted for mona/forkmesh",
                "body" to "Mobile review flow is waiting in your desktop inbox.",
                "repo" to "mona/forkmesh",
                "href" to "/mona/forkmesh/pulls/7",
                "actor" to "kai",
                "source" to "pull",
                "ts" to "1770000000000",
                "readAt" to 0L,
                "meta" to jsonOf("number" to 7L),
            ),
        )
        assertEq("dedupe-1", notification.id)
        assertEq("pull_submitted", notification.kind)
        assertEq("Pull request", notification.kindLabel)
        assertEq("Pull request submitted for mona/forkmesh", notification.title)
        assertEq("mona/forkmesh", notification.repo)
        assertEq("kai", notification.actor)
        assertEq(1770000000000L, notification.ts)
        expect(notification.isUnread)
        assertEq(7L, notification.meta["number"])
        assertEq("Repo", notification.filterGroup)

        val defaults = ForkNotification.fromJson(jsonOf("read_at" to 5L))
        assertEq("pending_inbox", defaults.kind)
        assertEq("Notification", defaults.title)
        assertEq("Inbox", defaults.kindLabel)
        assertEq(5L, defaults.readAt)
        expect(!defaults.isUnread)
        val read = notification.copyWith(readAt = 99L)
        assertEq(99L, read.readAt)
        assertEq(0L, notification.copyWith().readAt)
        assertEq("System", ForkNotification(id = "x", kind = "host_online", title = "t").filterGroup)
        assertEq("Mentions", ForkNotification(id = "x", kind = "mention", title = "t").filterGroup)
    }

    fun testNotificationPageReadsListAndUnreadCount() {
        val page = NotificationPage.fromJson(
            jsonOf(
                "ok" to true,
                "unread" to "2",
                "notifications" to listOf(
                    jsonOf("id" to "n1", "kind" to "mention", "title" to "Mention", "ts" to 2L),
                    jsonOf("id" to "n2", "kind" to "host_offline", "title" to "Host offline"),
                ),
            ),
        )
        assertEq(2, page.unread)
        assertEq(2, page.notifications.size)
        assertEq("Mention", page.notifications.first().kindLabel)
        assertEq("Host", page.notifications.last().kindLabel)
        assertEq(2, page.total)
        assertEq(1, NotificationPage.fromJson(jsonOf("items" to listOf(jsonOf("id" to "i")))).notifications.size)

        val capped = NotificationPage.fromJson(
            jsonOf(
                "unread" to 1,
                "total" to "137",
                "notifications" to listOf(jsonOf("id" to "n1", "kind" to "mention", "title" to "Mention")),
            ),
        )
        assertEq(1, capped.notifications.size)
        assertEq(137, capped.total)
    }

    fun testBountyWalletParsesWalletFieldsAndBuildsLabelsAndLinks() {
        val wallet = BountyWallet.fromJson(
            jsonOf(
                "address" to "OwnerBountyWallet111111111111111111111111111",
                "balanceLamports" to "1234567890",
                "balanceSol" to "1.23456789",
                "uri" to "solana:OwnerBountyWallet111111111111111111111111111",
            ),
        )
        expect(wallet.hasWallet)
        assertEq("OwnerBountyWallet111111111111111111111111111", wallet.address)
        assertEq(1234567890L, wallet.balanceLamports)
        assertEq(1.23456789, wallet.balanceSol)
        assertEq("solana:OwnerBountyWallet111111111111111111111111111", wallet.payUri)
        assertEq("1.23456789 SOL (1234567890 lamports)", wallet.balanceLabel)
        assertEq("OwnerB...111111", wallet.shortAddress)
        assertEq(
            "https://explorer.solana.com/address/OwnerBountyWallet111111111111111111111111111",
            wallet.explorerAddressUrl,
        )
        assertEq("https://explorer.solana.com/tx/payout-signature", solanaExplorerSignatureUrl("payout-signature"))
        assertEq("", solanaExplorerAddressUrl("  "))
        assertEq("https://explorer.solana.com/tx/a%20b%2Fc", solanaExplorerSignatureUrl(" a b/c "))
        assertEq(BountyWallet(), BountyWallet.fromJson("not a map"))
    }

    fun testIssueParsesPublicBountyFundingFields() {
        val issue = Issue.fromJson(
            jsonOf(
                "number" to 8L,
                "title" to "Pay a maintainer",
                "bountyUsd" to 75L,
                "bountyAddress" to "escrow111",
                "bountyStatus" to "funded",
                "bountyRequiredLamports" to 9000L,
                "bountyReceivedLamports" to 7000L,
                "bountyAmountSol" to 0.000009,
                "bountyPayUri" to "solana:escrow111?amount=0.000009",
                "bountyPayee" to "maintainer-node",
                "bountyPayoutSig" to "payout-sig",
            ),
        )
        assertEq(75.0, issue.bountyUsd)
        assertEq("escrow111", issue.bountyAddress)
        assertEq("funded", issue.bountyStatus)
        assertEq(9000L, issue.bountyRequiredLamports)
        assertEq(7000L, issue.bountyReceivedLamports)
        assertEq(0.000009, issue.bountyAmountSol)
        assertEq("solana:escrow111?amount=0.000009", issue.bountyPayUri)
        assertEq("maintainer-node", issue.bountyPayee)
        assertEq("payout-sig", issue.bountyPayoutSig)
        assertEq("7000 / 9000 lamports", issue.bountyProgressLabel)
        expect(issue.hasBountyFunding)
    }

    fun testIssueBountyReadsNestedRecordsAndDerivesStatus() {
        val nested = Issue.fromJson(
            jsonOf(
                "number" to 3L,
                "title" to "Nested",
                "bounty" to jsonOf("funding" to jsonOf("address" to "x")),
            ),
        )
        assertEq("x", nested.bountyAddress, "a bounty record may wrap its own funding object")
        val funding = IssueBounty.fromJson(jsonOf("funding" to jsonOf("bountyAddress" to "f1", "bountyStatus" to " ")))
        assertEq("f1", funding.address)
        assertEq("open", funding.status)
        assertEq("paid", IssueBounty(status = "", payoutSig = "sig").statusLabel)
        assertEq("funded", IssueBounty(status = "", requiredLamports = 5, receivedLamports = 5).statusLabel)
        assertEq("3 lamports received", IssueBounty(receivedLamports = 3).progressLabel)
        val updated = Issue(number = 1, title = "t", bountyUsd = 10.0).copyWithBounty(
            IssueBounty(address = "a", status = "", payoutSig = "p"),
        )
        assertEq(10.0, updated.bountyUsd)
        assertEq("paid", updated.bountyStatus)
        assertEq("a", updated.bountyAddress)
    }

    fun testAgentSessionParsesWorkerAgentSessionPayloads() {
        val session = AgentSession.fromJson(
            jsonOf(
                "id" to "42",
                "issueNumber" to "12",
                "issueTitle" to "Stabilize signed inbox",
                "status" to "running",
                "provider" to "openai",
                "model" to "gpt-5.5",
                "branchName" to "agent/fix-inbox",
                "lastError" to "",
                "createdAtMs" to "1700000000000",
                "startedAtMs" to 1700000001000L,
                "finishedAtMs" to 0L,
                "numTurns" to "7",
                "durationMs" to 125000L,
                "costUsd" to "1.25",
                "createPr" to true,
                "prNumber" to "7",
                "baseRef" to "abc123",
                "baseBranch" to "main",
                "merged" to true,
                "mergedAtMs" to "1700000100000",
                "promptTokens" to "1000",
                "completionTokens" to 500L,
                "totalTokens" to "1500",
                "contextTokens" to "12000",
                "contextWindow" to "32000",
                "maxOutputTokens" to "4000",
                "estimatedCredits" to "6",
                "spendBeforeUsd" to "2.00",
                "spendAfterUsd" to "3.25",
                "diffStats" to jsonOf("files" to "3", "ahead" to "2", "behind" to "1", "conflicted" to true),
            ),
        )
        assertEq(42, session.id)
        assertEq(12, session.issueNumber)
        assertEq("Stabilize signed inbox", session.issueTitle)
        assertEq("Merged", session.statusLabel)
        assertEq("OpenAI", session.providerLabel)
        assertEq("Stabilize signed inbox", session.displayTitle)
        assertEq("agent/fix-inbox", session.branchName)
        assertEq(7, session.numTurns)
        assertEq("2m 5s", session.durationLabel)
        assertEq("$1.25", session.costLabel)
        expect(!session.isActive)
        expect(session.createPr)
        assertEq(7, session.prNumber)
        assertEq("PR #7", session.prLabel)
        assertEq("abc123", session.baseRef)
        assertEq("main", session.baseBranch)
        expect(session.merged)
        assertEq(1700000100000L, session.mergedAtMs)
        assertEq(1000, session.promptTokens)
        assertEq(500, session.completionTokens)
        assertEq(1500, session.totalTokens)
        assertEq(12000, session.contextTokens)
        assertEq(32000, session.contextWindow)
        assertEq(4000, session.maxOutputTokens)
        assertEq(6, session.estimatedCredits)
        assertEq(2.0, session.spendBeforeUsd)
        assertEq(3.25, session.spendAfterUsd)
        assertEq(3, session.filesChanged)
        assertEq(2, session.ahead)
        assertEq(1, session.behind)
        expect(session.conflicted)
        assertEq("3 files · ↑2 ↓1", session.diffLabel)
        assertEq("Done · 7 turns · 2m 5s · $1.25 · 3 files · ↑2 ↓1", session.latestStatusSummary())
    }

    fun testAgentSessionLabelsFollowDartFormatting() {
        assertEq("<$0.01", AgentSession(id = 1, costUsd = 0.004).costLabel)
        assertEq("", AgentSession(id = 1).costLabel)
        assertEq("$0.13", AgentSession(id = 1, costUsd = 0.125).costLabel)
        assertEq("45s", AgentSession(id = 1, durationMs = 45_900).durationLabel)
        assertEq("Agent session #9", AgentSession(id = 9).displayTitle)
        assertEq("Issue #4", AgentSession(id = 9, issueNumber = 4).displayTitle)
        assertEq("Waiting", AgentSession(id = 1, status = "waiting_for_input").statusLabel)
        assertEq("Needs Review", AgentSession(id = 1, status = "needs_review").statusLabel)
        assertEq("Unknown", AgentSession(id = 1).statusLabel)
        assertEq("Cloudflare AI", AgentSession(id = 1, provider = "Cloudflare-AI").providerLabel)
        assertEq("Local Llama", AgentSession(id = 1, provider = "local_llama").providerLabel)
        assertEq("Agent", AgentSession(id = 1).providerLabel)
        assertEq("-1", AgentSession.fromJson(jsonOf("id" to 1L)).filesChanged.toString())
        assertEq(50, AgentSession.fromJson(jsonOf("id" to 1L)).priority)
        assertEq(7, AgentSession.fromJson(jsonOf("id" to 1L, "priority" to 7L)).priority)
        assertEq("1 file", AgentSession(id = 1, filesChanged = 1).diffLabel)
        assertEq("", AgentSession(id = 1, ahead = 0, behind = 0).diffLabel)

        val failed = AgentSession(id = 1, status = "error", lastError = "x".repeat(200))
        assertEq("Failed: " + "x".repeat(117) + "...", failed.latestStatusSummary())
        assertEq("...", failed.statusFailureReason(3))
        assertEq("Failed", AgentSession(id = 1, status = "failed").latestStatusSummary())
        assertEq("Running", AgentSession(id = 1, status = "running").latestStatusSummary())

        val now = 1_800_000_000_000L
        val limited = AgentSession(id = 1, rateLimitResetAtMs = now + 3_725_000)
        assertEq("Limit resets in 1h 2m", limited.rateLimitCountdownLabel(nowMs = now))
        assertEq("Limit resets in 2d 0h", AgentSession(id = 1, rateLimitResetAtMs = now + 172_800_000).rateLimitCountdownLabel(now))
        assertEq("Limit resets in 10d 0h", AgentSession(id = 1, rateLimitResetAtMs = now + 30L * 86_400_000).rateLimitCountdownLabel(now))
        assertEq("", AgentSession(id = 1, rateLimitResetAtMs = now - 1).rateLimitCountdownLabel(now))

        val seconds = AgentSession.fromJson(jsonOf("id" to 1L, "rateLimitResetAt" to 1_800_000_000L))
        assertEq(1_800_000_000_000L, seconds.rateLimitResetAtMs)
        val before = System.currentTimeMillis()
        val after = AgentSession.fromJson(jsonOf("id" to 1L, "x-ratelimit-reset-after" to "90"))
        assertEq(90_000L, after.rateLimitResetAfterMs)
        expect(after.rateLimitResetAtMs >= before + 90_000 && after.rateLimitResetAtMs <= System.currentTimeMillis() + 90_000)
        assertEq(1500L, AgentSession.fromJson(jsonOf("id" to 1L, "retryAfterMs" to 1500L)).rateLimitResetAfterMs)
        assertEq("quota", AgentSession.fromJson(jsonOf("id" to 1L, "reason" to " quota ")).statusReason)
    }

    fun testAgentTranscriptDefaults() {
        val transcript = AgentTranscript.fromJson(jsonOf("status" to "done", "transcript" to "log"))
        assertEq("done", transcript.status)
        assertEq("log", transcript.transcript)
        expect(!transcript.sealedForOtherDevice)
        assertEq(0, AgentSessionPage().sealedForOthers)
    }

    fun testOrgModelsParseFromJson() {
        val summary = OrgSummary.fromJson(jsonOf("name" to "acme", "role" to "OWNER"))
        assertEq("acme", summary.name)
        assertEq("owner", summary.role)
        expect(summary.canManage)

        val profile = OrgProfile.fromJson(
            jsonOf(
                "org" to "acme",
                "displayName" to "Acme Inc",
                "description" to "We make things",
                "createdAt" to 1700000000000L,
                "members" to 3L,
                "teams" to 2L,
                "viewerRole" to "admin",
                "repos" to listOf(jsonOf("repo" to "widget", "node" to "alice"), jsonOf("repo" to "", "node" to "ignored")),
            ),
        )
        assertEq("Acme Inc", profile.title)
        expect(profile.canManage)
        expect(!profile.isOwner)
        assertEq("widget", profile.repos.single().repo)
        assertEq("alice", profile.repos.single().node)
        assertEq(1700000000000L, profile.createdMs)
        assertEq(3, profile.members)

        val team = OrgTeam.fromJson(jsonOf("team" to "Core", "permission" to "Write", "members" to 4L))
        assertEq("core", team.team)
        assertEq("write", team.permission)
        assertEq(4, team.members)

        val repo = OrgRepo.fromJson(jsonOf("repo" to "API", "node_owner" to "Bob"))
        assertEq("api", repo.repo)
        assertEq("Bob", repo.node)

        val member = OrgMember.fromJson(jsonOf("name" to " Alice ", "role" to "Member", "since" to "7"))
        assertEq("alice", member.name)
        assertEq("member", member.role)
        assertEq(7L, member.sinceMs)
        assertEq(listOf("owner", "admin", "maintainer", "member", "auditor", "guest"), ORG_ROLES)
        assertEq(listOf("read", "write", "maintain", "admin"), ORG_TEAM_PERMISSIONS)
    }

    fun testOrgErrorMessageMapsKnownCodesAndFallsBackForUnknown() {
        assertEq("That name is already taken.", orgErrorMessage("org_name_taken"))
        assertEq("An organization must keep at least one owner.", orgErrorMessage("last_owner"))
        assertEq("Request failed (mystery).", orgErrorMessage("mystery"))
        assertEq("Request failed.", orgErrorMessage(""))
        val error = OrgApiException("forbidden", orgErrorMessage("forbidden"))
        assertEq("You do not have permission to do that.", error.toString())
        assertEq("forbidden", error.code)
    }

    fun testReleaseLabels() {
        val sha = "1938916325d5839850fbc39db05a3a1f836b10615ac4467725b9f49e864884fb"
        val release = RepoRelease.fromJson(
            jsonOf(
                "tag" to "v1.2.3",
                "channel" to "latest",
                "tag_commit" to "abcdef1234567890",
                "created_at" to 1700000000000L,
                "assets" to listOf(
                    jsonOf("name" to "forkmesh-linux-x86_64", "blob_sha256" to sha, "size" to 14949672L, "os" to "linux", "arch" to "x86_64"),
                    jsonOf("name" to "forkmesh-darwin-arm64", "sha256" to "b2", "size" to "2048", "os" to "darwin"),
                    jsonOf("size" to 5L),
                ),
            ),
            downloads = mapOf(sha to 3, "b2" to 1),
        )
        assertEq("abcdef12", release.shortCommit)
        assertEq("2 artifacts", release.assetSummary)
        val first = release.assets.first()
        assertEq("14.3 MB", first.sizeLabel)
        assertEq(3, first.downloads)
        assertEq("3 downloads", first.downloadLabel)
        assertEq("1938916325d5", first.shortSha)
        assertEq("linux / x86_64", first.platformLabel)
        val second = release.assets.last()
        assertEq("2.0 KB", second.sizeLabel)
        assertEq("1 download", second.downloadLabel)
        assertEq("darwin", second.platformLabel)
        assertEq("512 B", RepoReleaseAsset(name = "a", sha256 = "", size = 512).sizeLabel)
        assertEq("unknown size", RepoReleaseAsset(name = "a", sha256 = "").sizeLabel)
        assertEq("artifact", RepoReleaseAsset(name = "a", sha256 = "", os = " ").platformLabel)
        assertEq("No published artifacts yet", RepoRelease(tag = "v0").assetSummary)
        assertEq(modelLocalDate(1700000000000L), release.createdDate)
        assertEq("", RepoRelease(tag = "v0").createdDate)
    }

    fun testPullEventDisplayTitles() {
        assertEq("Approved", PullEvent(type = "review", state = "approve").displayTitle)
        assertEq("Requested changes", PullEvent(type = "review", state = "changes_requested").displayTitle)
        assertEq("Review comment", PullEvent(type = "review", state = "comment").displayTitle)
        assertEq("Review", PullEvent(type = "review", state = "other").displayTitle)
        assertEq("Line comment", PullEvent(type = "line-comment").displayTitle)
        assertEq("Suggestion state", PullEvent(type = "suggestion-state").displayTitle)
        assertEq("Comment", PullEvent(type = "comment").displayTitle)
        assertEq("Kai", PullEvent(type = "comment", author = "key", authorName = "Kai").displayAuthor)
        assertEq("key", PullEvent(type = "comment", author = "key").displayAuthor)

        val pull = PullRequest.fromJson(jsonOf("number" to 7L, "title" to "t", "baseBranch" to "main", "head" to "feature", "submittedAt" to 5L))
        assertEq("main", pull.baseBranch)
        assertEq("feature", pull.headBranch)
        assertEq(5L, pull.createdMs)
        assertEq("open", pull.status)
        assertEq("", PullRequest(number = 1, title = "t").createdDate)
    }

    fun testActionRunLabels() {
        val run = ActionRun(
            id = 7,
            status = "awaiting_approval",
            ref = "refs/heads/main",
            commit = "abcdef1234567890",
            startedAtMs = 1_000,
            finishedAtMs = 126_000,
        )
        assertEq("Awaiting approval", run.statusLabel)
        expect(run.isActive)
        assertEq("main", run.refLabel)
        assertEq("abcdef12", run.shortCommit)
        assertEq("2m 5s", run.durationLabel)
        assertEq("Action run #7", run.displayName)
        assertEq("Failed", ActionRun(id = 1, status = "failure").statusLabel)
        assertEq("Cancelled", ActionRun(id = 1, status = "Canceled").statusLabel)
        assertEq("Weird State", ActionRun(id = 1, status = "weird_state").statusLabel)
        assertEq("Unknown", ActionRun(id = 1).statusLabel)
        assertEq("v1", ActionRun(id = 1, ref = "refs/tags/v1").refLabel)
        assertEq("9s", ActionRun(id = 1, startedAtMs = 1_000, finishedAtMs = 10_999).durationLabel)
        assertEq("", ActionRun(id = 1, startedAtMs = 5, finishedAtMs = 5).durationLabel)
        assertEq("Success", ActionLog.fromJson(jsonOf("id" to "7", "status" to "passed")).statusLabel)

        val workflow = ActionWorkflow.fromJson(jsonOf("path" to ".forkmesh/ci.yml", "on" to "push", "steps" to "3", "valid" to null))
        assertEq(listOf("push"), workflow.triggers)
        assertEq("push", workflow.triggerLabel)
        assertEq(3, workflow.stepCount)
        expect(workflow.valid)
        assertEq(".forkmesh/ci.yml", workflow.displayName)
        expect(!ActionWorkflow.fromJson(jsonOf("valid" to false)).valid)
        assertEq("No triggers", ActionWorkflow().triggerLabel)
        assertEq(listOf("a", "b"), ActionWorkflow.fromJson(jsonOf("triggers" to listOf("a", "b"))).triggers)

        val actions = RepoActions.fromJson(jsonOf("workflows" to listOf(jsonOf("name" to "CI")), "runs" to "nope"))
        assertEq("CI", actions.workflows.single().name)
        assertEq(0, actions.runs.size)
        val command = DesktopCommandResult.fromJson(jsonOf("ok" to true, "queued" to "2"))
        expect(command.ok)
        assertEq(2, command.queued)
    }

    fun testRepoTreeEntryEpochAndDateParsing() {
        assertEq(1700000000000L, RepoTreeEntry.fromJson(jsonOf("path" to "a", "date" to 1700000000L)).updatedMs)
        assertEq(1700000000000L, RepoTreeEntry.fromJson(jsonOf("path" to "a", "updatedAt" to 1700000000000L)).updatedMs)
        assertEq(1700000000000L, RepoTreeEntry.fromJson(jsonOf("path" to "a", "mtime" to " 1700000000000 ")).updatedMs)
        assertEq(1700000000000L, RepoTreeEntry.fromJson(jsonOf("path" to "a", "date" to "1700000000")).updatedMs)
        assertEq(
            Instant.parse("2026-07-04T12:00:00Z").toEpochMilli(),
            RepoTreeEntry.fromJson(jsonOf("path" to "a", "date" to "2026-07-04T12:00:00Z")).updatedMs,
        )
        assertEq(
            Instant.parse("2026-07-04T10:30:00Z").toEpochMilli(),
            RepoTreeEntry.fromJson(jsonOf("path" to "a", "date" to "2026-07-04 12:30:00+02:00")).updatedMs,
        )
        assertEq(0L, RepoTreeEntry.fromJson(jsonOf("path" to "a", "date" to "yesterday")).updatedMs)
        expect(RepoTreeEntry.fromJson(jsonOf("path" to "a")).updatedAt == null)

        val entry = RepoTreeEntry.fromJson(
            jsonOf("path" to "lib/src/main.dart", "mode" to "100644", "oid" to "abc", "bytes" to "12", "commitMessage" to "  Fix bug  \nbody"),
        )
        assertEq("main.dart", entry.name)
        assertEq("lib/src/main.dart", entry.path)
        assertEq("file", entry.type)
        assertEq("abc", entry.sha)
        assertEq(12L, entry.size)
        assertEq("Fix bug", entry.lastMessage)
        expect(RepoTreeEntry.fromJson(jsonOf("name" to "d", "kind" to "Directory")).isDirectory)
        expect(RepoTreeEntry.fromJson(jsonOf("name" to "d", "type" to "subtree")).isDirectory)

        val nested = RepoTree.fromJson(
            jsonOf("tree" to listOf(jsonOf("name" to "x", "path" to "a/x"), jsonOf("name" to "")), "ts" to "9"),
            path = "b",
        )
        assertEq("a/x", nested.entries.single().path, "paths that already contain '/' are kept")
        assertEq(9L, nested.cachedAt)
        assertEq("b", nested.path)
    }

    fun testRepoCommitBranchAndSearchParsing() {
        val detail = RepoCommitDetail.fromJson(
            jsonOf(
                "files" to listOf(jsonOf("file" to "a.txt", "additions" to 3L, "deletions" to 1L), jsonOf("adds" to "1")),
                "diff" to "diff --git",
                "truncated" to true,
                "servedBy" to "mirror",
            ),
            fallbackCommit = jsonOf("hash" to "abc"),
        )
        assertEq("abc", detail.commit["hash"])
        assertEq(1, detail.files.size)
        assertEq("3", detail.files.single().adds)
        expect(detail.truncated)
        assertEq("mirror", detail.source)
        assertEq(jsonOf("hash" to "z"), RepoCommitDetail.fromJson(listOf(1L), fallbackCommit = jsonOf("hash" to "z")).commit)

        val branch = RepoBranch.fromJson(jsonOf("branch" to "main", "commit" to "abc", "default" to true, "worktree" to "/w"))
        assertEq("main", branch.name)
        assertEq("abc", branch.sha)
        expect(branch.isDefault)
        expect(branch.hasWorktree)

        val search = RepoSearchResults.fromJson(
            jsonOf("code" to listOf(jsonOf("file" to "a.kt", "lineNumber" to "4", "snippet" to "val x"), jsonOf("text" to "no path"))),
        )
        assertEq(1, search.code.size)
        assertEq(4, search.code.single().line)
        assertEq("val x", search.code.single().text)
        assertEq(0, RepoSearchResults.fromJson(null).code.size)
    }

    fun testFederatedReplyLifecycleAndProvenance() {
        val tombstone = FederatedReply.fromJson(jsonOf("id" to "r1", "tombstone" to true, "depth" to 12L, "ts" to "5", "body" to "gone"))
        assertEq("r1", tombstone.remoteId)
        assertEq("tombstoned", tombstone.lifecycle)
        expect(tombstone.tombstone)
        assertEq("Deleted on the remote instance", tombstone.displayBody)
        assertEq(8, tombstone.depth)
        assertEq(5L, tombstone.timestamp)
        assertEq("", tombstone.backlink)
        assertEq("activitypub", tombstone.sourceSoftware)

        val reply = FederatedReply.fromJson(
            jsonOf(
                "remoteId" to "https://social.example/notes/1",
                "body" to "hello",
                "depth" to -3L,
                "moderated" to true,
                "edited" to true,
                "nativeEvent" to true,
                "provenance" to jsonOf("instance" to "social.example", "software" to "mastodon"),
            ),
        )
        assertEq("https://social.example/notes/1", reply.backlink)
        assertEq("social.example", reply.sourceInstance)
        assertEq("mastodon", reply.sourceSoftware)
        assertEq("moderated", reply.lifecycle)
        assertEq("Hidden by remote moderation", reply.displayBody)
        assertEq(0, reply.depth)
        expect(!reply.nativeEvent, "a remote projection is never native")
        assertEq("hello", FederatedReply.fromJson(jsonOf("lifecycle" to "edited", "body" to "hello")).displayBody)
        expect(FederatedReply.fromJson(jsonOf("lifecycle" to "awaiting-redelivery")).moderated)
    }

    fun testChatMemberGroup() {
        val group = MemberGroup(
            name = "alice",
            members = listOf(Member(id = "k1", name = "alice"), Member(id = "k2", name = "alice", online = true)),
        )
        expect(group.online)
        assertEq("k1", group.id)
        assertEq(2, group.nodeCount)
    }

    fun testDartScalarHelpers() {
        assertEq("1.25", modelTrimDouble(1.25))
        assertEq("2", modelTrimDouble(2.0))
        assertEq("0.000009", modelTrimDouble(0.000009))
        assertEq("1e-7", modelTrimDouble(1e-7))
        assertEq(31L, modelInt("0x1F"))
        assertEq(0L, modelInt("1.5"))
        assertEq(1L, modelInt(1.9))
    }
}
