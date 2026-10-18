import FMBase
import FMChat
import FMControl
import Foundation
import Observation

public struct ShortcutStep: Equatable, Sendable, Identifiable {
    public static let maxSteps = 32

    public var number: Int
    public var name: String
    public var shortcut: String

    public var id: Int { number }

    public init(number: Int, name: String, shortcut: String = "") {
        self.number = number
        self.name = name
        self.shortcut = shortcut
    }

    public static func list(_ value: JSONValue?) -> [ShortcutStep] {
        var steps: [ShortcutStep] = []
        for item in value?.arrayOrEmpty ?? [] {
            guard steps.count < maxSteps else { break }
            let name: String
            let shortcut: String
            if let text = item.stringValue {
                name = clean(text)
                shortcut = ""
            } else if let object = item.objectValue {
                shortcut = clean(object["shortcut"]?.stringValue ?? "")
                let named = clean(object["name"]?.stringValue ?? "")
                name = named.isEmpty ? shortcut : named
            } else {
                continue
            }
            guard !name.isEmpty else { continue }
            steps.append(ShortcutStep(number: steps.count + 1, name: name, shortcut: shortcut))
        }
        return steps
    }

    static func clean(_ text: String) -> String {
        text.split(whereSeparator: \.isNewline).joined(separator: " ").trimmingCharacters(in: .whitespaces)
    }

    public static func countText(_ count: Int) -> String? {
        count > 0 ? (count == 1 ? "1 step" : "\(count) steps") : nil
    }

    public func accessibilityLabel(of total: Int, opens: Bool) -> String {
        "Step \(number) of \(total), \(name)" + (opens ? ", opens shortcut" : "")
    }
}

public struct ShortcutEntry: Equatable, Sendable, Identifiable {
    public var file: String
    public var scope: String
    public var name: String
    public var description: String
    public var kind: String
    public var agent: String
    public var model: String
    public var uses: Int
    public var runnable: Bool
    public var running: Bool
    public var lastOutput: String
    public var lastOk: Bool
    public var finished: Int
    public var lastMs: Int64 = -1
    public var expectedMs: Int64 = -1
    public var startedAtMs: Int64 = 0
    public var queued = 0
    public var onPhone = false
    public var steps: [ShortcutStep] = []

    public var id: String { scope + "|" + file }

    public init(
        file: String, scope: String, name: String, description: String = "", kind: String, agent: String = "",
        model: String = ""
    ) {
        self.file = file
        self.scope = scope
        self.name = name
        self.description = description
        self.kind = kind
        self.agent = agent
        self.model = model
        uses = 0
        runnable = false
        running = false
        lastOutput = ""
        lastOk = false
        finished = 0
    }

    public init(json: JSONObject, nowMs: Int64 = ShortcutProgress.nowMs()) {
        file = json.str("file")
        scope = json.str("scope")
        name = json.str("name")
        description = json.str("description")
        kind = json.str("kind")
        agent = json.str("agent")
        model = json.str("model")
        uses = json.int("uses")
        runnable = json.bool("runnable")
        running = json.bool("running")
        lastOutput = json.str("lastOutput")
        lastOk = json.bool("lastOk")
        finished = json.int("finished")
        queued = json.int("queued")
        lastMs = json.int64("lastMs", -1)
        expectedMs = json.int64("expectedMs", lastMs)
        let elapsed = json.int64("elapsedMs", -1)
        startedAtMs = running && elapsed >= 0 ? nowMs - elapsed : 0
        steps = ShortcutStep.list(json["steps"])
    }

    public var stepsText: String? { ShortcutStep.countText(steps.count) }

    public var isSkill: Bool { scope != "shortcut" }
    public var isScript: Bool { kind == "script" }

    public var queueable: Bool { isScript && runnable && !onPhone }
}

public struct ShortcutCatalog: Equatable, Sendable {
    public var repo: String
    public var repos: [String]
    public var entries: [ShortcutEntry]
    public var onPhone = false

    public init(json: JSONObject) {
        repo = json.str("repo")
        repos = json.strings("repos")
        entries = json.maps("entries").map { ShortcutEntry(json: $0) }
    }

    public init(repo: String, repos: [String], entries: [ShortcutEntry], onPhone: Bool = false) {
        self.repo = repo
        self.repos = repos
        self.entries = entries
        self.onPhone = onPhone
    }

    public var shortcuts: [ShortcutEntry] { entries.filter { !$0.isSkill }.sorted { $0.uses > $1.uses } }

    public var scriptRunning: Bool { !onPhone && entries.contains { $0.isScript && $0.running } }

    public var skills: [ShortcutEntry] { entries.filter(\.isSkill).sorted { $0.uses > $1.uses } }

    public func linked(_ step: ShortcutStep, from entry: ShortcutEntry) -> ShortcutEntry? {
        guard !step.shortcut.isEmpty, step.shortcut != entry.file else { return nil }
        return entries.first { $0.scope == entry.scope && $0.file == step.shortcut }
    }

    public func pick(preferred: String) -> String {
        repos.first { $0.caseInsensitiveCompare(preferred) == .orderedSame } ?? repos.first ?? preferred
    }
}

public struct ShortcutRun: Equatable, Sendable {
    public static let maxOutput = 96 * 1024

    public var id: Int
    public var repo: String
    public var file: String
    public var name: String
    public var running: Bool
    public var ok: Bool
    public var message: String
    public var output: String
    public var expectedMs: Int64
    public var startedAtMs: Int64

    public init(json: JSONObject, nowMs: Int64 = ShortcutProgress.nowMs()) {
        id = json.int("id")
        repo = json.str("repo")
        file = json.str("file")
        name = json.str("name")
        running = json.bool("running")
        ok = json.bool("ok")
        message = json.str("message")
        output = Self.tail(json.str("output"))
        expectedMs = json.int64("expectedMs", -1)
        let startedAt = json.int64("startedAtMs")
        let elapsed = json.int64("elapsedMs", -1)
        startedAtMs = running && startedAt > 0 ? startedAt : (running && elapsed >= 0 ? nowMs - elapsed : 0)
    }

    public func progress(nowMs: Int64 = ShortcutProgress.nowMs()) -> ShortcutProgress? {
        guard running, startedAtMs > 0 else { return nil }
        return ShortcutProgress(elapsedMs: nowMs - startedAtMs, expectedMs: expectedMs)
    }

    public var title: String { name.isEmpty ? file : name }

    public func matches(repo: String, file: String) -> Bool {
        self.file == file && self.repo.caseInsensitiveCompare(repo) == .orderedSame
    }

    public mutating func absorb(_ event: JSONObject, nowMs: Int64 = ShortcutProgress.nowMs()) {
        if startedAtMs == 0, event.bool("running") {
            let elapsed = event.int64("elapsedMs", -1)
            if elapsed >= 0 { startedAtMs = nowMs - elapsed }
            expectedMs = event.int64("expectedMs", expectedMs)
        }
        running = event.bool("running")
        ok = event.bool("ok")
        message = event.str("message")
        output = Self.tail(output + event.str("chunk"))
    }

    public var lastLine: String { Self.lastLine(output) }

    public static func lastLine(_ text: String, maxChars: Int = 200) -> String {
        for raw in text.split(separator: "\n", omittingEmptySubsequences: false).reversed() {
            let frame = raw.split(separator: "\r", omittingEmptySubsequences: false).last.map(String.init) ?? ""
            let line = frame.replacingOccurrences(of: "\u{1B}\\[[0-9;?]*[A-Za-z]", with: "", options: .regularExpression)
                .trimmingCharacters(in: .whitespaces)
            if !line.isEmpty { return String(line.prefix(maxChars)) }
        }
        return ""
    }

    public static func tail(_ text: String) -> String {
        guard text.count > maxOutput else { return text }
        let cut = text.suffix(maxOutput)
        guard let newline = cut.firstIndex(of: "\n"), cut.index(after: newline) < cut.endIndex else {
            return String(cut)
        }
        return String(cut[cut.index(after: newline)...])
    }
}

public struct ShortcutRowStatus: Equatable, Sendable {
    public var running: Bool
    public var ok: Bool
    public var line: String
    public var ran = true
    public var lastMs: Int64 = -1
    public var startedAtMs: Int64 = 0
    public var expectedMs: Int64 = -1

    public var tookText: String? {
        !running && lastMs >= 0 ? "took \(ShortcutProgress.duration(lastMs))" : nil
    }

    public func progress(nowMs: Int64 = ShortcutProgress.nowMs()) -> ShortcutProgress? {
        guard running, startedAtMs > 0 else { return nil }
        return ShortcutProgress(elapsedMs: nowMs - startedAtMs, expectedMs: expectedMs)
    }
}

public struct ShortcutProgress: Equatable, Sendable {
    public var fraction: Double?
    public var text: String

    public init(elapsedMs: Int64, expectedMs: Int64) {
        let elapsed = max(0, elapsedMs)
        if expectedMs <= 0 {
            fraction = nil
            text = "Running for \(Self.duration(elapsed)) · no earlier run to estimate from"
        } else if elapsed < expectedMs {
            fraction = min(0.99, Double(elapsed) / Double(expectedMs))
            text = "\(Self.duration(elapsed)) of about \(Self.duration(expectedMs)) · \(Self.duration(expectedMs - elapsed)) left"
        } else {
            fraction = 0.99
            text = "\(Self.duration(elapsed)) · \(Self.duration(elapsed - expectedMs)) longer than the last run"
        }
    }

    public static func duration(_ ms: Int64) -> String {
        let total = max(0, ms / 1000)
        let hours = total / 3600
        let minutes = total % 3600 / 60
        let seconds = total % 60
        let pad = { (value: Int64) in value < 10 ? "0\(value)" : "\(value)" }
        if hours > 0 { return "\(hours)h \(pad(minutes))m" }
        if minutes > 0 { return "\(minutes)m \(pad(seconds))s" }
        return "\(seconds)s"
    }

    public static func nowMs() -> Int64 { Int64(Date().timeIntervalSince1970 * 1000) }
}

@MainActor
public protocol ShortcutMirror: AnyObject {
    func mirroredRepos() -> [String]
    func worktree(repo: String) -> URL?
}

public enum ShortcutAction {
    public static func run(repo: String, scope: String, file: String) -> JSONObject {
        named("runShortcut", repo: repo, scope: scope, file: file)
    }

    public static func queue(repo: String, scope: String, file: String) -> JSONObject {
        named("queueShortcut", repo: repo, scope: scope, file: file)
    }

    public static func unqueue(repo: String, scope: String, file: String) -> JSONObject {
        named("unqueueShortcut", repo: repo, scope: scope, file: file)
    }

    static func named(_ action: String, repo: String, scope: String, file: String) -> JSONObject {
        ["action": .string(action), "repo": .string(repo), "scope": .string(scope), "file": .string(file)]
    }
}

@MainActor
@Observable
public final class ShortcutsStore {
    @ObservationIgnored public let family: DesktopControlFamily
    @ObservationIgnored private var tapId: UUID?
    @ObservationIgnored private var shownRepo = ""
    @ObservationIgnored public var mirror: ShortcutMirror?

    public private(set) var catalog: ShortcutCatalog?
    public private(set) var runs: [Int: ShortcutRun] = [:]
    public var shownRunId: Int?
    public private(set) var error = ""
    public private(set) var unsupported = false
    public private(set) var loading = false

    public init(channel: DesktopControlCalling) {
        family = DesktopControlFamily(.insights, channel: channel)
        tapId = family.addEventTap { [weak self] suffix, data in
            guard let self else { return }
            if suffix == "shortcutOutput" {
                self.absorb(data)
                return
            }
            guard suffix == "changed", !self.shownRepo.isEmpty else { return }
            Task { await self.refresh(repo: self.shownRepo) }
        }
    }

    @discardableResult
    public func load(preferred: String) async -> String {
        let target = catalog.flatMap { $0.onPhone ? nil : $0.pick(preferred: preferred) } ?? preferred
        await refresh(repo: target)
        let picked = catalog?.pick(preferred: target) ?? target
        if picked != target { await refresh(repo: picked) }
        return picked
    }

    public func refresh(repo: String) async {
        shownRepo = repo
        loading = true
        defer { loading = false }
        if catalog == nil || catalog?.onPhone == true {
            await loadPhone(repo: repo, exact: true)
        }
        do {
            let value = ShortcutCatalog(json: try await family.call("shortcuts", ["repo": .string(repo)], timeout: 25))
            guard repo == shownRepo else { return }
            catalog = value
            error = ""
            unsupported = false
        } catch {
            guard repo == shownRepo else { return }
            unsupported = DesktopControlFamily.isUnsupported(error)
            self.error = TeamErrorText.describe(error)
            if catalog?.onPhone != true || catalog?.repo.caseInsensitiveCompare(repo) != .orderedSame {
                await loadPhone(repo: repo, exact: false)
            }
            return
        }
        await loadRun()
    }

    public func loadPhone(repo: String, exact: Bool) async {
        guard let mirror else { return }
        let repos = mirror.mirroredRepos()
        let match = repos.first { $0.caseInsensitiveCompare(repo) == .orderedSame }
        guard let picked = exact ? match : (match ?? repos.first) else { return }
        let worktree = mirror.worktree(repo: picked)
        let value = await Task.detached {
            PhoneShortcuts.catalog(repo: picked, repos: repos, worktree: worktree)
        }.value
        guard repo == shownRepo else { return }
        catalog = value
    }

    public func phonePrompt(repo: String, entry: ShortcutEntry) async throws -> String {
        let prompt = PhoneShortcuts.prompt(try await text(repo: repo, entry: entry))
        if prompt.isEmpty { throw LocalControlException("empty_shortcut") }
        return prompt
    }

    public var lastRun: ShortcutRun? { runs.values.max { $0.id < $1.id } }

    public var shownRun: ShortcutRun? { shownRunId.flatMap { runs[$0] } ?? lastRun }

    public var newestRuns: [ShortcutRun] { runs.values.sorted { $0.id > $1.id } }

    public var anyRunning: Bool { runs.values.contains(where: \.running) }

    public func loadRun(id: Int? = nil) async {
        let params: JSONObject = id.map { ["id": .int(Int64($0))] } ?? [:]
        guard let answer = try? await family.call("shortcutRun", params) else { return }
        if id == nil, let rows = answer["runs"]?.arrayValue {
            var listed: [Int: ShortcutRun] = [:]
            for row in rows.compactMap(\.objectValue) {
                var run = ShortcutRun(json: row)
                guard run.id > 0 else { continue }
                if let known = runs[run.id], known.matches(repo: run.repo, file: run.file) { run.output = known.output }
                listed[run.id] = run
            }
            runs = listed
        }
        guard answer["run"] != nil else { return }
        let run = ShortcutRun(json: answer.obj("run"))
        guard run.id > 0, id == nil || run.id == id else { return }
        runs[run.id] = run
    }

    public func absorb(_ event: JSONObject) {
        let id = event.int("id")
        guard id > 0 else { return }
        if var current = runs[id] {
            current.absorb(event)
            runs[id] = current
            return
        }
        var fresh = ShortcutRun(json: event)
        fresh.absorb(event)
        runs[id] = fresh
        for stale in runs.keys.sorted().dropLast(Self.keptRuns) { runs[stale] = nil }
        Task { await loadRun(id: id) }
    }

    static let keptRuns = 20

    public func liveRun(repo: String, entry: ShortcutEntry) -> ShortcutRun? {
        guard !entry.isSkill else { return nil }
        return newestRuns.first { $0.matches(repo: repo, file: entry.file) }
    }

    public func isRunning(repo: String, file: String) -> Bool {
        runs.values.contains { $0.running && $0.matches(repo: repo, file: file) }
    }

    public func status(repo: String, entry: ShortcutEntry) -> ShortcutRowStatus {
        if let run = liveRun(repo: repo, entry: entry), run.running || entry.running {
            return ShortcutRowStatus(
                running: run.running, ok: run.ok, line: run.lastLine, lastMs: entry.lastMs,
                startedAtMs: run.startedAtMs > 0 ? run.startedAtMs : entry.startedAtMs,
                expectedMs: run.startedAtMs > 0 ? run.expectedMs : entry.expectedMs)
        }
        if entry.running {
            return ShortcutRowStatus(
                running: true, ok: false, line: entry.lastOutput, lastMs: entry.lastMs,
                startedAtMs: entry.startedAtMs, expectedMs: entry.expectedMs)
        }
        if entry.lastOutput.isEmpty && entry.finished == 0 {
            return ShortcutRowStatus(running: false, ok: false, line: "", ran: false, lastMs: entry.lastMs)
        }
        return ShortcutRowStatus(running: false, ok: entry.lastOk, line: entry.lastOutput, lastMs: entry.lastMs)
    }

    public func text(repo: String, entry: ShortcutEntry) async throws -> String {
        if entry.onPhone {
            guard let worktree = mirror?.worktree(repo: repo) else { throw LocalControlException("not_mirrored") }
            let read = await Task.detached { PhoneShortcuts.text(worktree: worktree, entry: entry) }.value
            guard let read else { throw LocalControlException("unknown_shortcut") }
            return read
        }
        return try await family.call(
            "shortcut", ["repo": .string(repo), "scope": .string(entry.scope), "file": .string(entry.file)]
        ).str("text")
    }

    public func prepare(repo: String, entry: ShortcutEntry) async throws -> TeamConfirmation {
        let params = ShortcutAction.run(repo: repo, scope: entry.scope, file: entry.file)
        guard let confirmation = TeamConfirmation(json: try await family.call("prepare", params), params: params) else {
            throw LocalControlException("bad_answer")
        }
        return confirmation
    }

    @discardableResult
    public func run(_ confirmation: TeamConfirmation) async throws -> String {
        try await family.call("run", confirmation.runParams, timeout: 45).str("message")
    }

    public func repoBusy(repo: String) -> Bool {
        catalog?.scriptRunning == true || runs.values.contains { $0.running && $0.repo.caseInsensitiveCompare(repo) == .orderedSame }
    }

    @discardableResult
    public func runNow(repo: String, entry: ShortcutEntry) async throws -> String {
        try await perform(ShortcutAction.run(repo: repo, scope: entry.scope, file: entry.file), repo: repo)
    }

    @discardableResult
    public func queue(repo: String, entry: ShortcutEntry) async throws -> String {
        try await perform(ShortcutAction.queue(repo: repo, scope: entry.scope, file: entry.file), repo: repo)
    }

    @discardableResult
    public func unqueue(repo: String, entry: ShortcutEntry) async throws -> String {
        try await perform(ShortcutAction.unqueue(repo: repo, scope: entry.scope, file: entry.file), repo: repo)
    }

    private func perform(_ params: JSONObject, repo: String) async throws -> String {
        guard let confirmation = TeamConfirmation(json: try await family.call("prepare", params), params: params) else {
            throw LocalControlException("bad_answer")
        }
        let message = try await run(confirmation)
        await refresh(repo: repo)
        return message
    }

    public func stop() {
        if let tapId { family.removeEventTap(tapId) }
        tapId = nil
    }
}
