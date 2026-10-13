import Foundation

public enum HomeStatKind: String, CaseIterable, Sendable {
    case agents, pings, chat, mesh, timers, hosts, memory, requests
}

public enum HomeStatTone: String, Sendable, Equatable {
    case idle, live, attention, alert
}

public struct HomeStat: Identifiable, Equatable, Sendable {
    public let kind: HomeStatKind
    public let label: String
    public let value: String
    public let detail: String
    public let symbol: String
    public let tone: HomeStatTone

    public var id: String { kind.rawValue }

    public init(kind: HomeStatKind, label: String, value: String, detail: String, symbol: String, tone: HomeStatTone) {
        self.kind = kind
        self.label = label
        self.value = value
        self.detail = detail
        self.symbol = symbol
        self.tone = tone
    }

    public var spoken: String { "\(label.capitalized) \(value), \(detail)" }
}

public struct HomeAgentTally: Equatable, Sendable {
    public var working: Int
    public var waiting: Int
    public var failed: Int
    public var done: Int

    public init(working: Int = 0, waiting: Int = 0, failed: Int = 0, done: Int = 0) {
        self.working = working
        self.waiting = waiting
        self.failed = failed
        self.done = done
    }

    public var live: Int { working + waiting }
    public var total: Int { working + waiting + failed + done }
}

public struct HomeDashboardInput: Equatable, Sendable {
    public var agents: HomeAgentTally
    public var runningAgents: Int
    public var pings: Int
    public var chatUnread: Int
    public var online: Int
    public var relay: ShellRelayState
    public var timers: Int
    public var hostsAttention: Int

    public init(
        agents: HomeAgentTally = HomeAgentTally(), runningAgents: Int = 0, pings: Int = 0, chatUnread: Int = 0,
        online: Int = 0, relay: ShellRelayState = .offline, timers: Int = 0, hostsAttention: Int = 0
    ) {
        self.agents = agents
        self.runningAgents = runningAgents
        self.pings = pings
        self.chatUnread = chatUnread
        self.online = online
        self.relay = relay
        self.timers = timers
        self.hostsAttention = hostsAttention
    }

    public var working: Int { max(agents.working, runningAgents - agents.waiting, 0) }
}

public enum HomeDashboard {
    public static let memoryWarnMegabytes: Double = 800

    public static func stats(_ input: HomeDashboardInput) -> [HomeStat] {
        [agents(input), pings(input)]
    }

    public static func agents(_ input: HomeDashboardInput) -> HomeStat {
        let tally: HomeAgentTally = input.agents
        let working: Int = input.working
        let detail: String
        let tone: HomeStatTone
        if tally.failed > 0 {
            detail = "\(tally.failed) failed"
            tone = .alert
        } else if tally.waiting > 0 {
            detail = "\(tally.waiting) waiting"
            tone = .attention
        } else if working > 0 {
            detail = "running"
            tone = .live
        } else {
            detail = tally.done > 0 ? "\(tally.done) done" : "idle"
            tone = .idle
        }
        return HomeStat(
            kind: .agents, label: "AGENTS", value: compact(working + tally.waiting), detail: detail, symbol: "sparkles",
            tone: tone)
    }

    public static func pings(_ input: HomeDashboardInput) -> HomeStat {
        HomeStat(
            kind: .pings, label: "PINGS", value: compact(input.pings), detail: input.pings > 0 ? "unread" : "all read",
            symbol: "tray", tone: input.pings > 0 ? .attention : .idle)
    }

    public static func chat(_ input: HomeDashboardInput) -> HomeStat {
        HomeStat(
            kind: .chat, label: "CHAT", value: compact(input.chatUnread),
            detail: input.chatUnread > 0 ? "new" : "caught up", symbol: "bubble.left.and.bubble.right",
            tone: input.chatUnread > 0 ? .attention : .idle)
    }

    public static func mesh(_ input: HomeDashboardInput) -> HomeStat {
        let detail: String
        let tone: HomeStatTone
        switch input.relay {
        case .connected:
            detail = "online"
            tone = .live
        case .connecting:
            detail = "linking"
            tone = .attention
        case .offline:
            detail = "offline"
            tone = .alert
        }
        return HomeStat(
            kind: .mesh, label: "MESH", value: compact(input.online), detail: detail, symbol: "point.3.connected.trianglepath.dotted",
            tone: tone)
    }

    public static func timers(_ input: HomeDashboardInput) -> HomeStat {
        HomeStat(
            kind: .timers, label: "TIMERS", value: compact(input.timers), detail: input.timers > 0 ? "running" : "none",
            symbol: "timer", tone: input.timers > 0 ? .live : .idle)
    }

    public static func hosts(_ input: HomeDashboardInput) -> HomeStat {
        HomeStat(
            kind: .hosts, label: "HOSTS", value: compact(input.hostsAttention),
            detail: input.hostsAttention > 0 ? "need you" : "all good", symbol: "server.rack",
            tone: input.hostsAttention > 0 ? .alert : .idle)
    }

    public static func memory(megabytes: Double) -> HomeStat {
        let value: String = megabytes.isFinite && megabytes > 0 ? compact(Int(megabytes.rounded())) : "–"
        return HomeStat(
            kind: .memory, label: "MEM", value: value, detail: "MB app", symbol: "memorychip",
            tone: megabytes >= memoryWarnMegabytes ? .attention : .idle)
    }

    public static func requests(perSecond: Double) -> HomeStat {
        let rate: Double = perSecond.isFinite ? max(perSecond, 0) : 0
        let value: String = rate >= 10 || rate == rate.rounded() ? compact(Int(rate.rounded())) : String(format: "%.1f", rate)
        return HomeStat(
            kind: .requests, label: "NET", value: value, detail: "req/s", symbol: "arrow.up.arrow.down",
            tone: rate > 0 ? .live : .idle)
    }

    public static func greeting(hour: Int) -> String {
        switch hour {
        case 5..<12: return "Good morning"
        case 12..<17: return "Good afternoon"
        case 17..<22: return "Good evening"
        default: return "Up late"
        }
    }

    public static func headline(_ input: HomeDashboardInput) -> String {
        var parts: [String] = []
        let working: Int = input.working
        if working > 0 { parts.append(plural(working, "agent") + " working") }
        if input.agents.waiting > 0 { parts.append("\(input.agents.waiting) waiting on you") }
        if input.agents.failed > 0 { parts.append("\(input.agents.failed) failed") }
        if input.pings > 0 { parts.append(plural(input.pings, "ping")) }
        if input.chatUnread > 0 { parts.append("\(input.chatUnread) unread") }
        if input.hostsAttention > 0 { parts.append(plural(input.hostsAttention, "host") + " to check") }
        if input.timers > 0 { parts.append(plural(input.timers, "timer") + " running") }
        if parts.isEmpty {
            return input.relay == .offline ? "Offline · nothing running" : "All quiet"
        }
        return parts.prefix(3).joined(separator: " · ")
    }

    public static func compact(_ count: Int) -> String {
        let n: Int = max(count, 0)
        if n < 1000 { return "\(n)" }
        if n < 10000 {
            let tenths: Int = n / 100
            return tenths % 10 == 0 ? "\(tenths / 10)k" : "\(tenths / 10).\(tenths % 10)k"
        }
        if n < 1_000_000 { return "\(n / 1000)k" }
        return "\(n / 1_000_000)M"
    }

    public static func uptime(_ seconds: TimeInterval) -> String {
        let total: Int = max(Int(seconds.isFinite ? seconds : 0), 0)
        let hours: Int = total / 3600
        let minutes: Int = (total % 3600) / 60
        if hours > 0 { return "\(hours)h \(minutes)m" }
        if minutes > 0 { return "\(minutes)m \(total % 60)s" }
        return "\(total)s"
    }

    static func plural(_ count: Int, _ noun: String) -> String {
        "\(count) \(noun)\(count == 1 ? "" : "s")"
    }
}
