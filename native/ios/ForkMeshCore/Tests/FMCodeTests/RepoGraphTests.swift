import FMApi
import FMBase
import XCTest
@testable import FMCode

@MainActor
final class RepoGraphTests: XCTestCase {
    private let commits: [JSONObject] = [
        ["hash": "aaaaaaa0000000000000", "parents": "bbbbbbb0000000000000 ccccccc0000000000000", "subject": "Merge feature"],
        ["hash": "ccccccc0000000000000", "parents": "ddddddd0000000000000", "subject": "Feature work"],
        ["hash": "bbbbbbb0000000000000", "parents": "ddddddd0000000000000", "subject": "Main work"],
        ["hash": "ddddddd0000000000000", "parents": "", "subject": "Root"],
    ]

    func testMarksLandOnTheirCommitsWithTheDistanceFromTheTip() {
        let marks = [
            RepoSurfaceMark(surface: .desktop, commit: "AAAAAAA00000"),
            RepoSurfaceMark(surface: .web, commit: "ccccccc00000-dirty-1700000000"),
            RepoSurfaceMark(surface: .mobile, commit: "ddddddd0000000000000"),
        ].compactMap { $0 }
        let graph = RepoGraph.build(commits, marks: marks)
        XCTAssertEqual(graph.rows.map(\.marks), [[.desktop], [.web], [], [.mobile]])
        XCTAssertEqual(graph.lanes, 2)
        XCTAssertTrue(graph.rows[0].graph.isMerge)
        XCTAssertEqual(graph.spots.map(\.place), [.tip, .behind(2), .behind(3)])
        XCTAssertEqual(graph.spots.map(\.detail), ["At the tip", "2 commits behind", "3 commits behind"])
        XCTAssertTrue(graph.marked)
    }

    func testUnknownAndOutsideMarks() {
        let outside = RepoSurfaceMark(surface: .web, commit: "eeeeeee1234")
        let graph = RepoGraph.build(commits, marks: [outside].compactMap { $0 })
        XCTAssertEqual(graph.spots.map(\.place), [.unknown, .outside, .unknown])
        XCTAssertFalse(graph.marked)
        XCTAssertTrue(RepoCommitsTabModel.showsSurfaces(graph, repo: Repository(owner: "o", name: "ForkMesh")))
        XCTAssertFalse(RepoCommitsTabModel.showsSurfaces(graph, repo: Repository(owner: "o", name: "other")))
    }

    func testMarkRejectsNonCommits() {
        XCTAssertNil(RepoSurfaceMark(surface: .desktop, commit: "unknown"))
        XCTAssertNil(RepoSurfaceMark(surface: .desktop, commit: "dirty"))
        XCTAssertNil(RepoSurfaceMark(surface: .web, commit: "dev"))
        XCTAssertNil(RepoSurfaceMark(surface: .web, commit: "abc12"))
        XCTAssertEqual(RepoSurfaceMark(surface: .web, commit: " ABCDEF123456-dirty ")?.commit, "abcdef123456")
    }

    func testMarksModelLoadsEverySurface() async {
        let model = RepoSurfaceMarksModel(loaders: [
            .mobile: { "ddddddd0000000000000" },
            .web: { RepoSurfaceMarksModel.webCommit(["rev": "bbbbbbb00000"]) },
            .desktop: { RepoSurfaceMarksModel.desktopCommit(["version": "1.0"]) },
        ])
        await model.load()
        XCTAssertTrue(model.loaded)
        XCTAssertEqual(model.marks.map(\.surface), [.web, .mobile])
        XCTAssertEqual(model.marks.map(\.short), ["bbbbbbb", "ddddddd"])
    }

    func testAppBuildReadsTheCommit() {
        let build = AppBuild(infoDictionary: ["CFBundleShortVersionString": "1.4.0", "FORKMESH_BUILD_COMMIT": "abc1234def"])
        XCTAssertEqual(build.commit, "abc1234def")
        XCTAssertEqual(AppBuild(infoDictionary: ["FORKMESH_BUILD_COMMIT": "$(FORKMESH_BUILD_COMMIT)"]).commit, "")
        XCTAssertEqual(AppBuild(infoDictionary: nil).commit, "")
    }
}
