// New for the native app (WS7): the owner's hosts (Vultr mirror nodes, SSH-installed nodes) as
// the desktop's Hosts page knows them, read and changed over the desktop control channel (LAN or
// relay). The desktop keeps the Vultr key and the SSH keys; these are only rows and results.
//
// Desktop side: desktop/src/MainWindowLocalControlHosts.cpp and LocalControlHosts.h.

import FMBase
import Foundation

/// A host's health, from the desktop's own probe.
public enum HostHealth: String, Equatable, Sendable {
    case online
    case attention
    case offline
    case provisioning
    case unknown

    public init(wire: String) {
        self = HostHealth(rawValue: wire) ?? .unknown
    }

    public var label: String {
        switch self {
        case .online: return "Online"
        case .attention: return "Needs attention"
        case .offline: return "Offline"
        case .provisioning: return "Provisioning"
        case .unknown: return "Checking"
        }
    }

    /// For sorting: what needs a look comes first.
    public var urgency: Int {
        switch self {
        case .offline: return 0
        case .attention: return 1
        case .provisioning: return 2
        case .unknown: return 3
        case .online: return 4
        }
    }
}

/// Something the phone can ask the desktop to do to a host.
public enum HostAction: String, Equatable, Sendable, CaseIterable, Identifiable {
    case update
    case installBinary
    case restart
    case stop
    case start
    case install
    case destroy
    case terminal

    public var id: String { rawValue }

    public var title: String {
        switch self {
        case .update: return "Update ForkMesh"
        case .installBinary: return "Install the desktop's build"
        case .restart: return "Restart service"
        case .stop: return "Stop service"
        case .start: return "Start service"
        case .install: return "Create host"
        case .destroy: return "Destroy server"
        case .terminal: return "SSH terminal"
        }
    }

    public var isDestructive: Bool { self == .destroy || self == .stop }
    public var opensTerminal: Bool { self == .terminal }
    /// Needs the host's name typed back, not just a tap.
    public var needsTypedName: Bool { self == .destroy }
}

public struct HostRow: Equatable, Sendable, Identifiable {
    public var name: String
    public var address: String
    public var user: String
    public var status: String
    public var provider: String
    public var region: String
    public var plan: String
    public var monthlyCost: Double
    public var ramMb: Int
    public var diskGb: Int
    public var dnsHostname: String
    public var health: HostHealth
    public var healthLabel: String
    public var version: String
    public var online: Bool
    public var actions: [HostAction]
    /// Detail only: the newest log lines and the desktop's last result line.
    public var recentLog: [String]
    public var lastResult: String

    public var id: String { name.lowercased() }

    public init(
        name: String, address: String = "", user: String = "", status: String = "", provider: String = "",
        region: String = "", plan: String = "", monthlyCost: Double = 0, ramMb: Int = 0, diskGb: Int = 0,
        dnsHostname: String = "", health: HostHealth = .unknown, healthLabel: String = "", version: String = "",
        online: Bool = false, actions: [HostAction] = [], recentLog: [String] = [], lastResult: String = ""
    ) {
        self.name = name
        self.address = address
        self.user = user
        self.status = status
        self.provider = provider
        self.region = region
        self.plan = plan
        self.monthlyCost = monthlyCost
        self.ramMb = ramMb
        self.diskGb = diskGb
        self.dnsHostname = dnsHostname
        self.health = health
        self.healthLabel = healthLabel
        self.version = version
        self.online = online
        self.actions = actions
        self.recentLog = recentLog
        self.lastResult = lastResult
    }

    public init(json: JSONObject) {
        self.init(
            name: json.str("name"), address: json.str("address"), user: json.str("user"),
            status: json.str("status"), provider: json.str("provider"), region: json.str("region"),
            plan: json.str("plan"), monthlyCost: json.double("monthlyCost"), ramMb: json.int("ramMb"),
            diskGb: json.int("diskGb"), dnsHostname: json.str("dnsHostname"),
            health: HostHealth(wire: json.str("health")), healthLabel: json.str("healthLabel"),
            version: json.str("version"), online: json.bool("online"),
            actions: (json["actions"]?.strings ?? []).compactMap(HostAction.init(rawValue:)),
            recentLog: json["recentLog"]?.strings ?? [], lastResult: json.str("lastResult"))
    }

    public var isVultr: Bool { provider.lowercased() == "vultr" }

    /// "vc2-1c-1gb · ewr · $5/mo"
    public var planLine: String {
        var parts: [String] = []
        if !plan.isEmpty { parts.append(plan) }
        if !region.isEmpty { parts.append(region) }
        if monthlyCost > 0 { parts.append(HostFormat.monthly(monthlyCost)) }
        return parts.joined(separator: " · ")
    }

    /// The health line: the desktop's own probe text when it has one.
    public var healthLine: String {
        let probe = healthLabel.trimmingCharacters(in: CharacterSet(charactersIn: "●◌ ").union(.whitespaces))
        return probe.isEmpty ? health.label : probe
    }
}

public struct HostsSnapshot: Equatable, Sendable {
    public var hosts: [HostRow]
    public var online: Int
    public var attention: Int
    /// The desktop holds a Vultr key, so it can create servers.
    public var canCreate: Bool
    /// An install or create is running on the desktop.
    public var busy: Bool
    public var provisioning: String
    public var desktop: String

    public init(
        hosts: [HostRow] = [], online: Int = 0, attention: Int = 0, canCreate: Bool = false, busy: Bool = false,
        provisioning: String = "", desktop: String = ""
    ) {
        self.hosts = hosts
        self.online = online
        self.attention = attention
        self.canCreate = canCreate
        self.busy = busy
        self.provisioning = provisioning
        self.desktop = desktop
    }

    public init(json: JSONObject) {
        self.init(
            hosts: json.maps("hosts").map(HostRow.init(json:)), online: json.int("online"),
            attention: json.int("attention"), canCreate: json.bool("canCreate"), busy: json.bool("busy"),
            provisioning: json.str("provisioning"), desktop: json.str("desktop"))
    }

    /// Needs-attention first, then by name.
    public var sorted: [HostRow] {
        hosts.sorted { a, b in
            if a.health.urgency != b.health.urgency { return a.health.urgency < b.health.urgency }
            return a.name.localizedCaseInsensitiveCompare(b.name) == .orderedAscending
        }
    }

    public var summary: String {
        if hosts.isEmpty { return "No hosts yet" }
        if attention == 0 && online == hosts.count {
            return "All \(hosts.count) host\(hosts.count == 1 ? "" : "s") online"
        }
        var parts = ["\(hosts.count) saved", "\(online) online"]
        if attention > 0 { parts.append("\(attention) need\(attention == 1 ? "s" : "") attention") }
        return parts.joined(separator: " · ")
    }
}

/// A Vultr plan the desktop would deploy a mirror on.
public struct HostPlanChoice: Equatable, Sendable, Identifiable {
    public var id: String
    public var monthlyCost: Double
    public var ramMb: Int
    public var diskGb: Int
    public var vcpus: Int
    /// US regions, in the desktop's order of preference.
    public var regions: [String]

    public init(id: String, monthlyCost: Double = 0, ramMb: Int = 0, diskGb: Int = 0, vcpus: Int = 0, regions: [String] = []) {
        self.id = id
        self.monthlyCost = monthlyCost
        self.ramMb = ramMb
        self.diskGb = diskGb
        self.vcpus = vcpus
        self.regions = regions
    }

    public init(json: JSONObject) {
        self.init(
            id: json.str("id"), monthlyCost: json.double("monthlyCost"), ramMb: json.int("ramMb"),
            diskGb: json.int("diskGb"), vcpus: json.int("vcpus"), regions: json["regions"]?.strings ?? [])
    }

    public var label: String {
        var parts = [HostFormat.monthly(monthlyCost)]
        if vcpus > 0 { parts.append("\(vcpus) vCPU") }
        if ramMb > 0 { parts.append(HostFormat.memory(ramMb)) }
        if diskGb > 0 { parts.append("\(diskGb) GB disk") }
        return parts.joined(separator: " · ")
    }
}

/// What the desktop offers for a new host.
public struct HostCreateOptions: Equatable, Sendable {
    public var vultr: Bool
    public var plans: [HostPlanChoice]
    public var defaultPlan: String
    public var defaultRegion: String
    public var suggestedName: String

    public init(
        vultr: Bool = false, plans: [HostPlanChoice] = [], defaultPlan: String = "", defaultRegion: String = "",
        suggestedName: String = ""
    ) {
        self.vultr = vultr
        self.plans = plans
        self.defaultPlan = defaultPlan
        self.defaultRegion = defaultRegion
        self.suggestedName = suggestedName
    }

    public init(json: JSONObject) {
        self.init(
            vultr: json.bool("vultr"), plans: json.maps("plans").map(HostPlanChoice.init(json:)),
            defaultPlan: json.str("defaultPlan"), defaultRegion: json.str("defaultRegion"),
            suggestedName: json.str("suggestedName"))
    }
}

/// The desktop's answer to hosts.prepare: what will happen, and the one-use token to do it.
public struct HostConfirmation: Equatable, Sendable, Identifiable {
    public var token: String
    public var action: HostAction
    public var host: String
    public var summary: String
    /// Set for destroy: the host's name, which must be typed back exactly.
    public var typeToConfirm: String
    public var expiresInMs: Int

    public init(token: String, action: HostAction, host: String, summary: String, typeToConfirm: String = "", expiresInMs: Int = 60_000) {
        self.token = token
        self.action = action
        self.host = host
        self.summary = summary
        self.typeToConfirm = typeToConfirm
        self.expiresInMs = expiresInMs
    }

    public init?(json: JSONObject) {
        guard let action = HostAction(rawValue: json.str("action")), !json.str("token").isEmpty else { return nil }
        self.init(
            token: json.str("token"), action: action, host: json.str("host"), summary: json.str("summary"),
            typeToConfirm: json.str("typeToConfirm"), expiresInMs: json.int("expiresInMs", 60_000))
    }

    public var id: String { token }

    public var needsTypedName: Bool { !typeToConfirm.isEmpty }

    /// Whether `typed` confirms: the host's name exactly, ignoring only surrounding spaces (the
    /// desktop checks the same).
    public func accepts(typed: String) -> Bool {
        if !needsTypedName { return true }
        return typed.trimmingCharacters(in: .whitespacesAndNewlines) == typeToConfirm
    }
}

/// The desktop finished an action a phone started (the hosts.action event).
public struct HostActionOutcome: Equatable, Sendable, Identifiable {
    public var action: String
    public var host: String
    public var ok: Bool
    public var message: String
    public var atMs: Int

    public var id: String { "\(action):\(host):\(atMs)" }

    public init(action: String, host: String, ok: Bool, message: String, atMs: Int) {
        self.action = action
        self.host = host
        self.ok = ok
        self.message = message
        self.atMs = atMs
    }

    public init(json: JSONObject, atMs: Int) {
        self.init(action: json.str("action"), host: json.str("host"), ok: json.bool("ok"), message: json.str("message"), atMs: atMs)
    }

    public var headline: String {
        let what = HostAction(rawValue: action)?.title ?? action
        return "\(what) \(host.isEmpty ? "" : "of \(host) ")\(ok ? "finished" : "failed")"
    }
}

public enum HostFormat {
    public static func monthly(_ cost: Double) -> String {
        cost == cost.rounded() ? "$\(Int(cost))/mo" : String(format: "$%.2f/mo", cost)
    }

    public static func memory(_ mb: Int) -> String {
        mb >= 1024 && mb % 1024 == 0 ? "\(mb / 1024) GB RAM" : "\(mb) MB RAM"
    }
}
