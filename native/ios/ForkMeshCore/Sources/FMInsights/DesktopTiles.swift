import FMBase
import Foundation

public struct DesktopTileEntry: Equatable, Sendable {
    public let id: String
    public let title: String
    public let group: String
    public let hint: String

    public init(id: String, title: String, group: String, hint: String) {
        self.id = id
        self.title = title
        self.group = group
        self.hint = hint
    }
}

public struct DesktopTilesReply: Equatable, Sendable {
    public var tiles: [String: StatsTileData]
    public var sampledAt: Int64

    public var offered: Set<String> { Set(tiles.keys) }
}

public enum DesktopTiles {
    public static let prefix = "desktop."
    public static let maxSeries = 60

    public static func boardId(_ desktopId: String) -> String { prefix + desktopId }

    public static func isDesktopTile(_ id: String) -> Bool { id.hasPrefix(prefix) }

    static let groupSymbols: [String: String] = [
        "Workspace": "folder",
        "Ratchet": "chart.line.uptrend.xyaxis",
        "Issues & pulls": "exclamationmark.circle",
        "Chat & pings": "bubble.left.and.bubble.right",
        "Agents": "sparkles",
        "Agent tools": "wrench.and.screwdriver",
        "Actions": "play.circle",
        "Realtime": "bolt",
        "Network & logs": "network",
        "This machine": "desktopcomputer",
        "Market & rewards": "cart",
        "Users": "person.2",
    ]

    public static let entries: [DesktopTileEntry] = [
        DesktopTileEntry(id: "repos", title: "Repositories", group: "Workspace",
                         hint: "Repositories on this node, split by how they are served"),
        DesktopTileEntry(id: "repo-commits", title: "Commits", group: "Workspace",
                         hint: "Commits on the open repository's served branch, per week for the last year (from the last mirror advert)"),
        DesktopTileEntry(id: "repo-merges", title: "Merges", group: "Workspace",
                         hint: "Merge commits on the open repository, per week"),
        DesktopTileEntry(id: "repo-branches", title: "Branches", group: "Workspace",
                         hint: "Local branches the open repository's mirror holds"),
        DesktopTileEntry(id: "repo-worktrees", title: "Worktrees", group: "Workspace",
                         hint: "Worktrees the open repository's mirror advertises, with its registered pulls"),
        DesktopTileEntry(id: "repo-size", title: "Repository size", group: "Workspace",
                         hint: "Tracked bytes in the open repository, per day, from its generated statistics"),
        DesktopTileEntry(id: "repo-lines", title: "Lines of code", group: "Workspace",
                         hint: "Tracked lines in the open repository, per day"),
        DesktopTileEntry(id: "repo-files", title: "Tracked files", group: "Workspace",
                         hint: "Tracked files in the open repository, per day"),
        DesktopTileEntry(id: "scm-changes", title: "Working-tree changes", group: "Workspace",
                         hint: "Uncommitted rows Source Control lists for the open repository"),
        DesktopTileEntry(id: "teammates", title: "Special agents", group: "Workspace",
                         hint: "AI special agents configured for the open repository"),
        DesktopTileEntry(id: "ratchet", title: "Ratchet mode", group: "Ratchet",
                         hint: "Whether Ratchet Mode is on for the open repository: commits that grow the tracked bytes past the day's ceiling are refused"),
        DesktopTileEntry(id: "ratchet-ceiling", title: "Ratchet ceiling", group: "Ratchet",
                         hint: "Today's size ceiling, with the repository's tracked bytes per day beneath it"),
        DesktopTileEntry(id: "ratchet-headroom", title: "Ratchet headroom", group: "Ratchet",
                         hint: "Bytes left under the ceiling before a commit is refused, per day"),
        DesktopTileEntry(id: "issues-open", title: "Open issues", group: "Issues & pulls",
                         hint: "Open issues in the open repository, with the last 30 days of the burn-up"),
        DesktopTileEntry(id: "issues-closed", title: "Closed issues", group: "Issues & pulls",
                         hint: "Closed issues in the open repository, over 30 days"),
        DesktopTileEntry(id: "issues-priority", title: "Issues by priority", group: "Issues & pulls",
                         hint: "Open issues grouped by priority band"),
        DesktopTileEntry(id: "issues-labels", title: "Issues by label", group: "Issues & pulls",
                         hint: "Labels on open issues, most used first"),
        DesktopTileEntry(id: "issues-assigned", title: "Assigned issues", group: "Issues & pulls",
                         hint: "Open issues with an assignee, by assignee"),
        DesktopTileEntry(id: "issues-progress", title: "Issue progress", group: "Issues & pulls",
                         hint: "Average reported progress across open issues, by quarter"),
        DesktopTileEntry(id: "issues-votes", title: "Issue votes", group: "Issues & pulls",
                         hint: "Uptime-credit votes across open issues, by issue"),
        DesktopTileEntry(id: "issues-bounties", title: "Issue bounties", group: "Issues & pulls",
                         hint: "Bounties posted on open issues, by issue"),
        DesktopTileEntry(id: "issues-rewards", title: "Reward estimates", group: "Issues & pulls",
                         hint: "Contributor reward estimates on issues, by status"),
        DesktopTileEntry(id: "pulls", title: "Pull requests", group: "Issues & pulls",
                         hint: "Pull requests in the open repository: open, merged, closed"),
        DesktopTileEntry(id: "pulls-conflicts", title: "Conflicting pulls", group: "Issues & pulls",
                         hint: "Open pull requests whose last conflict probe found conflicts"),
        DesktopTileEntry(id: "discussions", title: "Discussions", group: "Issues & pulls",
                         hint: "Discussions in the open repository, by status, with comments"),
        DesktopTileEntry(id: "chat-unread", title: "Unread messages", group: "Chat & pings",
                         hint: "Unread chat messages, by channel"),
        DesktopTileEntry(id: "chat-channels", title: "Channels", group: "Chat & pings",
                         hint: "Chat channels this node follows"),
        DesktopTileEntry(id: "pings", title: "Pings", group: "Chat & pings",
                         hint: "Pings received, per day over two weeks"),
        DesktopTileEntry(id: "pings-kinds", title: "Pings by kind", group: "Chat & pings",
                         hint: "Pings grouped by what raised them"),
        DesktopTileEntry(id: "roster-versions", title: "App versions", group: "Chat & pings",
                         hint: "Roster members by the ForkMesh version they advertise"),
        DesktopTileEntry(id: "tasks", title: "Organization tasks", group: "Chat & pings",
                         hint: "Tasks loaded from the organization board"),
        DesktopTileEntry(id: "agents", title: "Agent sessions", group: "Agents",
                         hint: "Every agent session on this node, by status"),
        DesktopTileEntry(id: "agents-running", title: "Running agents", group: "Agents",
                         hint: "Sessions running right now, with sessions started per day over two weeks"),
        DesktopTileEntry(id: "agent-success", title: "Agent success rate", group: "Agents",
                         hint: "Finished sessions that succeeded or merged"),
        DesktopTileEntry(id: "agent-tokens", title: "Tokens used", group: "Agents",
                         hint: "Tokens across every agent session, per day"),
        DesktopTileEntry(id: "agent-spend", title: "Agent spend", group: "Agents",
                         hint: "Estimated cost of agent sessions, per day; the note carries the provider bills when known"),
        DesktopTileEntry(id: "agent-turns", title: "Agent turns", group: "Agents",
                         hint: "Model turns across every session, per day"),
        DesktopTileEntry(id: "agent-runtime", title: "Agent run time", group: "Agents",
                         hint: "Time agents have spent running, with the last twenty sessions"),
        DesktopTileEntry(id: "agent-prs", title: "Agent pull requests", group: "Agents",
                         hint: "Sessions that opened a pull request, and how many merged"),
        DesktopTileEntry(id: "agent-errors", title: "Agent errors", group: "Agents",
                         hint: "Sessions that ended with an error, with the newest message"),
        DesktopTileEntry(id: "agents-providers", title: "Agents by provider", group: "Agents",
                         hint: "Sessions grouped by the provider that ran them"),
        DesktopTileEntry(id: "agents-models", title: "Agents by model", group: "Agents",
                         hint: "Sessions grouped by model"),
        DesktopTileEntry(id: "agent-skills", title: "Skills used", group: "Agents",
                         hint: "Skills sessions reported using, most used first"),
        DesktopTileEntry(id: "agent-grades", title: "Task grades", group: "Agents",
                         hint: "Grades given to finished agent tasks"),
        DesktopTileEntry(id: "agent-teammates", title: "Special agent sessions", group: "Agents",
                         hint: "Sessions started by AI special agents, by special agent"),
        DesktopTileEntry(id: "claude-usage", title: "Claude usage", group: "Agents",
                         hint: "Claude subscription usage for the 5-hour and weekly windows"),
        DesktopTileEntry(id: "tool-calls", title: "Tool calls", group: "Agent tools",
                         hint: "Agent tool calls recorded by the efficiency ledger, per five minutes over the last two hours"),
        DesktopTileEntry(id: "tool-errors", title: "Tool errors", group: "Agent tools",
                         hint: "Tool calls that returned an error, by family"),
        DesktopTileEntry(id: "tokens-saved", title: "Tokens saved", group: "Agent tools",
                         hint: "Context tokens the tool layer avoided injecting"),
        DesktopTileEntry(id: "context-injected", title: "Context injected", group: "Agent tools",
                         hint: "Tokens tool results actually put into context"),
        DesktopTileEntry(id: "tool-baseline", title: "Baseline tokens", group: "Agent tools",
                         hint: "What the naive alternative to each measured call would have cost, against what was injected"),
        DesktopTileEntry(id: "tool-latency", title: "Tool latency", group: "Agent tools",
                         hint: "Average measured tool call latency"),
        DesktopTileEntry(id: "tool-coverage", title: "Tool coverage", group: "Agent tools",
                         hint: "Tool families observed, with calls per family"),
        DesktopTileEntry(id: "tool-runtimes", title: "Runtime tools", group: "Agent tools",
                         hint: "Runtime tool names the agents called, most used first"),
        DesktopTileEntry(id: "tool-while-off", title: "Calls while off", group: "Agent tools",
                         hint: "Calls that arrived while their family was switched off"),
        DesktopTileEntry(id: "improvement-signals", title: "Improvement signals", group: "Agent tools",
                         hint: "Potential tool-use improvements from the realtime log, by kind"),
        DesktopTileEntry(id: "improvement-stake", title: "Context at stake", group: "Agent tools",
                         hint: "Context tokens the logged improvement patterns cost, by family"),
        DesktopTileEntry(id: "guardrail-blocked", title: "Blocked tool calls", group: "Agent tools",
                         hint: "Tool calls the guardrails blocked, per hour over the last day"),
        DesktopTileEntry(id: "guardrail-decisions", title: "Guardrail decisions", group: "Agent tools",
                         hint: "Approvals, denials and agents stopped by the guardrails"),
        DesktopTileEntry(id: "workflows", title: "Workflows", group: "Actions",
                         hint: "Workflow files in the open repository, by trigger"),
        DesktopTileEntry(id: "action-runs", title: "Action runs", group: "Actions",
                         hint: "Workflow runs on this node, by status"),
        DesktopTileEntry(id: "action-history", title: "Runs per day", group: "Actions",
                         hint: "Workflow runs started per day over two weeks"),
        DesktopTileEntry(id: "action-failed", title: "Failed runs", group: "Actions",
                         hint: "Workflow runs that failed, per day over two weeks"),
        DesktopTileEntry(id: "action-success", title: "Action success rate", group: "Actions",
                         hint: "Finished workflow runs that succeeded"),
        DesktopTileEntry(id: "action-duration", title: "Action duration", group: "Actions",
                         hint: "Average finished run time, with the last twenty runs"),
        DesktopTileEntry(id: "action-wait", title: "Action queue wait", group: "Actions",
                         hint: "Average time from a run being created to it starting, with the last twenty runs"),
        DesktopTileEntry(id: "action-workflow-times", title: "Time per workflow", group: "Actions",
                         hint: "Average finished run time of each workflow"),
        DesktopTileEntry(id: "action-slowest", title: "Slowest run", group: "Actions",
                         hint: "The longest finished workflow run on record"),
        DesktopTileEntry(id: "action-last", title: "Last run time", group: "Actions",
                         hint: "How long the most recent finished run took"),
        DesktopTileEntry(id: "action-steps", title: "Action steps", group: "Actions",
                         hint: "Workflow steps executed across every run, by outcome"),
        DesktopTileEntry(id: "action-pull-runs", title: "Pull request runs", group: "Actions",
                         hint: "Runs triggered by pull requests, and how many were held for a first-time author"),
        DesktopTileEntry(id: "agents-live", title: "Agents running now", group: "Realtime",
                         hint: "Sessions running at this moment, sampled every refresh"),
        DesktopTileEntry(id: "agent-queue", title: "Agent queue", group: "Realtime",
                         hint: "Sessions queued or waiting to run, sampled every refresh"),
        DesktopTileEntry(id: "agent-context", title: "Context in use", group: "Realtime",
                         hint: "How full each running session's context window is"),
        DesktopTileEntry(id: "agent-background", title: "Agent subprocesses", group: "Realtime",
                         hint: "Background processes running sessions still own, sampled every refresh"),
        DesktopTileEntry(id: "compilers", title: "Compilers", group: "Realtime",
                         hint: "Compiler processes under the agents, with their memory"),
        DesktopTileEntry(id: "tool-in-flight", title: "Tool calls in flight", group: "Realtime",
                         hint: "Tool calls started and not yet answered, sampled every refresh"),
        DesktopTileEntry(id: "action-queue", title: "Action queue", group: "Realtime",
                         hint: "Workflow runs awaiting approval, queued or running right now"),
        DesktopTileEntry(id: "relay-latency", title: "Relay latency", group: "Realtime",
                         hint: "Round trip to the active relay, sampled every refresh, with every relay measured"),
        DesktopTileEntry(id: "network-requests", title: "Network requests", group: "Realtime",
                         hint: "HTTP requests this session by destination, live: green is allowed by a firewall rule, red is denied by one, amber has no decision yet, blue stays on this machine or the firewall is off"),
        DesktopTileEntry(id: "background-tasks", title: "Background tasks", group: "Realtime",
                         hint: "Open background work tickets, sampled every refresh"),
        DesktopTileEntry(id: "loop-busy", title: "Event loop busy", group: "Realtime",
                         hint: "Share of time the GUI event dispatcher was busy, with wakeups per second"),
        DesktopTileEntry(id: "nodes-online", title: "Nodes online", group: "Network & logs",
                         hint: "Roster members with a live link, by platform"),
        DesktopTileEntry(id: "linked-nodes", title: "Linked nodes", group: "Network & logs",
                         hint: "Nodes linked to your account"),
        DesktopTileEntry(id: "network-catalog", title: "Network catalog", group: "Network & logs",
                         hint: "Relays, nodes and hosts the Network page lists"),
        DesktopTileEntry(id: "servers", title: "Relays configured", group: "Network & logs",
                         hint: "Relay servers saved on this node, and which is active"),
        DesktopTileEntry(id: "services", title: "Service status", group: "Network & logs",
                         hint: "Website and desktop service checks that are operational"),
        DesktopTileEntry(id: "web-alerts", title: "Web alerts", group: "Network & logs",
                         hint: "Alerts the website raised for this account"),
        DesktopTileEntry(id: "firewall", title: "Request firewall", group: "Network & logs",
                         hint: "Outbound requests the request firewall allowed or blocked"),
        DesktopTileEntry(id: "uptime", title: "Connection uptime", group: "Network & logs",
                         hint: "How long this session has been connected, and the lifetime total"),
        DesktopTileEntry(id: "log-lines", title: "Log lines", group: "Network & logs",
                         hint: "Network log lines this session, by category"),
        DesktopTileEntry(id: "log-history", title: "Log history", group: "Network & logs",
                         hint: "Network log lines per day over the 30-day archive"),
        DesktopTileEntry(id: "stalls", title: "UI stalls", group: "Network & logs",
                         hint: "Times the GUI thread stalled this session"),
        DesktopTileEntry(id: "cpu", title: "CPU", group: "This machine",
                         hint: "This app's CPU share, sampled while the page is open"),
        DesktopTileEntry(id: "memory", title: "Memory", group: "This machine",
                         hint: "This app's resident memory, sampled while the page is open"),
        DesktopTileEntry(id: "host-memory", title: "Host memory", group: "This machine",
                         hint: "Memory in use across the whole machine"),
        DesktopTileEntry(id: "swap", title: "Swap", group: "This machine",
                         hint: "Swap in use across the whole machine"),
        DesktopTileEntry(id: "disk", title: "Disk free", group: "This machine",
                         hint: "Free space on the volume holding the open repository"),
        DesktopTileEntry(id: "open-files", title: "Open files", group: "This machine",
                         hint: "File descriptors this app holds against its soft limit"),
        DesktopTileEntry(id: "threads", title: "Threads", group: "This machine",
                         hint: "Threads in this process"),
        DesktopTileEntry(id: "app-uptime", title: "App uptime", group: "This machine",
                         hint: "Time since this app instance launched"),
        DesktopTileEntry(id: "restarts", title: "Restarts", group: "This machine",
                         hint: "Rebuild-and-restart and update cycles this install has timed"),
        DesktopTileEntry(id: "restart-time", title: "Restart time", group: "This machine",
                         hint: "How long recent restarts took, from the click to the relaunched window's first paint"),
        DesktopTileEntry(id: "loop-lag", title: "Event-loop lag", group: "This machine",
                         hint: "95th percentile GUI event-loop lag from the Qt runtime diagnostics"),
        DesktopTileEntry(id: "events-per-second", title: "Events per second", group: "This machine",
                         hint: "Qt events dispatched per second"),
        DesktopTileEntry(id: "widgets", title: "Widgets", group: "This machine",
                         hint: "Live QWidget count, with the QObject total"),
        DesktopTileEntry(id: "windows", title: "Windows", group: "This machine",
                         hint: "Top-level windows, and how many are visible"),
        DesktopTileEntry(id: "timers", title: "Qt timers", group: "This machine",
                         hint: "Active Qt timers against every registered timer"),
        DesktopTileEntry(id: "thread-pool", title: "Thread pool", group: "This machine",
                         hint: "Global thread pool workers busy, against its maximum"),
        DesktopTileEntry(id: "diff-cache", title: "Diff cache", group: "This machine",
                         hint: "Memory held by cached branch diffs"),
        DesktopTileEntry(id: "packages", title: "Package installs", group: "Market & rewards",
                         hint: "Installs and downloads across marketplace packages"),
        DesktopTileEntry(id: "release-downloads", title: "Release downloads", group: "Market & rewards",
                         hint: "Downloads across the open repository's release assets"),
        DesktopTileEntry(id: "rewards", title: "Contributor rewards", group: "Market & rewards",
                         hint: "Reward records fetched for the reward pool, by status"),
        DesktopTileEntry(id: "ledger", title: "Ledger records", group: "Market & rewards",
                         hint: "Payable ledger records from the relay, and this machine's own"),
        DesktopTileEntry(id: "wallets", title: "Wallet balances", group: "Market & rewards",
                         hint: "Public balances of the wallets this machine names"),
        DesktopTileEntry(id: "payouts", title: "Settled payouts", group: "Market & rewards",
                         hint: "Rewards paid from this machine and proven on-chain"),
        DesktopTileEntry(id: "users", title: "Users", group: "Users",
                         hint: "Accounts in the directory, by activity bucket"),
        DesktopTileEntry(id: "users-activity", title: "User activity", group: "Users",
                         hint: "Commits, pulls, issues and discussions across every account"),
        DesktopTileEntry(id: "users-world", title: "World steps", group: "Users",
                         hint: "Steps taken in World across every account, with office visits"),
    ]

    public static let catalog: [StatsTileCatalogEntry] = entries.map {
        StatsTileCatalogEntry(id: boardId($0.id), title: $0.title, group: $0.group, hint: $0.hint,
                              symbol: groupSymbols[$0.group] ?? "desktopcomputer")
    }

    public static let ids: Set<String> = Set(catalog.map(\.id))

    public static let wide: Set<String> = Set(
        ["issues-open", "agents", "tool-calls", "cpu", "repo-commits", "action-history", "pings", "agent-tokens", "log-history"]
            .map(boardId))

    public static let defaults: [StatsTilePlacement] = catalog.map {
        StatsTilePlacement(id: $0.id, width: wide.contains($0.id) ? 2 : 1)
    }

    public static func reply(_ json: JSONObject) -> DesktopTilesReply {
        var tiles: [String: StatsTileData] = [:]
        for tile in json.maps("tiles") {
            let id = tile.str("id")
            if !id.isEmpty { tiles[boardId(id)] = data(tile) }
        }
        return DesktopTilesReply(tiles: tiles, sampledAt: json.int64("sampledAt"))
    }

    public static func data(_ json: JSONObject) -> StatsTileData {
        let series = json.obj("series")
        let values = Array(series.list("values").map { value -> Double in
            let number = value.doubleValue ?? 0
            return number.isFinite ? number : 0
        }.suffix(maxSeries))
        let labels = Array(series.strings("labels").suffix(values.count))
        let trend: StatsTrendPolarity
        switch json.str("trend") {
        case "up": trend = .upIsGood
        case "down": trend = .downIsGood
        default: trend = .neutral
        }
        return StatsTileData(
            value: json.str("value"), note: json.str("note"),
            series: StatsTileSeries(labels: labels, values: values, categorical: series.bool("categorical")),
            available: json.optBool("available") ?? true, status: StatsTileStatus(rawValue: json.str("status")) ?? .none,
            trend: trend)
    }
}
