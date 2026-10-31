import FMApi
import FMBase
import FMNet
import Foundation
import XCTest

@testable import FMInsights

let fleetStatusJSON = """
{"ok":true,"now":1000,"systems":[
 {"id":"website","label":"Website","status":"operational"},
 {"id":"runner:fmnext-r1","label":"Runner — fmnext-r1","status":"operational"},
 {"id":"core:n2","label":"Core node — n2","status":"down","reason":"n2 has not written a cluster heartbeat"},
 {"id":"core:n1","label":"Core node — n1","status":"operational"},
 {"id":"runner:fmnext-r2","label":"Runner — fmnext-r2","status":"degraded","reason":"claude login expired"},
 {"id":"core:","status":"operational"},
 {"id":"mirror:vultr1","label":"vultr1","status":"operational"}],
 "current":{}}
"""

final class FleetLightsTests: XCTestCase {
    func testReportListsEveryCoreThenEveryRunner() {
        let lights = StatusReport(json: object(fleetStatusJSON)).fleetLights
        XCTAssertEqual(lights.map(\.name), ["n2", "n1", "fmnext-r1", "fmnext-r2"])
        XCTAssertEqual(lights.map(\.kind), [.core, .core, .runner, .runner])
        XCTAssertEqual(lights.map(\.healthy), [false, true, true, false])
        XCTAssertEqual(lights[0].reason, "n2 has not written a cluster heartbeat")
    }

    func testCoreLightsCarryTheirCpuMemoryAndDisk() {
        let lights = StatusReport(json: object("""
        {"systems":[
         {"id":"core:n1","status":"operational","resources":{"cpu":12.5,"memory":81,"disk":140}},
         {"id":"core:n2","status":"operational","resources":{"cpu":null,"memory":-1}},
         {"id":"core:n3","status":"operational"},
         {"id":"runner:r1","status":"operational","resources":{"cpu":50}}],
         "current":{}}
        """)).fleetLights
        XCTAssertEqual(lights[0].resources, NodeResources(cpu: 12.5, memory: 81, disk: 100))
        XCTAssertEqual(lights[0].resources?.values, [12.5, 81, 100])
        XCTAssertEqual(lights[0].label, "Core node n1, operational, CPU 13%, memory 81%, disk 100%")
        XCTAssertNil(lights[1].resources)
        XCTAssertNil(lights[2].resources)
        XCTAssertNil(lights[3].resources)
        XCTAssertEqual(NodeResources(cpu: nil, memory: 40, disk: nil).label, "CPU unknown, memory 40%, disk unknown")
    }

    func testStatusLightsAreTheSystemsWithoutNodes() {
        let report = StatusReport(json: object("""
        {"systems":[
         {"id":"website","label":"Website","status":"operational"},
         {"id":"core:n1","status":"operational"},
         {"id":"api","label":"API","status":"degraded","reason":"slow"},
         {"id":"runner:r1","status":"operational"},
         {"id":"mirror:vultr1","status":"down"},
         {"id":"durables","label":"Durables","status":"future"},
         {"id":"email","status":""}],
         "current":{}}
        """))
        let lights = report.statusLights
        XCTAssertEqual(lights.map(\.id), ["website", "api", "mirror:vultr1", "email"])
        XCTAssertEqual(lights.map(\.state), [.operational, .degraded, .down, .unknown])
        XCTAssertEqual(lights[1].label, "API, degraded")
        XCTAssertEqual(lights[1].reason, "slow")
        XCTAssertEqual(lights.map(\.troubled), [false, true, true, false])
    }

    @MainActor func testADegradedSystemBringsTheRefreshForward() async {
        let transport = FakeInsightsTransport()
        transport.replies["/api/status"] = object(#"{"systems":[{"id":"core:n1","status":"operational"},{"id":"api","status":"degraded"}],"current":{}}"#)
        var clock = Date(timeIntervalSince1970: 0)
        let model = FleetLightsModel(transport: transport, now: { clock })
        await model.refresh()
        XCTAssertEqual(model.systems.map(\.id), ["api"])
        XCTAssertFalse(model.allHealthy)
        clock = clock.addingTimeInterval(FleetLightsModel.troubledFloor + 1)
        await model.refresh()
        XCTAssertEqual(transport.calls.count, 2)
    }

    func testMonitorFilterMatchesSystemsAndPrefixes() {
        let filter = StatusMonitorFilter(systems: ["api", "website"], prefixes: ["core:", ""])
        XCTAssertTrue(filter.shows("api"))
        XCTAssertTrue(filter.shows("core:n1"))
        XCTAssertFalse(filter.shows("runner:r1"))
        XCTAssertFalse(filter.shows("apis"))
        XCTAssertFalse(filter.shows("mirror:vultr1"))
        XCTAssertTrue(StatusMonitorFilter.all.shows("anything"))
        XCTAssertFalse(StatusMonitorFilter(systems: [], prefixes: []).shows("api"))
    }

    @MainActor func testOnlyWatchedSystemsAndNodesAreShown() async {
        let transport = FakeInsightsTransport()
        transport.replies["/api/status"] = object(fleetStatusJSON)
        let model = FleetLightsModel(transport: transport)
        await model.load()
        XCTAssertEqual(model.shownSystems.map(\.id), ["website", "mirror:vultr1"])
        XCTAssertEqual(model.shownLights.count, 4)
        model.watch = StatusWatch(filter: StatusMonitorFilter(systems: ["website"], prefixes: ["core:"]))
        XCTAssertEqual(model.shownSystems.map(\.id), ["website"])
        XCTAssertEqual(model.shownLights.map(\.id), ["core:n2", "core:n1"])
        XCTAssertEqual(model.runners.count, 2)
        model.watch = StatusWatch(filter: StatusMonitorFilter(systems: ["website"], prefixes: ["runner:"]))
        XCTAssertEqual(model.watchedLights.map(\.name), ["fmnext-r1", "fmnext-r2"])
        XCTAssertFalse(model.allHealthy)
        model.watch = StatusWatch(filter: StatusMonitorFilter(systems: ["website"], prefixes: []))
        XCTAssertEqual(model.shownLights, [])
        XCTAssertTrue(model.allHealthy)
    }

    @MainActor func testOnlyProblemsHidesTheGreenDots() async {
        let transport = FakeInsightsTransport()
        transport.replies["/api/status"] = object(fleetStatusJSON)
        let model = FleetLightsModel(transport: transport)
        await model.load()
        model.watch = StatusWatch(onlyProblems: true)
        XCTAssertEqual(model.shownLights.map(\.id), ["core:n2", "runner:fmnext-r2"])
        XCTAssertEqual(model.shownSystems, [])
        XCTAssertEqual(model.watchedSystems.map(\.id), ["website", "mirror:vultr1"])
    }

    @MainActor func testCustomMonitorsAreProbedByThePhone() async {
        let transport = FakeInsightsTransport()
        transport.replies["/api/status"] = object(#"{"systems":[{"id":"website","status":"operational"}],"current":{}}"#)
        let prober = FakeProber(up: ["https://blog.example.com/"])
        let model = FleetLightsModel(transport: transport, prober: prober)
        let blog = MonitorTarget(id: "a1", name: "Blog", url: URL(string: "https://blog.example.com/")!)
        let shop = MonitorTarget(id: "b2", name: "Shop", url: URL(string: "https://shop.example/")!)
        model.watch = StatusWatch(filter: StatusMonitorFilter(systems: [], prefixes: []), custom: [blog, shop])
        XCTAssertEqual(model.shownSystems.map(\.state), [.unknown, .unknown])
        await model.load()
        XCTAssertEqual(model.shownSystems.map(\.id), ["custom:a1", "custom:b2"])
        XCTAssertEqual(model.shownSystems.map(\.name), ["Blog", "Shop"])
        XCTAssertEqual(model.shownSystems.map(\.state), [.operational, .down])
        XCTAssertFalse(model.allHealthy)
        model.watch.onlyProblems = true
        XCTAssertEqual(model.shownSystems.map(\.id), ["custom:b2"])
        model.watch = StatusWatch(custom: [blog])
        await model.probeCustom()
        XCTAssertEqual(model.customLights.map(\.state), [.operational])
        XCTAssertEqual(model.shownSystems.map(\.id), ["website", "custom:a1"])
        XCTAssertTrue(model.allHealthy)
    }

    @MainActor func testRefreshMinutesSetTheHealthyFloor() async {
        let transport = FakeInsightsTransport()
        transport.replies["/api/status"] = object(#"{"systems":[{"id":"core:n1","status":"operational"}],"current":{}}"#)
        var clock = Date(timeIntervalSince1970: 0)
        let model = FleetLightsModel(transport: transport, now: { clock })
        model.watch = StatusWatch(healthyMinutes: 5)
        await model.refresh()
        clock = clock.addingTimeInterval(4 * 60)
        await model.refresh()
        XCTAssertEqual(transport.calls.count, 1)
        clock = clock.addingTimeInterval(60)
        await model.refresh()
        XCTAssertEqual(transport.calls.count, 2)
        XCTAssertEqual(StatusWatch().healthyFloor, FleetLightsModel.healthyFloor)
    }

    func testHTTPProbeIsGreenBelow400() async {
        let http = FakeHTTPClient()
        let probe = HTTPStatusProbe(http: http)
        let url = URL(string: "https://blog.example.com/")!
        http.enqueue(status: 200, text: "ok")
        let ok = await probe.probe(url)
        XCTAssertTrue(ok)
        XCTAssertEqual(http.lastRequest?.method, "GET")
        XCTAssertEqual(http.lastRequest?.timeout, 10)
        http.enqueue(status: 302, text: "")
        let moved = await probe.probe(url)
        XCTAssertTrue(moved)
        http.enqueue(status: 404, text: "")
        let missing = await probe.probe(url)
        XCTAssertFalse(missing)
        http.enqueue(error: HTTPError.timedOut)
        let slow = await probe.probe(url)
        XCTAssertFalse(slow)
    }

    func testOnlyOperationalIsGreen() {
        for (raw, healthy) in [("operational", true), ("degraded", false), ("down", false), ("", false)] {
            let report = StatusReport(json: object(#"{"systems":[{"id":"core:n1","status":"\#(raw)"}],"current":{}}"#))
            XCTAssertEqual(report.fleetLights.first?.healthy, healthy, raw)
        }
    }

    func testLabelsNameTheNodeAndItsState() {
        XCTAssertEqual(FleetLight(id: "core:n1", kind: .core, name: "n1", healthy: true).label,
                       "Core node n1, operational")
        XCTAssertEqual(FleetLight(id: "runner:r", kind: .runner, name: "r", healthy: false).label,
                       "Runner r, not signed in or not responding")
    }

    @MainActor func testRefreshWaitsLongerWhileEverythingIsGreen() async {
        let transport = FakeInsightsTransport()
        transport.replies["/api/status"] = object(#"{"systems":[{"id":"core:n1","status":"operational"}],"current":{}}"#)
        var clock = Date(timeIntervalSince1970: 0)
        let model = FleetLightsModel(transport: transport, now: { clock })
        await model.refresh()
        XCTAssertEqual(model.cores.map(\.name), ["n1"])
        clock = clock.addingTimeInterval(FleetLightsModel.troubledFloor + 1)
        await model.refresh()
        XCTAssertEqual(transport.calls.count, 1)
        clock = clock.addingTimeInterval(FleetLightsModel.healthyFloor)
        await model.refresh()
        XCTAssertEqual(transport.calls.count, 2)
    }

    @MainActor func testRefreshComesBackSoonerWhileANodeIsRed() async {
        let transport = FakeInsightsTransport()
        transport.replies["/api/status"] = object(fleetStatusJSON)
        var clock = Date(timeIntervalSince1970: 0)
        let model = FleetLightsModel(transport: transport, now: { clock })
        await model.refresh()
        XCTAssertFalse(model.allHealthy)
        XCTAssertEqual(model.runners.count, 2)
        clock = clock.addingTimeInterval(FleetLightsModel.troubledFloor + 1)
        await model.refresh()
        XCTAssertEqual(transport.calls.count, 2)
    }

    @MainActor func testAFailedLoadKeepsTheLastLights() async {
        let transport = FakeInsightsTransport()
        transport.replies["/api/status"] = object(fleetStatusJSON)
        let model = FleetLightsModel(transport: transport)
        await model.load()
        transport.failures["/api/status"] = OrgApiException("http_500", "x")
        await model.load()
        XCTAssertEqual(model.lights.count, 4)
        XCTAssertTrue(model.loaded)
    }
}

struct FakeProber: StatusProbing {
    let up: Set<String>

    func probe(_ url: URL) async -> Bool { up.contains(url.absoluteString) }
}
