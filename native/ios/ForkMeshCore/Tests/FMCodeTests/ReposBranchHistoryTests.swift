import FMAccount
import FMApi
import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMCode

@MainActor
final class ReposBranchHistoryTests: XCTestCase {
    private let repo = Repository(owner: "owner", name: "repo", defaultBranch: "main")

    private func client(head: String?) -> FakeRepoClient {
        let client = FakeRepoClient()
        client.branchesResult = {
            [
                RepoBranch(name: "main", sha: "aaaaaaa1234", isDefault: true),
                RepoBranch(name: "feature/x", sha: "bbbbbbb1234"),
            ]
        }
        client.commitsResult = { [["hash": "aaaaaaa1234", "subject": "Default tip"]] }
        client.historyResult = { _ in
            let first = head ?? "bbbbbbb1234"
            return RepoHistoryRead(commits: [["hash": .string(first), "subject": "Branch tip"]], head: head)
        }
        return client
    }

    func testABranchRouteReadsThatBranchsHistoryAndTrustsAMatchingTip() async {
        let client = client(head: "bbbbbbb1234")
        let model = RepoCommitsTabModel(client: client, repo: repo, ref: "feature/x")

        await model.start()

        XCTAssertEqual(client.historyRefs, ["feature/x"])
        XCTAssertEqual(client.commitsCalls, 0)
        XCTAssertEqual(model.commitRows().map(\.subject), ["Branch tip"])
        XCTAssertEqual(model.refSupport, .honored)
        XCTAssertNil(model.refNotice)
        XCTAssertEqual(model.branchLabel, "feature/x")

        await model.refresh()
        XCTAssertEqual(client.historyRefs, ["feature/x", "feature/x"])
        XCTAssertEqual(client.clearedCaches, 1)
    }

    func testAServerThatIgnoresTheRefIsDetectedAndSaidSo() async {
        let client = client(head: "aaaaaaa1234")
        let model = RepoCommitsTabModel(client: client, repo: repo, ref: "feature/x")

        await model.start()

        XCTAssertEqual(model.refSupport, .ignored)
        XCTAssertEqual(
            model.refNotice,
            "This server lists only the default branch's history, so these are the commits of main, not feature/x.")
        XCTAssertEqual(model.historyCount, 1)
    }

    func testTheDefaultBranchOrNoRefKeepsTheCachedDefaultRead() async {
        for ref in [nil, "", "main"] as [String?] {
            let client = client(head: nil)
            let model = RepoCommitsTabModel(client: client, repo: repo, ref: ref)
            await model.start()
            XCTAssertNil(model.ref)
            XCTAssertTrue(client.historyRefs.isEmpty)
            XCTAssertEqual(client.commitsCalls, 1)
            XCTAssertEqual(model.branchLabel, "main")
            XCTAssertNil(model.refSupport)
        }
    }

    func testDetectionFallsBackToTheFirstCommitAndStaysUnsureWhenItCannotTell() {
        let branches = [
            RepoBranch(name: "main", sha: "aaaaaaa1234", isDefault: true),
            RepoBranch(name: "same", sha: "aaaaaaa1234"),
            RepoBranch(name: "feature/x", sha: "bbbbbbb1234"),
        ]
        func detect(_ ref: String, _ read: RepoHistoryRead, _ list: [RepoBranch] = branches) -> RepoHistoryRefSupport {
            RepoHistoryRefSupport.detect(ref: ref, read: read, branches: list, defaultBranch: "main")
        }
        let listed = RepoHistoryRead(commits: [["hash": "BBBBBBB1234abcd"]])
        XCTAssertEqual(listed.tip, "bbbbbbb1234abcd")
        XCTAssertEqual(detect("feature/x", listed), .honored)
        XCTAssertEqual(detect("same", RepoHistoryRead(commits: [], head: "aaaaaaa1234")), .honored)
        XCTAssertEqual(detect("gone", RepoHistoryRead(commits: [], head: "aaaaaaa1234")), .unknown)
        XCTAssertEqual(detect("gone", RepoHistoryRead(commits: [], head: "ddddddd1234")), .honored)
        XCTAssertEqual(detect("feature/x", RepoHistoryRead(commits: [], head: "eeeeeee1234")), .honored)
        XCTAssertEqual(detect("feature/x", RepoHistoryRead(commits: [])), .unknown)
        XCTAssertEqual(detect("bbbbbbb", RepoHistoryRead(commits: [], head: "bbbbbbb1234"), []), .honored)
        XCTAssertEqual(detect("feature/x", RepoHistoryRead(commits: [], head: "ddddddd1234"), []), .unknown)
    }

    func testTheHistoryReadCarriesTheRefAndTheServersResolvedCommit() async throws {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/repo/owner/repo/history") { request in
            let query = URLComponents(url: request.url, resolvingAgainstBaseURL: false)?.queryItems ?? []
            let ref = query.first { $0.name == "ref" }?.value ?? ""
            let tip = ref == "feature/x" ? "bbbbbbb1234" : "aaaaaaa1234"
            return HTTPResponse(status: 200, json: [
                "ok": true, "commit": .string(tip),
                "commits": [["hash": .string(tip), "subject": .string(ref.isEmpty ? "default" : ref)]],
            ])
        }
        let api = ApiService(settings: StaticApiSettings(serverUrl: "https://forkmesh.test"), http: http, sleep: { _ in })

        let read = try await api.history("owner", "repo", ref: "feature/x")

        XCTAssertEqual(read.head, "bbbbbbb1234")
        XCTAssertEqual(read.commits.first?["subject"], "feature/x")
        let url = try XCTUnwrap(http.requests.last?.url)
        XCTAssertEqual(url.path, "/api/repo/owner/repo/history")
        XCTAssertTrue(url.absoluteString.hasSuffix("history?ref=feature/x") || url.absoluteString.hasSuffix("history?ref=feature%2Fx"))

        let plain = try await api.history("owner", "repo", ref: "")
        XCTAssertEqual(plain.head, "aaaaaaa1234")
        XCTAssertNil(http.requests.last?.url.query)
        let legacy = RepoHistoryRead(json: .array([["hash": "ccccccc1234"]]))
        XCTAssertNil(legacy.head)
        XCTAssertEqual(legacy.tip, "ccccccc1234")
    }
}
