// The state and decisions of the Activity screen: network stats, funds
// received, the relay's activity log and "Node activity" (NodeActivitySection).
//
// The relay log and the node-sync state belong to FMChat, which FMTools does not import: they
// reach the screen through `ToolsHooks`, which whoever owns the relay fills in (see
// docs/FMTools.md). A missing hook reads as an empty log / no node section.

import FMApi
import FMBase
import Foundation
import Observation

/// Late-bound inputs from other features, registered in `Services` by ToolsFeature.register.
@MainActor
public final class ToolsHooks {
    /// The relay's connection log, oldest first ("HH:MM:SS  relay connected"): RelayService.log.
    public var relayLog: (@MainActor () -> [String])?
    /// This phone's node-sync state, nil while NodeSyncService is absent or disabled.
    public var nodeActivity: (@MainActor () -> NodeActivitySnapshot?)?
    /// "Sync now": NodeSyncService.syncNow.
    public var syncNodeNow: (@MainActor () -> Void)?
    /// Relay frames received since launch, for the RELAY metric: RelayService.framesReceived.
    public var relayFrames: (@MainActor () -> Int)?

    public init() {}
}

/// What NodeActivitySection reads from NodeSyncService, AuthService and LocalNodeSyncApplier.
public struct NodeActivitySnapshot: Equatable, Sendable {
    public enum Mode: Equatable, Sendable { case disabled, standby, active }
    public enum Connection: Equatable, Sendable { case offline, connecting, live }

    /// One kind of pending item: `label` is already `kind.label(count)` ("agent prompts").
    public struct Count: Equatable, Sendable {
        public var kind: String
        public var count: Int
        public var label: String
        /// Agent prompts, about edits and branch operations need a host to apply.
        public var hostBound: Bool

        public init(kind: String, count: Int, label: String, hostBound: Bool) {
            self.kind = kind
            self.count = count
            self.label = label
            self.hostBound = hostBound
        }
    }

    public var mode: Mode
    public var connection: Connection
    public var owner: String
    public var lastSyncAt: Date?
    public var pendingTotal: Int
    public var counts: [Count]
    public var ackedTotal: Int
    public var lastError: String
    public var syncing: Bool
    /// True when the account has no desktop device (`auth.session?.desktopDevices.isEmpty ?? true`).
    public var noDesktop: Bool
    /// Nil when the phone holds no local repository (no LocalNodeSyncApplier).
    public var applier: Applier?

    public struct Applier: Equatable, Sendable {
        public var appliedTotal: Int
        public var leftTotal: Int
        /// Why items went back, most frequent first: (reason, times).
        public var leftReasons: [LeftReason]

        public init(appliedTotal: Int, leftTotal: Int, leftReasons: [LeftReason]) {
            self.appliedTotal = appliedTotal
            self.leftTotal = leftTotal
            self.leftReasons = leftReasons
        }
    }

    public struct LeftReason: Equatable, Sendable {
        public var reason: String
        public var times: Int

        public init(reason: String, times: Int) {
            self.reason = reason
            self.times = times
        }
    }

    public init(mode: Mode, connection: Connection, owner: String = "", lastSyncAt: Date? = nil, pendingTotal: Int = 0,
                counts: [Count] = [], ackedTotal: Int = 0, lastError: String = "", syncing: Bool = false,
                noDesktop: Bool = true, applier: Applier? = nil) {
        self.mode = mode
        self.connection = connection
        self.owner = owner
        self.lastSyncAt = lastSyncAt
        self.pendingTotal = pendingTotal
        self.counts = counts
        self.ackedTotal = ackedTotal
        self.lastError = lastError
        self.syncing = syncing
        self.noDesktop = noDesktop
        self.applier = applier
    }
}

/// The colour a badge or icon of the tools screens takes; views map it onto the palette.
public enum ToolsTone: Equatable, Sendable {
    case tertiary, secondary, success, warning, danger, accent
}

/// NodeActivitySection's words, for one snapshot.
public struct NodeActivityPresentation: Equatable, Sendable {
    public let badge: String
    public let badgeTone: ToolsTone
    public let title: String
    public let body: String
    public let tokens: [String]
    public let lastError: String
    /// The FmNeedsDesktopNote detail, or nil when not shown.
    public let needsDesktopDetail: String?
    public let applierTokens: [String]
    public let appliedNote: String?
    public let leftReasonLines: [String]
    public let syncLabel: String
    public let syncing: Bool

    public static let sectionTitle = "Node activity"
    public static let syncTooltip = "Run one signed sync now"

    /// Nil when the section is not shown (no NodeSyncService, or sync disabled).
    public init?(_ node: NodeActivitySnapshot?, now: Date = Date()) {
        guard let node, node.mode != .disabled else { return nil }
        let active = node.mode == .active
        let live = node.connection == .live
        badge = !active ? "standby" : live ? "live" : node.connection == .connecting ? "connecting" : "offline"
        badgeTone = !active ? .tertiary : live ? .success : .warning
        title = active ? "This device is acting as a node" : "A desktop node is handling this account"
        body = active
            ? "It signs as \(node.owner) and reads the account inbox while the app is open. Nothing runs in the background."
            : "The desktop node drains this account's inbox. This phone stays out of the way so the two never claim the same items."
        var tokens = [
            node.lastSyncAt.map { "Synced \(Format.relativeTimeFrom($0, now: now))" } ?? "Never synced",
            node.pendingTotal == 0 ? "No pending items" : "\(node.pendingTotal) pending",
        ]
        for count in node.counts where count.count > 0 { tokens.append("\(count.count) \(count.label)") }
        if node.ackedTotal > 0 { tokens.append("\(node.ackedTotal) applied") }
        self.tokens = tokens
        lastError = node.lastError
        let hostBound = node.counts.filter { $0.hostBound && $0.count > 0 }
        if !hostBound.isEmpty && node.noDesktop {
            let listed = hostBound.map { "\($0.count) \($0.label)" }.joined(separator: " and ")
            needsDesktopDetail = "\(listed) reached this phone but need a host to apply. They are held here and never acknowledged, so a desktop node can still pick the work up."
        } else {
            needsDesktopDetail = nil
        }
        if let applier = node.applier, applier.appliedTotal > 0 || applier.leftTotal > 0 {
            var applierTokens = [String]()
            if applier.appliedTotal > 0 { applierTokens.append("\(applier.appliedTotal) committed here") }
            if applier.leftTotal > 0 { applierTokens.append("\(applier.leftTotal) left for a desktop") }
            self.applierTokens = applierTokens
            appliedNote = applier.appliedTotal > 0
                ? "Applied items are committed to this device only. Push the repository to publish them."
                : nil
            leftReasonLines = applier.leftReasons.prefix(3).map { "· \($0.reason)\($0.times > 1 ? " (×\($0.times))" : "")" }
        } else {
            applierTokens = []
            appliedNote = nil
            leftReasonLines = []
        }
        syncing = node.syncing
        syncLabel = node.syncing ? "Syncing…" : "Sync now"
    }
}

/// One line of the relay's activity log ("HH:MM:SS  body").
public struct ActivityLogLine: Equatable, Sendable, Identifiable {
    public let id: Int
    /// "1:30:00 PM" (12-hour, from the line's 24-hour stamp).
    public let timestamp: String
    public let body: String
    public let icon: String
    public let tone: ToolsTone

    public init(id: Int, line: String) {
        self.id = id
        let chars = Array(line)
        timestamp = chars.count >= 8 ? Format.clockTime12From24(String(chars[0..<8])) : ""
        body = chars.count > 10 ? String(chars[10...]).dartTrimmed : line
        let lower = body.lowercased()
        let failed = lower.contains("failed") || lower.contains("error")
        icon = failed ? "error_outline" : lower.contains("connected") ? "check_circle_outline"
            : lower.contains("left") ? "logout" : "bolt_outlined"
        tone = failed ? .danger : lower.contains("connected") ? .success : .accent
    }
}

/// A section loaded by a future: loading, failed with the error's text, or loaded.
public enum ActivityLoad<Value: Equatable & Sendable>: Equatable, Sendable {
    case loading
    case failed(String)
    case loaded(Value)

    public var value: Value? {
        if case .loaded(let value) = self { return value }
        return nil
    }
}

@MainActor
@Observable
public final class ActivityScreenModel {
    public static let initialLogCount = 12
    public static let fundsNote =
        "Historical finalized public accounting only, not a ForkMesh or current wallet balance. User-owned funds, community-pool funds, pending allocations, and completed on-chain transfers are distinct states."

    @ObservationIgnored private let loadStats: @MainActor () async throws -> NetworkStats
    @ObservationIgnored private let loadBoards: @MainActor () async throws -> NetworkLeaderboards
    @ObservationIgnored public let hooks: ToolsHooks?

    public private(set) var stats: ActivityLoad<NetworkStats> = .loading
    public private(set) var leaderboards: ActivityLoad<NetworkLeaderboards> = .loading
    public var showAllLogs = false

    public init(
        stats: @escaping @MainActor () async throws -> NetworkStats,
        leaderboards: @escaping @MainActor () async throws -> NetworkLeaderboards,
        hooks: ToolsHooks? = nil
    ) {
        loadStats = stats
        loadBoards = leaderboards
        self.hooks = hooks
    }

    public convenience init(api: ApiService, hooks: ToolsHooks?) {
        self.init(stats: { try await api.networkStats() }, leaderboards: { try await api.networkLeaderboards() }, hooks: hooks)
    }

    /// initState and every Retry: both futures start again.
    public func reload() async {
        stats = .loading
        leaderboards = .loading
        async let s: Void = loadStatsNow()
        async let b: Void = loadBoardsNow()
        _ = await (s, b)
    }

    private func loadStatsNow() async {
        do { stats = .loaded(try await loadStats()) } catch { stats = .failed(dartErrorText(error)) }
    }

    private func loadBoardsNow() async {
        do { leaderboards = .loaded(try await loadBoards()) } catch { leaderboards = .failed(dartErrorText(error)) }
    }

    // MARK: Relay log

    public var logLines: [String] { hooks?.relayLog?() ?? [] }

    /// Newest first; the first twelve unless expanded.
    public var visibleLogs: [ActivityLogLine] {
        let lines = logLines
        let newestFirst = Array(lines.enumerated().reversed())
        let shown = showAllLogs ? newestFirst : Array(newestFirst.prefix(ActivityScreenModel.initialLogCount))
        return shown.map { ActivityLogLine(id: $0.offset, line: $0.element) }
    }

    public var logCountLabel: String { "\(logLines.count) events" }
    public var showsLogToggle: Bool { logLines.count > ActivityScreenModel.initialLogCount }
    public var logToggleLabel: String { showAllLogs ? "Show less" : "Show all \(logLines.count) logs" }
    public var logToggleIcon: String { showAllLogs ? "expand_less" : "expand_more" }

    // MARK: Node activity

    public var node: NodeActivityPresentation? { NodeActivityPresentation(hooks?.nodeActivity?()) }

    public func syncNodeNow() { hooks?.syncNodeNow?() }

    // MARK: Funds

    /// Non-empty groups, three entries each: (title, [(name, amount)]).
    nonisolated public static func fundsGroups(_ boards: NetworkLeaderboards) -> [(title: String, rows: [(name: String, amount: String)])] {
        let groups: [(String, [FundsReceivedEntry])] = [
            ("Mainnodes", boards.fundsMainnodes),
            ("Contributors", boards.fundsContributors),
            ("Projects", boards.fundsProjects),
        ]
        return groups.compactMap { title, entries in
            let rows = entries.prefix(3).map { (name: $0.name.isEmpty ? "Unnamed" : $0.name, amount: $0.amountLabel) }
            return rows.isEmpty ? nil : (title: title, rows: Array(rows))
        }
    }
}
