import FMApi
import FMBase
import FMNet
import XCTest

@testable import FMCode

final class CollabFindAndOutlineTests: XCTestCase {
    func testFindWrapsAndCountsLines() {
        var find = FindInText()
        find.search("foo", in: "foo bar\nFOO\nfood")
        XCTAssertEqual(find.matches.map(\.line), [1, 2, 3])
        XCTAssertEqual(find.status, "1 of 3")
        find.previous()
        XCTAssertEqual(find.currentMatch?.line, 3)
        find.next()
        XCTAssertEqual(find.currentMatch?.line, 1)
        find.options = FindOptions(caseSensitive: true, wholeWord: true)
        find.search("foo", in: "foo bar\nFOO\nfood")
        XCTAssertEqual(find.matches.count, 1)
        find.search("zzz", in: "abc")
        XCTAssertEqual(find.status, "No results")
        find.search("b", in: "a\nb\nc\nb")
        find.jump(toLine: 3)
        XCTAssertEqual(find.currentMatch?.line, 4)
        XCTAssertEqual(find.matches.last?.column, 1)
    }

    func testReplaceAllAndGoToLine() {
        let (text, count) = FindInText.replacing("a-a-A", query: "a", with: "bb")
        XCTAssertEqual(text, "bb-bb-bb")
        XCTAssertEqual(count, 3)
        XCTAssertEqual(GoToLine.parse("12:4", lineCount: 10), 10)
        XCTAssertEqual(GoToLine.parse(" 3 ", lineCount: 10), 3)
        XCTAssertNil(GoToLine.parse("x", lineCount: 10))
        XCTAssertNil(GoToLine.parse("0", lineCount: 10))
    }

    func testOutlineFindsDefinitionsAcrossLanguages() {
        let swift = "public struct Foo {\n    func bar() {\n        if x { baz() }\n    }\n}\nextension Foo {}"
        XCTAssertEqual(CodeOutline.symbols(swift).map(\.name), ["Foo", "bar", "Foo"])
        XCTAssertEqual(CodeOutline.symbols(swift)[1].depth, 1)
        let python = "class A:\n    async def run(self):\n        return go(1)\n"
        XCTAssertEqual(CodeOutline.symbols(python).map(\.name), ["A", "run"])
        let js = "export async function load() {}\nconst x = (a) => a\nfn main() {}"
        XCTAssertEqual(CodeOutline.symbols(js).map(\.name), ["load", "x", "main"])
        let cpp = "void MainWindow::open(int x)\n{\n    else if (x) {\n}"
        XCTAssertEqual(CodeOutline.symbols(cpp).map(\.name), ["MainWindow::open"])
        let refs = CodeOutline.references(to: "MainWindow::open", in: "a.open (1)\nopened\nopen", excludingLine: 3)
        XCTAssertEqual(refs.map(\.line), [1])
        XCTAssertTrue(refs[0].isCall)
        XCTAssertEqual(refs[0].id, "3:MainWindow::open#1")
        let twice = "struct Foo {}\nextension Foo {}\nlet a = Foo()"
        let first = CodeOutline.references(to: "Foo", in: twice, excludingLine: 1)
        let second = CodeOutline.references(to: "Foo", in: twice, excludingLine: 2)
        XCTAssertTrue(Set(first.map(\.id)).isDisjoint(with: second.map(\.id)))
    }
}

final class CollabReferenceLinkTests: XCTestCase {
    let repo = RepoRef(owner: "forkmesh", name: "app")

    func testReferencesMatchTheDesktopRules() {
        let refs = ReferenceLinks.references("See #12, PR #4 and pull request #5; issue #6 in abc1234 or fe/app#3. x#9 https://x.test/a.")
        XCTAssertEqual(refs.map(\.kind), [.issue, .pull, .pull, .issue, .commit, .issue, .url])
        XCTAssertEqual(refs[5].repo, RepoRef(owner: "fe", name: "app"))
        XCTAssertEqual(refs[5].label, "fe/app#3")
        XCTAssertEqual(refs[6].url, "https://x.test/a")
        XCTAssertEqual(refs[0].route(in: repo), .issue(repo, number: 12))
        XCTAssertEqual(refs[1].route(in: repo), .pull(repo, number: 4, tab: .conversation))
        XCTAssertEqual(refs[4].route(in: repo), .commit(repo, sha: "abc1234"))
    }

    func testCodeSpansAndFencesAreLeftAlone() {
        XCTAssertTrue(ReferenceLinks.references("`#1` and\n```\n#2\n```\n").isEmpty)
        XCTAssertTrue(ReferenceLinks.references("deadbeefdeadbeefdeadbeefdeadbeefdeadbeef0").isEmpty)
        XCTAssertEqual(ReferenceLinks.markdown("fix #7", repo: repo), "fix [#7](forkmesh://forkmesh/app/issues/7)")
        XCTAssertEqual(ReferenceLinks.markdown("see fe/app#3!", repo: repo), "see [fe/app#3](forkmesh://fe/app/issues/3)!")
    }

    func testRunsLinkEveryReferenceInTheRepo() {
        XCTAssertEqual(ReferenceLinks.runs("Fixes #7 via PR #8 (abc1234), see https://x.test/a.", repo: repo), [
            ReferenceRun(text: "Fixes ", url: nil),
            ReferenceRun(text: "#7", url: "forkmesh://forkmesh/app/issues/7"),
            ReferenceRun(text: " via ", url: nil),
            ReferenceRun(text: "PR #8", url: "forkmesh://forkmesh/app/pulls/8"),
            ReferenceRun(text: " (", url: nil),
            ReferenceRun(text: "abc1234", url: "forkmesh://forkmesh/app/commit/abc1234"),
            ReferenceRun(text: "), see ", url: nil),
            ReferenceRun(text: "https://x.test/a", url: "https://x.test/a"),
            ReferenceRun(text: ".", url: nil),
        ])
        XCTAssertEqual(ReferenceLinks.runs("abc1234 #2", repo: repo, commits: false), [
            ReferenceRun(text: "abc1234 ", url: nil),
            ReferenceRun(text: "#2", url: "forkmesh://forkmesh/app/issues/2"),
        ])
    }

    func testRunsWithoutARepoLinkOnlyQualifiedReferences() {
        XCTAssertEqual(ReferenceLinks.runs("#7, PR #8, abc1234 and fe/app#3 forkmesh://tasks/ab12", repo: nil), [
            ReferenceRun(text: "#7, PR #8, abc1234 and ", url: nil),
            ReferenceRun(text: "fe/app#3", url: "forkmesh://fe/app/issues/3"),
            ReferenceRun(text: " ", url: nil),
            ReferenceRun(text: "forkmesh://tasks/ab12", url: "forkmesh://tasks/ab12"),
        ])
        XCTAssertEqual(ReferenceLinks.runs("plain words", repo: nil), [ReferenceRun(text: "plain words", url: nil)])
    }
}

final class CollabCodeMapTests: XCTestCase {
    let tree: JSONObject = [
        "name": "", "size": 100, "commit": "c",
        "children": [
            ["name": "src", "type": "dir", "size": 60, "children": [
                ["name": "a.swift", "type": "file", "path": "src/a.swift", "size": 40],
                ["name": "b.swift", "type": "file", "path": "src/b.swift", "size": 20],
            ]],
            ["name": "README.md", "type": "file", "path": "README.md", "size": 30],
            ["name": "empty", "type": "dir", "size": 0],
        ],
    ]

    func testSquarifyFillsTheRectangle() {
        let rects = CodeMapLayout.squarify([6, 6, 4, 3, 2, 2, 1], in: CodeMapRect(x: 0, y: 0, width: 6, height: 4))
        XCTAssertEqual(rects.reduce(0) { $0 + $1.area }, 24, accuracy: 0.0001)
        for r in rects {
            XCTAssertGreaterThanOrEqual(r.x, -0.0001)
            XCTAssertLessThanOrEqual(r.x + r.width, 6.0001)
            XCTAssertLessThanOrEqual(r.y + r.height, 4.0001)
        }
        XCTAssertEqual(rects[0].area, 6, accuracy: 0.0001)
    }

    func testCellsNestDirectoriesAndSkipEmptyOnes() {
        let root = CodeMapNode(json: tree)
        XCTAssertEqual(root.fileCount, 3)
        XCTAssertEqual(root.find("src/b.swift")?.size, 20)
        let cells = CodeMapLayout.cells(root, in: CodeMapRect(x: 0, y: 0, width: 300, height: 200))
        XCTAssertEqual(cells.map(\.node.path), ["src", "src/a.swift", "src/b.swift", "README.md"])
        XCTAssertEqual(cells.first { $0.node.path == "src/a.swift" }?.colorSlot, 0)
        XCTAssertEqual(CodeMapLayout.hit(cells, x: 5, y: 30)?.node.path, "src/a.swift")
        XCTAssertEqual(CodeMapLayout.breadcrumb("src/ui").map(\.path), ["", "src", "src/ui"])
        XCTAssertEqual(CodeMapLayout.sizeLabel(1536), "1.5 KB")
        XCTAssertEqual(CodeMapLayout.sizeLabel(20 * 1024 * 1024), "20 MB")
    }
}

final class CollabCommitGraphTests: XCTestCase {
    func testLanesForkAndMerge() {
        let rows = CommitGraph.layout([
            ("m", ["a", "f"]), ("f", ["a"]), ("a", ["r"]), ("r", []),
        ])
        XCTAssertEqual(rows.map(\.column), [0, 1, 0, 0])
        XCTAssertTrue(rows[0].isMerge)
        XCTAssertEqual(rows[0].lanesOut, [0, 1])
        XCTAssertTrue(rows[2].edges.contains(CommitGraphEdge(from: 1, to: 0)))
        XCTAssertEqual(rows[3].lanesOut, [])
        XCTAssertEqual(CommitGraph.width(rows), 2)
        XCTAssertEqual(CommitGraph.parents(["parents": "A b"]), ["a", "b"])
        XCTAssertEqual(CommitGraph.parents(["parents": [["sha": "C"], "d"]]), ["c", "d"])
    }
}

final class CollabArchiveTests: XCTestCase {
    func testCrcAndZipLayout() {
        XCTAssertEqual(ZipCRC32.checksum(Array("123456789".utf8)), 0xCBF4_3926)
        var zip = ZipStoreWriter(date: Date(timeIntervalSince1970: 0))
        XCTAssertTrue(zip.add(path: "r/a.txt", bytes: Data("hi".utf8)))
        XCTAssertFalse(zip.add(path: "../evil", bytes: Data()))
        let data = zip.finish()
        XCTAssertEqual(Array(data.prefix(4)), [0x50, 0x4B, 0x03, 0x04])
        XCTAssertEqual(Array(data.suffix(22).prefix(4)), [0x50, 0x4B, 0x05, 0x06])
        XCTAssertEqual(data.suffix(22)[data.count - 22 + 10], 1)
        XCTAssertEqual(ZipStoreWriter.safeName("/a//b"), "a/b")
        XCTAssertNil(ZipStoreWriter.safeName("a/./b"))
        XCTAssertEqual(RepoArchiveBuilder.fileName(repo: "app", ref: "feat/x"), "app-feat-x.zip")
    }

    @MainActor func testBuilderWalksTheTreeAndSkipsHugeFiles() async throws {
        final class Reader: RepoArchiveReading {
            func tree(_ owner: String, _ name: String, path: String, ref: String) async throws -> RepoTree {
                if path.isEmpty {
                    return RepoTree(path: "", entries: [
                        RepoTreeEntry(name: "src", path: "src", type: "dir"),
                        RepoTreeEntry(name: "a.txt", path: "a.txt", type: "file", size: 2),
                        RepoTreeEntry(name: "big.bin", path: "big.bin", type: "file", size: RepoArchiveBuilder.maxFileBytes + 1),
                    ])
                }
                return RepoTree(path: path, entries: [RepoTreeEntry(name: "b.txt", path: "src/b.txt", type: "file", size: 1)])
            }

            func rawBytes(_ owner: String, _ name: String, _ path: String, ref: String, maxBytes: Int) async throws -> Data {
                Data(path.utf8)
            }
        }
        let result = try await RepoArchiveBuilder(reader: Reader()).build(owner: "o", name: "r", ref: "")
        XCTAssertEqual(result.files, 2)
        XCTAssertEqual(result.skipped, ["big.bin"])
        XCTAssertEqual(result.fileName, "r.zip")
        XCTAssertNotNil(String(data: result.data, encoding: .isoLatin1)?.range(of: "r/src/b.txt"))
    }
}

final class CollabLedgerAndSettingsTests: XCTestCase {
    @MainActor final class Ledger: ReviewLedgerReading {
        var files: [String: String] = [:]
        var compared: [(String, String)] = []

        func tree(_ owner: String, _ name: String, path: String, ref: String) async throws -> RepoTree {
            let prefix = path + "/"
            let entries = files.keys.filter { $0.hasPrefix(prefix) }.map {
                RepoTreeEntry(name: String($0.dropFirst(prefix.count)), path: $0, type: "file")
            }
            if entries.isEmpty { throw RepoException("missing") }
            return RepoTree(path: path, entries: entries)
        }

        func blob(_ owner: String, _ name: String, _ path: String, ref: String) async throws -> RepoBlob {
            guard let text = files[path] else { throw RepoException("missing") }
            return RepoBlob(path: path, content: text)
        }

        func compare(_ owner: String, _ name: String, base: String, head: String) async throws -> RepoComparison {
            compared.append((base, head))
            return RepoComparison(baseOid: base, headOid: head, mergeBaseOid: base, commitCount: 1, patch: "diff --git a/x b/x\n", commits: "")
        }
    }

    @MainActor func testLedgerReadsEventsInOrderAndFallsBackToCompare() async throws {
        let reader = Ledger()
        let head = String(repeating: "e", count: 40)
        reader.files[".forkmesh/pulls/5/pull.md"] = "---\ntitle: T\nbase: main\nhead: f\nauthor: K\ncreationBaseOid: \(String(repeating: "1", count: 40))\ncreationHeadOid: \(head)\n---\nBody"
        reader.files[".forkmesh/pulls/5/0002-review.md"] = "---\ntype: review\nauthor: B\nstate: approve\n---\nok"
        reader.files[".forkmesh/pulls/5/0001-comment.md"] = "---\ntype: comment\nauthor: C\n---\nfirst"
        let record = try await ReviewLedger(reader: reader, owner: "o", name: "r").load(5)
        XCTAssertEqual(record.events.map(\.type), ["comment", "review"])
        XCTAssertEqual(record.patch, "diff --git a/x b/x\n")
        XCTAssertEqual(reader.compared.first?.1, head)
        XCTAssertEqual(record.currentPatchset, head)
        reader.files[".forkmesh/pulls/5/changes.patch"] = "stored"
        let stored = try await ReviewLedger(reader: reader, owner: "o", name: "r").load(5)
        XCTAssertEqual(stored.patch, "stored")
        do {
            _ = try await ReviewLedger(reader: reader, owner: "o", name: "r").load(6)
            XCTFail("expected missing")
        } catch {
            XCTAssertTrue("\(error)".contains("#6"))
        }
    }

    @MainActor func testSettingsClientSendsOnlyChangesWithTheSession() async throws {
        let transport = FakeCollabTransport()
        transport.responder = { method, url, _ in
            if method == "GET" && url.path.hasSuffix("/about") {
                return HTTPResponse(status: 200, text: "{\"ok\":true,\"description\":\"D\",\"website\":\"\",\"fediverse\":{\"handle\":\"@r@x\",\"followers\":3,\"settings\":{\"federate\":true,\"broadcastEvents\":false,\"acceptComments\":true}}}")
            }
            if url.path.hasSuffix("/forks") && method == "POST" {
                return HTTPResponse(status: 200, text: "{\"ok\":true,\"created\":false,\"queued\":true,\"fork\":{\"owner\":\"me\",\"name\":\"r\"}}")
            }
            return HTTPResponse(status: 200, text: "{\"ok\":true,\"count\":1,\"forks\":[{\"owner\":\"a\",\"name\":\"r\"},{\"owner\":\"\"}]}")
        }
        let client = RepoSettingsClient(transport: transport, owner: "o", name: "r", sessionToken: { "TOKEN" })
        let about = try await client.about()
        XCTAssertFalse(about.broadcastEvents)
        XCTAssertEqual(about.followers, 3)
        let unchanged = try await client.saveAbout(from: about, to: about)
        XCTAssertFalse(unchanged)
        var edited = about
        edited.website = "https://x.test"
        edited.broadcastEvents = true
        let saved = try await client.saveAbout(from: about, to: edited)
        XCTAssertTrue(saved)
        let body = transport.requests.last!.body
        XCTAssertEqual(body.str("sessionToken"), "TOKEN")
        XCTAssertNil(body["description"])
        XCTAssertEqual(body.obj("fediverse")["broadcastEvents"], .bool(true))
        let forks = try await client.forks()
        XCTAssertEqual(forks.rows.map(\.owner), ["a"])
        let outcome = try await client.fork()
        XCTAssertTrue(outcome.message.contains("managed mirror"))
        XCTAssertEqual(RepoSettingsRules.validateWebsite("ftp://x"), "Enter a full http(s) address.")
        XCTAssertNil(RepoSettingsRules.validateWebsite(""))
        XCTAssertTrue(RepoSettingsRules.confirmsName(" app ", repo: "app"))
        XCTAssertEqual(RepoSettingsSection(route: "danger"), .danger)
        XCTAssertEqual(RepoSettingsSection(route: nil), .general)
    }
}
