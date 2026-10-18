// The repository-read parts of native/android core/src/test/.../api/ApiServiceTest.kt: public-route
// reads of mirror-served repositories, the offline and TTL caches, published issues / pulls /
// discussions, releases, commits, search, branches, mirrors, federated replies and `rawBytes`.

import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

@MainActor
final class ApiServiceRepoTests: XCTestCase {
    // MARK: Routing

    func testMirrorServedReadsAddressThePublicRouteNotTheServingNode() async throws {
        // The flagship's catalog row is published by one mirror; reading through that node's own
        // route answers 503 mirror_unavailable while the organization route serves across the
        // whole mirror group.
        let http = FakeHTTPClient { request in
            let path = request.apiPath
            if path == "/api/repositories" {
                return apiJSON([
                    "repositories": [
                        [
                            "owner": "mirror35",
                            "name": "forkmesh",
                            "rootCommit": "root1",
                            "logicalOwners": [["kind": "organization", "owner": "forkmesh"]],
                        ],
                        ["owner": "solo", "name": "tool"],
                    ],
                ])
            }
            if path.hasPrefix("/api/repo/mirror35/") {
                return apiJSON(["error": "mirror_unavailable"], status: 503)
            }
            if path.hasSuffix("/tree") {
                return apiJSON(["entries": [["name": "README.md", "path": "README.md", "type": "blob"]]])
            }
            if path.hasSuffix("/history") {
                return apiJSON(["commits": [["hash": "abc", "subject": "first"]]])
            }
            return apiText(#"{"error":"not_found"}"#, status: 404)
        }
        let api = makeTestApi(http)

        let repos = try await api.repositories()
        let repo = try XCTUnwrap(repos.first { $0.name == "forkmesh" })
        XCTAssertEqual(repo.owner, "mirror35")
        XCTAssertEqual(repo.fullName, "forkmesh/forkmesh")

        let tree = try await api.tree(repo.owner, repo.name)
        XCTAssertEqual(tree.entries.map(\.name), ["README.md"])
        let commits = try await api.commits(repo.owner, repo.name)
        XCTAssertEqual(commits.map { $0.str("subject") }, ["first"])
        XCTAssertEqual(api.rawURL(repo.owner, repo.name, "logo.png").path, "/api/repo/forkmesh/forkmesh/raw")
        let paths = http.requests.map(\.apiPath)
        XCTAssertFalse(paths.contains { $0.hasPrefix("/api/repo/mirror35/") }, "\(paths)")
        XCTAssertTrue(paths.contains("/api/repo/forkmesh/forkmesh/history"), "\(paths)")

        // A repository with no other public name keeps its own route.
        _ = try await api.tree("solo", "tool")
        XCTAssertTrue(http.requests.map(\.apiPath).contains("/api/repo/solo/tool/tree"))
    }

    // MARK: Caches

    func testAnOfflineRepoHostSkipsRefetchForAMinute() async throws {
        let clock = FixedClock(epochMillis: 1_000_000)
        let http = FakeHTTPClient { _ in apiJSON(["error": "offline"], status: 503) }
        let api = makeTestApi(http, clock: clock)

        let first = await apiAssertThrows { try await api.tree("owner", "repo") }
        XCTAssertTrue(
            apiErrorMessage(first).contains("Repo host is offline. Open the Qt desktop node"), apiErrorMessage(first))
        let skipped = await apiAssertThrows { try await api.blob("owner", "repo", "README.md") }
        XCTAssertTrue(apiErrorMessage(skipped).contains("Skipping refetch for a minute"), apiErrorMessage(skipped))
        XCTAssertEqual(http.requests.count, 1)

        clock.now = clock.now.addingTimeInterval(ApiService.offlineRepoTtl)
        http.answerAll { _ in apiJSON(["content": "hello"]) }
        let blob = try await api.blob("owner", "repo", "README.md")
        XCTAssertEqual(blob.content, "hello")
        XCTAssertEqual(http.requests.count, 2)

        http.answerAll { _ in apiJSON(["error": "offline"], status: 503) }
        _ = await apiAssertThrows { try await api.tree("owner", "repo", path: "x") }
        api.clearRepoCache("owner", "repo")
        http.answerAll { _ in apiJSON(["entries": []]) }
        let tree = try await api.tree("owner", "repo", path: "x")
        XCTAssertEqual(tree.entries.count, 0)
    }

    func testRepoContentIsCachedForTheTtlAndA500IsRetriedOnce() async throws {
        let clock = FixedClock(epochMillis: 0)
        let calls = ApiCallCounter()
        let http = FakeHTTPClient { _ in
            let n = calls.next()
            return n == 1 ? apiText("boom", status: 500) : apiJSON(["content": .string("v\(n)")])
        }
        let api = makeTestApi(http, clock: clock)

        let first = try await api.blob("o", "r", "a.txt")
        XCTAssertEqual(first.content, "v2")
        let cached = try await api.blob("o", "r", "a.txt")
        XCTAssertEqual(cached.content, "v2")
        XCTAssertEqual(calls.value, 2)

        clock.now = clock.now.addingTimeInterval(ApiService.cacheTtl)
        let refreshed = try await api.blob("o", "r", "a.txt")
        XCTAssertEqual(refreshed.content, "v3")

        // A repo 500 on both attempts reads as the host's internal error.
        http.answerAll { _ in apiText("boom", status: 500) }
        let error = await apiAssertThrows { try await api.blob("o", "r", "b.txt") }
        XCTAssertTrue(apiErrorMessage(error).contains("Repo host returned an internal error"), apiErrorMessage(error))
    }

    // MARK: Published issues

    func testPublishedMarkdownIssuesIncludeBountyAndVoteCount() async throws {
        let issueMarkdown = """
            ---
            title: Stabilize signed inbox
            status: open
            authorName: Alice
            labels: [mobile, signed]
            milestone: v2 mobile
            priority: 4
            assignees: [mona, kai]
            bountyUsd: 150
            bountyAddress: escrow-md
            bountyStatus: open
            bountyRequiredLamports: 4500
            bountyReceivedLamports: 1500
            bountyAmountSol: 0.0000045
            bountyPayUri: solana:escrow-md?amount=0.0000045
            bountyPayee: maintainer-node
            bountyPayoutSig: payout-md
            ---
            Mobile should make signed collaboration clear.
            """
        let voteMarkdown = """
            ---
            type: vote
            authorName: Voter
            ts: 10
            ---
            """
        let http = apiRouteByPath { path, query, _ in
            let p = query["path"] ?? ""
            if path.hasSuffix("/tree") && p == "issues" {
                return apiJSON(["entries": [["name": "12", "path": "issues/12", "type": "dir"]]])
            }
            if path.hasSuffix("/tree") && p == "issues/12" {
                return apiJSON([
                    "entries": [
                        ["name": "issue.md", "path": "issues/12/issue.md", "type": "file"],
                        ["name": "001-vote.md", "path": "issues/12/001-vote.md", "type": "file"],
                        ["name": "002-vote.md", "path": "issues/12/002-vote.md", "type": "file"],
                    ],
                ])
            }
            if path.hasSuffix("/blob") && p == "issues/12/issue.md" {
                return apiJSON(["content": .string(issueMarkdown)])
            }
            if path.hasSuffix("/blob") && (p == "issues/12/001-vote.md" || p == "issues/12/002-vote.md") {
                return apiJSON(["content": .string(voteMarkdown)])
            }
            return nil
        }
        let issues = try await makeTestApi(http, url: "ws://127.0.0.1:9/ws").publishedIssues("owner", "repo")

        XCTAssertEqual(issues.count, 1)
        let issue = try XCTUnwrap(issues.first)
        XCTAssertEqual(issue.title, "Stabilize signed inbox")
        XCTAssertEqual(issue.body, "Mobile should make signed collaboration clear.")
        XCTAssertEqual(issue.author, "Alice")
        XCTAssertEqual(issue.labels, ["mobile", "signed"])
        XCTAssertEqual(issue.milestone, "v2 mobile")
        XCTAssertEqual(issue.priority, 4)
        XCTAssertEqual(issue.assignees, ["mona", "kai"])
        XCTAssertEqual(issue.votes, 2)
        XCTAssertEqual(issue.events.first?.ts, 10)
        XCTAssertEqual(issue.bountyUsd, 150)
        XCTAssertEqual(issue.bountyAddress, "escrow-md")
        XCTAssertEqual(issue.bountyStatus, "open")
        XCTAssertEqual(issue.bountyRequiredLamports, 4500)
        XCTAssertEqual(issue.bountyReceivedLamports, 1500)
        XCTAssertEqual(issue.bountyAmountSol, 0.0000045)
        XCTAssertEqual(issue.bountyPayUri, "solana:escrow-md?amount=0.0000045")
        XCTAssertEqual(issue.bountyPayee, "maintainer-node")
        XCTAssertEqual(issue.bountyPayoutSig, "payout-md")
    }

    func testPublishedIssuesReadTheSplitForkmeshIssuesJsonLayout() async throws {
        // Issues are split by status into .forkmesh/issues/open/<n>/ and
        // .forkmesh/issues/closed/<n>/ (pre-split repos keep <n>/ at the root); each folder
        // holds one issue-<n>.json signed-event record.
        func record(_ number: Int, _ status: String, _ title: String, _ body: String) -> String {
            JSON.encode([
                "schema": "forkmesh-issue-v1",
                "number": .int(Int64(number)),
                "title": .string(title),
                "status": .string(status),
                "authorName": "Alice",
                "labels": ["mobile"],
                "votes": 1,
                "events": [[
                    "type": "open",
                    "id": .string("open-\(number)"),
                    "authorName": "Alice",
                    "ts": 1,
                    "title": .string(title),
                    "body": .string(body),
                    "attachments": [],
                ]],
            ] as JSONValue)
        }
        let open3 = record(3, "open", "Split open", "From the open folder")
        let closed1 = record(1, "closed", "Split closed", "done")
        let legacy2 = record(2, "open", "Legacy spot", "old spot")
        let http = apiRouteByPath { path, query, _ in
            let p = query["path"] ?? ""
            if path.hasSuffix("/tree") {
                switch p {
                case ".forkmesh/issues":
                    return apiJSON([
                        "entries": [
                            ["name": "open", "path": ".forkmesh/issues/open", "type": "dir"],
                            ["name": "closed", "path": ".forkmesh/issues/closed", "type": "dir"],
                            ["name": "2", "path": ".forkmesh/issues/2", "type": "dir"],
                        ],
                    ])
                case ".forkmesh/issues/open":
                    return apiJSON(["entries": [["name": "3", "path": ".forkmesh/issues/open/3", "type": "dir"]]])
                case ".forkmesh/issues/closed":
                    return apiJSON(["entries": [["name": "1", "path": ".forkmesh/issues/closed/1", "type": "dir"]]])
                default:
                    return nil
                }
            }
            if path.hasSuffix("/blob") {
                switch p {
                case ".forkmesh/issues/open/3/issue-3.json": return apiJSON(["content": .string(open3)])
                case ".forkmesh/issues/closed/1/issue-1.json": return apiJSON(["content": .string(closed1)])
                case ".forkmesh/issues/2/issue-2.json": return apiJSON(["content": .string(legacy2)])
                default: return nil
                }
            }
            return nil
        }
        let issues = try await makeTestApi(http).publishedIssues("owner", "repo")

        XCTAssertEqual(issues.map(\.number), [3, 2, 1])
        XCTAssertEqual(issues.first?.title, "Split open")
        XCTAssertEqual(issues.first?.body, "From the open folder")
        XCTAssertEqual(issues.first?.author, "Alice")
        XCTAssertEqual(issues.first?.labels, ["mobile"])
        XCTAssertEqual(issues.first?.votes, 1)
        XCTAssertEqual(issues.first?.isOpen, true)
        XCTAssertEqual(issues.last?.status, "closed")
    }

    func testIssueRecordBodyFollowsTheLatestSignedEditOfTheOpenEvent() async throws {
        let record = JSON.encode([
            "number": 5,
            "title": "Edited",
            "events": [
                ["type": "open", "id": "o5", "body": "first", "authorName": "Ann"],
                ["type": "edit", "target": "o5", "body": "second"],
                ["type": "edit", "target": "other", "body": "ignored"],
                ["type": "edit", "target": "o5", "body": "Depends on: #4"],
            ],
        ] as JSONValue)
        let http = apiRouteByPath { path, query, _ in
            let p = query["path"] ?? ""
            if path.hasSuffix("/tree") && p == ".forkmesh/issues/open" {
                return apiJSON(["entries": [["name": "5", "type": "dir"]]])
            }
            if path.hasSuffix("/blob") && p == ".forkmesh/issues/open/5/issue-5.json" {
                return apiJSON(["content": .string(record)])
            }
            return nil
        }
        let issues = try await makeTestApi(http).publishedIssues("owner", "repo")

        XCTAssertEqual(issues.count, 1)
        XCTAssertEqual(issues.first?.body, "Depends on: #4")
        XCTAssertEqual(issues.first?.author, "Ann")
    }

    func testPublishedIssuesReadRecordsInBatchesInsteadOfOneRequestEach() async throws {
        let numbers = Array(1...120)
        let record: @Sendable (Int) -> String = { number in
            JSON.encode([
                "number": .int(Int64(number)),
                "title": .string("Issue \(number)"),
                "events": [["type": "open", "id": .string("o\(number)"), "body": "b"]],
            ] as JSONValue)
        }
        let http = apiRouteByPath { path, query, request in
            if path.hasSuffix("/tree") && query["path"] == ".forkmesh/issues/open" {
                return apiJSON(["entries": .array(numbers.map { ["name": .string("\($0)"), "type": "dir"] })])
            }
            if path.hasSuffix("/blobs") {
                var blobs = JSONObject()
                for pair in request.apiRawQuery.split(separator: "&") where pair.hasPrefix("path=") {
                    let wanted = String(pair.dropFirst(5)).removingPercentEncoding ?? ""
                    guard let number = Int(wanted.split(separator: "/")[3]), number != 77 else { continue }
                    blobs[wanted] = ["content": .string(record(number))]
                }
                return apiJSON(["blobs": .object(blobs)])
            }
            if path.hasSuffix("/blob") && query["path"] == ".forkmesh/issues/open/77/issue-77.json" {
                return apiJSON(["content": .string(record(77))])
            }
            return nil
        }
        let issues = try await makeTestApi(http).publishedIssues("owner", "repo")

        XCTAssertEqual(issues.map(\.number), numbers.reversed())
        XCTAssertEqual(http.requests.filter { $0.apiPath.hasSuffix("/blobs") }.count, 3)
        XCTAssertEqual(http.requests.filter { $0.apiPath.hasSuffix("/blob") }.map { $0.apiQuery["path"] },
                       [".forkmesh/issues/open/77/issue-77.json"])
    }

    func testIssueRecordsAreParsedOffTheMainThread() async throws {
        let numbers = Array(1...60)
        let http = apiRouteByPath { path, query, request in
            if path.hasSuffix("/tree") && query["path"] == ".forkmesh/issues/open" {
                return apiJSON(["entries": .array(numbers.map { ["name": .string("\($0)"), "type": "dir"] })])
            }
            if path.hasSuffix("/blobs") {
                var blobs = JSONObject()
                for pair in request.apiRawQuery.split(separator: "&") where pair.hasPrefix("path=") {
                    let wanted = String(pair.dropFirst(5)).removingPercentEncoding ?? ""
                    let number = wanted.split(separator: "/")[3]
                    blobs[wanted] = ["content": .string(JSON.encode(["number": .string(String(number)), "title": "T"] as JSONValue))]
                }
                return apiJSON(["blobs": .object(blobs)])
            }
            return nil
        }
        let threads = MainThreadTally()
        let api = ApiService(
            settings: StaticApiSettings(serverUrl: "ws://test.local/ws"), http: http,
            clock: FixedClock(epochMillis: apiTestEpochMillis), sleep: { _ in },
            issueRecord: { number, text in
                threads.note()
                return PublishedRecords.issueFromRecordJson(number, text)
            })

        let issues = try await api.publishedIssues("owner", "repo")

        XCTAssertEqual(issues.map(\.number), numbers.reversed())
        XCTAssertEqual(threads.calls, 60)
        XCTAssertEqual(threads.onMain, 0)
    }

    // MARK: Published discussions and pulls

    func testPublishedDiscussionsIncludeCategoryAndReplies() async throws {
        let http = apiRouteByPath { path, query, _ in
            let p = query["path"] ?? ""
            if path.hasSuffix("/tree") && p == ".forkmesh/discussions" {
                return apiJSON(["entries": [["name": "3", "path": ".forkmesh/discussions/3", "type": "dir"]]])
            }
            if path.hasSuffix("/tree") && p == ".forkmesh/discussions/3" {
                return apiJSON([
                    "entries": [
                        ["name": "discussion.md", "path": ".forkmesh/discussions/3/discussion.md", "type": "file"],
                        ["name": "001-comment.md", "path": ".forkmesh/discussions/3/001-comment.md", "type": "file"],
                    ],
                ])
            }
            if path.hasSuffix("/blob") && p == ".forkmesh/discussions/3/discussion.md" {
                return apiJSON([
                    "content": """
                        ---
                        title: Mobile maintainer workflow
                        category: ideas
                        authorName: Alice
                        ---
                        How should maintainers review from phones?
                        """,
                ])
            }
            if path.hasSuffix("/blob") && p == ".forkmesh/discussions/3/001-comment.md" {
                return apiJSON([
                    "content": """
                        ---
                        type: comment
                        authorName: Mona
                        ts: 10
                        ---
                        Keep desktop node as the canonical apply surface.
                        """,
                ])
            }
            return nil
        }
        let discussions = try await makeTestApi(http).publishedDiscussions("owner", "repo")

        XCTAssertEqual(discussions.count, 1)
        let discussion = try XCTUnwrap(discussions.first)
        XCTAssertEqual(discussion.title, "Mobile maintainer workflow")
        XCTAssertEqual(discussion.category, "ideas")
        XCTAssertEqual(discussion.author, "Alice")
        XCTAssertEqual(discussion.events.map(\.body), ["Keep desktop node as the canonical apply surface."])
        XCTAssertEqual(discussion.events.first?.displayAuthor, "Mona")
    }

    func testPublishedDiscussionsSplitTheMarkedAnswerFromTheReplies() {
        let reply = PublishedRecords.discussionEventFromMarkdown("""
            ---
            type: comment
            id: reply-1
            author: helper-key
            ts: 10
            ---
            Use the desktop node.
            """)
        let answer = PublishedRecords.discussionEventFromMarkdown("""
            ---
            type: answer
            id: answer-1
            author: opener-key
            ts: 20
            target: reply-1
            targetAuthor: helper-key
            ---
            """)
        let discussion = PublishedRecords.discussionFromMarkdown("3", """
            ---
            title: How?
            category: Q&A
            author: opener-key
            authorName: Alice
            ---
            Question
            """, events: [reply, answer])
        XCTAssertEqual(reply.id, "reply-1")
        XCTAssertEqual(discussion.events.map(\.id), ["reply-1"])
        XCTAssertEqual(discussion.answer?.target, "reply-1")
        XCTAssertEqual(discussion.authorKey, "opener-key")
        XCTAssertEqual(discussion.author, "Alice")
    }

    func testPublishedMarkdownPullsReadTheLedgerBranchWithTimelineEvents() async throws {
        let http = apiRouteByPath { path, query, _ in
            let p = query["path"] ?? ""
            // Pull records must come from the ledger branch: the default branch holds stale
            // pre-ledger copies that all read as open.
            if p.hasPrefix(".forkmesh/pulls") && query["ref"] != "forkmesh/pulls" {
                return apiJSON(["error": "not_found"], status: 404)
            }
            if path.hasSuffix("/tree") && p == ".forkmesh/pulls" {
                return apiJSON(["entries": [["name": "7", "path": ".forkmesh/pulls/7", "type": "dir"]]])
            }
            if path.hasSuffix("/tree") && p == ".forkmesh/pulls/7" {
                // Listed out of order: the timeline is sorted by file name.
                return apiJSON([
                    "entries": [
                        ["name": "pull.md", "path": ".forkmesh/pulls/7/pull.md", "type": "file"],
                        ["name": "002-review.md", "path": ".forkmesh/pulls/7/002-review.md", "type": "file"],
                        ["name": "001-comment.md", "path": ".forkmesh/pulls/7/001-comment.md", "type": "file"],
                    ],
                ])
            }
            if path.hasSuffix("/blob") && p == ".forkmesh/pulls/7/pull.md" {
                return apiJSON([
                    "content": """
                        ---
                        title: Add signed review flow
                        status: open
                        base: main
                        head: review/mobile
                        ts: 1790830449950
                        author: w0F5vYij
                        authorName: jett
                        sig: abc
                        ---
                        Adds mobile review controls.

                        ```diff
                        diff --git a/lib/a.dart b/lib/a.dart
                        +review
                        ```
                        """,
                ])
            }
            if path.hasSuffix("/blob") && p == ".forkmesh/pulls/7/001-comment.md" {
                return apiJSON(["content": "---\ntype: comment\nauthorName: Kai\nts: 10\n---\nReview changes before merge."])
            }
            if path.hasSuffix("/blob") && p == ".forkmesh/pulls/7/002-review.md" {
                return apiJSON([
                    "content": "---\ntype: review\nstate: approve\nauthorName: Mona\nts: 11\n---\nLGTM from mobile.",
                ])
            }
            return nil
        }
        let pulls = try await makeTestApi(http).publishedPulls("owner", "repo")

        XCTAssertEqual(pulls.count, 1)
        let pull = try XCTUnwrap(pulls.first)
        XCTAssertEqual(pull.title, "Add signed review flow")
        XCTAssertTrue(pull.signed)
        XCTAssertEqual(pull.base, "main")
        XCTAssertEqual(pull.head, "review/mobile")
        XCTAssertEqual(pull.displayAuthor, "jett")
        XCTAssertEqual(pull.author, "w0F5vYij")
        XCTAssertEqual(pull.createdMs, 1_790_830_449_950)
        XCTAssertEqual(pull.patch, "diff --git a/lib/a.dart b/lib/a.dart\n+review")
        XCTAssertEqual(pull.events.count, 2)
        XCTAssertEqual(pull.events.first?.body, "Review changes before merge.")
        XCTAssertEqual(pull.events.last?.state, "approve")
    }

    func testPublishedPullsIncludePullsThatLiveOnlyOnTheirOwnBranch() async throws {
        let branchCommit = "0123456789abcdef0123456789abcdef01234567"
        let http = apiRouteByPath { path, query, _ in
            let p = query["path"] ?? ""
            let ref = query["ref"] ?? ""
            if path.hasSuffix("/tree") && p == ".forkmesh/pulls" && ref == "forkmesh/pulls" {
                return apiJSON([
                    "entries": [["name": "7", "path": ".forkmesh/pulls/7", "type": "dir"]],
                    "pullRefs": ["7": "aaaa", "8": .string(branchCommit), "bogus": "bbbb"],
                ])
            }
            if path.hasSuffix("/blob") && p == ".forkmesh/pulls/7/pull.md" && ref == "forkmesh/pulls" {
                return apiJSON(["content": "---\ntitle: Indexed\nstatus: open\n---\nBody."])
            }
            if path.hasSuffix("/blob") && p == ".forkmesh/pulls/8/pull.md" && ref == branchCommit {
                return apiJSON(["content": "---\ntitle: Brand new\nstatus: open\n---\nBody."])
            }
            return apiJSON(["error": "not_found"], status: 404)
        }
        let pulls = try await makeTestApi(http).publishedPulls("owner", "repo")
        XCTAssertEqual(pulls.map(\.number), [8, 7])
        XCTAssertEqual(pulls.map(\.title), ["Brand new", "Indexed"])
    }

    func testPublishedPullReadsOneRecordAndFallsBackToItsOwnBranch() async throws {
        let listings = ApiCallCounter()
        let http = apiRouteByPath { path, query, _ in
            let p = query["path"] ?? ""
            let ref = query["ref"] ?? ""
            if path.hasSuffix("/tree") && p == ".forkmesh/pulls" {
                listings.next()
                return apiJSON(["entries": [["name": "7", "path": ".forkmesh/pulls/7", "type": "dir"]]])
            }
            if path.hasSuffix("/blob") && p == ".forkmesh/pulls/7/pull.md" && ref == "forkmesh/pulls" {
                return apiJSON(["content": "---\ntitle: On the ledger\nstatus: open\n---\nBody."])
            }
            if path.hasSuffix("/blob") && p == ".forkmesh/pulls/9/pull.md" && ref == "forkmesh/pull/9" {
                return apiJSON(["content": "---\ntitle: Only on its branch\nstatus: open\n---\nBody."])
            }
            return apiJSON(["error": "not_found"], status: 404)
        }
        let api = makeTestApi(http)
        let listed = try await api.publishedPulls("owner", "repo")
        XCTAssertEqual(listed.map(\.number), [7])
        XCTAssertEqual(listings.value, 1)

        let ledger = try await api.publishedPull("owner", "repo", 7)
        XCTAssertEqual(ledger?.title, "On the ledger")
        let branch = try await api.publishedPull("owner", "repo", 9)
        XCTAssertEqual(branch?.number, 9)
        XCTAssertEqual(branch?.title, "Only on its branch")
        let absent = try await api.publishedPull("owner", "repo", 10)
        XCTAssertNil(absent)

        _ = try await api.publishedPulls("owner", "repo")
        XCTAssertEqual(listings.value, 2)
    }

    func testPullHeadsReadTheLedgerAndOneBatchWithoutTimelines() async throws {
        let branchCommit = "0123456789abcdef0123456789abcdef01234567"
        let http = apiRouteByPath { path, query, request in
            let p = query["path"] ?? ""
            if path.hasSuffix("/tree") && p == ".forkmesh/pulls" && query["ref"] == "forkmesh/pulls" {
                return apiJSON([
                    "entries": [
                        ["name": "7", "path": ".forkmesh/pulls/7", "type": "dir"],
                        ["name": "9", "path": ".forkmesh/pulls/9", "type": "dir"],
                    ],
                    "pullRefs": ["8": .string(branchCommit)],
                ])
            }
            if path.hasSuffix("/blobs") && query["ref"] == "forkmesh/pulls" {
                var blobs = JSONObject()
                for pair in request.apiRawQuery.split(separator: "&") where pair.hasPrefix("path=") {
                    let wanted = String(pair.dropFirst(5)).removingPercentEncoding ?? ""
                    let number = wanted.split(separator: "/")[2]
                    blobs[wanted] = ["content": .string("---\ntitle: Pull \(number)\nstatus: merged\nhead: agent/\(number)\n---\nBody.")]
                }
                return apiJSON(["blobs": .object(blobs)])
            }
            if path.hasSuffix("/blob") && p == ".forkmesh/pulls/8/pull.md" && query["ref"] == branchCommit {
                return apiJSON(["content": "---\ntitle: Pull 8\nstatus: open\nhead: agent/8\n---\nBody."])
            }
            return apiJSON(["error": "not_found"], status: 404)
        }
        let api = makeTestApi(http)

        let heads = try await api.publishedPullHeads("owner", "repo")
        XCTAssertEqual(heads.map(\.number), [9, 8, 7])
        XCTAssertEqual(heads.map(\.status), ["merged", "open", "merged"])
        XCTAssertEqual(heads.map(\.head), ["agent/9", "agent/8", "agent/7"])
        XCTAssertEqual(http.requests.filter { $0.apiPath.hasSuffix("/tree") }.count, 1)
        XCTAssertEqual(http.requests.filter { $0.apiPath.hasSuffix("/blobs") }.count, 1)
        XCTAssertEqual(http.requests.count, 3)

        _ = try await api.publishedPullHeads("owner", "repo")
        XCTAssertEqual(http.requests.count, 3)

        api.clearRepoCache("owner", "repo")
        _ = try await api.publishedPullHeads("owner", "repo")
        XCTAssertEqual(http.requests.count, 6)
    }

    func testPullHeadsReuseAFreshFullPullList() async throws {
        let http = apiRouteByPath { path, query, _ in
            let p = query["path"] ?? ""
            if path.hasSuffix("/tree") && p == ".forkmesh/pulls" {
                return apiJSON(["entries": [["name": "7", "path": ".forkmesh/pulls/7", "type": "dir"]]])
            }
            if path.hasSuffix("/blob") && p == ".forkmesh/pulls/7/pull.md" {
                return apiJSON(["content": "---\ntitle: Seven\nstatus: open\n---\nBody."])
            }
            return apiJSON(["error": "not_found"], status: 404)
        }
        let api = makeTestApi(http)
        _ = try await api.publishedPulls("owner", "repo")
        let before = http.requests.count

        let heads = try await api.publishedPullHeads("owner", "repo")
        XCTAssertEqual(heads.map(\.title), ["Seven"])
        XCTAssertEqual(http.requests.count, before)
    }

    func testBlobBatchesSkipFilesAlreadyRead() async throws {
        let http = apiRouteByPath { path, query, request in
            if path.hasSuffix("/blob") && query["path"] == "a.md" {
                return apiJSON(["content": "A"])
            }
            if path.hasSuffix("/blobs") {
                var blobs = JSONObject()
                for pair in request.apiRawQuery.split(separator: "&") where pair.hasPrefix("path=") {
                    let wanted = String(pair.dropFirst(5)).removingPercentEncoding ?? ""
                    blobs[wanted] = ["content": .string(wanted.uppercased())]
                }
                return apiJSON(["blobs": .object(blobs)])
            }
            return nil
        }
        let api = makeTestApi(http)
        _ = try await api.blob("owner", "repo", "a.md")

        let found = await api.blobs("owner", "repo", ["a.md", "b.md"])
        XCTAssertEqual(found["a.md"]?.content, "A")
        XCTAssertEqual(found["b.md"]?.content, "B.MD")
        let batch = try XCTUnwrap(http.requests.first { $0.apiPath.hasSuffix("/blobs") })
        XCTAssertFalse(batch.apiRawQuery.contains("a.md"))

        _ = await api.blobs("owner", "repo", ["a.md", "b.md"])
        XCTAssertEqual(http.requests.count, 2)
    }

    // MARK: Releases

    func testReleasesParseReleaseManifestsWithArtifactDownloadCounts() async throws {
        let sha = "1938916325d5839850fbc39db05a3a1f836b10615ac4467725b9f49e864884fb"
        let secondSha = "2938916325d5839850fbc39db05a3a1f836b10615ac4467725b9f49e864884fc"
        let manifest = JSON.encode([
            "schema": "forkmesh-release-v1",
            "tag": "v1.2.3",
            "channel": "latest",
            "tag_commit": "abcdef1234567890",
            "created_at": 1_700_000_000_000,
            "assets": [
                [
                    "name": "forkmesh-linux-x86_64", "blob_sha256": .string(sha), "size": 14_949_672, "os": "linux",
                    "arch": "x86_64",
                ],
                [
                    "name": "forkmesh-darwin-arm64", "sha256": .string(secondSha), "size": "2048", "os": "darwin",
                    "arch": "arm64",
                ],
            ],
        ] as JSONValue)
        let counts = JSONObject([(sha, JSONValue.int(3)), (secondSha, JSONValue.string("5"))])
        let http = apiRouteByPath { path, _, _ in
            switch path {
            case "/api/repo/owner/repo/tree":
                return apiJSON(["entries": [["name": "latest", "path": ".forkmesh/releases/latest", "type": "dir"]]])
            case "/api/repo/owner/repo/blob":
                return apiJSON(["content": .string(manifest)])
            case "/api/repo/owner/repo/releases/downloads":
                return apiJSON(["ok": true, "counts": .object(counts)])
            default:
                return apiJSON(["error": "not_found"], status: 404)
            }
        }
        let releases = try await makeTestApi(http).releases("owner", "repo")

        XCTAssertEqual(releases.count, 1)
        let release = try XCTUnwrap(releases.first)
        XCTAssertEqual(release.tag, "v1.2.3")
        XCTAssertEqual(release.channel, "latest")
        XCTAssertEqual(release.shortCommit, "abcdef12")
        XCTAssertEqual(release.assets.map(\.sha256), [sha, secondSha])
        XCTAssertEqual(release.assets.first?.downloads, 3)
        XCTAssertEqual(release.assets.first?.sizeLabel, "14.3 MB")
        XCTAssertEqual(release.assets.last?.downloads, 5)
        XCTAssertEqual(release.assets.last?.sizeLabel, "2.0 KB")
        let downloads = http.requests.filter { $0.apiPath == "/api/repo/owner/repo/releases/downloads" }
        XCTAssertEqual(downloads.map(\.apiRawQuery), [""])
        XCTAssertEqual(
            http.requests.first { $0.apiPath.hasSuffix("/blob") }?.apiQuery["path"],
            ".forkmesh/releases/latest/release.json")
    }

    func testReleasesReturnEmptyListWhenTheReleaseTreeIsAbsent() async throws {
        let http = apiRouteByPath { path, _, _ in
            switch path {
            case "/api/repo/owner/repo/releases/downloads": return apiJSON(["error": "offline"], status: 503)
            default: return apiJSON(["error": "not_found"], status: 404)
            }
        }
        let api = makeTestApi(http)

        let releases = try await api.releases("owner", "repo")
        XCTAssertEqual(releases.count, 0)
        let paths = http.requests.map(\.apiPath)
        XCTAssertTrue(paths.contains("/api/repo/owner/repo/releases/downloads"), "\(paths)")
        XCTAssertTrue(paths.contains("/api/repo/owner/repo/tree"), "\(paths)")
        XCTAssertEqual(
            http.requests.filter { $0.apiPath == "/api/repo/owner/repo/tree" }.map { $0.apiQuery["path"] },
            [".forkmesh/releases"])

        // The failed download count must not leave the repo marked offline.
        http.answerAll { _ in apiJSON(["entries": []]) }
        let tree = try await api.tree("owner", "repo", path: "src")
        XCTAssertEqual(tree.entries.count, 0)
    }

    // MARK: Git reads, mirrors, federated replies

    func testCommitsCommitDetailSearchAndBranches() async throws {
        let http = FakeHTTPClient { request in
            switch request.apiPath {
            case "/api/repo/o/r/commits", "/api/repo/o/r/history":
                return apiJSON(["commits": [["hash": "abc"], "junk"]])
            case "/api/repo/o/r/commit":
                return apiJSON(["files": [["path": "a.txt", "adds": 1]], "diff": "d"])
            case "/api/repo/o/r/search":
                return apiJSON(["code": [["path": "a.txt", "line": 3, "text": "x"]]])
            case "/api/repo/o/r/branches":
                return apiJSON(["branches": [["name": "main", "default": true], ["name": ""]]])
            default:
                return apiText("", status: 404)
            }
        }
        let api = makeTestApi(http)

        let commits = try await api.commits("o", "r")
        XCTAssertEqual(commits.map { JSONValue.object($0) }, [["hash": "abc"]])
        XCTAssertEqual(http.lastRequest?.apiPath, "/api/repo/o/r/history")
        _ = try await api.commits("o", "r")
        XCTAssertEqual(http.requests.count, 1)

        let detail = try await api.commitDetail("o", "r", "abc", fallbackCommit: ["hash": "abc"])
        XCTAssertEqual(http.lastRequest?.apiQuery, ["path": "abc"])
        XCTAssertEqual(detail.files.map(\.path), ["a.txt"])
        XCTAssertEqual(detail.commit["hash"], .string("abc"))

        let search = try await api.searchRepo("o", "r", "needle x", ref: "dev")
        XCTAssertEqual(search.code.first?.line, 3)
        XCTAssertEqual(http.lastRequest?.apiQuery, ["q": "needle x", "ref": "dev"])

        let branches = try await api.branches("o", "r")
        XCTAssertEqual(branches.map(\.name), ["main"])
    }

    func testMirrorSetCarriesTheSyncStatus() async throws {
        let http = FakeHTTPClient { _ in
            apiJSON([
                "generatedAt": 1_800_000_000_000,
                "summary": ["freshestSync": 1_799_999_990_000],
                "mirrors": [[
                    "owner": "backup", "repo": "proj", "status": "online", "integrity": "ok",
                    "cloneAvailable": true, "lastSync": 1_799_999_990_000,
                ]],
            ])
        }
        let set = try await makeTestApi(http).mirrorSet("alice", "proj")
        XCTAssertEqual(set.mirrors.map(\.label), ["backup/proj"])
        XCTAssertEqual(set.syncStatus?["state"], .string("verified"))
        let mirrors = try await makeTestApi(http).mirrors("alice", "proj")
        XCTAssertEqual(mirrors.map(\.label), ["backup/proj"])
    }

    func testFederatedRepliesAreReadFromTheProjectionOnly() async throws {
        let http = FakeHTTPClient { _ in
            apiJSON([
                "comments": [
                    ["remoteId": "https://mastodon.test/1", "author": "@a@mastodon.test", "body": "hi"],
                    ["author": "nobody"],
                ],
            ])
        }
        let api = makeTestApi(http)

        let commit = try await api.federatedReplies("o", "r", kind: "commit", number: 1)
        XCTAssertEqual(commit.count, 0)
        let unnumbered = try await api.federatedReplies("o", "r", kind: "issue", number: 0)
        XCTAssertEqual(unnumbered.count, 0)
        XCTAssertEqual(http.requests.count, 0)

        let replies = try await api.federatedReplies("o", "r", kind: "issue", number: 4)
        XCTAssertEqual(replies.count, 1)
        XCTAssertEqual(replies.first?.nativeEvent, false)
        XCTAssertEqual(http.requests.count, 1)
        XCTAssertEqual(http.lastRequest?.apiPath, "/api/repo/o/r/fedi-comments")
        XCTAssertEqual(http.lastRequest?.apiQuery, ["kind": "issue", "number": "4"])
    }

    // MARK: rawBytes

    func testRawBytesReturnsTheServedBytesUntouched() async throws {
        // The point of this method: bytes that do not survive a decode/encode round trip. 0xFF is
        // not valid UTF-8, so anything that went through a String on the way here would have
        // replaced it with U+FFFD.
        let served = Data([0x68, 0xFF, 0x0A])
        let http = FakeHTTPClient { _ in HTTPResponse(status: 200, body: served) }
        let bytes = try await makeTestApi(http).rawBytes("owner", "repo", "a.txt", ref: "main")

        XCTAssertEqual(bytes, served)
        XCTAssertEqual(http.lastRequest?.apiPath, "/api/repo/owner/repo/raw")
        XCTAssertEqual(http.lastRequest?.apiQuery["path"], "a.txt")
        XCTAssertEqual(http.lastRequest?.apiQuery["ref"], "main")
    }

    func testRawBytesRefusesAFilePastTheByteCeiling() async throws {
        let http = FakeHTTPClient { _ in HTTPResponse(status: 200, body: Data(count: 2048)) }
        let api = makeTestApi(http)
        let error = await apiAssertThrows { try await api.rawBytes("owner", "repo", "big.bin", maxBytes: 1024) }
        XCTAssertTrue(error is ApiException, "\(String(describing: error))")
    }

    func testRawBytesSurfacesANon200RatherThanReturningAnErrorPage() async throws {
        let http = FakeHTTPClient { _ in apiText("nope", status: 404) }
        let api = makeTestApi(http)
        let error = await apiAssertThrows { try await api.rawBytes("owner", "repo", "missing.txt") }
        XCTAssertTrue(error is ApiException, "\(String(describing: error))")
    }
}

private final class MainThreadTally: @unchecked Sendable {
    private let lock = NSLock()
    private var total = 0
    private var main = 0

    var calls: Int { lock.withLock { total } }
    var onMain: Int { lock.withLock { main } }

    func note() {
        let isMain = Thread.isMainThread
        lock.withLock {
            total += 1
            if isMain { main += 1 }
        }
    }
}
