import FMBase
import XCTest
@testable import FMApi

final class ShellBarLayoutTests: XCTestCase {
    private let items: [String] = ["home", "Agents", "Inbox", "Repos", "Chat", "notes", "timers", "settings"]

    private func keys(_ layout: ShellBarLayout, ranking: [String] = []) -> [String] {
        layout.slots(items, ranking: ranking) { $0 }.map { (slot: ShellBarSlot<String>) -> String in
            switch slot {
            case let .item(item): return item
            case let .folder(folder, members): return "\(folder.name)[\(members.joined(separator: ","))]"
            }
        }
    }

    func testAnEmptyLayoutKeepsTheBarAsItIs() {
        XCTAssertEqual(keys(ShellBarLayout()), items)
        XCTAssertTrue(ShellBarLayout().isEmpty)
    }

    func testHiddenItemsLeaveTheBarAndComeBack() {
        var layout = ShellBarLayout()
        layout.hide("Inbox")
        layout.hide("timers")
        layout.hide("Inbox")
        XCTAssertEqual(layout.hidden, ["Inbox", "timers"])
        XCTAssertEqual(keys(layout), ["home", "Agents", "Repos", "Chat", "notes", "settings"])
        layout.show("Inbox")
        XCTAssertEqual(keys(layout), ["home", "Agents", "Inbox", "Repos", "Chat", "notes", "settings"])
        layout.showAll()
        XCTAssertEqual(keys(layout), items)
    }

    func testAFolderSitsWhereItsFirstItemWasAndKeepsItsOwnOrder() {
        var layout = ShellBarLayout()
        let id = layout.createFolder("  Tools  ", with: "timers")
        layout.move("notes", toFolder: id)
        XCTAssertEqual(layout.folder(id)?.name, "Tools")
        XCTAssertEqual(keys(layout), ["home", "Agents", "Inbox", "Repos", "Chat", "Tools[timers,notes]", "settings"])
        layout.move("Agents", toFolder: id)
        XCTAssertEqual(keys(layout), ["home", "Tools[timers,notes,Agents]", "Inbox", "Repos", "Chat", "settings"])
        XCTAssertEqual(layout.folderOf("notes")?.id, id)
        layout.move("notes", toFolder: nil)
        XCTAssertNil(layout.folderOf("notes"))
        XCTAssertEqual(keys(layout), ["home", "Tools[timers,Agents]", "Inbox", "Repos", "Chat", "notes", "settings"])
    }

    func testHiddenItemsDoNotShowInFoldersAndAnEmptyFolderGoes() {
        var layout = ShellBarLayout()
        let id = layout.createFolder("Tools", with: "timers")
        layout.hide("timers")
        XCTAssertEqual(keys(layout), ["home", "Agents", "Inbox", "Repos", "Chat", "notes", "settings"])
        layout.hide("notes")
        layout.move("notes", toFolder: id)
        XCTAssertFalse(layout.isHidden("notes"))
        XCTAssertEqual(keys(layout), ["home", "Agents", "Inbox", "Repos", "Chat", "Tools[notes]", "settings"])
    }

    func testFoldersGetFreshIdsRenameAndRemoveKeepingTheirItems() {
        var layout = ShellBarLayout()
        let first = layout.createFolder("A", with: "notes")
        let second = layout.createFolder("", with: "timers")
        XCTAssertNotEqual(first, second)
        XCTAssertEqual(layout.folder(second)?.name, "Folder")
        layout.renameFolder(first, to: String(repeating: "x", count: 80))
        XCTAssertEqual(layout.folder(first)?.name.count, ShellBarLayout.nameLimit)
        layout.removeFolder(first)
        let third = layout.createFolder("C")
        XCTAssertFalse([second].contains(third))
        XCTAssertEqual(keys(layout), ["home", "Agents", "Inbox", "Repos", "Chat", "notes", "Folder[timers]", "settings"])
    }

    func testSteppingAnItemRewritesTheOrder() {
        var layout = ShellBarLayout()
        layout.step("Chat", .left, items: items, ranking: []) { $0 }
        XCTAssertEqual(keys(layout), ["home", "Agents", "Inbox", "Chat", "Repos", "notes", "timers", "settings"])
        layout.step("settings", .first, items: items, ranking: []) { $0 }
        XCTAssertEqual(keys(layout).first, "settings")
        layout.step("settings", .last, items: items, ranking: []) { $0 }
        XCTAssertEqual(keys(layout).last, "settings")
        XCTAssertFalse(layout.canStep("settings", .right, items: items, ranking: []) { $0 })
        XCTAssertTrue(layout.canStep("settings", .left, items: items, ranking: []) { $0 })
        XCTAssertFalse(layout.canStep("home", .first, items: items, ranking: []) { $0 })
    }

    func testSteppingAFolderMovesAllItsItemsAndHiddenItemsKeepTheirPlace() {
        var layout = ShellBarLayout()
        let id = layout.createFolder("Tools", with: "notes")
        layout.move("timers", toFolder: id)
        layout.hide("Inbox")
        let folderSlot = "folder:\(id)"
        layout.step(folderSlot, .first, items: items, ranking: []) { $0 }
        XCTAssertEqual(keys(layout), ["Tools[notes,timers]", "home", "Agents", "Repos", "Chat", "settings"])
        layout.show("Inbox")
        XCTAssertEqual(keys(layout).last, "Inbox")
        layout.step("timers", .left, items: items, ranking: []) { $0 }
        XCTAssertEqual(layout.folder(id)?.items, ["timers", "notes"])
        XCTAssertTrue(layout.canStep("notes", .left, items: items, ranking: []) { $0 })
        XCTAssertFalse(layout.canStep("timers", .left, items: items, ranking: []) { $0 })
    }

    func testSteppingUnderARankingKeepsWhatWasShown() {
        var layout = ShellBarLayout()
        layout.step("Chat", .left, items: items, ranking: ["settings", "notes"]) { $0 }
        XCTAssertEqual(keys(layout), ["settings", "notes", "home", "Agents", "Inbox", "Chat", "Repos", "timers"])
    }

    func testTheLayoutIsSavedAndAnEmptyOneClearsTheKey() {
        let store = InMemoryKeyValueStore()
        var layout = ShellBarLayout()
        layout.hide("Chat")
        layout.createFolder("Tools", with: "notes")
        layout.save(store)
        XCTAssertEqual(ShellBarLayout.load(store), layout)
        ShellBarLayout().save(store)
        XCTAssertFalse(store.contains(ShellBarLayout.key))
        store.set("not json", forKey: ShellBarLayout.key)
        XCTAssertEqual(ShellBarLayout.load(store), ShellBarLayout())
    }

    private func bar(_ layout: ShellBarLayout, ranking: [String] = [], keep: [String] = ["home"]) -> [String] {
        layout.barSlots(items, ranking: ranking, keep: keep) { $0 }.map { ShellBarLayout.slotId($0) { $0 } }
    }

    func testTheBarHoldsSixSlotsAndKeepsHome() {
        let layout = ShellBarLayout()
        XCTAssertEqual(bar(layout), ["home", "Agents", "Inbox", "Repos", "Chat", "notes"])
        XCTAssertTrue(layout.barIsFull(items, ranking: [], keep: ["home"]) { $0 })
        XCTAssertFalse(layout.inBar("settings", items: items, ranking: [], keep: ["home"]) { $0 })
        XCTAssertEqual(bar(layout, ranking: ["settings", "timers", "notes", "Chat", "Repos", "Inbox"]),
            ["settings", "timers", "notes", "Chat", "Repos", "home"])
        var hidden = ShellBarLayout()
        hidden.hide("home")
        XCTAssertEqual(bar(hidden).first, "home")
    }

    func testRemovingFromTheBarDoesNotPullTheNextItemIn() {
        var layout = ShellBarLayout()
        layout.removeFromBar(["Inbox", "home"], items: items, ranking: [], keep: ["home"]) { $0 }
        XCTAssertEqual(bar(layout), ["home", "Agents", "Repos", "Chat", "notes"])
        XCTAssertTrue(layout.isHidden("settings"))
        XCTAssertFalse(layout.isEmpty)
        XCTAssertTrue(layout.putInBar("settings", items: items, ranking: [], keep: ["home"]) { $0 })
        XCTAssertEqual(bar(layout), ["home", "Agents", "Repos", "Chat", "notes", "settings"])
        XCTAssertFalse(layout.putInBar("timers", items: items, ranking: [], keep: ["home"]) { $0 })
        XCTAssertFalse(layout.inBar("timers", items: items, ranking: [], keep: ["home"]) { $0 })
    }

    func testAFullBarTakesAnItemInPlaceOfAnother() {
        var layout = ShellBarLayout()
        XCTAssertTrue(layout.putInBar("settings", at: "Repos", items: items, ranking: [], keep: ["home"]) { $0 })
        XCTAssertEqual(bar(layout), ["home", "Agents", "Inbox", "settings", "Chat", "notes"])
        XCTAssertFalse(layout.putInBar("timers", at: "home", items: items, ranking: [], keep: ["home"]) { $0 })
        XCTAssertTrue(layout.putInBar("notes", at: "Agents", items: items, ranking: [], keep: ["home"]) { $0 })
        XCTAssertEqual(bar(layout), ["home", "notes", "Agents", "Inbox", "settings", "Chat"])
        XCTAssertTrue(layout.putInBar("home", at: "Chat", items: items, ranking: [], keep: ["home"]) { $0 })
        XCTAssertEqual(bar(layout), ["notes", "Agents", "Inbox", "settings", "Chat", "home"])
    }

    func testAFolderTakesOneSlotOfTheBar() {
        var layout = ShellBarLayout()
        let id = layout.createFolder("Tools", with: "timers")
        layout.move("settings", toFolder: id)
        layout.fit(items, ranking: [], keep: ["home"]) { $0 }
        XCTAssertEqual(bar(layout), ["home", "Agents", "Inbox", "Repos", "Chat", "notes"])
        XCTAssertTrue(layout.putInBar("timers", at: "notes", items: items, ranking: [], keep: ["home"]) { $0 })
        XCTAssertEqual(bar(layout), ["home", "Agents", "Inbox", "Repos", "Chat", "folder:\(id)"])
        XCTAssertTrue(layout.inBar("timers", items: items, ranking: [], keep: ["home"]) { $0 })
        XCTAssertFalse(layout.inBar("settings", items: items, ranking: [], keep: ["home"]) { $0 })
    }

    func testAnOldSavedLayoutWithoutABarStillLoads() {
        let store = InMemoryKeyValueStore()
        store.set(#"{"hidden":["Chat"],"folders":[],"order":["Inbox"]}"#, forKey: ShellBarLayout.key)
        XCTAssertEqual(ShellBarLayout.load(store), ShellBarLayout(hidden: ["Chat"], order: ["Inbox"]))
        var layout = ShellBarLayout()
        layout.fit(items, ranking: [], keep: ["home"]) { $0 }
        layout.save(store)
        XCTAssertEqual(ShellBarLayout.load(store).bar, ["home", "Agents", "Inbox", "Repos", "Chat", "notes"])
        layout.showAll()
        XCTAssertNil(layout.bar)
    }
}
