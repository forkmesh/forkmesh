// The owner-key gates (read proofs, desktop command requests) and the Mirrors, Releases,
// Worktrees, Actions and Agents tabs, plus the detail screen's tabs and deep links.

import FMAccount
import FMApi
import FMBase
import FMCrypto
import XCTest
@testable import FMCode

@MainActor
final class ReposTabsTests: XCTestCase {
    private let repo = Repository(owner: "Alice", name: "repo", defaultBranch: "main")

    private func signer(session: AuthSession?, identity: DeviceIdentity?) -> RepoSigner {
        RepoSigner(session: { session }, identity: identity, clock: FixedClock(epochMillis: 1_700_000_000_000))
    }

    // MARK: Owner signing

    func testActionsReadProofSignsTheCanonicalString() throws {
        let identity = try repoTestIdentity()
        let session = repoTestSession(nodeName: "Alice", pubkey: identity.publicKeyB64url)
        let proof = try RepoOwnerSigning.actionsReadProof(
            repo: repo, session: session, identity: identity, nowMillis: 1_700_000_000_000)
        XCTAssertEqual(proof.ownerAccount, "alice")
        XCTAssertEqual(proof.ts, "1700000000000")
        let canonical = "forkmesh-actions-read-v1\nAlice\nrepo\n1700000000000"
        XCTAssertEqual(RepoOwnerSigning.actionsReadCanonical(owner: "Alice", name: "repo", ts: proof.ts), canonical)
        XCTAssertTrue(Ed25519.verify(
            publicKey: identity.publicKeyBytes, message: Array(canonical.utf8),
            signature: Array(try B64.decodeUnpadded(proof.sig))))
    }

    func testActionsReadProofGates() throws {
        let identity = try repoTestIdentity()
        func message(_ session: AuthSession?, _ identity: DeviceIdentity?) -> String {
            do {
                _ = try RepoOwnerSigning.actionsReadProof(repo: repo, session: session, identity: identity, nowMillis: 1)
                return "signed"
            } catch {
                return dartErrorText(error)
            }
        }
        XCTAssertEqual(
            message(nil, identity),
            "Exception: Actions are owner-key gated. Sign in with the repo owner account and local owner key to view workflow logs.")
        XCTAssertEqual(
            message(repoTestSession(nodeName: "bob", pubkey: identity.publicKeyB64url), identity),
            "Exception: Actions are owner-key gated. This device is signed in as bob, not Alice.")
        XCTAssertEqual(message(repoTestSession(nodeName: "alice", pubkey: "desktop-key"), identity), "signed")
        var unpaired = repoTestSession(nodeName: "alice", pubkey: identity.publicKeyB64url)
        unpaired.capabilities = ["browse"]
        XCTAssertEqual(
            message(unpaired, identity),
            "Exception: Actions are owner-key gated. Pair this mobile device with the owner key before viewing workflow logs.")
        XCTAssertEqual(message(repoTestSession(nodeName: " alice ", pubkey: identity.publicKeyB64url + " "), identity), "signed")
    }

    func testDesktopCommandNeedsASignedInPairedDevice() async throws {
        let identity = try repoTestIdentity()
        let client = FakeRepoClient()
        func send(_ session: AuthSession?) async -> String {
            await RepoDesktopCommands.request(
                client: client, signer: signer(session: session, identity: identity), repo: repo, command: "release.draft",
                target: "release:v1", payload: [:], successMessage: "ok")
        }
        let signedOut = await send(nil)
        XCTAssertEqual(signedOut, "Exception: Sign in with the repo owner account to request desktop control.")
        let mobileKey = await send(repoTestSession(nodeName: "alice", pubkey: "desktop-key"))
        XCTAssertEqual(mobileKey, "ok")
        var unpaired = repoTestSession(nodeName: "alice", pubkey: identity.publicKeyB64url)
        unpaired.capabilities = ["browse"]
        let unpairedResult = await send(unpaired)
        XCTAssertEqual(unpairedResult, "Exception: Pair this mobile device with the owner key before requesting desktop control.")
        XCTAssertEqual(client.desktopCommands.count, 1)
        let other = await send(repoTestSession(nodeName: "bob", pubkey: identity.publicKeyB64url))
        XCTAssertEqual(other, "ok")
        XCTAssertEqual(client.desktopCommands.map(\.owner), ["Alice", "Alice"])
        XCTAssertEqual(client.desktopCommands.map(\.ownerAccount), ["alice", "bob"])
        client.desktopResult = { throw ApiException("HTTP 403 for /api/repo/Alice/repo/desktop-commands") }
        let failed = await send(repoTestSession(nodeName: "alice", pubkey: identity.publicKeyB64url))
        XCTAssertEqual(failed, "Exception: HTTP 403 for /api/repo/Alice/repo/desktop-commands")
    }

    func testOrganizationRepoRequestsNameTheOrganizationUnlessThisAccountServesIt() async throws {
        let identity = try repoTestIdentity()
        let client = FakeRepoClient()
        let orgRepo = Repository(owner: "alice", name: "repo", publicOwner: "acme")
        func send(_ node: String) async -> String {
            await RepoDesktopCommands.request(
                client: client,
                signer: signer(session: repoTestSession(nodeName: node, pubkey: identity.publicKeyB64url), identity: identity),
                repo: orgRepo, command: "release.draft", target: "release:v1", payload: [:], successMessage: "ok")
        }
        let member = await send("bob")
        let servingAccount = await send("alice")
        XCTAssertEqual(member, "ok")
        XCTAssertEqual(servingAccount, "ok")
        XCTAssertEqual(client.desktopCommands.map(\.owner), ["acme", "alice"])
        XCTAssertEqual(client.desktopCommands.map(\.ownerAccount), ["bob", "alice"])
        let first = client.desktopCommands[0]
        let canonical = "forkmesh-desktop-command-v1\nacme\nrepo\nrelease.draft\nrelease:v1\n\(first.ts)"
        XCTAssertTrue(Ed25519.verify(
            publicKey: identity.publicKeyBytes, message: Array(canonical.utf8),
            signature: Array(try B64.decodeUnpadded(first.sig))))
    }

    // MARK: Actions

    func testActionStatusTones() {
        XCTAssertEqual(RepoDisplay.actionStatusTone("Success"), .success)
        XCTAssertEqual(RepoDisplay.actionStatusTone("Awaiting approval"), .accent)
        XCTAssertEqual(RepoDisplay.actionStatusTone("Rejected"), .danger)
        XCTAssertEqual(RepoDisplay.actionStatusTone("Other"), .tertiary)
    }

    func testAgentsTabUsesTheSignedInAccountElseTheOwner() async {
        let client = FakeRepoClient()
        client.agentSessionsResult = {
            [AgentSession(id: 5, issueNumber: 9, status: "running", provider: "claude", branchName: "agent/x", numTurns: 3, conflicted: true)]
        }
        let signedIn = RepoAgentsTabModel(
            client: client, repo: repo, signer: signer(session: repoTestSession(nodeName: " Bob "), identity: nil))
        await signedIn.start()
        XCTAssertEqual(client.agentSessionAccounts, ["bob"])
        XCTAssertEqual(signedIn.countLabel, "1")
        XCTAssertTrue(signedIn.running)
        let session = signedIn.items[0]
        let chips = RepoAgentsTabModel.chips(session)
        XCTAssertEqual(chips.first, "#9")
        XCTAssertTrue(chips.contains("agent/x"))
        XCTAssertTrue(chips.contains("3 turns"))
        XCTAssertEqual(chips.last == "Conflict" || chips.contains("Conflict"), true)
        XCTAssertTrue(RepoAgentsTabModel.subtitle(session).hasPrefix("Issue #9 • "))

        let guest = RepoAgentsTabModel(client: client, repo: repo, signer: signer(session: nil, identity: nil))
        await guest.start()
        XCTAssertEqual(client.agentSessionAccounts.last, "Alice")
        XCTAssertEqual(RepoDisplay.agentStatusTone("Merged"), .success)
        XCTAssertEqual(RepoDisplay.agentStatusTone("Stopped"), .danger)
        XCTAssertEqual(RepoDisplay.agentStatusTone("Waiting"), .accent)
    }

    func testAgentSessionDetailReadsTheTranscript() async {
        let client = FakeRepoClient()
        client.transcriptResult = { AgentTranscript(status: "done", transcript: "\nall good\n") }
        let session = AgentSession(id: 5, status: "running", model: "opus")
        let model = RepoAgentSessionModel(client: client, repo: repo, session: session, ownerAccount: "alice")
        XCTAssertEqual(model.title, "Agent session #5")
        XCTAssertEqual(model.transcriptText, "No transcript has been pushed by the desktop node yet.")
        await model.start()
        XCTAssertEqual(client.transcriptCalls.first?.id, 5)
        XCTAssertEqual(client.transcriptCalls.first?.ownerAccount, "alice")
        XCTAssertEqual(model.transcriptText, "all good")
        XCTAssertEqual(model.transcriptStatus, AgentSession(id: 5, status: "done").statusLabel)
        XCTAssertTrue(model.chips().contains { $0.icon == "memory_outlined" && $0.label == "opus" })
    }

    func testAgentSessionDetailShowsTheDesktopsPictures() async {
        let client = FakeRepoClient()
        client.transcriptResult = { AgentTranscript(status: "done", transcript: "Attached image: /home/tester/shot.png\n") }
        let session = AgentSession(id: 5, status: "done")
        let named = RepoAgentSessionModel(client: client, repo: repo, session: session, ownerAccount: "alice")
        await named.start()
        XCTAssertEqual(named.attachments.map(\.name), ["shot.png"])
        XCTAssertFalse(named.attachments[0].canFetch)
        let listed = AgentAttachmentRef(
            meta: AgentAttachmentMeta(id: "a1", name: "shot.png", mediaType: "image/png", size: 10),
            origin: .desktop(sessionId: 5))
        let model = RepoAgentSessionModel(
            client: client, repo: repo, session: session, ownerAccount: "alice", desktopAttachments: { [listed] })
        await model.start()
        XCTAssertEqual(model.attachments, [listed])
        XCTAssertTrue(model.attachments[0].showsThumbnail)
    }

    // MARK: Mirrors, releases, worktrees

    func testMirrorsTabAndSyncStatusCard() async {
        let client = FakeRepoClient()
        client.mirrorSetResult = {
            RepoMirrorSet(
                mirrors: [RepoMirror(owner: "m1", name: "repo", online: true), RepoMirror(owner: "", name: "repo")],
                syncStatus: ["severity": "warning", "label": "Behind", "summary": "2 mirrors lag"])
        }
        let model = RepoMirrorsTabModel(client: client, repo: repo)
        await model.start()
        XCTAssertEqual(model.mirrors.map(\.label), ["m1/repo", "repo"])
        XCTAssertEqual(model.mirrors.map(RepoMirrorsTabModel.mirrorBadge), ["live", "offline"])
        XCTAssertEqual(model.mirrors.map(RepoMirrorsTabModel.mirrorSubtitle), ["Online mirror", "Offline or recently unseen"])
        XCTAssertEqual(model.mirrors.map(RepoMirrorsTabModel.mirrorIcon), ["cloud_done_outlined", "cloud_off_outlined"])
        let card = model.syncCard!
        XCTAssertEqual(card.icon, "warning_amber_outlined")
        XCTAssertEqual(card.tone, .warning)
        XCTAssertEqual(card.badge, "behind")
        XCTAssertEqual(card.summary, "2 mirrors lag")
        XCTAssertEqual(RepoSyncStatusCard(status: ["severity": "ok", "label": .null]).label, "")
        XCTAssertEqual(RepoSyncStatusCard(status: ["severity": "ok"]).tone, .success)
        XCTAssertEqual(RepoSyncStatusCard(status: [:]).icon, "sync")
    }

    func testReleasesTabRequestsDesktopWork() async throws {
        let identity = try repoTestIdentity()
        let client = FakeRepoClient()
        client.releasesResult = { [RepoRelease(tag: "v1.2.0", channel: "stable")] }
        let model = RepoReleasesTabModel(
            client: client, repo: repo,
            signer: signer(session: repoTestSession(nodeName: "alice", pubkey: identity.publicKeyB64url), identity: identity))
        await model.start()
        XCTAssertEqual(RepoReleaseAction.all.map(\.label), ["Draft release", "Generate notes", "Create release", "Publish artifacts"])
        let text = await model.request(RepoReleaseAction.all[3], for: model.releases.data![0])
        XCTAssertEqual(text, "Publish artifacts request queued. Approve it in the ForkMesh desktop app.")
        XCTAssertEqual(client.desktopCommands[0].command, "release.publish")
        XCTAssertEqual(client.desktopCommands[0].target, "release:v1.2.0")
        XCTAssertEqual(JSON.encode(client.desktopCommands[0].payload), #"{"tag":"v1.2.0","channel":"stable"}"#)
        XCTAssertEqual(JSON.encode(RepoDesktopCommands.releasePayload(RepoRelease(tag: "v2"))), #"{"tag":"v2"}"#)
        XCTAssertEqual(RepoReleasesTabModel.assetTitle(RepoReleaseAsset(name: "", sha256: "0123456789abcdef")), "0123456789ab")
    }

    func testWorktreesTabListsOnlyWorktreeBranches() async {
        let client = FakeRepoClient()
        client.branchesResult = {
            [
                RepoBranch(name: "main", sha: "aaaaaaa1234", isDefault: true, worktreePath: "/w/main"),
                RepoBranch(name: "dev", sha: "bbb"),
                RepoBranch(name: "agent/wt", sha: "abc", worktreePath: "/tmp/wt"),
            ]
        }
        let model = RepoWorktreesTabModel(client: client, repo: repo)
        await model.start()
        XCTAssertEqual(model.worktrees.data?.map(\.name), ["main", "agent/wt"])
        XCTAssertEqual(RepoWorktreesTabModel.chips(model.worktrees.data![0]), ["default", "aaaaaaa"])
        // A short sha is shown whole instead of crashing as substring(0, 7) did.
        XCTAssertEqual(RepoWorktreesTabModel.chips(model.worktrees.data![1]), ["abc"])
    }

    // MARK: Detail screen

    func testTabsAndDeepLinks() {
        XCTAssertEqual(RepoDetailTab.allCases.map(\.tabLabel), [
            "Code", "Issues", "Pulls", "Discussions", "Commits", "Mirrors", "Releases", "About", "Actions", "Agents", "Worktrees",
        ])
        XCTAssertEqual(RepoDetailTab.allCases.compactMap(\.tabCaption), ["replicas", "workflows", "sessions"])
        XCTAssertEqual(RepoDetailTab.allCases.compactMap(\.tabFloatingActionLabel), ["New issue", "New PR", "New discussion"])

        let repo = Repository(owner: "o", name: "r")
        let model = RepoDetailModel(
            repo: repo, initialTab: .pulls, initialTarget: NotificationDeepLink(repo: repo, initialTab: .pulls, number: 7))
        XCTAssertEqual(model.tab, .pulls)
        XCTAssertEqual(model.takeInitialDestination(), .pull(7))
        XCTAssertNil(model.takeInitialDestination(), "opens once")

        XCTAssertEqual(
            RepoDetailModel.destination(for: NotificationDeepLink.parse("/o/r/commit/abc123")!),
            .commit(["hash": "abc123", "message": "Commit abc123"]))
        XCTAssertNil(RepoDetailModel.destination(for: NotificationDeepLink.parse("/o/r/issues")!))
        XCTAssertEqual(RepoDetailModel.destination(for: NotificationDeepLink.parse("/o/r/issues/4")!), .issue(4))
        XCTAssertEqual(RepoDetailModel.destination(for: NotificationDeepLink.parse("/o/r/discussions/2")!), .discussion(2))
        XCTAssertNil(RepoDetailModel.destination(for: NotificationDeepLink.parse("/o/r/mirrors")!))
        XCTAssertNil(RepoDetailModel(repo: repo).takeInitialDestination())
    }

    // MARK: Small helpers

    func testSizesSourcesGlyphsAndBadges() {
        XCTAssertEqual(RepoDisplay.humanSize(12), "12 B")
        XCTAssertEqual(RepoDisplay.humanSize(1536), "1.5 KB")
        XCTAssertEqual(RepoDisplay.humanSize(20 * 1024), "20 KB")
        XCTAssertEqual(RepoDisplay.humanSize(3 * 1024 * 1024 + 400_000), "3.4 MB")
        XCTAssertEqual(RepoDisplay.humanSize(1024 * 1024 * 1024), "1.0 GB")
        XCTAssertNil(RepoDisplay.sourceLabel("  "))
        XCTAssertEqual(RepoDisplay.sourceLabel("LIVE"), "live source")
        XCTAssertEqual(RepoDisplay.sourceLabel("live-mirror"), "live mirror")
        XCTAssertEqual(RepoDisplay.sourceLabel("Served by x"), "Served by x")
        XCTAssertEqual(RepoDisplay.sourceLabel("live node-b"), "live node-b")
        XCTAssertEqual(RepoDisplay.sourceLabel("qt-node-a"), "served by qt-node-a")
        XCTAssertEqual(RepoDisplay.fileGlyph("main.DART"), "code_rounded")
        XCTAssertEqual(RepoDisplay.fileGlyph("pubspec.lock"), "data_object_rounded")
        XCTAssertEqual(RepoDisplay.fileGlyph("logo.svg"), "image_outlined")
        XCTAssertEqual(RepoDisplay.fileGlyph("NOTES.txt"), "article_outlined")
        XCTAssertEqual(RepoDisplay.fileGlyph("Makefile"), "terminal_rounded")
        XCTAssertEqual(RepoDisplay.fileGlyph("LICENSE"), "terminal_rounded")
        XCTAssertEqual(RepoDisplay.fileGlyph("blob"), "description_outlined")
        XCTAssertEqual(RepoDisplay.badgeTone("Open"), .success)
        XCTAssertEqual(RepoDisplay.badgeTone("live"), .success)
        XCTAssertEqual(RepoDisplay.badgeTone("offline"), .danger)
        XCTAssertEqual(RepoDisplay.badgeTone("Changes requested"), .danger)
        XCTAssertEqual(RepoDisplay.badgeTone("merged"), .accent)
        XCTAssertEqual(RepoDisplay.badgeTone("3 turns"), .secondary)
    }
}
