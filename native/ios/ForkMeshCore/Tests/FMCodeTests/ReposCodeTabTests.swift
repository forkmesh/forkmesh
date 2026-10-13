// Branch switching, the explorer rows and
// breadcrumbs, go to file, the file preview and the code viewer's status bar.

import FMApi
import FMBase
import FMDiff
import XCTest
@testable import FMCode

@MainActor
final class ReposCodeTabTests: XCTestCase {
    private let repo = Repository(owner: "owner", name: "repo", defaultBranch: "main")

    /// `BranchAwareApiService`.
    private func branchAware() -> FakeRepoClient {
        let client = FakeRepoClient()
        client.branchesResult = { [RepoBranch(name: "main", sha: "aaa", isDefault: true), RepoBranch(name: "dev", sha: "bbb")] }
        client.treeResult = { path, _ in
            RepoTree(
                path: path,
                entries: [
                    RepoTreeEntry(name: "README.md", path: "README.md", type: "file"),
                    RepoTreeEntry(name: "lib", path: "lib", type: "dir"),
                ],
                source: "qt-node-a")
        }
        client.searchResult = [RepoCodeSearchMatch(path: "lib/main.dart", line: 1, text: "void main")]
        return client
    }

    func testCodeTabSwitchesBranchesAndReloadsTheTreeWithTheSelectedRef() async {
        let client = branchAware()
        let model = RepoCodeTabModel(client: client, repo: repo)
        await model.start()
        XCTAssertEqual(model.branchPillLabel, "Branch: main")
        XCTAssertEqual(client.treeRefs, ["main"])
        XCTAssertEqual(model.branchNames, ["main", "dev"])
        XCTAssertFalse(model.branchPillMuted)

        await model.switchRef("dev")
        XCTAssertEqual(model.branchPillLabel, "Branch: dev")
        XCTAssertEqual(client.treeRefs.last, "dev")
        XCTAssertEqual(client.clearedCaches, 1)
        // Picking the selected branch again does nothing.
        await model.switchRef("dev")
        XCTAssertEqual(client.treeRefs.count, 2)
        XCTAssertEqual(model.branchNames, ["dev", "main"])
    }

    func testEmptyDefaultBranchLetsTheHostChooseItsDefaultRef() async {
        let client = branchAware()
        let model = RepoCodeTabModel(client: client, repo: Repository(owner: "owner", name: "repo", defaultBranch: ""))
        await model.start()
        XCTAssertEqual(model.branchPillLabel, "Branch: Default branch")
        XCTAssertEqual(client.treeRefs, [""])
    }

    func testGoToFileSearchesTheSelectedRefAndOpensWithIt() async {
        let client = branchAware()
        let search = RepoGoToFileModel(client: client, repo: repo, ref: "main")
        XCTAssertNil(search.results)
        search.query = "m"
        await search.search()
        XCTAssertNil(search.results, "one character does not search")
        search.query = " main "
        await search.search()
        XCTAssertEqual(client.searchRefs, ["main"])
        XCTAssertEqual(client.searchQueries, ["main"])
        XCTAssertEqual(search.results?.data?.map(\.path), ["lib/main.dart"])
        XCTAssertEqual(search.scopeLine, "Searching owner/repo on main.")
        XCTAssertEqual(RepoGoToFileModel.matchSubtitle(search.results!.data![0]), "1: void main")
        XCTAssertNil(RepoGoToFileModel.matchSubtitle(RepoCodeSearchMatch(path: "a")))
        XCTAssertEqual(RepoGoToFileModel.matchSubtitle(RepoCodeSearchMatch(path: "a", text: "x")), "x")

        let file = RepoFileModel(client: client, repo: repo, path: "lib/main.dart", ref: "main")
        await file.start()
        XCTAssertEqual(file.fileName, "main.dart")
        XCTAssertEqual(client.blobRefs, ["main"])
        XCTAssertTrue(file.editable)
    }

    func testGoToFileExplainsAnEmptyResult() async {
        let client = FakeRepoClient()
        let search = RepoGoToFileModel(client: client, repo: repo, ref: "dev")
        search.query = "zz"
        await search.search()
        XCTAssertEqual(search.results?.data ?? [], [])
        XCTAssertEqual(search.noMatchesMessage, "No code results for \"zz\".")
    }

    func testCodeAndFilePreviewSurfaceSourceContext() async {
        let client = branchAware()
        let model = RepoCodeTabModel(client: client, repo: repo)
        await model.start()
        XCTAssertEqual(model.sourceLabel, "served by qt-node-a")

        let file = RepoFileModel(client: client, repo: repo, path: "README.md", ref: "main")
        await file.start()
        XCTAssertEqual(RepoDisplay.sourceLabel(file.loadedBlob.source), "served by qt-node-a")
        XCTAssertEqual(RepoFileModel.preview(for: file.loadedBlob), .text("void main() {}"))
    }

    func testUnsupportedPreviewShowsPathAndRefAwareRawURL() async {
        let client = branchAware()
        client.nextBlob = RepoBlob(path: "README.md", content: "AAAB", encoding: "base64", source: "qt-node-a")
        let file = RepoFileModel(client: client, repo: repo, path: "README.md", ref: "main")
        await file.start()
        XCTAssertTrue(file.rawURL.contains("ref=main"))
        guard case .unsupported(let message) = RepoFileModel.preview(for: file.loadedBlob) else {
            return XCTFail("a NUL byte is binary")
        }
        XCTAssertTrue(message.contains("binary file"))
    }

    func testPreviewKindsAndDecoding() {
        XCTAssertEqual(RepoDisplay.previewKind("a/B.PNG"), .image)
        XCTAssertEqual(RepoDisplay.previewKind("clip.webm"), .video)
        XCTAssertEqual(RepoDisplay.previewKind("main.swift"), .text)
        XCTAssertEqual(RepoFileModel.preview(for: RepoBlob(path: "a.png", content: "")), .image)
        XCTAssertEqual(
            RepoFileModel.preview(for: RepoBlob(path: "a.mov", content: "")), .unsupported(message: RepoFileModel.videoMessage))
        XCTAssertEqual(RepoFileModel.preview(for: RepoBlob(path: "a.txt", content: "")), .text("Empty file."))
        // Base64 with line breaks, as hosts send it.
        XCTAssertEqual(
            RepoFileModel.preview(for: RepoBlob(path: "a.txt", content: "aGVs\nbG8=", encoding: "base64")), .text("hello"))
        XCTAssertEqual(
            RepoFileModel.preview(for: RepoBlob(path: "a.txt", content: "!!!", encoding: "BASE64")),
            .unsupported(message: RepoFileModel.binaryMessage))
        XCTAssertNil(RepoDisplay.decodedText(RepoBlob(path: "a", content: "a\u{0}b")))
        XCTAssertFalse(RepoFileModel(client: FakeRepoClient(), repo: repo, path: "logo.png").editable)
    }

    /// `ExplorerApiService`.
    private func explorer(now: Date) -> FakeRepoClient {
        let client = FakeRepoClient()
        client.branchesResult = {
            [
                RepoBranch(name: "feature/x", sha: "bbbbbbb1234"),
                RepoBranch(name: "main", sha: "aaaaaaa1234", isDefault: true),
                RepoBranch(name: "agent/wt", sha: "ccccccc1234", worktreePath: "/tmp/wt"),
            ]
        }
        let nowMs = Int(now.epochMillis)
        client.treeResult = { path, _ in
            if path == "lib" {
                return RepoTree(path: path, entries: [
                    RepoTreeEntry(name: "main.dart", path: "lib/main.dart", type: "file", size: 1536, updatedMs: nowMs - 3 * 3_600_000),
                ])
            }
            return RepoTree(
                path: path,
                entries: [
                    RepoTreeEntry(name: "lib", path: "lib", type: "dir", updatedMs: nowMs - 2 * 86_400_000),
                    RepoTreeEntry(name: "README.md", path: "README.md", type: "file", size: 2048),
                    RepoTreeEntry(name: "tiny.txt", path: "tiny.txt", type: "file", size: 12),
                ],
                source: "qt-node-a")
        }
        client.nextBlob = RepoBlob(path: "README.md", content: "void main() {}\n")
        return client
    }

    func testExplorerRowsShowKindSizeAndLastCommitAge() async {
        let now = Date()
        let client = explorer(now: now)
        let model = RepoCodeTabModel(
            client: client,
            repo: Repository(owner: "owner", name: "repo", description: "Mesh-hosted repository", defaultBranch: "main"))
        await model.start()
        XCTAssertTrue(model.showDescription)
        let entries = model.tree.data?.entries ?? []
        XCTAssertEqual(entries.map { RepoCodeTabModel.entryMeta($0, now: now) }, ["Directory · 2d ago", "2.0 KB", "12 B"])
        XCTAssertEqual(entries.map(RepoCodeTabModel.entryGlyph), ["folder_outlined", "article_outlined", "article_outlined"])
        XCTAssertEqual(model.crumbs, [RepoCrumb(label: "Root", path: "", current: true)])
    }

    func testBreadcrumbsTrackTheOpenFolderAndStepBackUp() async {
        let now = Date()
        let client = explorer(now: now)
        let model = RepoCodeTabModel(client: client, repo: repo)
        await model.start()
        XCTAssertFalse(model.canGoUp)

        await model.openDir("lib")
        XCTAssertEqual(model.crumbs.map(\.label), ["Root", "lib"])
        XCTAssertEqual(model.crumbs.map(\.current), [false, true])
        XCTAssertEqual(model.tree.data?.entries.map(\.name), ["main.dart"])
        XCTAssertEqual(RepoCodeTabModel.entryMeta(model.tree.data!.entries[0], now: now), "1.5 KB · 3h ago")
        XCTAssertFalse(model.showDescription)

        await model.up()
        XCTAssertEqual(model.path, "")
        XCTAssertEqual(model.tree.data?.entries.map(\.name), ["lib", "README.md", "tiny.txt"])
        XCTAssertTrue(model.showDescription)

        await model.openDir("lib")
        await model.openDir(model.crumbs[0].path)
        XCTAssertEqual(model.tree.data?.entries.first?.name, "lib")
        XCTAssertEqual(client.treePaths, ["", "lib", "", "lib", ""])
    }

    func testNestedCrumbsCarryTheirPaths() async {
        let model = RepoCodeTabModel(client: FakeRepoClient(), repo: repo)
        await model.openDir("a/b/c")
        XCTAssertEqual(model.crumbs.map(\.path), ["", "a", "a/b", "a/b/c"])
        await model.up()
        XCTAssertEqual(model.path, "a/b")
    }

    func testTreeFailureOffersRetryThatDropsTheCache() async {
        let client = FakeRepoClient()
        client.treeResult = { _, _ in throw ApiException("Repo host is offline.") }
        let model = RepoCodeTabModel(client: client, repo: repo)
        await model.start()
        XCTAssertEqual(model.treeErrorMessage, "Could not load files: Exception: Repo host is offline.")
        await model.openDir(model.path, refresh: true)
        XCTAssertEqual(client.clearedCaches, 1)
    }

    func testFileViewerStatusBarNamesTheLanguage() {
        let preview = RepoCodePreview(path: "README.md", content: "void main() {}\n")
        XCTAssertEqual(preview.languageLabel, "Markdown")
        XCTAssertEqual(preview.statusLine(activeLine: 1), "Ln 1 · 1 line · 15 B")
        let swift = RepoCodePreview(path: "a.swift", content: "let a = 1\r\n// b\n")
        XCTAssertEqual(swift.lineCount, 2)
        XCTAssertEqual(swift.statusLine(activeLine: 2), "Ln 2 · 2 lines · 16 B")
        XCTAssertEqual(swift.pieces(1).map(\.kind), [.comment])
        XCTAssertEqual(swift.gutterWidth, 44)
        XCTAssertEqual(swift.estimatedWidth, 9 * 8.2 + 44 + 40, accuracy: 0.0001)
    }
}
