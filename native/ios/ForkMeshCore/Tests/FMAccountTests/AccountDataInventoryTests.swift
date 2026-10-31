import FMBase
import Foundation
import XCTest
@testable import FMAccount

final class AccountDataInventoryTests: XCTestCase {
    static let payload: JSONObject = [
        "ok": true,
        "generatedAt": 5,
        "subjects": [
            ["id": "account", "kind": "account", "label": "Your account", "tables": 2, "rows": 3, "nodes": ["alice-laptop"]],
            ["id": "org:acme", "kind": "organization", "label": "acme", "role": "member", "readable": false, "tables": 1, "rows": 1],
        ],
        "groups": [
            ["subject": "account", "table": "notifications", "label": "Notifications", "category": "Notifications", "rows": 2, "deletable": true],
            ["subject": "account", "table": "users", "label": "Your account record", "category": "Account", "rows": 1],
            ["subject": "account", "table": "steps_keys", "label": "Steps keys", "category": "Other", "rows": 1, "withheld": true],
            ["subject": "org:acme", "table": "orgs", "label": "Organization record", "category": "Organizations", "rows": 1, "readable": false],
        ],
        "store": [["key": "ios.home", "kind": "layout", "label": "Home screen and bar layout", "bytes": 120, "updatedAt": 9]],
    ]

    func testParsesSubjectsGroupsAndStore() {
        let inventory = AccountDataInventory.parse(Self.payload)
        XCTAssertEqual(inventory.subjects.map(\.id), ["account", "org:acme"])
        XCTAssertEqual(inventory.subjects[1].title, "Organization acme")
        XCTAssertTrue(inventory.subjects[0].subtitle.contains("alice-laptop"))
        XCTAssertTrue(inventory.subjects[1].subtitle.contains("owners and admins"))
        XCTAssertEqual(inventory.store.first?.title, "Home screen and bar layout")
        XCTAssertEqual(inventory.totalRows, 6)
        let sections = inventory.groups(for: "account")
        XCTAssertEqual(sections.map(\.category), ["Account", "Notifications", "Other"])
        XCTAssertTrue(sections[2].groups[0].note.contains("never shown"))
    }

    func testRemovingRowsDropsAnEmptiedGroup() {
        let inventory = AccountDataInventory.parse(Self.payload)
        let once = inventory.removing(group: "account/notifications", rows: 1)
        XCTAssertEqual(once.groups.first { $0.table == "notifications" }?.rows, 1)
        let twice = once.removing(group: "account/notifications", rows: 1)
        XCTAssertNil(twice.groups.first { $0.table == "notifications" })
        XCTAssertTrue(twice.removingStore("ios.home").store.isEmpty)
    }

    func testRowTitlesFieldsAndTimes() {
        let row = AccountDataRow([
            "rowid": 4, "deletable": true,
            "fields": ["recipient_bi": "(hashed identifier)", "ts": 1_760_000_000_000, "data": ["title": "Mention", "body": "hi"]],
        ], index: 0)
        XCTAssertEqual(row.id, "4")
        XCTAssertTrue(row.deletable)
        XCTAssertEqual(row.title, "Mention")
        XCTAssertEqual(row.fields.first { $0.name == "ts" }?.text, "2025-10-09 08:53 UTC")
        XCTAssertTrue(row.fields.first { $0.name == "data" }?.isStructured ?? false)
        let bare = AccountDataRow(["fields": ["kind": "(withheld: security credential)"]], index: 2)
        XCTAssertEqual(bare.id, "i2")
        XCTAssertFalse(bare.deletable)
        XCTAssertEqual(bare.title, "Item")
    }

    func testPathsEncodeQueryValues() {
        XCTAssertEqual(AccountDataPaths.rows(subject: "org:acme", table: "orgs", after: 0),
                       "/api/accounts/data/rows?subject=org%3Aacme&table=orgs")
        XCTAssertEqual(AccountDataPaths.rows(subject: "account", table: "notifications", after: 50),
                       "/api/accounts/data/rows?subject=account&table=notifications&after=50")
        XCTAssertEqual(AccountDataPaths.row(table: "notifications", rowid: 7), "/api/accounts/data/rows/notifications/7")
        XCTAssertEqual(AccountDataPaths.storeItem("ios.home"), "/api/accounts/data/store/ios.home")
    }

    @MainActor func testBrowserLoadsPagesAndDeletesARow() async {
        let fake = FakeAccountEndpoints()
        fake.answers["GET /api/accounts/data"] = .success(Self.payload)
        let path = AccountDataPaths.rows(subject: "account", table: "notifications", after: 0)
        fake.answers["GET \(path)"] = .success([
            "ok": true, "readable": true, "next": 9,
            "rows": [["rowid": 3, "deletable": true, "fields": ["data": ["title": "One"]]]],
        ])
        fake.answers["GET \(AccountDataPaths.rows(subject: "account", table: "notifications", after: 9))"] = .success([
            "ok": true, "readable": true, "next": 0,
            "rows": [["rowid": 9, "deletable": true, "fields": ["data": ["title": "Two"]]]],
        ])
        let browser = AccountDataBrowser(client: fake)
        await browser.load()
        let group = browser.inventory!.groups.first { $0.table == "notifications" }!
        await browser.loadRows(group)
        await browser.loadRows(group, more: true)
        XCTAssertEqual(browser.page(group)?.rows.map(\.title), ["One", "Two"])
        XCTAssertEqual(browser.page(group)?.next, 0)
        let deleted = await browser.delete(browser.page(group)!.rows[0], in: group)
        XCTAssertTrue(deleted)
        XCTAssertEqual(fake.calls.last?.method, "DELETE")
        XCTAssertEqual(fake.calls.last?.path, "/api/accounts/data/rows/notifications/3")
        XCTAssertEqual(browser.page(group)?.rows.map(\.title), ["Two"])
        XCTAssertEqual(browser.inventory?.groups.first { $0.table == "notifications" }?.rows, 1)
    }

    @MainActor func testBrowserSaysWhenTheServerIsOld() async {
        let fake = FakeAccountEndpoints()
        fake.answers["GET /api/accounts/data"] = .failure(AccountEndpointError("http_404", status: 404))
        let browser = AccountDataBrowser(client: fake)
        await browser.load()
        XCTAssertEqual(browser.error, "This server cannot list your data yet.")
    }
}

final class AccountSyncTests: XCTestCase {
    @MainActor final class FakeSource: AccountSyncSource {
        let key = "ios.rss"
        let kind = "subscriptions"
        let label = "News"
        var value: JSONValue?
        var restored: [JSONValue] = []
        init(_ value: JSONValue?) { self.value = value }
        func snapshot() -> JSONValue? { value }
        func restore(_ value: JSONValue) -> Int {
            restored.append(value)
            return 1
        }
    }

    @MainActor func testPreferenceSourcesSplitHomeFromSettingsAndSkipSecrets() {
        let store = InMemoryKeyValueStore()
        store.set("{\"columns\":4}", forKey: "shell/home-style")
        store.set(["a", "b"], forKey: "shell/bar-recent")
        store.set("dark", forKey: "appearance/themeMode")
        store.set(true, forKey: "device/haptics")
        store.set("secret", forKey: "auth/session")
        store.set("room", forKey: "p/one/server/room")
        store.set(String(repeating: "x", count: AccountSyncKeys.maxValueChars + 1), forKey: "chat/channelGroups")
        let home = PreferenceSyncSource.home(store).snapshot()?.obj("values")
        XCTAssertEqual(home?.keys, ["shell/home-style"])
        let settings = PreferenceSyncSource.settings(store).snapshot()?.obj("values")
        XCTAssertEqual(Set(settings?.keys ?? []), ["appearance/themeMode", "device/haptics"])
        let fresh = InMemoryKeyValueStore()
        let source = PreferenceSyncSource.settings(fresh)
        let restored = source.restore(.object([
            "type": "forkmesh.mobile-settings", "schemaVersion": 1,
            "values": ["appearance/themeMode": "light", "auth/session": "stolen", "shell/home-style": "{}"],
        ]))
        XCTAssertEqual(restored, 1)
        XCTAssertEqual(fresh.string(forKey: "appearance/themeMode"), "light")
        XCTAssertNil(fresh.string(forKey: "auth/session"))
        XCTAssertNil(fresh.string(forKey: "shell/home-style"))
    }

    @MainActor func testSavesOnlyChangedSourcesWhileEnabled() async {
        let fake = FakeAccountEndpoints()
        let prefs = InMemoryKeyValueStore()
        let source = FakeSource(.object(["feeds": ["https://example.com/rss"]]))
        let sync = AccountSyncService(client: fake, prefs: prefs, sources: [source], device: "iPhone",
                                      clock: FixedClock(epochMillis: 1_000), account: { "alice" })
        let none = await sync.saveIfChanged()
        XCTAssertEqual(none, 0)
        XCTAssertTrue(fake.calls.isEmpty)
        sync.enabled = true
        let first = await sync.saveIfChanged()
        XCTAssertEqual(first, 1)
        XCTAssertEqual(fake.calls.last?.method, "PUT")
        XCTAssertEqual(fake.calls.last?.path, "/api/accounts/data/store/ios.rss")
        XCTAssertEqual(fake.calls.last?.body?.str("kind"), "subscriptions")
        XCTAssertEqual(fake.calls.last?.body?.str("device"), "iPhone")
        XCTAssertEqual(sync.lastSavedAt, 1_000)
        let again = await sync.saveIfChanged()
        XCTAssertEqual(again, 0)
        source.value = .object(["feeds": []])
        let changed = await sync.saveIfChanged()
        XCTAssertEqual(changed, 1)
        XCTAssertEqual(fake.calls.count, 2)
        let forced = await sync.saveNow()
        XCTAssertEqual(forced, 1)
        sync.enabled = false
        XCTAssertFalse(prefs.keys.contains { $0.hasPrefix("accountSync/hash/") })
    }

    @MainActor func testRestoreAppliesTheSavedCopyAndReportsAnEmptyAccount() async {
        let fake = FakeAccountEndpoints()
        let source = FakeSource(nil)
        let sync = AccountSyncService(client: fake, prefs: InMemoryKeyValueStore(), sources: [source], account: { "alice" })
        fake.answers["GET /api/accounts/data/store/ios.rss"] = .failure(AccountEndpointError("not_found", status: 404))
        let nothing = await sync.restore()
        XCTAssertEqual(nothing, 0)
        XCTAssertEqual(sync.error, "Nothing is saved on ForkMesh yet.")
        fake.answers["GET /api/accounts/data/store/ios.rss"] = .success(["ok": true, "item": ["key": "ios.rss", "value": ["feeds": ["x"]]]])
        let applied = await sync.restore()
        XCTAssertEqual(applied, 1)
        XCTAssertEqual(sync.error, "")
        XCTAssertEqual(source.restored, [.object(["feeds": ["x"]])])
    }

    @MainActor func testOldServerStopsSaving() async {
        let fake = FakeAccountEndpoints()
        fake.answers["PUT /api/accounts/data/store/ios.rss"] = .failure(AccountEndpointError("http_404", status: 404))
        let sync = AccountSyncService(client: fake, prefs: InMemoryKeyValueStore(), sources: [FakeSource(.int(1))], account: { "alice" })
        let saved = await sync.saveNow()
        XCTAssertEqual(saved, 0)
        XCTAssertEqual(sync.error, "This server cannot save your data yet.")
    }
}
