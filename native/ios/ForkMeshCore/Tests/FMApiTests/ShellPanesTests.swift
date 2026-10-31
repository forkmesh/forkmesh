import XCTest

@testable import FMApi

final class ShellPanesTests: XCTestCase {
    let repo = RepoRef(owner: "forkmesh", name: "app")

    func testPhonesAndTheCoverScreenKeepTheStack() {
        XCTAssertEqual(ShellPanes.metrics(width: 402, height: 874, pad: false), .phone)
        XCTAssertEqual(ShellPanes.metrics(width: 440, height: 956, pad: false), .phone)
        XCTAssertEqual(ShellPanes.metrics(width: 466, height: 678, pad: false), .phone)
    }

    func testTheInnerDisplaySplitsInBothPoses() {
        let portrait: ShellPaneMetrics = ShellPanes.metrics(width: 669, height: 951, pad: false)
        XCTAssertTrue(portrait.wide)
        XCTAssertFalse(portrait.rail)
        XCTAssertEqual(portrait.list, 280)
        let landscape: ShellPaneMetrics = ShellPanes.metrics(width: 951, height: 669, pad: false)
        XCTAssertTrue(landscape.wide)
        XCTAssertTrue(landscape.rail)
        XCTAssertEqual(landscape.list, 350)
    }

    func testALandscapePhoneIsNotWide() {
        XCTAssertEqual(ShellPanes.metrics(width: 956, height: 440, pad: false), .phone)
        XCTAssertFalse(ShellPanes.roomy(width: 956, height: 440))
        XCTAssertTrue(ShellPanes.roomy(width: 951, height: 669))
        XCTAssertFalse(ShellPanes.roomy(width: 466, height: 678))
    }

    func testThePadKeepsItsSidebar() {
        XCTAssertEqual(ShellPanes.metrics(width: 1024, height: 1366, pad: true), .phone)
    }

    func testTheOpenDetailNamesItsRow() {
        let layout: ShellPaneLayout = ShellPanes.layout(
            path: [.pull(repo, number: 4, tab: .conversation)], tab: .code, wide: true)
        let open: Route? = ShellPanes.opened(layout)
        XCTAssertEqual(open, .pull(repo, number: 4, tab: .conversation))
        XCTAssertTrue(ShellPanes.opens(open, owners: ["ForkMesh"], name: "App"))
        XCTAssertTrue(ShellPanes.opens(open, owners: ["node-7f3a", "forkmesh"], name: "app"))
        XCTAssertFalse(ShellPanes.opens(open, owners: ["forkmesh"], name: "web"))
        XCTAssertNil(ShellPanes.opened(ShellPanes.layout(path: [], tab: .code, wide: false)))
        XCTAssertFalse(ShellPanes.opens(nil, owners: ["forkmesh"], name: "app"))
    }

    func testListWidthStaysInBounds() {
        XCTAssertEqual(ShellPanes.listWidth(content: 500), 280)
        XCTAssertEqual(ShellPanes.listWidth(content: 2000), 380)
    }

    func testNarrowAndChatStayAStack() {
        XCTAssertEqual(ShellPanes.layout(path: [.tasks(filter: nil)], tab: .agents, wide: false), .stack)
        XCTAssertEqual(ShellPanes.layout(path: [], tab: .chat, wide: true), .stack)
    }

    func testTabRoutesOpenBesideTheTabList() {
        XCTAssertEqual(ShellPanes.layout(path: [], tab: .code, wide: true), .split(list: nil, detail: []))
        let path: [Route] = [.repo(repo, section: nil), .repo(repo, section: .pulls)]
        XCTAssertEqual(ShellPanes.layout(path: path, tab: .code, wide: true), .split(list: nil, detail: path))
    }

    func testListHubPagesTakeTheListColumn() {
        let path: [Route] = [.tasks(filter: nil), .task(id: "t1")]
        XCTAssertEqual(
            ShellPanes.layout(path: path, tab: .agents, wide: true),
            .split(list: .tasks(filter: nil), detail: [.task(id: "t1")]))
    }

    func testOtherHubPagesUseTheWholeWidth() {
        XCTAssertEqual(ShellPanes.layout(path: [.stats], tab: .agents, wide: true), .full)
    }

    func testPicksFromTheListReplaceTheDetail() {
        let layout: ShellPaneLayout = .split(list: .tasks(filter: nil), detail: [.task(id: "t1")])
        XCTAssertEqual(ShellPanes.picked([.task(id: "t2")], layout: layout), [.tasks(filter: nil), .task(id: "t2")])
        XCTAssertNil(ShellPanes.picked([], layout: layout))
        XCTAssertEqual(ShellPanes.detailPath([], layout: layout), [.tasks(filter: nil)])
        XCTAssertEqual(ShellPanes.detailPath([.note(id: "n")], layout: .split(list: nil, detail: [])), [.note(id: "n")])
    }
}
