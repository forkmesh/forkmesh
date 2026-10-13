import FMApi
import FMBase
import FMChat
import FMControl
import Foundation
import XCTest

@testable import FMInsights

@MainActor
final class FakeDesktopChannel: DesktopControlCalling {
    var requests: [(String, JSONObject)] = []
    var replies: [String: JSONObject] = [:]
    var failure: Error?

    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        requests.append((method, params))
        if let failure { throw failure }
        return replies[method] ?? [:]
    }

    func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID { UUID() }
    func removeEventTap(_ id: UUID) {}
}

let errorsJSON = """
{"ok":true,"total":4,"hours":[0,0,1,3],"groupCount":2,"usersHit":2,"truncated":false,"groups":[
 {"status":"500","method":"GET","path":"/api/repo/a/b/tree","message":"boom","source":"server","count":3,
  "firstSeen":10,"lastSeen":20,"hours":[1,2],"users":[{"name":"alice","count":2}],"userCount":1,"anonymous":1},
 {"status":"520","method":"JS","path":"/client-error/chat","message":"","source":"javascript","count":1,"users":[],"userCount":0,"anonymous":1}]}
"""

final class AdminModelTests: XCTestCase {
    func testErrorGroupsParseAndDescribeThemselves() {
        let report = ErrorReport(json: object(errorsJSON))
        XCTAssertEqual(report.groups.count, 2)
        let first = report.groups[0]
        XCTAssertEqual(first.requestLine, "GET /api/repo/a/b/tree")
        XCTAssertEqual(first.affected, "1 user · 1 anonymous")
        XCTAssertEqual(first.users.first?.name, "alice")
        XCTAssertEqual(report.groups[1].title, "HTTP 520")
        XCTAssertEqual(report.groups[1].sourceLabel, "Browser")
        XCTAssertEqual(report.filtered(source: "javascript").count, 1)
        XCTAssertEqual(report.filtered(source: nil).count, 2)
        let prompt = first.agentPrompt(host: "app.next.forkmesh.com")
        XCTAssertTrue(prompt.contains("app.next.forkmesh.com"))
        XCTAssertTrue(prompt.contains("Occurrences in the last 24 hours: 3"))
        XCTAssertFalse(prompt.contains("alice"))
    }

    func testUsersParseAdminFieldsAndFilter() {
        let users = AdminUsers(json: object("""
        {"ok":true,"users":[{"name":"amy","email":"amy@x.io","isAdmin":true,"emailVerified":true,"createdAt":200},
                            {"name":"bob","email":"b@y.io","emailVerified":false,"createdAt":1700000000000,"nodes":["n1","n2"]}],
         "quarantinedUsers":[{"name":"eve","accountStatus":"active","createdAt":5}]}
        """))
        XCTAssertEqual(users.users.count, 2)
        XCTAssertEqual(users.users[0].badges, ["Admin"])
        XCTAssertEqual(users.users[1].badges, ["Unverified"])
        XCTAssertEqual(users.users[1].nodes, 2)
        XCTAssertEqual(users.users[0].createdAt, 200_000)
        XCTAssertEqual(users.filtered("", showQuarantined: false).map(\.name), ["bob", "amy"])
        XCTAssertEqual(users.filtered("y.io", showQuarantined: false).map(\.name), ["bob"])
        XCTAssertEqual(users.filtered("", showQuarantined: true).first?.badges, ["Quarantined", "Unverified"])
    }

    func testDatabaseInventoryAndRowsParse() {
        let inventory = DatabaseInventory(json: object("""
        {"ok":true,"sampledAt":9,"tables":[{"name":"b","rows":2,"columns":[{"name":"id","type":"INTEGER"}]},
         {"name":"a","rows":5,"restricted":true,"gaps":["plaintext secret-named column: token"]}]}
        """))
        XCTAssertEqual(inventory.totalRows, 7)
        XCTAssertEqual(inventory.filtered("").map(\.name), ["a", "b"])
        XCTAssertEqual(inventory.filtered("B").map(\.name), ["b"])
        XCTAssertTrue(inventory.tables[1].restricted)
        let page = DatabasePage(json: object("""
        {"ok":true,"table":"feeds","columns":[{"name":"id"},{"name":"data"},{"name":"note"}],
         "rows":[{"rowid":7,"digest":"d","values":{"id":7,"data":null,"note":null},"dataMode":"sealed","data":{"k":"v"}}],
         "nextCursor":"7"}
        """))
        XCTAssertEqual(page.rows.count, 1)
        XCTAssertEqual(page.rows[0].id, "7")
        XCTAssertEqual(page.rows[0].cells.map(\.value), ["7", "(decoded below)", "NULL"])
        XCTAssertTrue(page.rows[0].data.contains("\"k\""))
        XCTAssertEqual(page.nextCursor, "7")
    }

    func testDatabaseGridSizesColumnsToTheirContent() {
        let page = DatabasePage(json: object("""
        {"ok":true,"table":"t","columns":[{"name":"id"},{"name":"label"},{"name":"blob"}],
         "rows":[{"rowid":12345,"values":{"id":1,"label":"two\\nlines","blob":"\(String(repeating: "x", count: 90))"}}]}
        """))
        XCTAssertEqual(DatabaseGrid.widths(columns: page.columns, rows: page.rows), [3, 9, DatabaseGrid.maxChars])
        XCTAssertEqual(DatabaseGrid.widths(columns: page.columns, rows: []), [3, 5, 4])
        XCTAssertEqual(DatabaseGrid.rowidWidth(page.rows), 5)
        XCTAssertEqual(DatabaseGrid.line("two\nlines"), "two lines")
    }

    func testCronHealth() {
        XCTAssertEqual(CronSummary(json: object(#"{"ok":true,"summary":{"ticks":5,"failedTicks":0,"lastTickAt":10,"gapMs":1000}}"#)).health, .good)
        XCTAssertEqual(CronSummary(json: object(#"{"ok":true,"summary":{"ticks":5,"failedTicks":2,"lastTickAt":10,"gapMs":1000}}"#)).health, .warning)
        XCTAssertEqual(CronSummary(json: object(#"{"ok":true,"summary":{"lastTickAt":10,"gapMs":3600000}}"#)).health, .critical)
        XCTAssertEqual(CronSummary(json: object(#"{"ok":false,"error":"cron_report_unavailable"}"#)).unavailable, "cron_report_unavailable")
    }

    func testStorageFormatsBytes() {
        let usage = StorageUsage(json: object(#"{"ok":true,"storage":{"databaseBytes":1536,"tables":12,"engine":"rqlite","x":null}}"#))
        XCTAssertEqual(usage.entries.map(\.label), ["databaseBytes", "engine", "tables"])
        XCTAssertEqual(usage.entries.map(\.value), ["1.5 KB", "rqlite", "12"])
    }

    func testNamesAndSegmentsAreValidatedBeforeAnyRequest() {
        XCTAssertTrue(AdminClient.validName("alice-1"))
        XCTAssertFalse(AdminClient.validName("a/b"))
        XCTAssertFalse(AdminClient.validName(""))
        XCTAssertFalse(AdminClient.validName("é"))
        XCTAssertTrue(AdminClient.validSegment("forkmesh.app"))
        XCTAssertFalse(AdminClient.validSegment(".."))
        XCTAssertFalse(AdminClient.validSegment("a?b"))
    }

    @MainActor func testClientUsesThePhoneAdminRoutesAndBearerUserActions() async throws {
        let transport = FakeInsightsTransport()
        let client = AdminClient(transport: transport)
        _ = try await client.overview()
        _ = try await client.rows(table: "my table", cursor: "5", limit: 500)
        try await client.perform(.quarantine, target: " Mallory ")
        try await client.purgeRepository(owner: "alice", name: "demo")
        XCTAssertEqual(transport.calls[0].path, "/api/admin/phone/overview")
        XCTAssertEqual(transport.calls[1].path, "/api/admin/phone/database?op=rows&table=my%20table&cursor=5&limit=50")
        XCTAssertEqual(transport.calls[2].method, "POST")
        XCTAssertEqual(transport.calls[2].path, "/api/accounts/admin-quarantine")
        XCTAssertEqual(transport.calls[2].body, ["target": "mallory"])
        XCTAssertEqual(transport.calls[3].method, "DELETE")
        XCTAssertEqual(transport.calls[3].path, "/api/repositories?owner=alice&name=demo")
        do {
            try await client.perform(.delete, target: "../x")
            XCTFail("expected a refusal")
        } catch let error as InsightsError {
            XCTAssertEqual(error.code, "invalid_user")
        }
        XCTAssertEqual(transport.calls.count, 4)
    }

    @MainActor func testBatchActionsAreBoundedAndReportEachFailure() async {
        let transport = FakeInsightsTransport()
        transport.failures["/api/accounts/admin-delete"] = OrgApiException("target_is_admin", "x")
        let client = AdminClient(transport: transport)
        let names = (0..<40).map { "user\($0)" }
        let failures = await client.perform(.delete, targets: names)
        XCTAssertEqual(transport.calls.count, AdminClient.maxBatch)
        XCTAssertEqual(failures.count, AdminClient.maxBatch)
        XCTAssertEqual(failures["user0"]?.message, "Administrators cannot be quarantined or deleted here.")
    }

    @MainActor func testScreenModelNoticesADeniedAdmin() async {
        let transport = FakeInsightsTransport()
        transport.failures["/api/admin/phone/overview"] = OrgApiException("admin_required", "admin required")
        let model = AdminScreenModel(transport: transport)
        await model.reload()
        XCTAssertTrue(model.denied)
        XCTAssertEqual(model.overview.error?.needsAdmin, true)
    }

    @MainActor func testUserActionsRefreshTheListAndRecordTheOutcome() async {
        let transport = FakeInsightsTransport()
        transport.replies["/api/accounts/users"] = object(#"{"ok":true,"users":[{"name":"bob"}]}"#)
        let model = AdminScreenModel(transport: transport, section: .users)
        await model.reload()
        XCTAssertEqual(model.users.value?.users.count, 1)
        let ok = await model.perform(.quarantine, target: "bob")
        XCTAssertTrue(ok)
        XCTAssertEqual(model.lastAction, "Quarantined bob")
        transport.failures["/api/accounts/admin-delete"] = OrgApiException("self_delete_forbidden", "x")
        let failed = await model.perform(.delete, target: "bob")
        XCTAssertFalse(failed)
        XCTAssertEqual(model.actionError?.message, "You cannot delete your own account.")
    }

    func testSectionsParseFromTheRoute() {
        XCTAssertEqual(AdminSection(route: "errors"), .errors)
        XCTAssertEqual(AdminSection(route: nil), .overview)
    }
}

final class DesktopStatsTests: XCTestCase {
    @MainActor func testClientCallsTheStatsFamilyAndBoundsParameters() async throws {
        let channel = FakeDesktopChannel()
        channel.replies[DesktopStatsMethod.timeline] = object(
            #"{"days":[{"dayTs":1,"errors":2,"warnings":1,"info":5},{"dayTs":2,"errors":9,"warnings":0,"info":0}]}"#)
        channel.replies[DesktopStatsMethod.quality] = object(
            #"{"score":72,"grade":"C","files":[{"path":"b.cpp","score":90},{"path":"a.cpp","score":40,"findings":["deep nesting"]}]}"#)
        channel.replies[DesktopStatsMethod.ratchetPrepare] = object(#"{"token":"t1","summary":"Turn on ratchet","expiresAt":5}"#)
        channel.replies[DesktopStatsMethod.ratchetRun] = object(#"{"commits":10,"ratchet":{"enabled":true}}"#)
        let client = DesktopStatsClient(channel: channel)
        let timeline = try await client.timeline(days: 400)
        XCTAssertEqual(channel.requests[0].1["days"], .int(30))
        XCTAssertEqual(timeline.busiest?.dayTs, 2)
        XCTAssertEqual(timeline.errorSeries.values, [2, 9])
        let quality = try await client.quality(repo: "a/b")
        XCTAssertEqual(quality.files.map(\.path), ["a.cpp", "b.cpp"])
        XCTAssertEqual(quality.tone, .warning)
        let confirmation = try await client.prepareRatchet(repo: "a/b", enabled: true)
        let snapshot = try await client.runRatchet(confirmation)
        XCTAssertTrue(snapshot.ratchetEnabled)
        XCTAssertEqual(channel.requests.last?.1, ["token": "t1"])
    }

    @MainActor func testAnOlderDesktopReadsAsUnsupported() async {
        let channel = FakeDesktopChannel()
        channel.failure = LocalControlException("unsupported")
        let model = DesktopStatsModel(client: DesktopStatsClient(channel: channel))
        await model.loadTimeline()
        await model.loadSentry()
        XCTAssertEqual(model.timeline, .unsupported)
        XCTAssertEqual(model.sentry, .unsupported)
        channel.failure = LocalControlException("offline")
        await model.loadRepo("a/b")
        XCTAssertEqual(model.quality, .failed("Your desktop is not reachable right now."))
    }

    @MainActor func testRatchetNeedsAConfirmationBeforeItRuns() async {
        let channel = FakeDesktopChannel()
        channel.replies[DesktopStatsMethod.ratchetPrepare] = object(#"{"summary":"no token"}"#)
        let model = DesktopStatsModel(client: DesktopStatsClient(channel: channel))
        await model.prepareRatchet("a/b", enabled: true)
        XCTAssertNil(model.pendingRatchet)
        XCTAssertFalse(model.ratchetError.isEmpty)
        await model.confirmRatchet()
        XCTAssertFalse(channel.requests.contains { $0.0 == DesktopStatsMethod.ratchetRun })
    }

    func testHealthParsesTheDiagFamilyAnswer() {
        let health = DesktopHealth(json: object(
            #"{"level":"strained","cpuPercent":-1,"memory":{"total":0},"agents":{"running":0,"queued":0},"stalls":3}"#))
        XCTAssertEqual(health.tone, .critical)
        XCTAssertNil(health.memoryUsedShare)
        let tiles = StatsTiles.desktop(health)
        XCTAssertEqual(tiles["desktopLoad"]?.available, false)
        XCTAssertEqual(tiles["stalls"]?.value, "3")
    }
}

final class RepoInsightsTests: XCTestCase {
    func issue(_ number: Int, events: [IssueEvent]) -> Issue {
        Issue(number: number, title: "i\(number)", status: "open", body: "", author: "a", events: events)
    }

    func testBurnupFollowsOpenAndStatusEventsLikeTheDesktop() {
        let issues = [
            issue(1, events: [IssueEvent(type: "open", ts: 100), IssueEvent(type: "status", status: "closed", ts: 300)]),
            issue(2, events: [IssueEvent(type: "open", ts: 200)]),
            issue(3, events: [IssueEvent(type: "comment", ts: 50)]),
            issue(4, events: [IssueEvent(type: "open", ts: 150), IssueEvent(type: "status", status: "closed", ts: 250),
                              IssueEvent(type: "status", status: "open", ts: 350)]),
        ]
        let series = IssueBurnup.series(issues, start: 400, end: 0, intervals: 4)
        XCTAssertEqual(series.map(\.timestampMs), [0, 100, 200, 300, 400])
        XCTAssertEqual(series.map(\.openCount), [0, 1, 3, 1, 2])
        XCTAssertEqual(series.map(\.closedCount), [0, 0, 0, 2, 1])
        XCTAssertEqual(IssueBurnup.firstTimestamp(issues, fallback: 9), 100)
        XCTAssertEqual(IssueBurnup.firstTimestamp([], fallback: 9), 9)
        XCTAssertEqual(IssueBurnup.series([], start: 0, end: 10, intervals: 0).count, 2)
    }

    func testSummaryBucketsCommitsByWeekAndRanksContributors() {
        let week: Int64 = 7 * 24 * 60 * 60 * 1000
        let now: Int64 = 1_789_400_000_000
        let commits: [JSONObject] = [
            ["author": ["name": "Ann"], "date": .int(now / 1000)],
            ["author": "Bo", "timestamp": .int(now - week)],
            ["authorName": "Bo", "date": "1970-01-01T00:00:00Z"],
            ["author": "Ann", "ts": .string(String(now - 2 * week))],
        ]
        let summary = RepoInsightsSummary(
            commits: commits, issues: [issue(1, events: [IssueEvent(type: "open", ts: 5)])], pulls: [], repository: nil, nowMs: now)
        XCTAssertEqual(summary.weekly.count, RepoInsightsSummary.weeks)
        XCTAssertEqual(summary.weekly.last?.commits, 1)
        XCTAssertEqual(summary.commitsInWindow, 3)
        XCTAssertEqual(summary.contributors.map(\.name), ["Ann", "Bo"])
        XCTAssertEqual(summary.contributors.map(\.commits), [2, 2])
        XCTAssertEqual(summary.openIssues, 1)
        XCTAssertEqual(summary.burnup.count, 13)
    }
}

final class CrashReportTests: XCTestCase {
    func tempStore() throws -> (CrashReportStore, URL) {
        let base = FileManager.default.temporaryDirectory.appendingPathComponent("ws20-crash-\(UUID().uuidString)")
        return (CrashReportStore(directory: { base }), base)
    }

    func testStoreDedupesSortsAndBounds() throws {
        let (store, base) = try tempStore()
        defer { try? FileManager.default.removeItem(at: base) }
        let first = CrashReport(id: "a", kind: .crash, receivedAt: 1, summary: "SIGSEGV")
        store.append([first])
        store.append([first, CrashReport(id: "b", kind: .hang, receivedAt: 2, summary: "Main thread hung 3 s")])
        XCTAssertEqual(store.load().map(\.id), ["b", "a"])
        store.append((0..<30).map { CrashReport(id: "n\($0)", kind: .cpu, receivedAt: Int64(100 + $0), summary: "cpu") })
        let kept = store.load()
        XCTAssertEqual(kept.count, CrashReportStore.maxReports)
        XCTAssertEqual(kept.first?.id, "n29")
        store.remove("n29")
        XCTAssertEqual(store.load().first?.id, "n28")
        store.clear()
        XCTAssertTrue(store.load().isEmpty)
    }

    func testReportsAreBoundedAndProduceALogLineAndPrompt() {
        let report = CrashReport(
            kind: .crash, receivedAt: 1, appVersion: "1.2.3", osVersion: "iOS 18.1",
            summary: String(repeating: "s", count: 1000), detail: String(repeating: "d", count: 40_000))
        XCTAssertEqual(report.summary.count, CrashReport.summaryLimit)
        XCTAssertEqual(report.detail.count, CrashReport.detailLimit)
        XCTAssertTrue(report.stallLogLine.hasPrefix("[metrickit] crash "))
        XCTAssertTrue(report.stallLogLine.hasSuffix("app=1.2.3 os=iOS 18.1"))
        XCTAssertTrue(report.agentPrompt().contains("App version: 1.2.3"))
        XCTAssertLessThan(report.agentPrompt().count, 10_000)
    }

    func testTopFramesFollowTheAttributedThreadAndSlimmingKeepsOnlyIt() throws {
        let leaf: [String: Any] = ["binaryName": "UIKitCore", "offsetIntoBinaryTextSegment": 43]
        let root: [String: Any] = ["binaryName": "ForkMesh", "offsetIntoBinaryTextSegment": 26, "subFrames": [leaf]]
        let other: [String: Any] = ["binaryName": "libsystem_kernel.dylib", "offsetIntoBinaryTextSegment": 1]
        let tree: [String: Any] = [
            "callStackPerThread": true,
            "callStacks": [
                ["threadAttributed": false, "callStackRootFrames": [other]],
                ["threadAttributed": true, "callStackRootFrames": [root]],
            ],
        ]
        let raw = try JSONSerialization.data(withJSONObject: tree, options: [.prettyPrinted])
        XCTAssertEqual(CrashReport.topFrames(String(decoding: raw, as: UTF8.self), 8), ["ForkMesh+0x1a", "UIKitCore+0x2b"])
        XCTAssertEqual(CrashReport.topFrames(String(decoding: raw, as: UTF8.self), 1), ["ForkMesh+0x1a"])
        let slim = try XCTUnwrap(CrashReport.attributedStackJSON(raw))
        XCTAssertFalse(slim.contains("libsystem_kernel"))
        XCTAssertFalse(slim.contains("\n"))
        let report = CrashReport(kind: .crash, receivedAt: 1, summary: "signal 11", detail: slim)
        XCTAssertEqual(report.topFrames(8), ["ForkMesh+0x1a", "UIKitCore+0x2b"])
        XCTAssertEqual(CrashReport.topFrames("{ \"callStacks\" : [ {", 8), [])
        XCTAssertNil(CrashReport.attributedStackJSON(Data("not json".utf8)))
    }

    @MainActor func testModelCountsByKind() throws {
        let (store, base) = try tempStore()
        defer { try? FileManager.default.removeItem(at: base) }
        store.append([CrashReport(kind: .hang, receivedAt: 1, summary: "h"), CrashReport(kind: .hang, receivedAt: 2, summary: "h"),
                      CrashReport(kind: .crash, receivedAt: 3, summary: "c")])
        let model = CrashReportsModel(store: store)
        model.reload()
        XCTAssertEqual(model.counts.map(\.kind), [.crash, .hang])
        XCTAssertEqual(model.counts.map(\.count), [1, 2])
    }
}
