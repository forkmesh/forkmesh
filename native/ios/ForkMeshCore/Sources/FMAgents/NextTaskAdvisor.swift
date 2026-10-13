import FMApi
import FMBase
import FMLocalAI
import Foundation
import Observation

public struct NextTaskWork: Equatable, Sendable {
    public var title: String
    public var repo: String
    public var status: String
    public var note: String
    public var atMs: Int

    public init(title: String, repo: String = "", status: String = "", note: String = "", atMs: Int = 0) {
        self.title = title
        self.repo = repo
        self.status = status
        self.note = note
        self.atMs = atMs
    }
}

public struct NextTaskIssue: Equatable, Sendable {
    public var number: Int
    public var title: String
    public var labels: [String]
    public var priority: Int
    public var votes: Int
    public var summary: String

    public init(number: Int, title: String, labels: [String] = [], priority: Int = 0, votes: Int = 0, summary: String = "") {
        self.number = number
        self.title = title
        self.labels = labels
        self.priority = priority
        self.votes = votes
        self.summary = summary
    }
}

public struct NextTaskDoc: Equatable, Sendable {
    public var path: String
    public var text: String

    public init(path: String, text: String) {
        self.path = path
        self.text = text
    }
}

public struct NextTaskContext: Equatable, Sendable {
    public var repo: String
    public var live: [NextTaskWork]
    public var recent: [NextTaskWork]
    public var issues: [NextTaskIssue]
    public var closed: [NextTaskIssue]
    public var commits: [String]
    public var docs: [NextTaskDoc]
    public var docIndex: [String]
    public var pings: Int
    public var hostsAttention: Int
    public var hour: Int

    public init(
        repo: String = "", live: [NextTaskWork] = [], recent: [NextTaskWork] = [], issues: [NextTaskIssue] = [],
        closed: [NextTaskIssue] = [], commits: [String] = [], docs: [NextTaskDoc] = [], docIndex: [String] = [],
        pings: Int = 0, hostsAttention: Int = 0, hour: Int = 12
    ) {
        self.repo = repo
        self.live = live
        self.recent = recent
        self.issues = issues
        self.closed = closed
        self.commits = commits
        self.docs = docs
        self.docIndex = docIndex
        self.pings = pings
        self.hostsAttention = hostsAttention
        self.hour = hour
    }

    public static func recent(_ groups: [AgentRepoGroup], limit: Int = 15) -> [NextTaskWork] {
        var all: [NextTaskWork] = []
        for group in groups {
            let repo = group.repo.fullName
            for session in group.sessions {
                let at = [session.mergedAtMs, session.finishedAtMs, session.startedAtMs, session.createdAtMs].max() ?? 0
                all.append(NextTaskWork(
                    title: session.displayTitle, repo: repo, status: session.statusLabel,
                    note: session.isFailed ? session.statusReasonText : "", atMs: at))
            }
            for launched in group.launched {
                let title = launched.title.dartTrimmed
                if title.isEmpty { continue }
                let status = launched.isMergedBranch
                    ? "Merged" : (launched.displayStatus.isEmpty ? launched.status : launched.displayStatus)
                let at = [launched.mergedAtMs, launched.completedAtMs, launched.updatedAtMs, launched.createdAtMs].max() ?? 0
                all.append(NextTaskWork(
                    title: title, repo: repo, status: status,
                    note: launched.isFailed ? launched.lastError : "", atMs: at))
            }
        }
        var seen: Set<String> = []
        return all.sorted { $0.atMs > $1.atMs }.filter { seen.insert(NextTaskSuggestion.key($0.title)).inserted }
            .prefix(limit).map { $0 }
    }

    public static func issues(_ list: [Issue], limit: Int = 10) -> [NextTaskIssue] {
        list.filter(isOpen)
            .sorted { ($0.priority, $0.votes, $0.number) > ($1.priority, $1.votes, $1.number) }
            .prefix(limit)
            .map(brief)
    }

    public static func closed(_ list: [Issue], limit: Int = 10) -> [NextTaskIssue] {
        list.filter { !isOpen($0) }.sorted { $0.number > $1.number }.prefix(limit).map(brief)
    }

    public static func commits(_ rows: [JSONObject], limit: Int = 30) -> [String] {
        var seen: Set<String> = []
        return rows.compactMap { (row: JSONObject) -> String? in
            let subject = (row.firstStr("subject", "message", "title").components(separatedBy: .newlines).first ?? "")
                .trimmingCharacters(in: .whitespaces)
            if subject.isEmpty || subject.lowercased().hasPrefix("merge ") { return nil }
            return NextTaskPrompt.clip(subject, 120)
        }
        .filter { seen.insert(NextTaskSuggestion.key($0)).inserted }
        .prefix(limit).map { $0 }
    }

    private static func isOpen(_ issue: Issue) -> Bool {
        let status = issue.status.lowercased()
        return status == "open" || status.isEmpty
    }

    private static func brief(_ issue: Issue) -> NextTaskIssue {
        NextTaskIssue(
            number: issue.number, title: issue.title, labels: issue.labels, priority: issue.priority, votes: issue.votes,
            summary: NextTaskRepoReader.excerpt(issue.body, 200).replacingOccurrences(of: "\n", with: " "))
    }
}

public struct NextTaskSuggestion: Equatable, Sendable {
    public var title: String
    public var task: String
    public var why: String

    public init(title: String, task: String, why: String = "") {
        self.title = title
        self.task = task
        self.why = why
    }

    public var key: String { NextTaskSuggestion.key(title) }

    static func key(_ text: String) -> String {
        text.lowercased().filter { $0.isLetter || $0.isNumber }
    }

    public func repeats(_ other: NextTaskSuggestion) -> Bool {
        if !key.isEmpty && key == other.key { return true }
        if NextTaskSuggestion.similar(NextTaskSuggestion.words(title), NextTaskSuggestion.words(other.title)) { return true }
        let mine = NextTaskSuggestion.words(task)
        let theirs = NextTaskSuggestion.words(other.task)
        return mine.count >= 4 && theirs.count >= 4 && NextTaskSuggestion.jaccard(mine, theirs) >= 0.5
    }

    static let stopWords: Set<String> = [
        "a", "an", "the", "and", "or", "for", "with", "from", "into", "onto", "that", "this", "these", "those", "when",
        "then", "than", "its", "their", "our", "your", "you", "all", "any", "each", "every", "only", "just", "also",
        "more", "new", "add", "implement", "write", "create", "make", "build", "fix", "update", "improve", "support",
        "use", "using", "focus", "core", "logic", "feature", "task", "code", "app", "via", "per", "not", "out", "off",
        "are", "was", "can", "should", "must", "about", "on", "in", "of", "to", "by", "at", "is", "it", "be", "as",
    ]

    static let suffixes: [String] = ["ations", "ation", "ings", "ing", "ions", "ion", "ers", "ors", "er", "or", "ed", "es", "s"]

    static func words(_ text: String) -> Set<String> {
        var out: Set<String> = []
        for raw in text.lowercased().split(whereSeparator: { !($0.isLetter || $0.isNumber) }) {
            let word = String(raw)
            if word.count < 3 || stopWords.contains(word) { continue }
            let stem = stem(word)
            if stopWords.contains(stem) { continue }
            out.insert(stem)
        }
        return out
    }

    static func stem(_ word: String) -> String {
        if word.hasSuffix("ss") { return word }
        for suffix in suffixes where word.hasSuffix(suffix) && word.count - suffix.count >= 3 {
            return String(word.dropLast(suffix.count))
        }
        return word
    }

    static func jaccard(_ a: Set<String>, _ b: Set<String>) -> Double {
        let union = a.union(b).count
        return union == 0 ? 0 : Double(a.intersection(b).count) / Double(union)
    }

    static func similar(_ a: Set<String>, _ b: Set<String>) -> Bool {
        if a.isEmpty || b.isEmpty { return false }
        let shared = a.intersection(b).count
        let small = min(a.count, b.count)
        if jaccard(a, b) >= 0.5 { return true }
        if shared >= 2 && Double(shared) / Double(small) >= 0.66 { return true }
        return small == 1 && shared == 1 && max(a.count, b.count) <= 2
    }
}

public struct NextTaskAvoid: Equatable, Sendable {
    public var suggestion: NextTaskSuggestion
    public var because: String

    public init(_ suggestion: NextTaskSuggestion, because: String) {
        self.suggestion = suggestion
        self.because = because
    }
}

public struct NextTaskLedger {
    public static let entriesKey = "home/next-task/ledger"
    public static let deniedKey = "home/next-task/denied"
    public static let lessonsKey = "home/next-task/lessons"
    public static let reflectedKey = "home/next-task/reflected-ms"
    public static let most = 40
    public static let mostDenied = 200
    public static let reflectEvery = 4

    public enum Verdict: String, Sendable {
        case started
        case skipped
        case denied
    }

    public enum Reason: String, CaseIterable, Identifiable, Sendable {
        case done
        case useless
        case vague
        case elsewhere
        case other

        public var id: String { rawValue }

        public var label: String {
            switch self {
            case .done: return "Already done"
            case .useless: return "Not useful"
            case .vague: return "Too vague"
            case .elsewhere: return "Wrong project"
            case .other: return "Just not this"
            }
        }

        public var note: String {
            switch self {
            case .done: return "already done"
            case .useless: return "not useful"
            case .vague: return "too vague"
            case .elsewhere: return "wrong project"
            case .other: return ""
            }
        }
    }

    public struct Entry: Equatable, Sendable {
        public var verdict: Verdict
        public var title: String
        public var task: String
        public var atMs: Int
        public var reason: String = ""
        public var repo: String = ""

        public var suggestion: NextTaskSuggestion { NextTaskSuggestion(title: title, task: task) }

        public var because: String { Reason(rawValue: reason)?.note ?? "" }
    }

    public let store: KeyValueStore?

    public init(store: KeyValueStore?) { self.store = store }

    public var entries: [Entry] { read(Self.entriesKey) }

    public var denied: [Entry] { read(Self.deniedKey) }

    public func record(
        _ verdict: Verdict, _ suggestion: NextTaskSuggestion, atMs: Int, reason: Reason? = nil, repo: String = ""
    ) {
        let clean: (String) -> String = { $0.replacingOccurrences(of: "\t", with: " ").replacingOccurrences(of: "\n", with: " ") }
        let line = [verdict.rawValue, String(atMs), clean(suggestion.title), clean(suggestion.task), reason?.rawValue ?? "", clean(repo)]
            .joined(separator: "\t")
        push(line, Self.entriesKey, Self.most)
        if verdict == .denied { push(line, Self.deniedKey, Self.mostDenied) }
    }

    public func titles(_ verdict: Verdict, limit: Int) -> [String] {
        unique(verdict, limit: limit).map(\.title)
    }

    public func unique(_ verdict: Verdict, limit: Int) -> [Entry] {
        var seen: Set<String> = []
        let source = verdict == .denied ? denied : entries.filter { $0.verdict == verdict }
        return source.filter { seen.insert(NextTaskSuggestion.key($0.title)).inserted }.prefix(limit).map { $0 }
    }

    public var skipStreak: Int {
        entries.prefix { $0.verdict != .started }.count
    }

    public var lessons: [String] { store?.stringList(forKey: Self.lessonsKey) ?? [] }

    public var reflectedMs: Int { store?.int(forKey: Self.reflectedKey) ?? 0 }

    public var sinceReflection: Int {
        entries.filter { $0.atMs > reflectedMs }.reduce(0) { $0 + ($1.verdict == .denied ? 2 : 1) }
    }

    public var dueForReflection: Bool { sinceReflection >= Self.reflectEvery }

    public func save(lessons: [String], atMs: Int) {
        store?.set(lessons, forKey: Self.lessonsKey)
        store?.set(atMs, forKey: Self.reflectedKey)
    }

    public func markReflected(atMs: Int) {
        store?.set(atMs, forKey: Self.reflectedKey)
    }

    private func read(_ key: String) -> [Entry] {
        (store?.stringList(forKey: key) ?? []).compactMap { (line: String) -> Entry? in
            let parts = line.components(separatedBy: "\t")
            guard parts.count >= 4, let verdict = Verdict(rawValue: parts[0]), let at = Int(parts[1]) else { return nil }
            return Entry(
                verdict: verdict, title: parts[2], task: parts[3], atMs: at,
                reason: parts.count > 4 ? parts[4] : "", repo: parts.count > 5 ? parts[5] : "")
        }
    }

    private func push(_ line: String, _ key: String, _ most: Int) {
        let kept = (store?.stringList(forKey: key) ?? []).prefix(most - 1)
        store?.set([line] + kept, forKey: key)
    }
}

public enum NextTaskPrompt {
    public static let maxTitle = 80
    public static let maxTask = 600
    public static let attempts = 3

    public static let system = """
        You plan work for a developer who starts AI coding agents from their phone in ForkMesh. \
        Suggest the single most useful next task to hand to a coding agent right now. \
        Read the repository's documentation, recent commits and issues to learn what the project is, what was just done and what is still missing. \
        Weigh failed or unfinished agent work, the open issues, and what the developer has started, skipped or rejected before. \
        Prefer a concrete task an agent can finish in one run in the named repository, and name the files or area it touches when you know them. \
        Recent commits, closed issues and finished agent work are done: never suggest redoing them. \
        Never suggest a task that is already running, started, skipped, rejected or already shown, or anything close to one; pick a different part of the project instead.
        Reply with exactly three lines and nothing else:
        TITLE: at most 8 words
        TASK: the instruction for the coding agent, 1 to 3 imperative sentences
        WHY: at most 15 words
        """

    public static let reflectSystem = """
        You keep short notes on which suggested tasks a developer likes. \
        Read their recent decisions and the old notes, then write at most 4 updated notes. \
        Rejected ideas and their reasons matter most. \
        Each note is one line starting with "- ", under 15 words, and states a general preference, not a single task.
        """

    public static func temperature(skipStreak: Int) -> Double {
        min(0.6 + 0.1 * Double(skipStreak), 1.2)
    }

    public static func messages(_ context: NextTaskContext, ledger: NextTaskLedger, shown: [String]) -> [LocalChatMessage] {
        [.system(system), .user(describe(context, ledger: ledger, shown: shown))]
    }

    public static func describe(_ context: NextTaskContext, ledger: NextTaskLedger, shown: [String]) -> String {
        var lines: [String] = []
        let part: String
        switch context.hour {
        case 5..<12: part = "morning"
        case 12..<17: part = "afternoon"
        case 17..<22: part = "evening"
        default: part = "night"
        }
        lines.append("Time: \(part)")
        lines.append("Repository: \(context.repo.isEmpty ? "none chosen" : context.repo)")
        if !context.docIndex.isEmpty { lines.append("Documentation files: \(context.docIndex.joined(separator: ", "))") }
        for doc in context.docs where !doc.text.isEmpty {
            lines.append("From \(doc.path):")
            lines.append(doc.text)
        }
        section(&lines, "Recent commits, newest first (done)", context.commits)
        section(&lines, "Agents running now", context.live.map(line))
        section(&lines, "Recent agent work, newest first", context.recent.map(line))
        section(&lines, "Open issues, most important first", context.issues.map(line))
        section(&lines, "Recently closed issues (done)", context.closed.map(line))
        var attention: [String] = []
        if context.pings > 0 { attention.append("\(context.pings) unread pings") }
        if context.hostsAttention > 0 { attention.append("\(context.hostsAttention) hosts need sign-in") }
        if !attention.isEmpty { lines.append("Needs attention: \(attention.joined(separator: ", "))") }
        section(&lines, "What this developer prefers (learned)", ledger.lessons)
        section(&lines, "Suggestions they started", ledger.titles(.started, limit: 8))
        section(&lines, "Ideas they rejected, never suggest these or anything like them", ledger.unique(.denied, limit: 15).map(line))
        section(&lines, "Suggestions they skipped", ledger.titles(.skipped, limit: 10))
        section(&lines, "Already shown", shown.suffix(8).map { $0 })
        lines.append("Suggest the next task.")
        return lines.joined(separator: "\n")
    }

    public static func avoid(_ context: NextTaskContext, ledger: NextTaskLedger, shown: [NextTaskSuggestion]) -> [NextTaskAvoid] {
        var out: [NextTaskAvoid] = []
        out += ledger.denied.prefix(60).map { NextTaskAvoid($0.suggestion, because: "the developer rejected") }
        out += ledger.unique(.started, limit: 20).map { NextTaskAvoid($0.suggestion, because: "the developer already started") }
        out += ledger.unique(.skipped, limit: 20).map { NextTaskAvoid($0.suggestion, because: "the developer skipped") }
        out += shown.map { NextTaskAvoid($0, because: "was already shown") }
        out += context.live.map { NextTaskAvoid(NextTaskSuggestion(title: $0.title, task: ""), because: "is running now") }
        out += context.recent.filter { !$0.status.lowercased().contains("fail") }
            .map { NextTaskAvoid(NextTaskSuggestion(title: $0.title, task: ""), because: "an agent already did") }
        out += context.closed.map { NextTaskAvoid(NextTaskSuggestion(title: $0.title, task: ""), because: "is a closed issue") }
        return out
    }

    public static func repeated(_ suggestion: NextTaskSuggestion, in avoid: [NextTaskAvoid]) -> NextTaskAvoid? {
        avoid.first { suggestion.repeats($0.suggestion) }
    }

    public static func retry(_ suggestion: NextTaskSuggestion, _ match: NextTaskAvoid) -> String {
        "\"\(suggestion.title)\" is too close to \"\(match.suggestion.title)\", which \(match.because). "
            + "Suggest a different task about another part of the project. Reply with the same three lines."
    }

    public static func fallback(_ context: NextTaskContext, avoid: [NextTaskAvoid]) -> NextTaskSuggestion? {
        for issue in context.issues {
            let candidate = NextTaskSuggestion(
                title: clip(issue.title, maxTitle),
                task: "Resolve issue #\(issue.number) \"\(issue.title)\". Read the issue first, make the change and add or update tests.",
                why: "Gemma kept repeating old ideas; this is the top open issue.")
            if repeated(candidate, in: avoid) == nil { return candidate }
        }
        return nil
    }

    public static func reflectMessages(_ ledger: NextTaskLedger) -> [LocalChatMessage] {
        var lines: [String] = []
        section(&lines, "Old notes", ledger.lessons)
        let recent = ledger.entries.prefix(20).map { (entry: NextTaskLedger.Entry) -> String in
            switch entry.verdict {
            case .started: return "started: \(entry.title)"
            case .skipped: return "skipped: \(entry.title)"
            case .denied: return entry.because.isEmpty ? "rejected: \(entry.title)" : "rejected (\(entry.because)): \(entry.title)"
            }
        }
        section(&lines, "Recent decisions, newest first", recent)
        lines.append("Write the updated notes.")
        return [.system(reflectSystem), .user(lines.joined(separator: "\n"))]
    }

    public static func parse(_ text: String) -> NextTaskSuggestion? {
        var title = ""
        var task = ""
        var why = ""
        var loose: [String] = []
        for raw in text.components(separatedBy: .newlines) {
            let line = strip(raw)
            if line.isEmpty { continue }
            if let value = field(line, "title") {
                title = value
            } else if let value = field(line, "task") {
                task = value
            } else if let value = field(line, "why") {
                why = value
            } else if !task.isEmpty && why.isEmpty {
                task += " " + line
            } else {
                loose.append(line)
            }
        }
        if task.isEmpty { task = loose.joined(separator: " ") }
        task = clip(task, maxTask)
        if task.isEmpty { return nil }
        if title.isEmpty { title = task.components(separatedBy: CharacterSet(charactersIn: ".!?")).first ?? task }
        return NextTaskSuggestion(title: clip(title, maxTitle), task: task, why: clip(why, maxTitle * 2))
    }

    public static func parseLessons(_ text: String) -> [String] {
        text.components(separatedBy: .newlines)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { $0.hasPrefix("-") || $0.hasPrefix("*") || $0.hasPrefix("•") || ($0.first?.isNumber ?? false) }
            .map { (line: String) -> String in strip(String(line.drop { (c: Character) -> Bool in c.isNumber || "-*•.) ".contains(c) })) }
            .filter { !$0.isEmpty }
            .prefix(4)
            .map { clip($0, 120) }
    }

    private static func section(_ lines: inout [String], _ title: String, _ items: [String]) {
        if items.isEmpty { return }
        lines.append("\(title):")
        lines.append(contentsOf: items.map { "- \($0)" })
    }

    private static func line(_ work: NextTaskWork) -> String {
        var out = work.title
        if !work.repo.isEmpty { out += " [\(work.repo)]" }
        if !work.status.isEmpty { out += " (\(work.status))" }
        let note = work.note.dartTrimmed
        if !note.isEmpty { out += ": " + clip(note.replacingOccurrences(of: "\n", with: " "), 140) }
        return out
    }

    private static func line(_ issue: NextTaskIssue) -> String {
        var extras: [String] = []
        if !issue.labels.isEmpty { extras.append(issue.labels.prefix(3).joined(separator: ", ")) }
        if issue.priority > 0 { extras.append("priority \(issue.priority)") }
        if issue.votes > 0 { extras.append("\(issue.votes) votes") }
        var out = "#\(issue.number) \(issue.title)" + (extras.isEmpty ? "" : " (\(extras.joined(separator: "; ")))")
        if !issue.summary.isEmpty { out += ": " + issue.summary }
        return out
    }

    private static func line(_ entry: NextTaskLedger.Entry) -> String {
        entry.because.isEmpty ? entry.title : "\(entry.title) (\(entry.because))"
    }

    private static func field(_ line: String, _ name: String) -> String? {
        let lower = line.lowercased()
        guard lower.hasPrefix(name) else { return nil }
        let rest = line.dropFirst(name.count).drop { $0 == " " || $0 == "*" }
        guard rest.first == ":" else { return nil }
        return strip(String(rest.dropFirst()))
    }

    private static func strip(_ text: String) -> String {
        text.trimmingCharacters(in: CharacterSet.whitespaces.union(CharacterSet(charactersIn: "*`\"“”#")))
    }

    static func clip(_ text: String, _ most: Int) -> String {
        let clean = text.trimmingCharacters(in: .whitespacesAndNewlines)
        return clean.count <= most ? clean : String(clean.prefix(most - 1)) + "…"
    }
}

@MainActor
@Observable
public final class NextTaskAdvisor {
    public enum Phase: Equatable, Sendable {
        case idle
        case thinking
        case ready(NextTaskSuggestion)
        case off(String)
        case failed(String)
    }

    public typealias Generate = @MainActor ([LocalChatMessage], LocalGenerationParameters) async throws -> String

    public private(set) var phase: Phase = .idle
    public private(set) var learning = false
    public let ledger: NextTaskLedger
    public let log: NextTaskTrainingLog?
    @ObservationIgnored private let readiness: @MainActor () -> String?
    @ObservationIgnored private let generate: Generate
    @ObservationIgnored private let now: () -> Int
    @ObservationIgnored private var shown: [NextTaskSuggestion] = []
    @ObservationIgnored private var asked: (prompt: [LocalChatMessage], repo: String) = ([], "")
    @ObservationIgnored private var run: Task<Void, Never>?
    @ObservationIgnored private var reflection: Task<Void, Never>?

    public init(
        prefs: KeyValueStore?, readiness: @escaping @MainActor () -> String?, generate: @escaping Generate,
        log: NextTaskTrainingLog? = nil, now: @escaping () -> Int = { Int(Date().timeIntervalSince1970 * 1000) }
    ) {
        self.ledger = NextTaskLedger(store: prefs)
        self.log = log
        self.readiness = readiness
        self.generate = generate
        self.now = now
    }

    public convenience init(
        prefs: KeyValueStore?, manager: @escaping @MainActor () -> LocalModelManager?, log: NextTaskTrainingLog? = nil
    ) {
        self.init(
            prefs: prefs,
            readiness: { NextTaskAdvisor.readiness(manager()) },
            generate: { (messages: [LocalChatMessage], parameters: LocalGenerationParameters) async throws -> String in
                guard let local = manager() else { throw LocalEngineError.unavailable("On-device AI is not in this build.") }
                try await local.ensureReady()
                return try await local.engine.complete(messages: messages, parameters: parameters).text
            }, log: log)
    }

    public static func readiness(_ manager: LocalModelManager?) -> String? {
        guard let manager else { return "On-device AI is not in this build." }
        if let reason = manager.engineAvailability.reason { return reason }
        if !manager.isEnabled { return "Turn on Gemma to get task ideas." }
        if !manager.isDownloaded(manager.selectedModel) { return "Download \(manager.selectedModel.displayName) to get task ideas." }
        return nil
    }

    public var suggestion: NextTaskSuggestion? {
        if case .ready(let suggestion) = phase { return suggestion }
        return nil
    }

    public var thinking: Bool { phase == .thinking }

    public func suggestIfNeeded(_ context: @escaping @MainActor () async -> NextTaskContext) {
        switch phase {
        case .ready, .thinking: return
        case .off: if readiness() != nil { return }
        case .idle, .failed: break
        }
        suggest(context)
    }

    public func refresh(_ context: @escaping @MainActor () async -> NextTaskContext) {
        if phase == .thinking { return }
        if let current = suggestion { decide(.skipped, current) }
        suggest(context)
    }

    public func deny(_ reason: NextTaskLedger.Reason, _ context: @escaping @MainActor () async -> NextTaskContext) {
        if phase == .thinking { return }
        guard let current = suggestion else { return }
        decide(.denied, current, reason: reason)
        suggest(context)
    }

    @discardableResult
    public func started() -> NextTaskSuggestion? {
        guard let current = suggestion else { return nil }
        decide(.started, current)
        phase = .idle
        reflectIfDue()
        return current
    }

    public func waitForIdle() async {
        await reflection?.value
        await run?.value
    }

    private func suggest(_ context: @escaping @MainActor () async -> NextTaskContext) {
        if let reason = readiness() {
            phase = .off(reason)
            return
        }
        phase = .thinking
        run?.cancel()
        reflectIfDue()
        let pending = reflection
        run = Task { @MainActor [weak self] in
            await pending?.value
            guard let self, !Task.isCancelled else { return }
            let gathered = await context()
            await self.produce(gathered)
        }
    }

    private func decide(_ verdict: NextTaskLedger.Verdict, _ suggestion: NextTaskSuggestion, reason: NextTaskLedger.Reason? = nil) {
        let at = now()
        ledger.record(verdict, suggestion, atMs: at, reason: reason, repo: asked.repo)
        log?.append(NextTaskFeedback(
            verdict: verdict.rawValue, reason: reason?.rawValue ?? "", title: suggestion.title, task: suggestion.task,
            why: suggestion.why, repo: asked.repo, atMs: at, messages: asked.prompt))
    }

    private func produce(_ context: NextTaskContext) async {
        var parameters = LocalGenerationParameters(maxTokens: 220, temperature: NextTaskPrompt.temperature(skipStreak: ledger.skipStreak))
        let avoid = NextTaskPrompt.avoid(context, ledger: ledger, shown: shown)
        let prompt = NextTaskPrompt.messages(context, ledger: ledger, shown: shown.map(\.title))
        var messages = prompt
        var outcome: Phase?
        var repeats = 0
        for _ in 0..<NextTaskPrompt.attempts {
            if Task.isCancelled { return }
            do {
                let text = try await generate(messages, parameters)
                guard let parsed = NextTaskPrompt.parse(text) else { continue }
                guard let match = NextTaskPrompt.repeated(parsed, in: avoid) else {
                    outcome = .ready(parsed)
                    break
                }
                repeats += 1
                messages += [.assistant(text), .user(NextTaskPrompt.retry(parsed, match))]
                parameters.temperature = min(parameters.temperature + 0.2, 1.3)
            } catch is CancellationError {
                return
            } catch {
                outcome = .failed(LocalModelManager.describe(error))
                break
            }
        }
        if Task.isCancelled { return }
        if outcome == nil && repeats > 0 {
            outcome = NextTaskPrompt.fallback(context, avoid: avoid).map { Phase.ready($0) }
                ?? .failed("Gemma only came up with ideas you already passed on. Tap New idea to try again.")
        }
        let final = outcome ?? .failed("Gemma did not suggest a task. Try again.")
        if case .ready(let parsed) = final {
            shown.append(parsed)
            asked = (prompt, context.repo)
        }
        phase = final
    }

    private func reflectIfDue() {
        guard reflection == nil, ledger.dueForReflection, readiness() == nil else { return }
        learning = true
        reflection = Task { @MainActor [weak self] in
            guard let self else { return }
            let at = self.now()
            do {
                let text = try await self.generate(
                    NextTaskPrompt.reflectMessages(self.ledger), LocalGenerationParameters(maxTokens: 160, temperature: 0.3))
                let lessons = NextTaskPrompt.parseLessons(text)
                if lessons.isEmpty {
                    self.ledger.markReflected(atMs: at)
                } else {
                    self.ledger.save(lessons: lessons, atMs: at)
                }
            } catch {
                self.ledger.markReflected(atMs: at)
            }
            self.learning = false
            self.reflection = nil
        }
    }
}
