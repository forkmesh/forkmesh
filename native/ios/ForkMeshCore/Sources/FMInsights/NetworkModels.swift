import FMAccount
import FMApi
import FMBase
import Foundation
import Observation

public enum NodeDiagnosticSeverity: Int, Comparable, Equatable, Sendable {
    case ok = 0, info = 1, warning = 2, critical = 3

    public static func < (lhs: NodeDiagnosticSeverity, rhs: NodeDiagnosticSeverity) -> Bool { lhs.rawValue < rhs.rawValue }

    public var label: String {
        switch self {
        case .ok: return "OK"
        case .info: return "Info"
        case .warning: return "Warning"
        case .critical: return "Critical"
        }
    }

    public var tone: StatsTileStatus {
        switch self {
        case .ok: return .good
        case .info: return .none
        case .warning: return .warning
        case .critical: return .critical
        }
    }
}

public struct NodeDiagnosticFinding: Equatable, Sendable, Identifiable {
    public static let maxFindings = 8
    public static let maxMessage = 160

    public var id: String
    public var severity: NodeDiagnosticSeverity
    public var message: String

    public init(id: String, severity: NodeDiagnosticSeverity, message: String) {
        self.id = id
        self.severity = severity
        self.message = message
    }

    public static func parse(_ value: JSONValue?) -> [NodeDiagnosticFinding] {
        var findings: [NodeDiagnosticFinding] = []
        for item in value?.arrayValue ?? [] {
            if findings.count >= maxFindings { break }
            let object = item.objectValue ?? JSONObject()
            let id = String(object.str("i").prefix(32))
            let message = String(object.str("m").prefix(maxMessage))
            if id.isEmpty || message.isEmpty { continue }
            let raw = min(max(object.int("s", NodeDiagnosticSeverity.info.rawValue), 0), 3)
            findings.append(NodeDiagnosticFinding(
                id: id, severity: NodeDiagnosticSeverity(rawValue: raw) ?? .info, message: message))
        }
        return findings.sorted { $0.severity > $1.severity }
    }

    public static func worst(_ findings: [NodeDiagnosticFinding]) -> NodeDiagnosticSeverity {
        findings.map(\.severity).max() ?? .ok
    }
}

public struct NodeRow: Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var owner: String
    public var platform: String
    public var version: String
    public var online: Bool
    public var isSelf: Bool
    public var mirrors: Int
    public var diagnostics: [NodeDiagnosticFinding]

    public init(id: String, name: String, owner: String = "", platform: String = "", version: String = "",
                online: Bool, isSelf: Bool = false, mirrors: Int = 0, diagnostics: [NodeDiagnosticFinding] = []) {
        self.id = id
        self.name = name
        self.owner = owner
        self.platform = platform
        self.version = version
        self.online = online
        self.isSelf = isSelf
        self.mirrors = mirrors
        self.diagnostics = diagnostics
    }

    public init(member: Member, diagnostics: [NodeDiagnosticFinding] = []) {
        self.init(
            id: member.id, name: member.nodeName.isEmpty ? member.name : member.nodeName, owner: member.owner,
            platform: member.platform, version: member.version, online: member.online, isSelf: member.isSelf,
            mirrors: member.mirrors.count, diagnostics: diagnostics)
    }

    public var worst: NodeDiagnosticSeverity { NodeDiagnosticFinding.worst(diagnostics) }

    public var platformLabel: String {
        switch platform.lowercased() {
        case "linux": return "Linux"
        case "macos", "darwin", "osx": return "macOS"
        case "windows", "win": return "Windows"
        case "ios": return "iPhone"
        case "android": return "Android"
        case "web", "browser": return "Web"
        case "": return "Unknown"
        default: return platform
        }
    }

    public var subtitle: String {
        var parts: [String] = [platformLabel]
        if !version.isEmpty { parts.append("v\(version)") }
        if !owner.isEmpty && owner.lowercased() != name.lowercased() { parts.append("owner \(owner)") }
        if mirrors > 0 { parts.append("\(mirrors) mirror\(mirrors == 1 ? "" : "s")") }
        return parts.joined(separator: " · ")
    }

    public var accessibilityLabel: String {
        var text = "\(name), \(online ? "online" : "offline"), \(subtitle)"
        if worst >= .warning { text += ", \(diagnostics.filter { $0.severity >= .warning }.count) health warnings" }
        return text
    }
}

public enum NodeDirectory {
    public static let maxRows = 500

    public static func merge(live: [NodeRow], onlineNames: [String]) -> [NodeRow] {
        var rows: [NodeRow] = []
        var seen = Set<String>()
        for row in live {
            let key = row.name.lowercased()
            if key.isEmpty || seen.contains(key) { continue }
            seen.insert(key)
            rows.append(row)
        }
        for name in onlineNames {
            let key = name.lowercased()
            if key.isEmpty || seen.contains(key) { continue }
            seen.insert(key)
            rows.append(NodeRow(id: "relay:\(key)", name: name, online: true))
        }
        return Array(rows.sorted(by: order).prefix(maxRows))
    }

    static func order(_ a: NodeRow, _ b: NodeRow) -> Bool {
        if a.isSelf != b.isSelf { return a.isSelf }
        if a.worst != b.worst { return a.worst > b.worst }
        if a.online != b.online { return a.online }
        return a.name.lowercased() < b.name.lowercased()
    }

    public static func filter(_ rows: [NodeRow], query: String) -> [NodeRow] {
        let needle = query.trimmingCharacters(in: .whitespaces).lowercased()
        if needle.isEmpty { return rows }
        return rows.filter {
            $0.name.lowercased().contains(needle) || $0.owner.lowercased().contains(needle)
                || $0.platformLabel.lowercased().contains(needle)
        }
    }
}

public struct NetworkSnapshot: Equatable, Sendable {
    public var repos: Int
    public var hosts: Int
    public var clients: Int
    public var onlineNodes: [String]

    public init(json: JSONObject) {
        repos = json.int("repos")
        hosts = json.int("hosts")
        clients = json.int("clients")
        onlineNodes = json.list("onlineNodes").compactMap { value in
            let name = value.text.trimmingCharacters(in: .whitespaces)
            return name.isEmpty ? nil : name
        }
    }
}

public struct OnlineHistory: Equatable, Sendable {
    public struct Hour: Equatable, Sendable, Identifiable {
        public var hourTs: Int64
        public var nodeMinutes: Int
        public var id: Int64 { hourTs }
    }

    public struct Node: Equatable, Sendable, Identifiable {
        public var id: String
        public var label: String
        public var totalMinutes: Int
    }

    public var hours: [Hour]
    public var nodes: [Node]

    public init(json: JSONObject) {
        hours = json.maps("hours").map { Hour(hourTs: $0.int64("hourTs"), nodeMinutes: $0.int("nodeMinutes")) }
        nodes = json.maps("nodes").prefix(50).map {
            Node(id: $0.str("id"), label: $0.str("label"), totalMinutes: $0.int("totalMinutes"))
        }
    }

    public var series: StatsTileSeries {
        StatsTileSeries(labels: hours.map { String($0.hourTs) }, values: hours.map { Double($0.nodeMinutes) })
    }

    public var peakHour: Hour? { hours.max { $0.nodeMinutes < $1.nodeMinutes } }
}

public struct RelayRow: Equatable, Sendable, Identifiable {
    public var id: String
    public var label: String
    public var host: String
    public var active: Bool
    public var verifiedAt: Int64
    public var declared: Bool
    public var missing: [String]

    public init(profile: RelayProfile, activeId: String) {
        id = profile.id
        label = profile.label.isEmpty ? profile.host : profile.label
        host = profile.host
        active = profile.id == activeId
        verifiedAt = profile.verifiedAt
        declared = profile.features.isDeclared
        missing = RelayFeature.allCases.filter { !profile.features.has($0) }.map(\.wireName)
    }

    public init(id: String, label: String, host: String, active: Bool, verifiedAt: Int64 = 0,
                declared: Bool = false, missing: [String] = []) {
        self.id = id
        self.label = label
        self.host = host
        self.active = active
        self.verifiedAt = verifiedAt
        self.declared = declared
        self.missing = missing
    }
}

public struct EndpointRow: Equatable, Sendable, Identifiable {
    public var id: String { name }
    public var name: String
    public var url: String
    public var purpose: String

    public static func forRelay(serverUrl: String) -> [EndpointRow] {
        guard let components = URLComponents(string: serverUrl), let host = components.host, !host.isEmpty else {
            return []
        }
        let secure = components.scheme == "wss" || components.scheme == "https"
        let port = components.port.map { ":\($0)" } ?? ""
        let http = "\(secure ? "https" : "http")://\(host)\(port)"
        return [
            EndpointRow(name: "API", url: "\(http)/api", purpose: "Accounts, repositories, issues and pings"),
            EndpointRow(name: "Relay socket", url: serverUrl, purpose: "Chat, presence and desktop control"),
            EndpointRow(name: "Git", url: http, purpose: "Clone and fetch over HTTPS"),
            EndpointRow(name: "Status", url: "\(http)/api/status", purpose: "Health checks shown on the Status page"),
        ]
    }
}

public enum NetworkSection: String, CaseIterable, Sendable {
    case nodes, relays, endpoints, activity

    public var title: String {
        switch self {
        case .nodes: return "Nodes"
        case .relays: return "Relays"
        case .endpoints: return "Endpoints"
        case .activity: return "Activity"
        }
    }

    public init(route: String?) {
        self = NetworkSection(rawValue: (route ?? "").lowercased()) ?? .nodes
    }
}

@MainActor
@Observable
public final class NetworkScreenModel {
    @ObservationIgnored private let transport: InsightsRequesting
    @ObservationIgnored private let livePeers: @MainActor () -> [NodeRow]

    public var section: NetworkSection
    public var query = ""
    public private(set) var snapshot: InsightsLoad<NetworkSnapshot> = .idle
    public private(set) var history: InsightsLoad<OnlineHistory> = .idle

    public init(transport: InsightsRequesting, section: NetworkSection = .nodes,
                livePeers: @escaping @MainActor () -> [NodeRow] = { [] }) {
        self.transport = transport
        self.section = section
        self.livePeers = livePeers
    }

    public func reload() async {
        if snapshot.value == nil { snapshot = .loading }
        if history.value == nil { history = .loading }
        async let stats: Void = loadSnapshot()
        async let hours: Void = loadHistory()
        _ = await (stats, hours)
    }

    private func loadSnapshot() async {
        do {
            snapshot = .loaded(NetworkSnapshot(json: try await transport.call("GET", "/api/network/stats", nil)))
        } catch {
            if snapshot.value == nil { snapshot = .failed(InsightsError.from(error)) }
        }
    }

    private func loadHistory() async {
        do {
            history = .loaded(OnlineHistory(json: try await transport.call("GET", "/api/network/online-history", nil)))
        } catch {
            if history.value == nil { history = .failed(InsightsError.from(error)) }
        }
    }

    public var nodes: [NodeRow] {
        NodeDirectory.filter(
            NodeDirectory.merge(live: livePeers(), onlineNames: snapshot.value?.onlineNodes ?? []), query: query)
    }

    public var warningCount: Int { nodes.filter { $0.worst >= .warning }.count }
}
