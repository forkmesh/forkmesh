import FMAgentWatch
import FMApi
import FMBase
import FMChat
import FMControl
import Foundation

public enum AgentLineStatus: String, Equatable, Sendable {
    case running, queued, waiting, success, failed, stopped, merged, other
}

public struct AgentLine: Identifiable, Equatable, Sendable {
    public enum Source: Equatable, Sendable {
        case entry(AgentEntry)
        case control(UnifiedAgentRow)
    }

    public var id: String
    public var source: Source
    public var title: String
    public var face: String
    public var status: AgentLineStatus
    public var statusLabel: String
    public var isActive: Bool
    public var updatedAtMs: Int
    public var repoName: String
    public var branch: String
    public var hasWorktree: Bool
    public var ahead: Int
    public var behind: Int
    public var filesChanged: Int
    public var linesAdded: Int
    public var linesRemoved: Int
    public var conflicted: Bool
    public var prNumber: Int
    public var cloud: Bool
    public var outside: Bool
    public var attachments: [AgentAttachmentRef]
    public var provider: String
    public var runner: String = ""
    public var priority = 50
    public var readKey = ""
    public var finishedAtMs = 0
    public var desktopSessionId = 0
    public var desktopUnread: Bool?
    public var unread = false

    public var runnerBadge: String { runner.isEmpty ? "" : RunnerAgentText.badge(runner) }

    public init(
        id: String, source: Source, title: String, face: String, status: AgentLineStatus, statusLabel: String,
        isActive: Bool, updatedAtMs: Int, repoName: String, branch: String = "", hasWorktree: Bool = false,
        ahead: Int = -1, behind: Int = -1, filesChanged: Int = -1, linesAdded: Int = -1, linesRemoved: Int = -1,
        conflicted: Bool = false, prNumber: Int = 0, cloud: Bool = false, outside: Bool = false,
        attachments: [AgentAttachmentRef] = [], provider: String = ""
    ) {
        self.id = id
        self.source = source
        self.title = title
        self.face = face
        self.status = status
        self.statusLabel = statusLabel
        self.isActive = isActive
        self.updatedAtMs = updatedAtMs
        self.repoName = repoName
        self.branch = branch
        self.hasWorktree = hasWorktree
        self.ahead = ahead
        self.behind = behind
        self.filesChanged = filesChanged
        self.linesAdded = linesAdded
        self.linesRemoved = linesRemoved
        self.conflicted = conflicted
        self.prNumber = prNumber
        self.cloud = cloud
        self.outside = outside
        self.attachments = attachments
        self.provider = provider
    }

    public var thumbnail: AgentAttachmentRef? { attachments.first { $0.showsThumbnail } }

    public var accessibilitySummary: String {
        var parts: [String] = [unread ? "Unread" : "", title, statusLabel, repoName]
        if !runner.isEmpty { parts.append("on runner \(runner)") }
        if !branch.isEmpty { parts.append("branch \(branch)") }
        if hasWorktree { parts.append("worktree") }
        if ahead > 0 { parts.append(ahead == 1 ? "1 commit" : "\(ahead) commits") }
        if filesChanged >= 0 { parts.append(filesChanged == 1 ? "1 file changed" : "\(filesChanged) files changed") }
        if linesAdded > 0 || linesRemoved > 0 { parts.append("\(max(linesAdded, 0)) added, \(max(linesRemoved, 0)) removed") }
        if conflicted { parts.append("conflict") }
        if prNumber > 0 { parts.append("PR \(prNumber)") }
        return parts.filter { !$0.isEmpty }.joined(separator: ", ")
    }
}

public enum AgentLineDeletion: Equatable, Sendable {
    case desktop(sessionId: Int)
    case outside(id: Int)
    case runner(RunnerAgentRef, merged: Bool, active: Bool)
}

public enum AgentListView: String, CaseIterable, Sendable {
    case compact, zen, full, eagle

    public static let storageKey = "agents/listView"
    public static let eagleDefaultKey = "agents/listViewEagleDefault"

    public init(stored: String?) {
        self = AgentListView(rawValue: stored ?? "") ?? .eagle
    }

    public init(stored: String?, eagleDefaulted: Bool) {
        self = eagleDefaulted ? AgentListView(stored: stored) : .eagle
    }

    public var next: AgentListView {
        switch self {
        case .compact: return .zen
        case .zen: return .full
        case .full: return .eagle
        case .eagle: return .compact
        }
    }

    public var caption: String {
        switch self {
        case .compact: return "Compact"
        case .zen: return "Zen"
        case .full: return "Full"
        case .eagle: return "Eagle"
        }
    }
}

public enum AgentLineSort: String, CaseIterable, Sendable {
    case recent, running, status, priority, name, repo, changes

    public static let storageKey = "agents/listSort"

    public init(stored: String?) {
        self = AgentLineSort(rawValue: stored ?? "") ?? .recent
    }

    public var next: AgentLineSort {
        let all = AgentLineSort.allCases
        let index = all.firstIndex(of: self) ?? 0
        return all[(index + 1) % all.count]
    }

    public var caption: String {
        switch self {
        case .recent: return "Recent"
        case .running: return "Running"
        case .status: return "Status"
        case .priority: return "Priority"
        case .name: return "Name"
        case .repo: return "Repo"
        case .changes: return "Changes"
        }
    }

    public func sorted(_ lines: [AgentLine]) -> [AgentLine] {
        lines.sorted { a, b in
            if let first = precedes(a, b) { return first }
            if a.updatedAtMs != b.updatedAtMs { return a.updatedAtMs > b.updatedAtMs }
            return a.id < b.id
        }
    }

    private func precedes(_ a: AgentLine, _ b: AgentLine) -> Bool? {
        switch self {
        case .recent:
            return nil
        case .running:
            return a.isActive == b.isActive ? nil : a.isActive
        case .status:
            let x = AgentLineSort.statusRank(a.status)
            let y = AgentLineSort.statusRank(b.status)
            return x == y ? nil : x < y
        case .priority:
            return a.priority == b.priority ? nil : a.priority < b.priority
        case .name:
            let x = a.title.lowercased()
            let y = b.title.lowercased()
            return x == y ? nil : x < y
        case .repo:
            let x = a.repoName.lowercased()
            let y = b.repoName.lowercased()
            return x == y ? nil : x < y
        case .changes:
            let x = AgentLineSort.churn(a)
            let y = AgentLineSort.churn(b)
            if x != y { return x > y }
            return a.filesChanged == b.filesChanged ? nil : a.filesChanged > b.filesChanged
        }
    }

    static func statusRank(_ status: AgentLineStatus) -> Int {
        switch status {
        case .waiting: return 0
        case .running: return 1
        case .queued: return 2
        case .failed: return 3
        case .stopped: return 4
        case .success: return 5
        case .merged: return 6
        case .other: return 7
        }
    }

    static func churn(_ line: AgentLine) -> Int {
        max(line.linesAdded, 0) + max(line.linesRemoved, 0)
    }
}

public enum AgentLineChipSlot: Int, CaseIterable, Sendable {
    case status, provider, host, age, sync, files, churn, branch

    public var widestWords: [String] {
        switch self {
        case .status: return ["Running", "Queued", "Waiting", "Success", "Failed", "Stopped", "Merged", "Idle"]
        case .provider: return ["Claude", "Codex", "Llama"]
        case .host: return ["Desktop", "Cloud", "Runner", "Outside"]
        case .age: return ["now", "59m", "23h", "12mo"]
        case .sync: return ["Conflicts", "Behind"]
        case .files: return ["999+"]
        case .churn: return ["+9.9k\u{2212}9.9k"]
        case .branch: return ["Worktree", "Branch"]
        }
    }
}

public struct AgentLineChip: Equatable, Sendable {
    public var slot: AgentLineChipSlot
    public var word: String

    public init(slot: AgentLineChipSlot, word: String) {
        self.slot = slot
        self.word = word
    }
}

public struct AgentFolderSection: Identifiable, Equatable, Sendable {
    public let id: String
    public let name: String
    public let repoName: String
    public let collapsed: Bool
    public let lines: [AgentLine]

    public init(id: String = "", name: String = "", repoName: String = "", collapsed: Bool = false, lines: [AgentLine]) {
        self.id = id
        self.name = name
        self.repoName = repoName
        self.collapsed = collapsed
        self.lines = lines
    }

    public var isFolder: Bool { !id.isEmpty }
}

public struct AgentFolderFolds: Equatable, Sendable {
    public static let collapsedKey = "agents/folderCollapsed"
    public static let expandedKey = "agents/folderExpanded"

    public private(set) var collapsed: Set<String>
    public private(set) var expanded: Set<String>

    public init(collapsed: Set<String> = [], expanded: Set<String> = []) {
        self.collapsed = collapsed
        self.expanded = expanded.subtracting(collapsed)
    }

    public static func load(_ store: KeyValueStore?) -> AgentFolderFolds {
        AgentFolderFolds(
            collapsed: Set(store?.stringList(forKey: collapsedKey) ?? []),
            expanded: Set(store?.stringList(forKey: expandedKey) ?? []))
    }

    public func isCollapsed(_ section: AgentFolderSection) -> Bool {
        guard section.isFolder else { return false }
        if collapsed.contains(section.id) { return true }
        if expanded.contains(section.id) { return false }
        return section.collapsed
    }

    public mutating func set(_ id: String, collapsed fold: Bool) {
        guard !id.isEmpty else { return }
        if fold {
            collapsed.insert(id)
            expanded.remove(id)
        } else {
            expanded.insert(id)
            collapsed.remove(id)
        }
    }

    public func save(_ store: KeyValueStore?) {
        store?.set(collapsed.sorted(), forKey: Self.collapsedKey)
        store?.set(expanded.sorted(), forKey: Self.expandedKey)
    }
}

public enum AgentLines {
    public static func statusWord(_ status: AgentLineStatus) -> String {
        switch status {
        case .running: return "Running"
        case .queued: return "Queued"
        case .waiting: return "Waiting"
        case .success: return "Success"
        case .failed: return "Failed"
        case .stopped: return "Stopped"
        case .merged: return "Merged"
        case .other: return "Idle"
        }
    }

    public static func providerWord(_ provider: String) -> String {
        let value = provider.lowercased()
        if value.contains("claude") || value.contains("anthropic") { return "Claude" }
        if value.contains("codex") || value.contains("openai") || value.contains("gpt") { return "Codex" }
        if value.contains("llama") { return "Llama" }
        return ""
    }

    public static func runnerWord(_ name: String) -> String {
        let clean = name.dartTrimmed
        if clean.isEmpty { return "Runner" }
        return clean.count > 10 ? String(clean.prefix(9)) + "\u{2026}" : clean
    }

    public static func runnerCounts(_ lines: [AgentLine]) -> [String: Int] {
        var counts: [String: Int] = [:]
        for line in lines where line.isActive {
            let name: String = line.runner.dartTrimmed.lowercased()
            if !name.isEmpty {
                counts[name, default: 0] += 1
            }
        }
        return counts
    }

    public static func compactCount(_ value: Int) -> String {
        if value >= 10_000 { return "\(value / 1000)k" }
        if value >= 1000 { return "\(value / 1000).\(value % 1000 / 100)k" }
        return "\(max(value, 0))"
    }

    public static func chips(_ line: AgentLine, nowMs: Int) -> [AgentLineChip] {
        var chips = [AgentLineChip(slot: .status, word: statusWord(line.status))]
        let provider = providerWord(line.provider)
        if !provider.isEmpty { chips.append(AgentLineChip(slot: .provider, word: provider)) }
        chips.append(AgentLineChip(
            slot: .host, word: !line.runner.isEmpty ? runnerWord(line.runner) : line.cloud ? "Cloud" : line.outside ? "Outside" : "Desktop"))
        let age = shortAge(ms: line.updatedAtMs, nowMs: nowMs)
        if !age.isEmpty { chips.append(AgentLineChip(slot: .age, word: age)) }
        if line.conflicted {
            chips.append(AgentLineChip(slot: .sync, word: "Conflicts"))
        } else if line.behind > 0 {
            chips.append(AgentLineChip(slot: .sync, word: "Behind"))
        }
        if line.filesChanged >= 0 { chips.append(AgentLineChip(slot: .files, word: count(line.filesChanged))) }
        if line.linesAdded > 0 || line.linesRemoved > 0 {
            chips.append(AgentLineChip(
                slot: .churn,
                word: "+\(compactCount(line.linesAdded))\u{2212}\(compactCount(line.linesRemoved))"))
        }
        if !line.branch.isEmpty {
            chips.append(AgentLineChip(slot: .branch, word: line.hasWorktree ? "Worktree" : "Branch"))
        }
        return chips
    }

    public static func dialog(_ transcript: String, limit: Int = 120) -> String {
        var text = transcript
        if text.utf8.count > 6000 {
            text = String(text.suffix(6000))
            if let cut = text.range(of: "\n\n") { text = String(text[cut.upperBound...]) }
        }
        text = text.replacingOccurrences(of: "\r\n", with: "\n")
        if !text.hasSuffix("\n") {
            text = text.range(of: "\n", options: .backwards).map { String(text[..<$0.upperBound]) } ?? ""
        }
        for block in AgentTranscriptFormat.blocks(text).reversed() {
            let said = oneLine(spoken(block))
            if said.isEmpty { continue }
            return said.count > limit ? String(said.prefix(limit - 1)) + "\u{2026}" : said
        }
        return ""
    }

    public static func dialog(_ session: AgentLaunchSession) -> String {
        if session.isActive {
            let live = dialog(session.liveText + "\n")
            if !live.isEmpty { return live }
        }
        return dialog((session.history.last { !$0.isUser }?.text ?? "") + "\n")
    }

    private static func spoken(_ block: TranscriptBlock) -> String {
        switch block {
        case .text(let text), .heading(_, let text), .bullet(_, _, let text): return text
        case .prompt, .error, .steer, .tools, .task, .status, .code, .json, .table, .rule: return ""
        }
    }

    private static func oneLine(_ text: String) -> String {
        let plain = AgentTranscriptFormat.inline(text).map(\.text).joined()
        return plain.split(whereSeparator: \.isWhitespace).joined(separator: " ")
    }

    public static func status(_ raw: String, merged: Bool, active: Bool) -> AgentLineStatus {
        if merged { return .merged }
        let value = raw.lowercased()
        let has: ([String]) -> Bool = { words in words.contains { value.contains($0) } }
        if has(["fail", "error", "reject"]) { return .failed }
        if has(["stop", "cancel", "interrupt"]) { return .stopped }
        if has(["wait", "input", "approval"]) { return .waiting }
        if has(["queue", "pending", "claim"]) { return .queued }
        if has(["run", "start", "progress"]) { return .running }
        if has(["done", "complete", "success", "succeed", "finish"]) { return .success }
        return active ? .running : .other
    }

    public static func shortAge(ms: Int, nowMs: Int) -> String {
        if ms <= 0 { return "" }
        let secs = (nowMs - ms) / 1000
        if secs < 0 { return "now" }
        if secs < 60 { return "\(secs)s" }
        let mins = secs / 60
        if mins < 60 { return "\(mins)m" }
        let hours = mins / 60
        if hours < 24 { return "\(hours)h" }
        let days = hours / 24
        if days < 7 { return "\(days)d" }
        if days < 30 { return "\(days / 7)w" }
        let months = days / 30
        if months < 12 { return "\(months)mo" }
        return "\(days / 365)y"
    }

    public static func count(_ value: Int) -> String {
        value > 999 ? "999+" : "\(value)"
    }

    public static func line(_ row: UnifiedAgentRow) -> AgentLine {
        var built = AgentLine(
            id: "control:\(row.id)", source: .control(row), title: row.title,
            face: watchFaceName(provider: row.provider, model: row.model),
            status: status(row.status, merged: row.merged, active: row.isActive), statusLabel: row.statusLabel,
            isActive: row.isActive, updatedAtMs: row.updatedAtMs,
            repoName: row.repoName.isEmpty ? row.cwd : row.repoName, branch: row.branchName,
            hasWorktree: row.hasWorktree, ahead: row.ahead, behind: row.behind, filesChanged: row.filesChanged,
            linesAdded: row.linesAdded, linesRemoved: row.linesRemoved, conflicted: row.conflicted,
            prNumber: row.prNumber, cloud: !row.isDesktop, outside: row.outside, attachments: row.attachments,
            provider: row.provider)
        built.runner = row.runner
        built.priority = row.priority
        built.readKey = readKey(row)
        built.finishedAtMs = row.finishedAtMs
        if row.isDesktop && !row.outside && row.localId > 0 {
            built.desktopSessionId = row.localId
            built.desktopUnread = row.unread
        }
        return built
    }

    public static func readKey(_ row: UnifiedAgentRow) -> String {
        if row.outside { return "outside:\(row.localId)" }
        if row.isDesktop { return "desktop:\(row.repoName.lowercased())#\(row.localId)" }
        return row.cloudSessionId.isEmpty ? "control:\(row.id)" : "run:\(row.cloudSessionId)"
    }

    public static func line(_ entry: AgentEntry, live: UnifiedAgentRow? = nil) -> AgentLine {
        switch entry {
        case .desktop(let repo, let session):
            let updated = max(session.finishedAtMs, session.startedAtMs, session.createdAtMs, session.mergedAtMs)
            let active = live?.isActive ?? session.isActive
            var built = AgentLine(
                id: entry.id, source: .entry(entry), title: session.displayTitle,
                face: watchFaceName(provider: session.provider, model: session.model),
                status: live.map { status($0.status, merged: $0.merged, active: $0.isActive) }
                    ?? status(session.status, merged: session.merged, active: session.isActive),
                statusLabel: live?.statusLabel ?? session.statusLabel, isActive: active, updatedAtMs: updated,
                repoName: repo.name, branch: session.branchName,
                hasWorktree: live?.hasWorktree ?? session.hasWorktree,
                ahead: known(session.ahead, live?.ahead), behind: known(session.behind, live?.behind),
                filesChanged: known(session.filesChanged, live?.filesChanged),
                linesAdded: known(session.linesAdded, live?.linesAdded),
                linesRemoved: known(session.linesRemoved, live?.linesRemoved),
                conflicted: session.conflicted || (live?.conflicted ?? false), prNumber: session.prNumber,
                attachments: live?.attachments ?? [], provider: session.provider)
            built.priority = live?.priority ?? session.priority
            built.readKey = "desktop:\(repo.name.lowercased())#\(session.id)"
            built.finishedAtMs = max(session.finishedAtMs, live?.finishedAtMs ?? 0)
            if let live, live.localId > 0 {
                built.desktopSessionId = live.localId
                built.desktopUnread = live.unread
            }
            return built
        case .launched(let repo, let session, let pulls, let cloud):
            let chain = AgentsLogic.taskChain(session, pulls)
            let updated = max(session.updatedAtMs, session.completedAtMs, session.createdAtMs)
            let refs = session.history.first(where: \.isUser)
                .map { session.attachmentRefs($0, owner: repo.owner, repo: repo.name) } ?? []
            let merged = chain.isMerged || session.isMergedBranch
            let stats = session.branchStats
            var built = AgentLine(
                id: entry.id, source: .entry(entry), title: session.displayTitle,
                face: watchFaceName(provider: session.provider, model: session.requestedModel),
                status: status(session.status, merged: merged, active: session.isActive),
                statusLabel: chain.summary.isEmpty ? session.statusLabel : chain.summary,
                isActive: session.isActive, updatedAtMs: updated, repoName: repo.name, branch: session.branchName,
                ahead: stats.known ? stats.ahead : -1, behind: stats.known ? stats.behind : -1,
                filesChanged: stats.known ? stats.files : -1, linesAdded: stats.known ? stats.added : -1,
                linesRemoved: stats.known ? stats.removed : -1,
                conflicted: stats.conflicted || session.branchMerge.isConflict,
                prNumber: chain.prNumber, cloud: cloud || session.onRunner,
                attachments: refs, provider: session.provider)
            built.runner = session.runner
            built.readKey = session.sessionId.isEmpty ? entry.id : "run:\(session.sessionId)"
            built.finishedAtMs = session.completedAtMs
            return built
        }
    }

    public static func merge(entries: [AgentEntry], rows: [UnifiedAgentRow], cover: DesktopListCover? = nil) -> [AgentLine] {
        var desktop: [String: UnifiedAgentRow] = [:]
        var cloud: [String: UnifiedAgentRow] = [:]
        for row in rows where !row.outside {
            if row.isDesktop {
                desktop["\(row.repoName.lowercased())#\(row.localId)"] = row
            } else if !row.cloudSessionId.isEmpty {
                cloud[row.cloudSessionId] = row
            }
        }
        var used: Set<String> = []
        var launched: Set<String> = []
        var lines: [AgentLine] = entries.compactMap { entry in
            switch entry {
            case .desktop(let repo, let session):
                let key = "\(repo.name.lowercased())#\(session.id)"
                let live = desktop[key]
                if let live { used.insert(live.id) }
                if live == nil, let cover, cover.covers(createdAtMs: session.createdAtMs) { return nil }
                return line(entry, live: live)
            case .launched(let repo, let session, _, _):
                if !session.sessionId.isEmpty {
                    if launched.contains(session.sessionId) { return nil }
                    launched.insert(session.sessionId)
                }
                if let live = cloud[session.sessionId] {
                    used.insert(live.id)
                    if prefersRunScreen(live) { return line(live) }
                    return fresher(line(entry), live: live)
                }
                if claimedByDesktop(repo, session, rows: rows) { return nil }
                return line(entry)
            }
        }
        lines += rows.filter { !used.contains($0.id) }.map(line)
        return lines.sorted { a, b in
            if a.isActive != b.isActive { return a.isActive }
            if a.updatedAtMs != b.updatedAtMs { return a.updatedAtMs > b.updatedAtMs }
            return a.id < b.id
        }
    }

    public static func folderKey(_ line: AgentLine) -> String? {
        switch line.source {
        case .entry(.desktop(let repo, let session)):
            return session.id > 0 ? "\(repo.name.lowercased())#\(session.id)" : nil
        case .entry(.launched(let repo, let session, _, _)):
            return session.sessionId.isEmpty ? nil : "\(repo.name.lowercased())#cloud:\(session.sessionId)"
        case .control(let row):
            if row.outside { return nil }
            if row.isDesktop { return row.localId > 0 ? "\(row.repoName.lowercased())#\(row.localId)" : nil }
            return row.cloudSessionId.isEmpty ? nil : "\(row.repoName.lowercased())#cloud:\(row.cloudSessionId)"
        }
    }

    public static func folderSections(_ lines: [AgentLine], folders: [LocalAgentFolder]) -> [AgentFolderSection] {
        if folders.isEmpty { return lines.isEmpty ? [] : [AgentFolderSection(lines: lines)] }
        var home: [String: String] = [:]
        for folder in folders {
            let repo = folder.repoName.lowercased()
            for member in folder.members where home["\(repo)#\(member)"] == nil {
                home["\(repo)#\(member)"] = folder.id
            }
        }
        var grouped: [String: [AgentLine]] = [:]
        var loose: [AgentLine] = []
        for line in lines {
            if let key = folderKey(line), let id = home[key] {
                grouped[id, default: []].append(line)
            } else {
                loose.append(line)
            }
        }
        var sections: [AgentFolderSection] = folders.compactMap { folder in
            guard let inside = grouped.removeValue(forKey: folder.id) else { return nil }
            return AgentFolderSection(
                id: folder.id, name: folder.name, repoName: folder.repoName, collapsed: folder.collapsed, lines: inside)
        }
        if !loose.isEmpty { sections.append(AgentFolderSection(lines: loose)) }
        return sections
    }

    static func fresher(_ built: AgentLine, live: UnifiedAgentRow) -> AgentLine {
        guard built.status != .merged, live.updatedAtMs >= built.updatedAtMs else { return built }
        var line = built
        let state = status(live.status, merged: live.merged, active: live.isActive)
        if state != built.status { line.statusLabel = live.statusLabel }
        line.status = state
        line.isActive = live.isActive
        line.updatedAtMs = live.updatedAtMs
        line.finishedAtMs = max(built.finishedAtMs, live.finishedAtMs)
        return line
    }

    static func claimedByDesktop(_ repo: Repository, _ session: AgentLaunchSession, rows: [UnifiedAgentRow]) -> Bool {
        claimingDesktop(repo, session, rows: rows) != nil
    }

    static func claimingDesktop(_ repo: Repository, _ session: AgentLaunchSession, rows: [UnifiedAgentRow]) -> UnifiedAgentRow? {
        guard session.localAgentId > 0 else { return nil }
        let node = (session.runner.dartTrimmed.isEmpty ? session.targetNode : session.runner).lowercased()
        let name = repo.name.lowercased()
        if node.isEmpty { return nil }
        return rows.first {
            $0.isDesktop && !$0.outside && $0.localId == session.localAgentId && $0.node.lowercased() == node
                && $0.repoName.lowercased() == name
        }
    }

    public static func watchKeys(_ line: AgentLine, rows: [UnifiedAgentRow]) -> [String] {
        switch line.source {
        case .control(let row):
            return ["control:\(row.id)"]
        case .entry(.desktop(let repo, let session)):
            let name = repo.name.lowercased()
            let live = rows.filter { $0.isDesktop && !$0.outside && $0.repoName.lowercased() == name && $0.localId == session.id }
            return live.map { "control:\($0.id)" } + ["relay:\(repo.fullName)#\(session.id)"]
        case .entry(.launched(_, let session, _, _)):
            return rows.filter { !$0.isDesktop && $0.cloudSessionId == session.sessionId }.map { "control:\($0.id)" }
        }
    }

    public static func deletion(_ line: AgentLine) -> AgentLineDeletion? {
        switch line.source {
        case .control(let row):
            if row.outside { return row.localId < 0 ? .outside(id: row.localId) : nil }
            if row.isDesktop { return row.localId > 0 ? .desktop(sessionId: row.localId) : nil }
            if row.cloudSessionId.isEmpty { return nil }
            return .runner(
                RunnerAgentRef(
                    owner: row.repoOwner, repo: row.repoName, sessionId: row.cloudSessionId, orgScope: row.orgScope,
                    branch: row.branchName, base: row.baseBranch, pull: row.prNumber),
                merged: row.merged, active: row.isActive)
        case .entry(.desktop(_, let session)):
            return session.id > 0 ? .desktop(sessionId: session.id) : nil
        case .entry(.launched(let repo, let session, _, _)):
            if session.sessionId.isEmpty { return nil }
            return .runner(
                RunnerAgentRef(
                    owner: repo.owner, repo: repo.name, sessionId: session.sessionId, orgScope: session.orgScope,
                    branch: session.branchName, base: session.baseBranch, pull: session.prNumber),
                merged: line.status == .merged, active: line.isActive)
        }
    }

    public static func prefersRunScreen(_ row: UnifiedAgentRow) -> Bool {
        !row.isDesktop && (row.isRunner || !row.branchName.isEmpty)
    }

    public static func runRow(_ repo: Repository, _ session: AgentLaunchSession, rows: [UnifiedAgentRow]) -> UnifiedAgentRow? {
        guard !session.sessionId.isEmpty else { return nil }
        if let live = rows.first(where: { !$0.isDesktop && !$0.outside && $0.cloudSessionId == session.sessionId }) {
            return live
        }
        if let desktop = claimingDesktop(repo, session, rows: rows) { return desktop }
        var run = session
        run.owner = repo.owner
        run.repo = repo.name
        return UnifiedAgentRow.fromCloud(run)
    }

    private static func known(_ value: Int, _ live: Int?) -> Int {
        if let live, live >= 0 { return live }
        return value
    }
}
