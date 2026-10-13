import Foundation
import XCTest

@testable import FMApi

final class PageReturnTests: XCTestCase {
    let repo = RepoRef(owner: "forkmesh", name: "app")

    func testBackFromAPagePickedOnTheMenuReturnsToTheMenu() {
        let back = PageReturn.after(leaving: "menu", fromTab: .agents, fromPath: [], tab: .agents, path: [.notes])
        XCTAssertEqual(back, PageReturn(page: "menu", tab: .agents, landing: [.notes]))
        XCTAssertTrue(back?.holds(tab: .agents, path: [.notes]) ?? false)
        XCTAssertTrue(back?.holds(tab: .agents, path: [.notes, .timers]) ?? false)
        XCTAssertFalse(back?.restores(tab: .agents, path: [.notes, .timers]) ?? true)
        XCTAssertFalse(back?.restores(tab: .agents, path: [.notes]) ?? true)
        XCTAssertTrue(back?.restores(tab: .agents, path: []) ?? false)
    }

    func testARepositorySectionReturnsWhenPoppedToTheRepository() {
        let landing: [Route] = [.repo(repo, section: nil), .repo(repo, section: .pulls)]
        let back = PageReturn.after(leaving: "menu", fromTab: .inbox, fromPath: [], tab: .code, path: landing)
        XCTAssertTrue(back?.restores(tab: .code, path: [.repo(repo, section: nil)]) ?? false)
        XCTAssertFalse(back?.restores(tab: .inbox, path: []) ?? true)
        XCTAssertFalse(back?.holds(tab: .inbox, path: []) ?? true)
    }

    func testAPickThatLeavesNothingToGoBackFromKeepsNoReturn() {
        XCTAssertNil(PageReturn<String>.after(leaving: nil, fromTab: .agents, fromPath: [], tab: .agents, path: [.notes]))
        XCTAssertNil(PageReturn.after(leaving: "menu", fromTab: .inbox, fromPath: [], tab: .agents, path: []))
        XCTAssertNil(PageReturn.after(leaving: "menu", fromTab: .agents, fromPath: [.notes], tab: .agents, path: [.notes]))
    }

    func testAnotherPathDropsTheReturn() {
        let back = PageReturn(page: "menu", tab: .agents, landing: [.notes])
        XCTAssertFalse(back.holds(tab: .agents, path: [.timers]))
        XCTAssertFalse(back.restores(tab: .agents, path: [.timers]))
    }
}
