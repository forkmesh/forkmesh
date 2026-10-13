// The pure logic of the home shell: the page menu's destinations and tools,
// the top bar's status line and account label, the prompt bar's copy, and
// where a tapped push goes. Also the shell's view of the chat relay and the
// local desktop, which the Chat feature supplies (`ShellChatBridge`,
// `ShellLocalDesktopBridge`).

import FMAccount
import FMBase
import Foundation

/// `RelayConnectionState` as the shell draws it.
public enum ShellRelayState: String, Sendable, Equatable {
    case offline, connecting, connected
}

/// One page of the page menu. Each carries both weights of its glyph:
/// outlined while inactive, filled while active.
public struct ShellDestination: Equatable, Sendable, Identifiable {
    public let icon: String
    public let activeIcon: String
    public let label: String
    public var id: String { label }
}

public enum HomeShellCatalog {
    public static let destinations: [ShellDestination] = [
        ShellDestination(
            icon: "integration_instructions_outlined", activeIcon: "integration_instructions", label: "Code"),
        ShellDestination(icon: "smart_toy_outlined", activeIcon: "smart_toy", label: "Agents"),
        ShellDestination(icon: "chat_bubble_outline_rounded", activeIcon: "chat_bubble_rounded", label: "Chat"),
        ShellDestination(icon: "notifications_none_rounded", activeIcon: "notifications_active_outlined", label: "Inbox"),
    ]

    /// The bottom bar's four tabs, left to right (docs/native/NAVIGATION.md
    /// §2): Agents (the default), Inbox, Code, Chat. Everything else (the
    /// tools, status and Settings) is in the avatar's hub.
    public static let bottomBarOrder: [Int] = [agentsIndex, inboxIndex, codeIndex, chatIndex]

    public static let codeIndex = 0
    public static let agentsIndex = 1
    public static let chatIndex = 2
    /// Inbox: pings first (review requests, assignments, mentions and
    /// approvals join it as they are built).
    public static let inboxIndex = 3

    /// The page the app opens on.
    public static let defaultIndex = agentsIndex

    public static func index(of label: String) -> Int {
        destinations.firstIndex { $0.label == label } ?? 0
    }

    public static func tab(at index: Int) -> AppTab {
        switch index {
        case agentsIndex: return .agents
        case chatIndex: return .chat
        case inboxIndex: return .inbox
        default: return .code
        }
    }

    public static func index(of tab: AppTab) -> Int {
        switch tab {
        case .agents: return agentsIndex
        case .chat: return chatIndex
        case .inbox: return inboxIndex
        case .code: return codeIndex
        }
    }

    public static func searchScope(for tab: AppTab) -> SearchScope {
        switch tab {
        case .agents: return .everything
        case .inbox: return .issues
        case .code: return .repos
        case .chat: return .chat
        }
    }
}

/// `_TopBar._statusLabel`: the full state, for assistive technology and the
/// avatar's label.
public func shellStatusLabel(_ state: ShellRelayState, onlineCount: Int) -> String {
    switch state {
    case .connected: return "Connected - \(onlineCount) \(onlineCount == 1 ? "node" : "nodes") online"
    case .connecting: return "Connecting..."
    case .offline: return "Offline"
    }
}

/// `_TopBar._subtitle`: how many nodes are reachable (the banner below the bar
/// says when the relay is down), the guest's read-only notice, and the active
/// relay when the phone knows more than one (`relayLabel` empty otherwise).
public func shellSubtitle(_ state: ShellRelayState, onlineCount: Int, guest: Bool, relayLabel: String) -> String? {
    let connected = state == .connected
    let rest: String?
    if !guest {
        rest = connected ? shellStatusLabel(state, onlineCount: onlineCount) : nil
    } else if connected {
        rest = "Guest (read-only) - \(onlineCount) \(onlineCount == 1 ? "node" : "nodes") online"
    } else {
        rest = "Guest (read-only)"
    }
    if relayLabel.isEmpty { return rest }
    guard let rest else { return relayLabel }
    return "\(relayLabel) - \(rest)"
}

/// The name the avatar stands for: the account name, else the display name,
/// else the identity's short key.
public func shellAccountLabel(_ session: AuthSession?, displayName: String, identityShortKey: String) -> String {
    let accountName = sessionAccountName(session)
    if !accountName.isEmpty { return accountName }
    return displayName.isEmpty ? identityShortKey : displayName
}

/// The avatar's accessibility label, with the member's status line when set.
public func shellAvatarLabel(statusLabel: String, described: String) -> String {
    let base = "\(statusLabel) - your node profile"
    return described.isEmpty ? base : "\(base)\n\(described)"
}

/// The prompt bar's field hint.
public let shellPromptHint = "Start a new agent..."

/// The local mesh room's conversation id (`kMeshConversation`).
public let meshConversationId = "#mesh"

/// `_openChatFor`: the conversation to open for a tapped chat or direct
/// message push: the conversation the push names, else the first holding
/// unread messages of the right kind; nil leaves the chat tab on whatever was
/// last read.
public func pushChatConversation(isDirectMessage: Bool, unreadConversations: [String], pushed: String = "") -> String? {
    if !pushed.isEmpty && isPushDirectConversation(pushed) == isDirectMessage && (isDirectMessage || pushed.hasPrefix("#")) {
        return pushed
    }
    return unreadConversations.first { c in
        isDirectMessage ? isPushDirectConversation(c) : (c.hasPrefix("#") && c != meshConversationId)
    }
}

private func isPushDirectConversation(_ conversation: String) -> Bool {
    conversation.hasPrefix("@") || conversation.hasPrefix("direct:")
}

/// `_openNotificationFor`'s target: the record the push (or the inbox row it
/// named) links to, or nil for the inbox itself.
public func pushDeepLink(pushHref: String, rowHref: String?) -> NotificationDeepLink? {
    NotificationDeepLink.parse(!pushHref.isEmpty ? pushHref : (rowHref ?? ""))
}

/// The desktop on this network, as the top bar's button draws it.
public enum ShellLocalDesktop: Equatable, Sendable {
    /// No desktop in reach.
    case hidden
    /// A desktop is on this network, not being driven.
    case inReach
    case connecting(String)
    case awaitingApproval(String)
    case connected(String)
    case relay(String)

    public var isConnected: Bool {
        switch self {
        case .connected, .relay: return true
        case .hidden, .inReach, .connecting, .awaitingApproval: return false
        }
    }

    /// The accessibility label.
    public var label: String {
        switch self {
        case .hidden: return "No desktop connected"
        case .inReach: return "A desktop is on this network"
        case .connecting(let name): return "Connecting to \(name)"
        case .awaitingApproval(let name): return "Approve this phone on \(name)"
        case .connected(let name): return "Connected to \(name) on this network"
        case .relay(let name): return "Connected to \(name) through the relay"
        }
    }
}

/// What the shell reads from the chat relay and asks of it. The Chat feature
/// registers one (`Services.put(x, as: ShellChatBridge.self)`); without one
/// the shell draws an offline relay.
@MainActor
public protocol ShellChatBridge: AnyObject {
    var relayState: ShellRelayState { get }
    var onlineCount: Int { get }
    /// Unread messages across every conversation (the page menu's badge).
    var totalUnread: Int { get }
    var badgeUnread: Int { get }
    /// The open conversation ("#general", "@alice", "#mesh").
    var currentConversation: String { get }
    /// Conversations holding unread messages, most relevant first.
    var unreadConversations: [String] { get }
    /// `relay.connect()`: the banner's Retry.
    func connect()
    /// Opening the Chat tab clears its badge; each conversation stays unread until opened.
    func markBadgeSeen()
    func switchConversation(_ conversation: String)
}

/// The desktop-on-this-network state for the top bar, from the local control
/// service (Chat feature). Optional: no bridge, no button.
@MainActor
public protocol ShellLocalDesktopBridge: AnyObject {
    var localDesktop: ShellLocalDesktop { get }
}

public enum ShellBarSort: String, CaseIterable, Sendable {
    case usual
    case recent
    case frequent
}

public final class ShellBarOrder {
    public static let sortKey = "shell/bar-sort"
    public static let recentFirstKey = "shell/bar-recent-first"
    public static let recentKey = "shell/bar-recent"
    public static let usesKey = "shell/bar-uses"
    public static let limit = 48

    private let store: KeyValueStore

    public init(store: KeyValueStore) {
        self.store = store
    }

    public var sort: ShellBarSort {
        get {
            if let raw = store.string(forKey: ShellBarOrder.sortKey), let sort = ShellBarSort(rawValue: raw) {
                return sort
            }
            return store.bool(forKey: ShellBarOrder.recentFirstKey) == true ? .recent : .usual
        }
        set {
            store.set(newValue.rawValue, forKey: ShellBarOrder.sortKey)
            store.remove(ShellBarOrder.recentFirstKey)
        }
    }

    public var recent: [String] {
        store.stringList(forKey: ShellBarOrder.recentKey) ?? []
    }

    public var uses: [String: Int] {
        guard let lines = store.stringList(forKey: ShellBarOrder.usesKey) else {
            return Dictionary(recent.map { ($0, 1) }) { (first: Int, _: Int) in first }
        }
        return ShellBarOrder.decodeUses(lines)
    }

    public var ranking: [String] {
        ShellBarOrder.ranking(sort, recent: recent, uses: uses)
    }

    public func use(_ id: String) {
        let counts: [String: Int] = uses
        let next: [String] = ShellBarOrder.used(id, recent: recent)
        store.set(next, forKey: ShellBarOrder.recentKey)
        let counted: [String: Int] = ShellBarOrder.counted(id, uses: counts, recent: next)
        store.set(ShellBarOrder.encodeUses(counted), forKey: ShellBarOrder.usesKey)
    }

    @discardableResult
    public func visit(_ ids: [String]) -> Bool {
        guard let id = ShellBarOrder.counted(visit: ids, recent: recent) else { return false }
        use(id)
        return true
    }

    public func clear() {
        store.set([String](), forKey: ShellBarOrder.recentKey)
        store.set([String](), forKey: ShellBarOrder.usesKey)
    }

    public func ordered<Item>(_ items: [Item], id: (Item) -> String) -> [Item] {
        ShellBarOrder.ordered(items, recent: ranking, id: id)
    }

    public static func used(_ id: String, recent: [String]) -> [String] {
        Array(([id] + recent.filter { $0 != id }).prefix(limit))
    }

    public static func counted(_ id: String, uses: [String: Int], recent: [String]) -> [String: Int] {
        var next: [String: Int] = uses
        next[id, default: 0] += 1
        guard next.count > limit else { return next }
        let kept: [String] = [id] + frequent(next, recent: recent).filter { $0 != id }
        return Dictionary(uniqueKeysWithValues: kept.prefix(limit).map { ($0, next[$0] ?? 0) })
    }

    public static func frequent(_ uses: [String: Int], recent: [String]) -> [String] {
        var seen: [String: Int] = [:]
        for (index, key) in recent.enumerated() where seen[key] == nil {
            seen[key] = index
        }
        return uses.filter { $0.value > 0 }
            .sorted { (lhs: (key: String, value: Int), rhs: (key: String, value: Int)) -> Bool in
                if lhs.value != rhs.value { return lhs.value > rhs.value }
                let left: Int = seen[lhs.key] ?? Int.max
                let right: Int = seen[rhs.key] ?? Int.max
                return left == right ? lhs.key < rhs.key : left < right
            }
            .map(\.key)
    }

    public static func heat(_ id: String, uses: [String: Int], among ids: [String]) -> Double {
        let count: Int = uses[id] ?? 0
        let most: Int = ids.map { uses[$0] ?? 0 }.max() ?? 0
        guard count > 0, most > 0 else { return 0 }
        return min(log(1 + Double(count)) / log(1 + Double(most)), 1)
    }

    public static let frequentMinimumUses = 3
    public static let frequentMinimumHeat = 0.5
    public static let searchPull = 56.0

    public static func counted(visit ids: [String], recent: [String]) -> String? {
        let shown: [String] = ids.filter { !$0.isEmpty }
        guard let first = shown.first, !shown.contains(where: { $0 == recent.first }) else { return nil }
        return first
    }

    public static func frequentMark(_ id: String, uses: [String: Int], among ids: [String]) -> Double? {
        guard (uses[id] ?? 0) >= frequentMinimumUses else { return nil }
        let level: Double = heat(id, uses: uses, among: ids)
        return level >= frequentMinimumHeat ? level : nil
    }

    public static func revealsSearch(pulled: Double, shown: Bool) -> Bool {
        !shown && pulled >= searchPull
    }

    public static func fittedTile(height: Double, fixed: Double, rows: Int, tile: Double, minimum: Double) -> Double {
        guard rows > 0, height > 0 else { return tile }
        return min(tile, max(min(minimum, tile), (height - fixed) / Double(rows)))
    }

    public static func hubItemId(opening route: Route?, in groups: [HubGroup]) -> String? {
        guard let route else { return nil }
        return groups.lazy.flatMap(\.items).first { $0.route == route }?.id
    }

    public static func ranking(_ sort: ShellBarSort, recent: [String], uses: [String: Int]) -> [String] {
        switch sort {
        case .usual: return []
        case .recent: return recent
        case .frequent: return frequent(uses, recent: recent)
        }
    }

    public static func encodeUses(_ uses: [String: Int]) -> [String] {
        uses.sorted { $0.key < $1.key }.map { "\($0.value):\($0.key)" }
    }

    public static func decodeUses(_ lines: [String]) -> [String: Int] {
        var uses: [String: Int] = [:]
        for line in lines {
            guard let colon = line.firstIndex(of: ":"), let count = Int(line[..<colon]), count > 0 else { continue }
            let id = String(line[line.index(after: colon)...])
            if !id.isEmpty {
                uses[id] = count
            }
        }
        return uses
    }

    public static func ordered<Item>(_ items: [Item], recent: [String], id: (Item) -> String) -> [Item] {
        guard !recent.isEmpty else { return items }
        var rank: [String: Int] = [:]
        for (index, key) in recent.enumerated() where rank[key] == nil {
            rank[key] = index
        }
        return items.enumerated()
            .sorted { (lhs: (offset: Int, element: Item), rhs: (offset: Int, element: Item)) -> Bool in
                let left: Int = rank[id(lhs.element)] ?? Int.max
                let right: Int = rank[id(rhs.element)] ?? Int.max
                return left == right ? lhs.offset < rhs.offset : left < right
            }
            .map(\.element)
    }
}
