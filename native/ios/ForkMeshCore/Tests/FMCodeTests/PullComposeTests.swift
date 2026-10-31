import FMApi
import FMBase
import XCTest
@testable import FMCode

final class PullComposeTests: XCTestCase {
    private let fork = RemoteFork(
        url: "https://git.example.org/acme/repo", origin: "https://git.example.org", host: "git.example.org",
        owner: "acme", name: "repo")

    private let patch = "diff --git a/a.txt b/a.txt\n--- a/a.txt\n+++ b/a.txt\n@@ -1 +1 @@\n-old\n+new\n"

    @MainActor private func api() -> FakeCollabAPI {
        let api = FakeCollabAPI()
        api.branchList = [RepoBranch(name: "main", isDefault: true), RepoBranch(name: "fix/relay-retry")]
        api.forks = [fork]
        api.instanceHost = "forkmesh.com"
        api.comparison = RepoComparison(baseOid: "a", headOid: "b", commitCount: 2, patch: patch, commits: "From b\n")
        return api
    }

    func testRemoteForkParsesTheServerListAndRefusesUnsafeOrigins() {
        let data = try! JSON.parse("""
            {"ok":true,"forks":[
              {"url":"https://git.example.org/acme/repo","origin":"https://git.example.org","host":"git.example.org","owner":"acme","name":"repo","label":"Acme"},
              {"url":"http://evil.example/a/b","origin":"http://evil.example","host":"evil.example","owner":"a","name":"b"},
              {"origin":"https://x.example:8443","owner":"../x","name":"b"}
            ]}
            """)
        let forks = RemoteFork.list(data)
        XCTAssertEqual(forks.count, 1)
        XCTAssertEqual(forks.first?.displayName, "acme/repo on git.example.org")
        XCTAssertEqual(forks.first?.pullsURL?.absoluteString, "https://git.example.org/api/repo/acme/repo/pulls")
        XCTAssertEqual(
            RemoteFork.signedHead(host: "forkmesh.com", owner: "forkmesh", name: "forkmesh", branch: "fix/a"),
            "forkmesh.com/forkmesh/forkmesh:fix/a")
    }

    @MainActor func testComposerComparesBranchesAndOpensThePullHere() async {
        let api = api()
        let inbox = RecordingInbox()
        let model = NewPullModel(repo: collabRepo, api: api, head: "fix/relay-retry")
        await model.load()
        XCTAssertEqual(model.branches, ["main", "fix/relay-retry"])
        XCTAssertEqual(model.headChoices, ["fix/relay-retry"])
        XCTAssertEqual(model.targets.count, 2)
        XCTAssertEqual(api.compareRequests, ["owner/repo main...fix/relay-retry"])
        XCTAssertEqual(model.title, "Relay retry")
        XCTAssertEqual(model.files.map(\.path), ["a.txt"])
        XCTAssertTrue(model.ready)
        model.body = "Retries the relay.\n\n"
        let notice = await model.submit(inbox: inbox, host: api.instanceHost)
        XCTAssertEqual(notice?.message, CollabCopy.pendingNote)
        XCTAssertEqual(inbox.calls, ["open Relay retry main...fix/relay-retry"])
        XCTAssertEqual(inbox.openedPulls.first?.commits, "From b\n")
        XCTAssertEqual(inbox.openedPulls.first?.description, "Retries the relay.")
    }

    @MainActor func testComposerSendsToAForkOnAnotherInstance() async {
        let api = api()
        let inbox = RecordingInbox()
        let model = NewPullModel(repo: collabRepo, api: api, head: "fix/relay-retry")
        await model.load()
        model.target = fork
        let notice = await model.submit(inbox: inbox, host: api.instanceHost)
        XCTAssertEqual(notice?.message, "Sent to acme/repo on git.example.org as pull request #41.")
        XCTAssertEqual(inbox.remotePulls.first?.head, "forkmesh.com/owner/repo:fix/relay-retry")
        XCTAssertEqual(inbox.remotePulls.first?.base, "main")
        XCTAssertEqual(inbox.remotePulls.first?.patch, patch)

        inbox.remoteFailure = ApiException("remote_unreachable")
        let failed = await model.submit(inbox: inbox, host: api.instanceHost)
        XCTAssertNil(failed)
        XCTAssertTrue(model.errorText.hasPrefix("Nothing was delivered: git.example.org could not be reached"))
    }

    @MainActor func testComposerExplainsAnEmptyOrSameBranchComparison() async {
        let api = api()
        let model = NewPullModel(repo: collabRepo, api: api)
        await model.load()
        model.head = "main"
        await model.compare()
        XCTAssertEqual(model.compareError, PullCompose.sameBranch)
        api.comparison = RepoComparison()
        model.head = "fix/relay-retry"
        await model.compare()
        XCTAssertEqual(model.compareError, PullCompose.noChanges)
        XCTAssertFalse(model.ready)
        let inbox = RecordingInbox()
        model.title = "x"
        let notice = await model.submit(inbox: inbox, host: "forkmesh.com")
        XCTAssertNil(notice)
        XCTAssertTrue(inbox.calls.isEmpty)
    }

    func testMergedPullGetsATimelineEntryWhereItHappened() {
        var pull = PublishedPull(
            number: 7, title: "Fix", status: "merged", base: "main", head: "fix/a",
            events: [
                PullEvent(type: "review", author: "k2", authorName: "Bob", state: "approved", ts: 1_000),
                PullEvent(type: "comment", body: "thanks", author: "k1", authorName: "Ann", ts: 3_000_000),
            ],
            author: "k1", authorName: "Ann", mergedMs: 2_000_000, mergedBy: "k1")
        let events = PullTimeline.events(pull)
        XCTAssertEqual(events.map(\.type), ["review", "merged", "comment"])
        XCTAssertEqual(events[1].displayTitle, "Merged")
        XCTAssertEqual(events[1].authorName, "Ann")
        XCTAssertEqual(events[1].body, "Merged fix/a into main.")
        XCTAssertEqual(PullTimeline.icon(events[1]), "call_merge")

        pull.mergedMs = 0
        pull.mergedBy = ""
        XCTAssertEqual(PullTimeline.events(pull).map(\.type), ["review", "comment", "merged"])
        XCTAssertEqual(PullTimeline.author(PullTimeline.events(pull)[2]), "")

        pull.status = "open"
        XCTAssertEqual(PullTimeline.events(pull).count, 2)
    }

    func testPullStateDrivesTheHeaderAndTurnsReviewsOffWhenDone() {
        var pull = PublishedPull(number: 7, title: "Fix", status: "open", base: "main", head: "fix/a", authorName: "Ann")
        XCTAssertEqual(PullState(pull), .open)
        XCTAssertEqual(PullState.summary(pull, commits: 3), "Ann wants to merge 3 commits into main from fix/a")
        XCTAssertTrue(PullState.reviewable(pull))
        pull.status = "merged"
        XCTAssertEqual(PullState(pull).label, "Merged")
        XCTAssertEqual(PullState.summary(pull, commits: 1), "Ann's 1 commit merged into main from fix/a")
        XCTAssertFalse(PullState.reviewable(pull))
        pull.status = "closed"
        XCTAssertEqual(PullState(pull), .closed)
        XCTAssertFalse(PullState.reviewable(pull))
        XCTAssertEqual(PullEvent(type: "status", state: "closed").displayTitle, "Closed")
        XCTAssertEqual(PullEvent(type: "status", state: "open").displayTitle, "Reopened")
    }

    func testPublishedPullReadsTheMergeFromPullMarkdown() {
        let md = "---\ntitle: Fix\nstatus: merged\nbase: main\nhead: fix/a\nmergedAt: 1700000000000\nmergedBy: abc\n---\nbody\n"
        let pull = PublishedRecords.pullFromMarkdown("7", md)
        XCTAssertEqual(pull.mergedMs, 1_700_000_000_000)
        XCTAssertEqual(pull.mergedBy, "abc")
        XCTAssertEqual(PublishedRecords.pullHeadFromMarkdown("7", md).mergedMs, 1_700_000_000_000)
    }
}
