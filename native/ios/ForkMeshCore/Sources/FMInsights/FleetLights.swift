import FMBase
import FMNet
import Foundation
import Observation

public enum FleetLightKind: String, Equatable, Sendable {
    case core, runner
}

public struct FleetLight: Equatable, Sendable, Identifiable {
    public var id: String
    public var kind: FleetLightKind
    public var name: String
    public var healthy: Bool
    public var reason: String

    public init(id: String, kind: FleetLightKind, name: String, healthy: Bool, reason: String = "") {
        self.id = id
        self.kind = kind
        self.name = name
        self.healthy = healthy
        self.reason = reason
    }

    public var label: String {
        switch kind {
        case .core: return "Core node \(name), \(healthy ? "operational" : "has an issue")"
        case .runner: return "Runner \(name), \(healthy ? "signed in" : "not signed in or not responding")"
        }
    }
}

public struct StatusLight: Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var state: ServiceState
    public var reason: String

    public init(id: String, name: String, state: ServiceState, reason: String = "") {
        self.id = id
        self.name = name
        self.state = state
        self.reason = reason
    }

    public var label: String { "\(name), \(state.label.lowercased())" }

    public var troubled: Bool { state == .degraded || state == .down }
}

public struct StatusMonitorFilter: Equatable, Sendable {
    public var systems: Set<String>
    public var prefixes: [String]
    public var everything: Bool

    public init(systems: [String], prefixes: [String]) {
        self.systems = Set(systems)
        self.prefixes = prefixes.filter { !$0.isEmpty }
        everything = false
    }

    private init() {
        systems = []
        prefixes = []
        everything = true
    }

    public static let all = StatusMonitorFilter()

    public func shows(_ id: String) -> Bool {
        everything || systems.contains(id) || prefixes.contains { id.hasPrefix($0) }
    }
}

public struct MonitorTarget: Equatable, Sendable, Identifiable {
    nonisolated public static let idPrefix = "custom:"

    public var id: String
    public var name: String
    public var url: URL

    public init(id: String, name: String, url: URL) {
        self.id = id
        self.name = name
        self.url = url
    }

    public var lightId: String { MonitorTarget.idPrefix + id }
}

public struct StatusWatch: Equatable, Sendable {
    public var filter: StatusMonitorFilter
    public var onlyProblems: Bool
    public var healthyMinutes: Int
    public var custom: [MonitorTarget]

    public init(
        filter: StatusMonitorFilter = .all, onlyProblems: Bool = false,
        healthyMinutes: Int = Int(FleetLightsModel.healthyFloor / 60), custom: [MonitorTarget] = []
    ) {
        self.filter = filter
        self.onlyProblems = onlyProblems
        self.healthyMinutes = healthyMinutes
        self.custom = custom
    }

    public var healthyFloor: TimeInterval { TimeInterval(max(1, healthyMinutes)) * 60 }
}

public protocol StatusProbing: Sendable {
    func probe(_ url: URL) async -> Bool
}

public struct HTTPStatusProbe: StatusProbing {
    nonisolated public static let timeout: TimeInterval = 10

    private let http: HTTPClient

    public init(http: HTTPClient) {
        self.http = http
    }

    public func probe(_ url: URL) async -> Bool {
        let request = HTTPRequest(
            method: "GET", url: url, headers: ["Cache-Control": "no-cache"], timeout: HTTPStatusProbe.timeout)
        do {
            return try await http.stream(request).status < 400
        } catch {
            return false
        }
    }
}

extension StatusSystem {
    public static let corePrefix: String = "core:"
    public static let runnerPrefix: String = "runner:"

    public var fleetLight: FleetLight? {
        let kind: FleetLightKind
        let prefix: String
        if id.hasPrefix(StatusSystem.corePrefix) {
            kind = .core
            prefix = StatusSystem.corePrefix
        } else if id.hasPrefix(StatusSystem.runnerPrefix) {
            kind = .runner
            prefix = StatusSystem.runnerPrefix
        } else {
            return nil
        }
        let name: String = String(id.dropFirst(prefix.count))
        if name.isEmpty { return nil }
        return FleetLight(id: id, kind: kind, name: name, healthy: state == .operational, reason: reason)
    }

    public var statusLight: StatusLight? {
        if state == .future || id.hasPrefix(StatusSystem.corePrefix) || id.hasPrefix(StatusSystem.runnerPrefix) {
            return nil
        }
        return StatusLight(id: id, name: label, state: state, reason: reason)
    }
}

extension StatusReport {
    public var fleetLights: [FleetLight] {
        let lights: [FleetLight] = systems.compactMap(\.fleetLight)
        return lights.filter { $0.kind == .core } + lights.filter { $0.kind == .runner }
    }

    public var statusLights: [StatusLight] { systems.compactMap(\.statusLight) }
}

@MainActor
@Observable
public final class FleetLightsModel {
    nonisolated public static let healthyFloor: TimeInterval = 15 * 60
    nonisolated public static let troubledFloor: TimeInterval = 2 * 60

    public private(set) var lights: [FleetLight] = []
    public private(set) var systems: [StatusLight] = []
    public private(set) var probed: [String: Bool] = [:]
    public private(set) var loaded: Bool = false
    public var watch: StatusWatch = StatusWatch()

    @ObservationIgnored private let transport: InsightsRequesting
    @ObservationIgnored private let prober: StatusProbing?
    @ObservationIgnored private let now: () -> Date
    @ObservationIgnored private var lastLoadedAt: Date?
    @ObservationIgnored private var loading: Bool = false

    public init(transport: InsightsRequesting, prober: StatusProbing? = nil, now: @escaping () -> Date = Date.init) {
        self.transport = transport
        self.prober = prober
        self.now = now
    }

    public var cores: [FleetLight] { lights.filter { $0.kind == .core } }
    public var runners: [FleetLight] { lights.filter { $0.kind == .runner } }

    public var watchedLights: [FleetLight] { lights.filter { watch.filter.shows($0.id) } }

    public var customLights: [StatusLight] {
        watch.custom.map { (target: MonitorTarget) -> StatusLight in
            let state: ServiceState = probed[target.lightId].map { $0 ? .operational : .down } ?? .unknown
            return StatusLight(id: target.lightId, name: target.name, state: state)
        }
    }

    public var watchedSystems: [StatusLight] { systems.filter { watch.filter.shows($0.id) } + customLights }

    public var shownLights: [FleetLight] { watch.onlyProblems ? watchedLights.filter { !$0.healthy } : watchedLights }

    public var shownSystems: [StatusLight] {
        watch.onlyProblems ? watchedSystems.filter { $0.state != .operational } : watchedSystems
    }

    public var allHealthy: Bool { watchedLights.allSatisfy(\.healthy) && !watchedSystems.contains(where: \.troubled) }

    public func refresh() async {
        if let last = lastLoadedAt {
            let floor: TimeInterval = allHealthy ? watch.healthyFloor : FleetLightsModel.troubledFloor
            if now().timeIntervalSince(last) < floor { return }
        }
        await load()
    }

    public func load() async {
        if loading { return }
        loading = true
        defer { loading = false }
        async let results: [String: Bool] = FleetLightsModel.probe(watch.custom, with: prober)
        var report: StatusReport?
        do {
            report = StatusReport(json: try await transport.call("GET", "/api/status", nil))
        } catch {
            report = nil
        }
        let found: [String: Bool] = await results
        if !watch.custom.isEmpty { probed = found }
        guard let report = report else { return }
        lights = report.fleetLights
        systems = report.statusLights
        loaded = true
        lastLoadedAt = now()
    }

    public func probeCustom() async {
        let targets: [MonitorTarget] = watch.custom
        let found: [String: Bool] = await FleetLightsModel.probe(targets, with: prober)
        if targets == watch.custom { probed = found }
    }

    nonisolated static func probe(_ targets: [MonitorTarget], with prober: StatusProbing?) async -> [String: Bool] {
        guard let prober = prober, !targets.isEmpty else { return [:] }
        return await withTaskGroup(of: (String, Bool).self) { (group: inout TaskGroup<(String, Bool)>) -> [String: Bool] in
            for target in targets {
                group.addTask { (target.lightId, await prober.probe(target.url)) }
            }
            var out: [String: Bool] = [:]
            for await (id, ok) in group { out[id] = ok }
            return out
        }
    }
}
