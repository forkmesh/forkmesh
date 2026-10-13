import FMApi
import FMBase
import FMChat
import Foundation
import XCTest

@testable import FMInsights

@MainActor
final class FakeInsightsTransport: InsightsRequesting {
    var replies: [String: JSONObject] = [:]
    var failures: [String: Error] = [:]
    var calls: [(method: String, path: String, body: JSONObject?)] = []

    func call(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject {
        calls.append((method, path, body))
        let base = String(path.split(separator: "?", maxSplits: 1).first ?? "")
        if let failure = failures[path] ?? failures[base] { throw failure }
        return replies[path] ?? replies[base] ?? ["ok": true]
    }
}

func object(_ text: String) -> JSONObject { JSON.parseObject(text) }

let statusJSON = """
{"ok":true,"now":1000,"systems":[
 {"id":"website","label":"Website","status":"operational","uptime24hPct":99.9,"uptimePct":99.5,"checks24h":1440,"failures24h":1,
  "reason":null,"minutes":[{"minuteTs":1,"status":"operational"},{"minuteTs":2,"status":"future"}]},
 {"id":"api","label":"API","status":"down","reason":"timed out","reasonTs":900},
 {"id":"mirror:vultr1","label":"vultr1","status":"down"}],
 "current":{"catalogRepos":12,"onlineNodes":3,"errors24h":7,"mainnodeOnline":true,"monitoringState":null,"lastCronSampleTs":990}}
"""

final class StatusModelTests: XCTestCase {
    func testReportParsesSystemsAndCurrentState() {
        let report = StatusReport(json: object(statusJSON))
        XCTAssertEqual(report.systems.count, 3)
        XCTAssertEqual(report.systems[0].uptime24h, 99.9)
        XCTAssertEqual(report.systems[0].reason, "")
        XCTAssertEqual(report.systems[0].minutes.map(\.state), [.operational, .future])
        XCTAssertEqual(report.systems[1].reason, "timed out")
        XCTAssertTrue(report.systems[2].isMirror)
        XCTAssertEqual(report.current.errors24h, 7)
        XCTAssertEqual(report.current.monitoringState, "")
        XCTAssertEqual(report.overall, .down)
        XCTAssertEqual(report.headline, "1 system is down")
        XCTAssertEqual(report.coreSystems.count, 2)
        XCTAssertEqual(report.websiteUptime24h, 99.9)
    }

    func testCoresAndRunnersAreNodesApartFromServicesAndMirrors() {
        let report = StatusReport(json: object("""
        {"systems":[{"id":"website","status":"operational"},{"id":"core:n2","status":"operational"},
         {"id":"runner:fmnext-r1","status":"operational"},{"id":"mirror:a","status":"down"}],"current":{}}
        """))
        XCTAssertEqual(report.serviceSystems.map(\.id), ["website"])
        XCTAssertEqual(report.nodeSystems.map(\.id), ["core:n2", "runner:fmnext-r1"])
        XCTAssertEqual(report.mirrorSystems.map(\.id), ["mirror:a"])
        XCTAssertTrue(report.nodeSystems.allSatisfy(\.isNode))
    }

    func testMirrorOutagesDoNotRedTheHeadline() {
        let report = StatusReport(json: object("""
        {"systems":[{"id":"website","status":"operational"},{"id":"mirror:a","status":"down"}],"current":{}}
        """))
        XCTAssertEqual(report.overall, .operational)
        XCTAssertEqual(report.headline, "All systems operational")
        XCTAssertTrue(report.down.isEmpty)
        XCTAssertEqual(ConnectionLight.of(report: report, reachable: true), .live)
        XCTAssertEqual(ConnectionLight.of(report: nil, reachable: false), .offline)
    }

    func testStaleMonitoringSaysSo() {
        let report = StatusReport(json: object(#"{"systems":[],"current":{"monitoringState":"stale"}}"#))
        XCTAssertEqual(report.headline, "Monitoring is behind")
    }

    @MainActor func testScreenModelKeepsTheLastReportOnAFailedRefresh() async {
        let transport = FakeInsightsTransport()
        transport.replies["/api/status"] = object(statusJSON)
        let model = StatusScreenModel(transport: transport)
        await model.reload()
        XCTAssertNotNil(model.report.value)
        transport.failures["/api/status"] = OrgApiException("http_500", "x")
        await model.reload()
        XCTAssertNotNil(model.report.value)
    }

    @MainActor func testScreenModelReportsAFirstFailure() async {
        let transport = FakeInsightsTransport()
        transport.failures["/api/status"] = OrgApiException("http_404", "x")
        let model = StatusScreenModel(transport: transport)
        await model.reload()
        XCTAssertEqual(model.report.error?.code, "http_404")
    }
}

final class NetworkModelTests: XCTestCase {
    func testDiagnosticsParseLikeTheDesktopWireFormat() throws {
        let value = try JSON.parse(#"""
        [{"i":"disk-trend","s":3,"m":"Disk fills in 9 hours"},{"i":"fd","s":9,"m":"FDs"},{"i":"","s":2,"m":"x"},
         {"i":"clock","m":"Clock skew 4 s"},{"i":"zz","s":-1,"m":"fine"}]
        """#)
        let findings = NodeDiagnosticFinding.parse(value)
        XCTAssertEqual(findings.map(\.id), ["disk-trend", "fd", "clock", "zz"])
        XCTAssertEqual(findings[1].severity, .critical)
        XCTAssertEqual(findings[2].severity, .info)
        XCTAssertEqual(findings[3].severity, .ok)
        XCTAssertEqual(NodeDiagnosticFinding.worst(findings), .critical)
        XCTAssertEqual(NodeDiagnosticFinding.worst([]), .ok)
    }

    func testDiagnosticsAreBounded() {
        let many = JSONValue.array((0..<20).map { .object(["i": .string("c\($0)"), "s": 1, "m": .string(String(repeating: "x", count: 500))]) })
        let findings = NodeDiagnosticFinding.parse(many)
        XCTAssertEqual(findings.count, NodeDiagnosticFinding.maxFindings)
        XCTAssertEqual(findings[0].message.count, NodeDiagnosticFinding.maxMessage)
    }

    func testDirectoryMergesLivePeersWithTheRelayListAndSortsTroubleFirst() {
        let live = [
            NodeRow(id: "a", name: "alpha", platform: "linux", online: true),
            NodeRow(id: "b", name: "Beta", owner: "bob", platform: "macos", version: "0.9", online: true,
                    diagnostics: [NodeDiagnosticFinding(id: "disk", severity: .warning, message: "low")]),
            NodeRow(id: "me", name: "phone", platform: "ios", online: true, isSelf: true),
        ]
        let rows = NodeDirectory.merge(live: live, onlineNames: ["beta", "gamma", ""])
        XCTAssertEqual(rows.map(\.name), ["phone", "Beta", "alpha", "gamma"])
        XCTAssertEqual(rows[1].subtitle, "macOS · v0.9 · owner bob")
        XCTAssertTrue(rows[1].accessibilityLabel.contains("1 health warnings"))
        XCTAssertEqual(NodeDirectory.filter(rows, query: "BOB").map(\.name), ["Beta"])
        XCTAssertEqual(NodeDirectory.filter(rows, query: "iphone").map(\.name), ["phone"])
    }

    func testRowsBuildFromRelayMembers() {
        let member = Member(id: "n1", name: "Desk", isSelf: false, online: true, platform: "windows", version: "1.0",
                            mirrors: ["a/b"], owner: "alice", nodeName: "alice-desk")
        let row = NodeRow(member: member)
        XCTAssertEqual(row.name, "alice-desk")
        XCTAssertEqual(row.subtitle, "Windows · v1.0 · owner alice · 1 mirror")
    }

    func testEndpointsDeriveFromTheRelaySocket() {
        let rows = EndpointRow.forRelay(serverUrl: "wss://app.next.forkmesh.com/api/relay")
        XCTAssertEqual(rows.first?.url, "https://app.next.forkmesh.com/api")
        XCTAssertEqual(rows.last?.url, "https://app.next.forkmesh.com/api/status")
        XCTAssertEqual(EndpointRow.forRelay(serverUrl: "ws://192.168.1.5:8787/api/relay")[2].url, "http://192.168.1.5:8787")
        XCTAssertTrue(EndpointRow.forRelay(serverUrl: "nonsense").isEmpty)
    }

    func testSectionsParseFromTheRoute() {
        XCTAssertEqual(NetworkSection(route: "relays"), .relays)
        XCTAssertEqual(NetworkSection(route: nil), .nodes)
        XCTAssertEqual(NetworkSection(route: "bogus"), .nodes)
    }

    @MainActor func testScreenModelLoadsStatsAndHistory() async {
        let transport = FakeInsightsTransport()
        transport.replies["/api/network/stats"] = object(#"{"ok":true,"repos":4,"hosts":2,"clients":9,"onlineNodes":["gamma","alpha"]}"#)
        transport.replies["/api/network/online-history"] = object(
            #"{"ok":true,"hours":[{"hourTs":1,"nodeMinutes":30},{"hourTs":2,"nodeMinutes":90}],"nodes":[{"id":"x","label":"alpha","totalMinutes":120}]}"#)
        let model = NetworkScreenModel(transport: transport, livePeers: { [NodeRow(id: "a", name: "alpha", online: true)] })
        await model.reload()
        XCTAssertEqual(model.snapshot.value?.repos, 4)
        XCTAssertEqual(model.history.value?.peakHour?.nodeMinutes, 90)
        XCTAssertEqual(model.nodes.map(\.name), ["alpha", "gamma"])
    }
}

final class StatsBoardModelTests: XCTestCase {
    @MainActor func testBoardFillsTilesFromTheServerAndTheDesktop() async {
        let transport = FakeInsightsTransport()
        transport.replies["/api/network/stats"] = object(#"{"ok":true,"repos":12900,"hosts":2,"onlineNodes":["a","b"]}"#)
        transport.replies["/api/network/online-history"] = object(#"{"ok":true,"hours":[{"hourTs":1,"nodeMinutes":60}]}"#)
        transport.replies["/api/status"] = object(statusJSON)
        let health = DesktopHealth(json: object(
            #"{"level":"busy","cpuPercent":72.4,"memory":{"total":1000,"available":200},"agents":{"running":2,"queued":1},"stalls":0}"#))
        let model = StatsBoardModel(
            transport: transport, layoutStore: StatsLayoutStore(store: InMemoryKeyValueStore()), desktop: { health })
        await model.reload()
        XCTAssertEqual(model.data("repos")?.value, "12.9K")
        XCTAssertEqual(model.data("nodes")?.value, "2")
        XCTAssertEqual(model.data("nodeHours")?.value, "1h 0m")
        XCTAssertEqual(model.data("errors")?.value, "7")
        XCTAssertEqual(model.data("incidents")?.value, "1")
        XCTAssertEqual(model.data("uptime")?.value, "99.90%")
        XCTAssertEqual(model.data("desktopLoad")?.value, "72%")
        XCTAssertEqual(model.data("desktopLoad")?.status, .warning)
        XCTAssertEqual(model.data("desktopMemory")?.value, "80%")
        XCTAssertEqual(model.data("agents")?.note, "1 queued")
        XCTAssertTrue(model.failures.isEmpty)
    }

    @MainActor func testBoardRecordsReadingsAndShowsTrendsAndCodemagicMinutes() async throws {
        let transport = FakeInsightsTransport()
        transport.replies["/api/network/stats"] = object(#"{"ok":true,"repos":10,"hosts":2,"onlineNodes":["a"]}"#)
        transport.replies["/api/status"] = object(statusJSON)
        let prefs = InMemoryKeyValueStore()
        var clock = Date(timeIntervalSince1970: 1_000_000)
        var remaining = 30000.0
        let model = StatsBoardModel(
            transport: transport, layoutStore: StatsLayoutStore(store: prefs),
            codemagic: { try XCTUnwrap(CodemagicUsage(json: ["limitSeconds": .double(30000), "usedSeconds": .double(30000 - remaining)])) },
            history: StatsHistoryStore(store: prefs), now: { clock })
        for repos in [10, 10, 14, 14] {
            transport.replies["/api/network/stats"] = object(#"{"ok":true,"repos":\#(repos),"hosts":2,"onlineNodes":["a"]}"#)
            await model.reload()
            clock += 3600
            remaining -= 600
        }
        let repos = try XCTUnwrap(model.data("repos"))
        XCTAssertEqual(repos.series.values, [10, 10, 14, 14])
        XCTAssertEqual(repos.shownTrend ?? 0, 40, accuracy: 0.01)
        XCTAssertEqual(repos.trendTone, .good)
        let minutes = try XCTUnwrap(model.data("codemagic"))
        XCTAssertEqual(minutes.value, "470 min")
        XCTAssertEqual(minutes.series.values, [500, 490, 480, 470])
        XCTAssertEqual(minutes.status, .good)
        XCTAssertEqual(model.data("nodeHours")?.available, false)
    }

    @MainActor func testCodemagicExplainsWhyItHasNoMinutes() async {
        let model = StatsBoardModel(
            transport: FakeInsightsTransport(), layoutStore: StatsLayoutStore(store: InMemoryKeyValueStore()),
            desktop: { throw InsightsError("offline", "Your desktop is not reachable right now.") },
            codemagic: { throw InsightsError("codemagic_not_configured", "Add CODEMAGIC_API_TOKEN to the desktop's Actions variables.") })
        await model.reload()
        XCTAssertEqual(model.data("codemagic")?.available, false)
        XCTAssertEqual(model.data("codemagic")?.note, "Add CODEMAGIC_API_TOKEN to the desktop's Actions variables.")
        XCTAssertEqual(model.data("desktopLoad")?.available, false)
    }

    @MainActor func testServerTilesFillWhileTheDesktopIsStillAnswering() async throws {
        let transport = FakeInsightsTransport()
        transport.replies["/api/network/stats"] = object(#"{"ok":true,"repos":3,"hosts":1,"onlineNodes":["a"]}"#)
        transport.replies["/api/status"] = object(statusJSON)
        let gate = HealthGate()
        let model = StatsBoardModel(
            transport: transport, layoutStore: StatsLayoutStore(store: InMemoryKeyValueStore()),
            desktop: { try await gate.wait() })
        let reload = Task { await model.reload() }
        for _ in 0..<200 where model.data("repos") == nil || gate.waiting == nil { await Task.yield() }
        XCTAssertEqual(model.data("repos")?.value, "3")
        XCTAssertFalse(model.isLoading("repos"))
        XCTAssertTrue(model.isLoading("desktopLoad"))
        XCTAssertNil(model.data("desktopLoad"))
        XCTAssertTrue(model.loading)
        gate.open(DesktopHealth(json: object(#"{"cpuPercent":10,"agents":{"running":1,"queued":0},"stalls":0}"#)))
        await reload.value
        XCTAssertEqual(model.data("desktopLoad")?.value, "10%")
        XCTAssertFalse(model.isLoading("desktopLoad"))
        XCTAssertTrue(model.pending.isEmpty)
        XCTAssertFalse(model.loading)
        XCTAssertNotNil(model.updatedAt)
    }

    @MainActor func testADesktopTimeoutIsSlowNotUnreachable() async {
        let model = StatsBoardModel(
            transport: FakeInsightsTransport(), layoutStore: StatsLayoutStore(store: InMemoryKeyValueStore()),
            desktop: { throw LocalControlException("timeout") })
        await model.reload()
        XCTAssertEqual(model.desktopNote, "Your desktop took too long to answer. Pull to try again.")
        XCTAssertEqual(DesktopStatsClient.explain(LocalControlException("not_connected")), "Your desktop is not reachable right now.")
        XCTAssertEqual(DesktopStatsClient.explain(LocalControlException("sentry_not_installed")), "Install the Sentry package on the desktop.")
    }

    @MainActor func testWithoutADesktopItsTilesSayNoData() async {
        let transport = FakeInsightsTransport()
        transport.failures["/api/status"] = OrgApiException("http_500", "x")
        let model = StatsBoardModel(transport: transport, layoutStore: StatsLayoutStore(store: InMemoryKeyValueStore()))
        await model.reload()
        XCTAssertEqual(model.data("agents")?.available, false)
        XCTAssertEqual(model.failures, ["status"])
        XCTAssertFalse(model.desktopNote.isEmpty)
    }

    @MainActor func testReorderRemoveAddAndResetPersist() {
        let prefs = InMemoryKeyValueStore()
        let model = StatsBoardModel(transport: FakeInsightsTransport(), layoutStore: StatsLayoutStore(store: prefs))
        let original = model.visible.map(\.id)
        model.move(fromOffsets: IndexSet(integer: 0), toOffset: 3)
        XCTAssertEqual(model.visible.map(\.id), [original[1], original[2], original[0]] + original.dropFirst(3))
        model.remove(original[1])
        XCTAssertFalse(model.visible.map(\.id).contains(original[1]))
        XCTAssertTrue(model.addable.map(\.id).contains(original[1]))
        model.add(original[1])
        XCTAssertEqual(model.visible.last?.id, original[1])
        model.toggleWide(original[2])
        let reloaded = StatsBoardModel(transport: FakeInsightsTransport(), layoutStore: StatsLayoutStore(store: prefs))
        XCTAssertEqual(reloaded.visible, model.visible)
        reloaded.reset()
        XCTAssertEqual(reloaded.visible.map(\.id), original)
    }
}

@MainActor
final class HealthGate {
    var waiting: CheckedContinuation<DesktopHealth, Error>?

    func wait() async throws -> DesktopHealth {
        try await withCheckedThrowingContinuation { waiting = $0 }
    }

    func open(_ health: DesktopHealth) {
        waiting?.resume(returning: health)
        waiting = nil
    }
}
