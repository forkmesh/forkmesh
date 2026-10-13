import FMBase
import Foundation

public struct FleetCostRow: Equatable, Sendable, Identifiable {
    public var key: String
    public var count: Int
    public var monthlyCost: Double

    public var id: String { key }

    public init(key: String, count: Int, monthlyCost: Double) {
        self.key = key
        self.count = count
        self.monthlyCost = monthlyCost
    }
}

public struct FleetCosts: Equatable, Sendable {
    public var hosts: Int
    public var vultr: Int
    public var monthlyCost: Double
    public var byRegion: [FleetCostRow]
    public var byPlan: [FleetCostRow]

    public init(json: JSONObject) {
        hosts = json.int("hosts")
        vultr = json.int("vultr")
        monthlyCost = json.double("monthlyCost")
        byRegion = json.maps("byRegion").map {
            FleetCostRow(key: $0.str("region"), count: $0.int("count"), monthlyCost: $0.double("monthlyCost"))
        }
        byPlan = json.maps("byPlan").map {
            FleetCostRow(key: $0.str("plan"), count: $0.int("count"), monthlyCost: $0.double("monthlyCost"))
        }
    }

    public var yearlyCost: Double { monthlyCost * 12 }

    public var line: String {
        if vultr == 0 { return "No Vultr servers" }
        return "\(vultr) Vultr server\(vultr == 1 ? "" : "s") · \(RemoteFormat.money(monthlyCost))/month"
    }
}

public struct FleetTarget: Equatable, Sendable {
    public var available: Bool
    public var enabled: Bool
    public var desired: Int
    public var status: String
    public var nextCheckInMs: Int
    public var reconciling: Bool

    public init(json: JSONObject) {
        available = json.bool("available")
        enabled = json.bool("enabled")
        desired = json.int("desired")
        status = json.str("status")
        nextCheckInMs = json.int("nextCheckInMs", -1)
        reconciling = json.bool("reconciling")
    }

    public var line: String {
        if !available { return "Open the desktop's Hosts page once to manage the fleet size." }
        if !enabled { return "The desktop does not manage the fleet size." }
        var parts = ["Keeps \(desired) healthy mirror\(desired == 1 ? "" : "s")"]
        if reconciling { parts.append("checking now") }
        else if nextCheckInMs >= 0 { parts.append("next check in \(RemoteFormat.duration(ms: nextCheckInMs))") }
        return parts.joined(separator: " · ")
    }
}

public enum ProvisionStageState: String, Equatable, Sendable {
    case pending, active, done, failed

    public init(wire: String) { self = ProvisionStageState(rawValue: wire) ?? .pending }
}

public struct ProvisionStage: Equatable, Sendable, Identifiable {
    public var number: Int
    public var name: String
    public var state: ProvisionStageState
    public var detail: String

    public var id: Int { number }

    public init(json: JSONObject) {
        number = json.int("number")
        name = json.str("name")
        state = ProvisionStageState(wire: json.str("status"))
        detail = json.str("detail")
    }
}

public struct ProvisionStatus: Equatable, Sendable {
    public var active: Bool
    public var node: String
    public var stage: Int
    public var stageCount: Int
    public var state: String
    public var detail: String
    public var message: String
    public var gap: String
    public var resumable: Bool
    public var attempts: Int
    public var stages: [ProvisionStage]
    public var log: [String]

    public init(json: JSONObject) {
        active = json.bool("active")
        node = json.str("node")
        stage = json.int("stage")
        stageCount = json.int("stageCount", 6)
        state = json.str("state")
        detail = json.str("detail")
        message = json.str("message")
        gap = json.str("gap")
        resumable = json.bool("resumable")
        attempts = json.int("attempts")
        stages = json.maps("stages").map(ProvisionStage.init(json:))
        log = json.strings("log")
    }

    public var isVisible: Bool { active || resumable || (!node.isEmpty && state == "failed") }

    public var progress: Double {
        guard stageCount > 0 else { return 0 }
        let done = stages.filter { $0.state == .done }.count
        return min(1, Double(done) / Double(stageCount))
    }

    public var headline: String {
        let name = node.isEmpty ? "New mirror" : node
        if active {
            let current = stages.first { $0.number == stage }?.name ?? ""
            return current.isEmpty ? "Deploying \(name)" : "Deploying \(name): \(current)"
        }
        if resumable { return "\(name) is paused" }
        if state == "failed" { return "\(name) failed" }
        if state == "done" { return "\(name) is live" }
        return name
    }
}

public struct GatewayHealthRow: Equatable, Sendable, Identifiable {
    public var host: String
    public var downSinceMs: Int
    public var downForMs: Int
    public var retired: Bool

    public var id: String { host }

    public init(json: JSONObject) {
        host = json.str("host")
        downSinceMs = json.int("downSinceMs")
        downForMs = json.int("downForMs")
        retired = json.bool("retired")
    }

    public var line: String {
        retired
            ? "Down \(RemoteFormat.duration(ms: downForMs)) · retired from the automatic push"
            : "Down \(RemoteFormat.duration(ms: downForMs))"
    }
}

public struct FleetSummary: Equatable, Sendable {
    public var costs: FleetCosts
    public var fleet: FleetTarget
    public var provision: ProvisionStatus
    public var gateways: [GatewayHealthRow]
    public var mirrorClientStatus: String
    public var mirrorClientHealth: String
    public var pushing: Int
    public var pushPending: Int
    public var canCreate: Bool
    public var busy: Bool
    public var deploying: Int
    public var admin: Bool
    public var nodeDeleteInProgress: Bool
    public var nodeDeleteTarget: String
    public var lastResult: String
    public var running: [String]

    public init(json: JSONObject) {
        costs = FleetCosts(json: json.obj("costs"))
        fleet = FleetTarget(json: json.obj("fleet"))
        provision = ProvisionStatus(json: json.obj("provision"))
        gateways = json.maps("gateways").map(GatewayHealthRow.init(json:))
        mirrorClientStatus = json.obj("mirrorClient").str("status")
        mirrorClientHealth = json.obj("mirrorClient").str("health")
        pushing = json.obj("mirrorPush").int("pushing")
        pushPending = json.obj("mirrorPush").int("pending")
        canCreate = json.bool("canCreate")
        busy = json.bool("busy")
        deploying = json.int("deploying")
        admin = json.bool("admin")
        nodeDeleteInProgress = json.obj("nodeDelete").bool("inProgress")
        nodeDeleteTarget = json.obj("nodeDelete").str("target")
        lastResult = json.str("lastResult")
        running = json.strings("running")
    }

    public var gatewaysDown: Int { gateways.filter { !$0.retired }.count }
}

public struct FleetRepo: Equatable, Sendable, Identifiable {
    public var repo: String
    public var isPrivate: Bool
    public var published: Bool
    public var pushing: Bool

    public var id: String { repo }

    public init(json: JSONObject) {
        repo = json.str("repo")
        isPrivate = json.bool("private")
        published = json.bool("published")
        pushing = json.bool("pushing")
    }
}

public struct DiskUsageEntry: Equatable, Sendable, Identifiable {
    public var name: String
    public var path: String
    public var bytes: Double
    public var isTotal: Bool

    public var id: String { path }

    public init(json: JSONObject) {
        name = json.str("name")
        path = json.str("path")
        bytes = json.double("bytes")
        isTotal = json.bool("total")
    }
}

public struct DiskUsage: Equatable, Sendable {
    public var host: String
    public var path: String
    public var entries: [DiskUsageEntry]

    public init(json: JSONObject) {
        host = json.str("host")
        path = json.str("path")
        entries = json.maps("entries").map(DiskUsageEntry.init(json:))
    }

    public var total: Double { entries.first { $0.isTotal }?.bytes ?? children.reduce(0) { $0 + $1.bytes } }

    public var children: [DiskUsageEntry] { entries.filter { !$0.isTotal } }

    public func share(of entry: DiskUsageEntry) -> Double {
        total > 0 ? min(1, entry.bytes / total) : 0
    }

    public var parentPath: String? {
        if path == "/" || path.isEmpty { return nil }
        var parts = path.split(separator: "/").map(String.init)
        parts.removeLast()
        return "/" + parts.joined(separator: "/")
    }
}

public enum FleetDeployMode: String, CaseIterable, Sendable, Identifiable {
    case install
    case reinstall
    case source

    public var id: String { rawValue }

    public var title: String {
        switch self {
        case .install: return "Install the latest release"
        case .reinstall: return "Uninstall and reinstall all"
        case .source: return "Update all from source"
        }
    }
}

public enum FleetAction: Equatable, Sendable {
    case add(name: String, address: String, user: String)
    case forget(host: String)
    case deployAll(FleetDeployMode)
    case uninstall(host: String)
    case agentClis(host: String)
    case actions(host: String, enabled: Bool)
    case linkCode(String)
    case resume
    case fleetTarget(enabled: Bool, desired: Int)
    case reconcile
    case mirrorPush(repo: String)
    case deleteNode(String)

    public var wireName: String {
        switch self {
        case .add: return "add"
        case .forget: return "forget"
        case .deployAll: return "deployAll"
        case .uninstall: return "uninstall"
        case .agentClis: return "agentClis"
        case .actions: return "actions"
        case .linkCode: return "linkCode"
        case .resume: return "resume"
        case .fleetTarget: return "fleetTarget"
        case .reconcile: return "reconcile"
        case .mirrorPush: return "mirrorPush"
        case .deleteNode: return "deleteNode"
        }
    }

    public var params: JSONObject {
        var out: JSONObject = ["action": .string(wireName)]
        switch self {
        case .add(let name, let address, let user):
            out["host"] = .string(name.trimmingCharacters(in: .whitespaces))
            out["address"] = .string(address.trimmingCharacters(in: .whitespaces))
            out["user"] = .string(user.trimmingCharacters(in: .whitespaces))
        case .forget(let host), .uninstall(let host), .agentClis(let host), .deleteNode(let host):
            out["host"] = .string(host)
        case .deployAll(let mode):
            out["mode"] = .string(mode.rawValue)
        case .actions(let host, let enabled):
            out["host"] = .string(host)
            out["enabled"] = .bool(enabled)
        case .linkCode(let code):
            out["code"] = .string(code.trimmingCharacters(in: .whitespaces))
        case .fleetTarget(let enabled, let desired):
            out["enabled"] = .bool(enabled)
            out["desired"] = .int(Int64(desired))
        case .mirrorPush(let repo):
            out["repo"] = .string(repo)
        case .resume, .reconcile:
            break
        }
        return out
    }

    public var title: String {
        switch self {
        case .add: return "Add host"
        case .forget: return "Forget host"
        case .deployAll(let mode): return mode.title
        case .uninstall: return "Uninstall ForkMesh"
        case .agentClis: return "Install agent CLIs"
        case .actions(_, let enabled): return enabled ? "Turn Actions on" : "Turn Actions off"
        case .linkCode: return "Link a node"
        case .resume: return "Resume deployment"
        case .fleetTarget: return "Fleet target"
        case .reconcile: return "Check the fleet now"
        case .mirrorPush: return "Mirror push"
        case .deleteNode: return "Delete node"
        }
    }

    public var isDestructive: Bool {
        switch self {
        case .uninstall, .deleteNode, .forget: return true
        case .deployAll(let mode): return mode == .reinstall
        default: return false
        }
    }
}

public enum FleetInput {
    public static func validLinkCode(_ code: String) -> Bool {
        let trimmed = code.trimmingCharacters(in: .whitespaces)
        return trimmed.count == 6 && trimmed.allSatisfy(\.isASCIIDigit)
    }

    public static func validHostName(_ name: String) -> Bool {
        let trimmed = name.trimmingCharacters(in: .whitespaces)
        guard let first = trimmed.first, trimmed.count <= 63, first.isASCIIAlphanumeric else { return false }
        return trimmed.allSatisfy { $0.isASCIIAlphanumeric || $0 == "." || $0 == "_" || $0 == "-" }
    }

    public static func validAddress(_ address: String) -> Bool {
        let trimmed = address.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty, trimmed.count <= 253 else { return false }
        return trimmed.allSatisfy { $0.isASCIIAlphanumeric || $0 == "." || $0 == "-" || $0 == ":" }
    }

    public static func validUser(_ user: String) -> Bool {
        let trimmed = user.trimmingCharacters(in: .whitespaces)
        guard let first = trimmed.first, trimmed.count <= 32, first.isLowercase || first == "_" else { return false }
        return trimmed.allSatisfy { ($0.isASCII && $0.isLowercase) || $0.isASCIIDigit || $0 == "_" || $0 == "-" }
    }
}

extension Character {
    var isASCIIDigit: Bool { isASCII && isNumber }
    var isASCIIAlphanumeric: Bool { isASCII && (isLetter || isNumber) }
}
