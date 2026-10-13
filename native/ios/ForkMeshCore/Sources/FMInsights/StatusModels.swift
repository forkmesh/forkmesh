import FMBase
import Foundation
import Observation

public enum ServiceState: String, Equatable, Sendable {
    case operational, degraded, down, unknown, future

    public init(raw: String) {
        self = ServiceState(rawValue: raw) ?? .unknown
    }

    public var label: String {
        switch self {
        case .operational: return "Operational"
        case .degraded: return "Degraded"
        case .down: return "Down"
        case .unknown: return "No data"
        case .future: return "Pending"
        }
    }

    public var tone: StatsTileStatus {
        switch self {
        case .operational: return .good
        case .degraded: return .warning
        case .down: return .critical
        case .unknown, .future: return .none
        }
    }

    public var severity: Int {
        switch self {
        case .down: return 3
        case .degraded: return 2
        case .unknown: return 1
        case .operational, .future: return 0
        }
    }
}

public struct StatusMinute: Equatable, Sendable, Identifiable {
    public var ts: Int64
    public var state: ServiceState
    public var id: Int64 { ts }
}

public struct StatusSystem: Equatable, Sendable, Identifiable {
    public var id: String
    public var label: String
    public var state: ServiceState
    public var detail: String
    public var uptime24h: Double?
    public var uptime30d: Double?
    public var checks24h: Int
    public var failures24h: Int
    public var reason: String
    public var reasonTs: Int64
    public var observedAt: Int64
    public var minutes: [StatusMinute]

    public init(json: JSONObject) {
        id = json.str("id")
        label = json.str("label").isEmpty ? json.str("id") : json.str("label")
        state = ServiceState(raw: json.str("status"))
        detail = json.str("checkDescription")
        uptime24h = json["uptime24hPct"]?.doubleValue
        uptime30d = json["uptimePct"]?.doubleValue
        checks24h = json.int("checks24h")
        failures24h = json.int("failures24h")
        reason = json["reason"].flatMap { $0.isNull ? nil : $0.text } ?? ""
        reasonTs = json.int64("reasonTs")
        observedAt = json.int64("observedAt")
        minutes = json.maps("minutes").suffix(60).map {
            StatusMinute(ts: $0.int64("minuteTs"), state: ServiceState(raw: $0.str("status")))
        }
    }

    public var isMirror: Bool { id.hasPrefix("mirror:") }
}

public struct StatusCurrent: Equatable, Sendable {
    public var catalogRepos: Int?
    public var onlineNodes: Int?
    public var errors24h: Int?
    public var mainnodeOnline: Bool?
    public var mainnodeLastSeen: Int64
    public var monitoringState: String
    public var lastSampleTs: Int64

    public init(json: JSONObject) {
        catalogRepos = json["catalogRepos"]?.intValue
        onlineNodes = json["onlineNodes"]?.intValue
        errors24h = json["errors24h"]?.intValue
        mainnodeOnline = json["mainnodeOnline"]?.boolValue
        mainnodeLastSeen = json.int64("mainnodeLastSeenTs")
        monitoringState = json["monitoringState"].flatMap { $0.isNull ? nil : $0.text } ?? ""
        lastSampleTs = json.int64("lastCronSampleTs")
    }
}

public struct StatusReport: Equatable, Sendable {
    public var now: Int64
    public var systems: [StatusSystem]
    public var current: StatusCurrent

    public init(json: JSONObject) {
        now = json.int64("now")
        systems = json.maps("systems").map(StatusSystem.init(json:))
        current = StatusCurrent(json: json.obj("current"))
    }

    public var overall: ServiceState {
        let relevant = systems.filter { !$0.isMirror && $0.state != .future }
        guard let worst = relevant.max(by: { $0.state.severity < $1.state.severity }) else { return .unknown }
        if worst.state == .unknown && relevant.contains(where: { $0.state == .operational }) { return .operational }
        return worst.state
    }

    public var headline: String {
        if current.monitoringState == "stale" { return "Monitoring is behind" }
        if current.monitoringState == "empty" { return "No monitoring samples yet" }
        switch overall {
        case .operational: return "All systems operational"
        case .degraded: return "Some systems are degraded"
        case .down: return "\(down.count) system\(down.count == 1 ? " is" : "s are") down"
        case .unknown, .future: return "Status unknown"
        }
    }

    public var down: [StatusSystem] { systems.filter { $0.state == .down && !$0.isMirror } }

    public var coreSystems: [StatusSystem] { systems.filter { !$0.isMirror } }
    public var mirrorSystems: [StatusSystem] { systems.filter(\.isMirror) }

    public var websiteUptime24h: Double? {
        (systems.first { $0.id == "website" } ?? coreSystems.first)?.uptime24h
    }
}

public enum ConnectionLight: Equatable, Sendable {
    case live, degraded, offline

    public static func of(report: StatusReport?, reachable: Bool) -> ConnectionLight {
        if !reachable { return .offline }
        guard let report else { return .degraded }
        return report.overall == .operational ? .live : .degraded
    }
}

@MainActor
@Observable
public final class StatusScreenModel {
    @ObservationIgnored private let transport: InsightsRequesting
    public private(set) var report: InsightsLoad<StatusReport> = .idle
    public private(set) var lastLoadedAt: Date?

    public init(transport: InsightsRequesting) {
        self.transport = transport
    }

    public func reload() async {
        if report.value == nil { report = .loading }
        do {
            report = .loaded(StatusReport(json: try await transport.call("GET", "/api/status", nil)))
            lastLoadedAt = Date()
        } catch {
            if report.value == nil { report = .failed(InsightsError.from(error)) }
        }
    }
}
