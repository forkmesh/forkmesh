import FMBase
import Foundation

public enum ApprovalLifetime: String, CaseIterable, Codable, Hashable, Sendable {
    case once, session, worktree, persistent

    public var title: String {
        switch self {
        case .once: return "Allow once"
        case .session: return "Allow for this session"
        case .worktree: return "Allow for this worktree"
        case .persistent: return "Always allow"
        }
    }

    public var detail: String {
        switch self {
        case .once: return "Only this request."
        case .session: return "Matching requests until this run ends."
        case .worktree: return "Matching requests in this worktree."
        case .persistent: return "Saved on the desktop until you revoke it."
        }
    }
}

public enum ApprovalKind: String, CaseIterable, Hashable, Sendable {
    case command, network, file, secret, tool, other

    public var title: String {
        switch self {
        case .command: return "Run a command"
        case .network: return "Reach the network"
        case .file: return "Use a file"
        case .secret: return "Read a secret"
        case .tool: return "Use a tool"
        case .other: return "Permission"
        }
    }

    public var systemImage: String {
        switch self {
        case .command: return "terminal"
        case .network: return "network"
        case .file: return "doc"
        case .secret: return "key"
        case .tool: return "wrench.and.screwdriver"
        case .other: return "hand.raised"
        }
    }
}

public struct PendingApproval: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var sessionId: Int
    public var title: String
    public var repo: String
    public var provider: String
    public var capability: String
    public var kind: ApprovalKind
    public var question: String
    public var command: String
    public var askedAtMs: Int64
    public var lifetimes: [ApprovalLifetime]

    public init(
        id: String, sessionId: Int, title: String = "", repo: String = "", provider: String = "",
        capability: String = "", kind: ApprovalKind = .other, question: String = "", command: String = "",
        askedAtMs: Int64 = 0, lifetimes: [ApprovalLifetime] = ApprovalLifetime.allCases
    ) {
        self.id = id
        self.sessionId = sessionId
        self.title = title
        self.repo = repo
        self.provider = provider
        self.capability = capability
        self.kind = kind
        self.question = question
        self.command = command
        self.askedAtMs = askedAtMs
        self.lifetimes = lifetimes
    }

    public init?(json: JSONObject) {
        let id = json.str("id")
        guard AgentOpsIds.isApprovalId(id) else { return nil }
        let lifetimes = json.strings("lifetimes").compactMap(ApprovalLifetime.init(rawValue:))
        self.init(
            id: id, sessionId: json.int("sessionId"), title: json.str("title"), repo: json.str("repo"),
            provider: json.str("provider"), capability: json.str("capability"),
            kind: ApprovalKind(rawValue: json.str("kind")) ?? .other, question: json.str("question"),
            command: json.str("command"), askedAtMs: json.int64("askedAtMs"),
            lifetimes: lifetimes.isEmpty ? [.once] : lifetimes)
    }

    public var headline: String {
        let firstLine = { (text: String) -> String in
            text.split(separator: "\n", omittingEmptySubsequences: true).first.map(String.init) ?? ""
        }
        if !command.isEmpty { return firstLine(command) }
        let line = firstLine(question)
        return line.isEmpty ? kind.title : line
    }

    public var subtitle: String {
        [title, repo].filter { !$0.isEmpty }.joined(separator: " · ")
    }
}

public enum AgentOpsIds {
    public static func isApprovalId(_ id: String) -> Bool {
        id.utf8.count == 32 && id.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }
    }
}

public enum AgentOpsAction: String, CaseIterable, Hashable, Sendable {
    case approve, deny, merge, stop
    case continueRun = "continue"
    case revoke
    case delete
    case purge

    public var title: String {
        switch self {
        case .approve: return "Approve"
        case .deny: return "Deny"
        case .merge: return "Merge & clean up"
        case .stop: return "Stop"
        case .continueRun: return "Continue"
        case .revoke: return "Revoke"
        case .delete: return "Delete"
        case .purge: return "Delete permanently"
        }
    }

    public var isDestructive: Bool { self == .merge || self == .delete || self == .purge }
}

public enum BulkAgentAction: String, CaseIterable, Hashable, Sendable {
    case stop
    case continueRun = "continue"

    public var opsAction: AgentOpsAction { self == .stop ? .stop : .continueRun }

    public var title: String { self == .stop ? "Stop" : "Continue" }
}

public struct AgentOpsConfirmation: Equatable, Sendable, Identifiable {
    public var token: String
    public var action: AgentOpsAction
    public var summary: String
    public var destructive: Bool
    public var typeToConfirm: String
    public var expiresInMs: Int
    public var issuedAtMs: Int64

    public init(
        token: String, action: AgentOpsAction, summary: String, destructive: Bool = false,
        typeToConfirm: String = "", expiresInMs: Int = 60_000, issuedAtMs: Int64 = 0
    ) {
        self.token = token
        self.action = action
        self.summary = summary
        self.destructive = destructive
        self.typeToConfirm = typeToConfirm
        self.expiresInMs = expiresInMs
        self.issuedAtMs = issuedAtMs
    }

    public init?(json: JSONObject, issuedAtMs: Int64) {
        guard let action = AgentOpsAction(rawValue: json.str("action")), !json.str("token").isEmpty else {
            return nil
        }
        self.init(
            token: json.str("token"), action: action, summary: json.str("summary"),
            destructive: json.bool("destructive"), typeToConfirm: json.str("typeToConfirm"),
            expiresInMs: json.int("expiresInMs", 60_000), issuedAtMs: issuedAtMs)
    }

    public var id: String { token }

    public func isExpired(nowMs: Int64) -> Bool {
        nowMs - issuedAtMs >= Int64(expiresInMs)
    }
}

public struct BulkOutcome: Equatable, Sendable {
    public var action: BulkAgentAction
    public var done: [Int]
    public var skipped: [Int]

    public init(action: BulkAgentAction, done: [Int], skipped: [Int]) {
        self.action = action
        self.done = done
        self.skipped = skipped
    }

    public var message: String {
        let verb = action == .stop ? "Stopped" : "Continued"
        let base = done.count == 1 ? "\(verb) 1 agent" : "\(verb) \(done.count) agents"
        if skipped.isEmpty { return base + "." }
        let others = skipped.count == 1 ? "1 was" : "\(skipped.count) were"
        return base + "; \(others) not in a state to \(action.title.lowercased())."
    }
}

public struct RecycledAgent: Equatable, Hashable, Sendable, Identifiable {
    public var id: Int
    public var title: String
    public var owner: String
    public var name: String
    public var branch: String
    public var provider: String
    public var deletedAtMs: Int64
    public var removeInMs: Int64

    public init(
        id: Int, title: String = "", owner: String = "", name: String = "", branch: String = "",
        provider: String = "", deletedAtMs: Int64 = 0, removeInMs: Int64 = -1
    ) {
        self.id = id
        self.title = title
        self.owner = owner
        self.name = name
        self.branch = branch
        self.provider = provider
        self.deletedAtMs = deletedAtMs
        self.removeInMs = removeInMs
    }

    public init?(json: JSONObject) {
        let id = json.int("id")
        guard id > 0 else { return nil }
        self.init(
            id: id, title: json.str("title"), owner: json.str("owner"), name: json.str("name"),
            branch: json.str("branch"), provider: json.str("provider"), deletedAtMs: json.int64("deletedAtMs"),
            removeInMs: json.int64("removeInMs", -1))
    }

    public var repo: String {
        owner.isEmpty ? name : "\(owner)/\(name)"
    }

    public var headline: String {
        title.isEmpty ? "Agent #\(id)" : title
    }

    public var removal: String {
        let hourMs: Int64 = 60 * 60 * 1000
        if removeInMs >= hourMs { return "Removed for good in \(removeInMs / hourMs) h" }
        if removeInMs > 0 { return "Removed for good in under an hour" }
        return "Removed for good at the next sweep"
    }
}

public struct RecycleBin: Equatable, Sendable {
    public var agents: [RecycledAgent]
    public var count: Int
    public var truncated: Bool
    public var retentionHours: Int
    public var minHours: Int
    public var maxHours: Int

    public init(
        agents: [RecycledAgent] = [], count: Int = 0, truncated: Bool = false, retentionHours: Int = 24,
        minHours: Int = 1, maxHours: Int = 720
    ) {
        self.agents = agents
        self.count = count
        self.truncated = truncated
        self.retentionHours = retentionHours
        self.minHours = minHours
        self.maxHours = maxHours
    }

    public init(json: JSONObject) {
        let agents = json.maps("agents").compactMap(RecycledAgent.init(json:))
        self.init(
            agents: agents, count: max(json.int("count", agents.count), agents.count),
            truncated: json.bool("truncated"), retentionHours: json.int("retentionHours", 24),
            minHours: json.int("minHours", 1), maxHours: json.int("maxHours", 720))
    }

    public var isEmpty: Bool { agents.isEmpty }

    public var ids: [Int] { agents.map(\.id) }

    public var hint: String {
        if isEmpty { return "The recycle bin on the desktop is empty." }
        return "Deleted agents are kept on the desktop for \(retentionHours) hours. Restoring brings back the "
            + "session and its transcript; a pull request or issue assignment removed by the delete is not recreated."
    }
}

public enum RecycleBinChange: Equatable, Sendable {
    case restored(done: [Int], failed: [Int])
    case purged(done: [Int], skipped: [Int])

    public var message: String {
        switch self {
        case .restored(let done, let failed):
            var text = done.count == 1 ? "Agent restored." : "\(done.count) agents restored."
            if done.isEmpty { text = "" }
            if !failed.isEmpty {
                let lead = failed.count == 1 ? "1 agent" : "\(failed.count) agents"
                text += (text.isEmpty ? "" : " ") + "Could not restore \(lead); a live agent may already use that id."
            }
            return text
        case .purged(let done, let skipped):
            var text = done.count == 1 ? "1 agent deleted for good." : "\(done.count) agents deleted for good."
            if !skipped.isEmpty {
                text += skipped.count == 1 ? " 1 was already gone." : " \(skipped.count) were already gone."
            }
            return text
        }
    }
}

public struct BulkCandidate: Equatable, Hashable, Sendable {
    public var id: Int
    public var isActive: Bool
    public var stoppable: Bool
    public var merged: Bool

    public init(id: Int, isActive: Bool, stoppable: Bool, merged: Bool = false) {
        self.id = id
        self.isActive = isActive
        self.stoppable = stoppable
        self.merged = merged
    }

    public func allows(_ action: BulkAgentAction) -> Bool {
        switch action {
        case .stop: return isActive && stoppable
        case .continueRun: return !isActive && !merged
        }
    }
}

public struct AgentWorktree: Equatable, Hashable, Sendable, Identifiable {
    public var name: String
    public var branch: String
    public var head: String
    public var isMain: Bool
    public var detached: Bool
    public var locked: Bool
    public var prunable: Bool
    public var ahead: Int
    public var behind: Int
    public var sessionId: Int?

    public init(
        name: String, branch: String, head: String = "", isMain: Bool = false, detached: Bool = false,
        locked: Bool = false, prunable: Bool = false, ahead: Int = 0, behind: Int = 0, sessionId: Int? = nil
    ) {
        self.name = name
        self.branch = branch
        self.head = head
        self.isMain = isMain
        self.detached = detached
        self.locked = locked
        self.prunable = prunable
        self.ahead = ahead
        self.behind = behind
        self.sessionId = sessionId
    }

    public init(json: JSONObject) {
        let session = json.int("sessionId")
        self.init(
            name: json.str("name"), branch: json.str("branch"), head: json.str("head"),
            isMain: json.bool("isMain"), detached: json.bool("detached"), locked: json.bool("locked"),
            prunable: json.bool("prunable"), ahead: json.int("ahead"), behind: json.int("behind"),
            sessionId: session > 0 ? session : nil)
    }

    public var id: String { "\(name)|\(branch)" }

    public var syncLabel: String {
        if isMain { return "Main checkout" }
        if detached { return "Detached" }
        switch (ahead, behind) {
        case (0, 0): return "Up to date"
        case (_, 0): return "\(ahead) ahead"
        case (0, _): return "\(behind) behind"
        default: return "\(ahead) ahead, \(behind) behind"
        }
    }
}

public struct WorktreeList: Equatable, Sendable {
    public var repo: String
    public var base: String
    public var worktrees: [AgentWorktree]

    public init(repo: String = "", base: String = "", worktrees: [AgentWorktree] = []) {
        self.repo = repo
        self.base = base
        self.worktrees = worktrees
    }

    public init(json: JSONObject) {
        self.init(
            repo: json.str("repo"), base: json.str("base"),
            worktrees: json.maps("worktrees").map(AgentWorktree.init(json:)))
    }
}

public enum UsageState: String, Hashable, Sendable {
    case ok, high, exhausted, unknown
}

public struct UsageWindowMeter: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var label: String
    public var usedPercent: Int?
    public var resetAtMs: Int64?
    public var state: UsageState
    public var windowMs: Int64?

    public init(
        id: String, label: String, usedPercent: Int?, resetAtMs: Int64? = nil, state: UsageState,
        windowMs: Int64? = nil
    ) {
        self.id = id
        self.label = label
        self.usedPercent = usedPercent
        self.resetAtMs = resetAtMs
        self.state = state
        self.windowMs = windowMs ?? UsageWindowMeter.inferredWindowMs(id: id, label: label)
    }

    public init(json: JSONObject) {
        let used = json.int("usedPercent", -1)
        let reset = json.int64("resetAtMs")
        let window = json.int64("windowMs")
        self.init(
            id: json.str("id"), label: json.str("label"), usedPercent: used >= 0 ? min(used, 100) : nil,
            resetAtMs: reset > 0 ? reset : nil, state: UsageState(rawValue: json.str("state")) ?? .unknown,
            windowMs: window > 0 ? window : nil)
    }

    public static let hourMs: Int64 = 3_600_000
    public static let dayMs: Int64 = 86_400_000

    public static func inferredWindowMs(id: String, label: String) -> Int64? {
        let text = (id + " " + label).lowercased()
        if text.contains("5h") || text.contains("5-hour") || text.contains("five_hour") { return 5 * hourMs }
        if text.contains("week") || text.contains("seven_day") || text.contains("fable") { return 7 * dayMs }
        return nil
    }

    public var remainingPercent: Int? { usedPercent.map { 100 - $0 } }

    public func resetLabel(nowMs: Int64) -> String {
        guard let resetAtMs, resetAtMs > nowMs else { return "" }
        return "resets in " + UsageFormat.remaining(ms: resetAtMs - nowMs)
    }

    public func resetClockLabel(nowMs: Int64, timeZone: TimeZone = .current) -> String {
        guard let resetAtMs else { return "" }
        return UsageFormat.resetClock(atMs: resetAtMs, nowMs: nowMs, timeZone: timeZone)
    }

    public var stepMs: Int64? {
        guard let windowMs, windowMs > 0 else { return nil }
        return windowMs > UsageWindowMeter.dayMs ? UsageWindowMeter.dayMs : UsageWindowMeter.hourMs
    }

    public var segments: Int {
        guard let windowMs, let stepMs else { return 1 }
        return Int((windowMs + stepMs - 1) / stepMs)
    }

    public func elapsedFraction(nowMs: Int64) -> Double? {
        guard let windowMs, windowMs > 0, let resetAtMs, resetAtMs > nowMs else { return nil }
        return min(1, max(0, 1 - Double(resetAtMs - nowMs) / Double(windowMs)))
    }

    public func periodLabel(nowMs: Int64) -> String {
        guard let windowMs, let stepMs, let elapsed = elapsedFraction(nowMs: nowMs) else { return "" }
        let current = min(segments, Int(elapsed * Double(windowMs) / Double(stepMs)) + 1)
        return "\(stepMs == UsageWindowMeter.dayMs ? "Day" : "Hour") \(current) of \(segments)"
    }

    public func paceLabel(nowMs: Int64) -> String {
        guard let usedPercent, let elapsed = elapsedFraction(nowMs: nowMs) else { return "" }
        let ahead = usedPercent - Int((elapsed * 100).rounded())
        if ahead == 0 { return "on pace" }
        return "\(abs(ahead))% \(ahead > 0 ? "over" : "under") pace"
    }
}

public struct UsageAccount: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var label: String
    public var active: Bool
    public var signedIn: Bool?
    public var identity: String
    public var windows: [UsageWindowMeter]

    public init(
        id: String, label: String, active: Bool, signedIn: Bool? = nil, identity: String = "",
        windows: [UsageWindowMeter] = []
    ) {
        self.id = id
        self.label = label
        self.active = active
        self.signedIn = signedIn
        self.identity = identity
        self.windows = windows
    }

    public init(json: JSONObject) {
        self.init(
            id: json.str("id"), label: json.str("label"), active: json.bool("active"),
            signedIn: json.optBool("signedIn"), identity: json.str("identity"),
            windows: json.maps("windows").map(UsageWindowMeter.init(json:)))
    }
}

public struct UsageCredits: Equatable, Hashable, Sendable {
    public var used: Double?
    public var limit: Double?
    public var currency: String

    public init(used: Double?, limit: Double?, currency: String) {
        self.used = used
        self.limit = limit
        self.currency = currency
    }

    public var label: String {
        let unit = currency.isEmpty ? "" : " \(currency)"
        switch (used, limit) {
        case let (used?, limit?): return String(format: "%.2f of %.2f", used, limit) + unit
        case let (used?, nil): return String(format: "%.2f used", used) + unit
        default: return "Extra usage on"
        }
    }

    public var usedPercent: Int? {
        guard let used, let limit, limit > 0 else { return nil }
        return min(100, max(0, Int((used * 100 / limit).rounded())))
    }

    public var state: UsageState {
        guard let percent = usedPercent else { return used == nil ? .unknown : .ok }
        if percent >= 100 { return .exhausted }
        return percent >= 80 ? .high : .ok
    }

    public static func money(_ amount: Double, currency: String) -> String {
        let number = String(format: "%.2f", amount)
        switch currency.trimmingCharacters(in: .whitespaces).uppercased() {
        case "", "USD": return "$" + number
        case "EUR": return "\u{20AC}" + number
        case "GBP": return "\u{00A3}" + number
        case let code: return number + " " + code
        }
    }

    public var caption: String {
        switch (used, limit) {
        case let (used?, limit?):
            let percent = usedPercent.map { " (\($0)%)" } ?? ""
            return "\(Self.money(used, currency: currency)) of \(Self.money(limit, currency: currency))\(percent)"
        case let (used?, nil): return "\(Self.money(used, currency: currency)) used, no monthly limit"
        default: return "On, no reading yet"
        }
    }
}

public struct UsageProviderMeter: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var label: String
    public var windows: [UsageWindowMeter]
    public var accounts: [UsageAccount]
    public var credits: UsageCredits?

    public init(
        id: String, label: String, windows: [UsageWindowMeter] = [], accounts: [UsageAccount] = [],
        credits: UsageCredits? = nil
    ) {
        self.id = id
        self.label = label
        self.windows = windows
        self.accounts = accounts
        self.credits = credits
    }

    public init(json: JSONObject) {
        let accounts = json.maps("accounts").map(UsageAccount.init(json:))
        var credits: UsageCredits?
        if let object = json["credits"]?.objectValue {
            credits = UsageCredits(
                used: object["used"]?.doubleValue, limit: object["limit"]?.doubleValue,
                currency: object.str("currency"))
        }
        self.init(
            id: json.str("id"), label: json.str("label"),
            windows: json.maps("windows").map(UsageWindowMeter.init(json:)), accounts: accounts,
            credits: credits)
    }

    public var activeAccount: UsageAccount? { accounts.first { $0.active } }

    public var sessionWindow: UsageWindowMeter? { windows.first }

    public var weeklyWindow: UsageWindowMeter? {
        windows.first { $0.id == "weekly" } ?? windows.dropFirst().first
    }

    public var worstState: UsageState {
        let states = windows.map(\.state)
        if states.contains(.exhausted) { return .exhausted }
        if states.contains(.high) { return .high }
        if states.contains(.ok) { return .ok }
        return .unknown
    }
}

public struct UsageReminder: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var title: String
    public var body: String
    public var fireAtMs: Int64

    public init(id: String, title: String, body: String, fireAtMs: Int64) {
        self.id = id
        self.title = title
        self.body = body
        self.fireAtMs = fireAtMs
    }
}

public struct UsageSummary: Equatable, Sendable {
    public var providers: [UsageProviderMeter]
    public var claudeSpendUsd: Double?
    public var openAiSpendUsd: Double?
    public var atMs: Int64
    public var activeProvider: String

    public init(
        providers: [UsageProviderMeter] = [], claudeSpendUsd: Double? = nil, openAiSpendUsd: Double? = nil,
        atMs: Int64 = 0, activeProvider: String = ""
    ) {
        self.providers = providers
        self.claudeSpendUsd = claudeSpendUsd
        self.openAiSpendUsd = openAiSpendUsd
        self.atMs = atMs
        self.activeProvider = activeProvider
    }

    public init(json: JSONObject) {
        let spend = json.obj("spend")
        self.init(
            providers: json.maps("providers").map(UsageProviderMeter.init(json:)),
            claudeSpendUsd: spend["claudeUsd"]?.doubleValue, openAiSpendUsd: spend["openAiUsd"]?.doubleValue,
            atMs: json.int64("atMs"), activeProvider: json.str("activeProvider"))
    }

    public var activeMeter: UsageProviderMeter? {
        let metered = providers.filter { !$0.windows.isEmpty }
        return metered.first { $0.id == activeProvider } ?? metered.first
    }

    public var activeSession: (provider: UsageProviderMeter, window: UsageWindowMeter)? {
        guard let provider = activeMeter, let window = provider.windows.first else { return nil }
        return (provider, window)
    }

    public var totalSpendUsd: Double? {
        if claudeSpendUsd == nil && openAiSpendUsd == nil { return nil }
        return (claudeSpendUsd ?? 0) + (openAiSpendUsd ?? 0)
    }

    public func reminders(nowMs: Int64) -> [UsageReminder] {
        var out: [UsageReminder] = []
        for provider in providers {
            for window in provider.windows where window.state == .exhausted {
                guard let reset = window.resetAtMs, reset > nowMs else { continue }
                out.append(UsageReminder(
                    id: "usage.\(provider.id).\(window.id)",
                    title: "\(provider.label) is ready again",
                    body: "The \(window.label.lowercased()) usage limit has reset. Agents waiting on it can continue.",
                    fireAtMs: reset))
            }
        }
        return out.sorted { $0.fireAtMs < $1.fireAtMs }
    }
}

public enum UsageFormat {
    public static func remaining(ms: Int64) -> String {
        let minutes = max(1, Int((ms + 59_999) / 60_000))
        if minutes < 60 { return "\(minutes)m" }
        let hours = minutes / 60
        if hours < 48 {
            let rest = minutes % 60
            return rest == 0 ? "\(hours)h" : "\(hours)h \(rest)m"
        }
        return "\(hours / 24)d \(hours % 24)h"
    }

    public static func usd(_ value: Double) -> String { String(format: "$%.2f", value) }

    public static func resetClock(atMs resetAtMs: Int64, nowMs: Int64, timeZone: TimeZone = .current) -> String {
        guard resetAtMs > 0, resetAtMs > nowMs else { return "" }
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        let reset = Date(timeIntervalSince1970: Double(resetAtMs) / 1000)
        let now = Date(timeIntervalSince1970: Double(nowMs) / 1000)
        let days = calendar.dateComponents(
            [.day], from: calendar.startOfDay(for: now), to: calendar.startOfDay(for: reset)
        ).day ?? 0
        let clock = format(reset, "h:mm a", timeZone)
        if days == 0 { return "Resets \(clock)" }
        if days < 7 { return "Resets \(format(reset, "EEE", timeZone)) \(clock)" }
        return "Resets \(format(reset, "MMM d", timeZone)), \(clock)"
    }

    private static func format(_ date: Date, _ pattern: String, _ timeZone: TimeZone) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = timeZone
        formatter.dateFormat = pattern
        return formatter.string(from: date)
    }
}

public struct CapabilityGrant: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var capability: String
    public var kind: ApprovalKind
    public var resource: String
    public var decision: String
    public var lifetime: String
    public var worktree: String

    public init(
        id: String, capability: String, kind: ApprovalKind = .other, resource: String = "",
        decision: String = "", lifetime: String = "", worktree: String = ""
    ) {
        self.id = id
        self.capability = capability
        self.kind = kind
        self.resource = resource
        self.decision = decision
        self.lifetime = lifetime
        self.worktree = worktree
    }

    public init(json: JSONObject) {
        self.init(
            id: json.str("id"), capability: json.str("capability"),
            kind: ApprovalKind(rawValue: json.str("kind")) ?? .other, resource: json.str("resource"),
            decision: json.str("decision"), lifetime: json.str("lifetime"), worktree: json.str("worktree"))
    }

    public var allows: Bool { decision == "allow" }
}

public struct AgentDiffFile: Equatable, Sendable, Identifiable {
    public var path: String
    public var added: Int
    public var removed: Int
    public var binary: Bool

    public init(path: String, added: Int = 0, removed: Int = 0, binary: Bool = false) {
        self.path = path
        self.added = added
        self.removed = removed
        self.binary = binary
    }

    public var id: String { path }
}

public struct MergeVelocityWindow: Equatable, Sendable, Identifiable {
    public var id: String
    public var label: String
    public var count: Int
    public var top: Int
    public var avg: Double
    public var series: [Int]

    public init(id: String, label: String, count: Int, top: Int, avg: Double, series: [Int] = []) {
        self.id = id
        self.label = label
        self.count = count
        self.top = top
        self.avg = avg
        self.series = series
    }

    public var peak: Int { series.max() ?? 0 }

    public var record: Int { max(count, top) }

    public var share: Double { record > 0 ? Double(count) / Double(record) : 0 }

    public var avgShare: Double { record > 0 ? min(avg / Double(record), 1) : 0 }

    public var spoken: String {
        "\(label): \(count == 1 ? "1 merge" : "\(count) merges"), best \(record), average \(avgText)"
    }

    public var avgText: String {
        avg >= 10 || avg == 0 ? String(Int(avg.rounded())) : String(format: "%.1f", avg)
    }

    public var detail: String { "top \(max(count, top)) \u{00B7} avg \(avgText)" }
}

public struct MergeStats: Equatable, Sendable {
    public var base: String
    public var commit: String
    public var commits: Int?
    public var files: Int
    public var insertions: Int
    public var deletions: Int
    public var velocity: [MergeVelocityWindow]

    public init(
        base: String = "", commit: String = "", commits: Int? = nil, files: Int = 0, insertions: Int = 0,
        deletions: Int = 0, velocity: [MergeVelocityWindow] = []
    ) {
        self.base = base
        self.commit = commit
        self.commits = commits
        self.files = files
        self.insertions = insertions
        self.deletions = deletions
        self.velocity = velocity
    }

    public init?(json: JSONObject) {
        guard case .object? = json["stats"] else { return nil }
        let stats = json.obj("stats")
        self.init(
            base: stats.str("base"), commit: stats.str("commit"),
            commits: stats["commits"] == nil ? nil : stats.int("commits"), files: stats.int("files"),
            insertions: stats.int("insertions"), deletions: stats.int("deletions"),
            velocity: stats.maps("velocity").map {
                MergeVelocityWindow(
                    id: $0.str("id"), label: $0.str("label"), count: $0.int("count"), top: $0.int("top"),
                    avg: $0.double("avg"), series: $0.list("series").map { $0.int(or: 0) })
            })
    }

    public var shortCommit: String { String(commit.prefix(7)) }
    public var hasLanding: Bool { !commit.isEmpty }
    public var commitsText: String {
        guard let commits, commits > 0 else { return "" }
        return commits == 1 ? "1 commit" : "\(commits) commits"
    }
    public var filesText: String { files == 1 ? "1 file" : "\(files) files" }
    public var insertionsText: String { "+\(insertions)" }
    public var deletionsText: String { "\u{2212}\(deletions)" }

    public var insertionShare: Double? {
        let lines = insertions + deletions
        return lines > 0 ? Double(insertions) / Double(lines) : nil
    }

    public var summary: String {
        guard hasLanding else { return "" }
        var parts: [String] = []
        if !commitsText.isEmpty { parts.append(commitsText) }
        parts.append("\(filesText) \(insertionsText) \(deletionsText)")
        return parts.joined(separator: " \u{00B7} ")
    }
}

public struct MergeOutcome: Equatable, Sendable {
    public var branch: String
    public var stats: MergeStats?
    public var setAsideNote: String

    public init(branch: String, stats: MergeStats? = nil, setAsideNote: String = "") {
        self.branch = branch
        self.stats = stats
        self.setAsideNote = setAsideNote
    }

    public init(json: JSONObject) {
        self.init(branch: json.str("branch"), stats: MergeStats(json: json), setAsideNote: json.str("setAside"))
    }

    public func message(fallbackBranch: String = "") -> String {
        let name = branch.isEmpty ? fallbackBranch : branch
        let head = name.isEmpty ? "Merged and cleaned up." : "Merged \(name) and cleaned up."
        let summary = stats?.summary ?? ""
        let text = summary.isEmpty ? head : "\(head) \(summary)."
        let note = setAsideNote.trimmingCharacters(in: .whitespacesAndNewlines)
        return note.isEmpty ? text : "\(text) \(note)"
    }
}

public struct MergeLeaderboard: Equatable, Sendable {
    public var repo: String
    public var branch: String
    public var stats: MergeStats?

    public init(repo: String = "", branch: String = "", stats: MergeStats? = nil) {
        self.repo = repo
        self.branch = branch
        self.stats = stats
    }

    public init(json: JSONObject) {
        self.init(repo: json.str("repo"), branch: json.str("branch"), stats: MergeStats(json: json))
    }

    public var subtitle: String {
        var parts: [String] = []
        if !repo.isEmpty { parts.append(repo) }
        if !branch.isEmpty { parts.append("newest merge: \(branch)") }
        if let base = stats?.base, !base.isEmpty { parts.append("into \(base)") }
        return parts.joined(separator: " \u{00B7} ")
    }
}

public struct AgentDiff: Equatable, Sendable {
    public var sessionId: Int
    public var branch: String
    public var base: String
    public var merged: Bool
    public var files: [AgentDiffFile]
    public var committed: String
    public var uncommitted: String
    public var untracked: [String]
    public var hasWorktree: Bool
    public var truncated: Bool

    public init(json: JSONObject) {
        sessionId = json.int("id")
        branch = json.str("branch")
        base = json.str("base")
        merged = json.bool("merged")
        files = json.maps("files").map {
            AgentDiffFile(path: $0.str("path"), added: $0.int("added"), removed: $0.int("removed"), binary: $0.bool("binary"))
        }
        committed = json.str("committed")
        uncommitted = json.str("uncommitted")
        untracked = json.list("untracked").map { $0.text }
        hasWorktree = json.bool("hasWorktree")
        truncated = json.bool("truncated")
    }

    public var added: Int { files.reduce(0) { $0 + $1.added } }
    public var removed: Int { files.reduce(0) { $0 + $1.removed } }
    public var isEmpty: Bool { committed.isEmpty && uncommitted.isEmpty && untracked.isEmpty }

    public enum LineKind: Equatable, Sendable { case header, hunk, added, removed, context }

    public struct Line: Equatable, Sendable, Identifiable {
        public var id: Int
        public var kind: LineKind
        public var text: String
    }

    public static func lines(_ patch: String, limit: Int = 4000) -> [Line] {
        var out: [Line] = []
        var parts = patch.components(separatedBy: "\n")
        if parts.last == "" { parts.removeLast() }
        for (index, text) in parts.prefix(limit).enumerated() {
            let kind: LineKind
            if text.hasPrefix("diff --git") || text.hasPrefix("index ") || text.hasPrefix("+++") || text.hasPrefix("---") {
                kind = .header
            } else if text.hasPrefix("@@") {
                kind = .hunk
            } else if text.hasPrefix("+") {
                kind = .added
            } else if text.hasPrefix("-") {
                kind = .removed
            } else {
                kind = .context
            }
            out.append(Line(id: index, kind: kind, text: text))
        }
        return out
    }
}
