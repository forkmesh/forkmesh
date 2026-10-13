import FMBase
import FMLocalAI
import FMNet
import Foundation
@testable import FMAgents

let testAuthor = AgentAuthor(name: "Alice Mobile", email: "alice@example.com")

/// A small Git in memory: branches point at snapshots, with an index and a
/// working tree, enough for the agent's tools and branch isolation.
/// `GitAgentWorkspaceTests` runs against a real repository with FMCode's engine.
@MainActor
final class MemoryWorkspace: AgentWorkspace {
    struct Commit {
        let id: String
        let message: String
        let files: [String: String]
    }

    private(set) var working: [String: String]
    private(set) var index: [String: String]
    private(set) var branchTips: [String: Int] = [:]
    private(set) var commits: [Commit] = []
    private(set) var current: String
    var failStatus: AgentWorkspaceError?

    init(files: [String: String], branch: String = "main") {
        working = files
        index = files
        current = branch
        commits.append(Commit(id: MemoryWorkspace.sha(0), message: "Initial commit\n", files: files))
        branchTips[branch] = 0
    }

    static func sha(_ n: Int) -> String {
        String(format: "%040x", n + 0xabc0000)
    }

    var head: [String: String] { commits[branchTips[current] ?? 0].files }

    /// Newest first.
    var log: [Commit] {
        var out: [Commit] = []
        var index = branchTips[current] ?? 0
        while index >= 0 {
            out.append(commits[index])
            index -= 1
        }
        return out
    }

    private func check(_ path: String) throws {
        if path.hasPrefix("/") || path.split(separator: "/").contains("..") {
            throw AgentWorkspaceError("\(path) is outside the working tree.")
        }
        if path == ".git" || path.hasPrefix(".git/") {
            throw AgentWorkspaceError("\(path) is inside .git.")
        }
    }

    func listFiles(_ directory: String) async throws -> [AgentFileEntry] {
        let prefix = directory.isEmpty ? "" : directory + "/"
        var entries: [String: AgentFileEntry] = [:]
        for (path, text) in working where path.hasPrefix(prefix) {
            let rest = String(path.dropFirst(prefix.count))
            let parts = rest.split(separator: "/", maxSplits: 1).map(String.init)
            if parts.count == 2 {
                entries[parts[0]] = AgentFileEntry(name: parts[0], path: prefix + parts[0], kind: .directory)
            } else {
                entries[rest] = AgentFileEntry(
                    name: rest, path: path, kind: .file, size: text.utf8.count, tracked: index[path] != nil
                )
            }
        }
        return Array(entries.values)
    }

    func readFile(_ path: String) async throws -> AgentFileContent {
        try check(path)
        guard let text = working[path] else { throw AgentWorkspaceError("\(path) does not exist.") }
        if text.contains("\u{0}") {
            return AgentFileContent(path: path, bytes: text.utf8.count, text: "", isBinary: true)
        }
        return AgentFileContent(path: path, bytes: text.utf8.count, text: text)
    }

    func writeFile(_ path: String, _ text: String) async throws {
        try check(path)
        working[path] = text
    }

    func status() async throws -> AgentWorktreeStatus {
        if let failStatus { throw failStatus }
        let head = self.head
        var staged: [AgentChange] = []
        var unstaged: [AgentChange] = []
        var untracked: [AgentChange] = []
        for path in Set(head.keys).union(index.keys).sorted() {
            switch (head[path], index[path]) {
            case (nil, .some): staged.append(AgentChange(path: path, kind: .added))
            case (.some, nil): staged.append(AgentChange(path: path, kind: .deleted))
            case (.some(let a), .some(let b)) where a != b: staged.append(AgentChange(path: path, kind: .modified))
            default: break
            }
        }
        for path in Set(index.keys).union(working.keys).sorted() {
            switch (index[path], working[path]) {
            case (nil, .some): untracked.append(AgentChange(path: path, kind: .untracked))
            case (.some, nil): unstaged.append(AgentChange(path: path, kind: .deleted))
            case (.some(let a), .some(let b)) where a != b: unstaged.append(AgentChange(path: path, kind: .modified))
            default: break
            }
        }
        return AgentWorktreeStatus(staged: staged, unstaged: unstaged, untracked: untracked)
    }

    func stageAll() async throws {
        index = working
    }

    func commit(message: String, author: AgentAuthor) async throws -> String {
        let id = MemoryWorkspace.sha(commits.count)
        commits.append(Commit(id: id, message: message, files: index))
        branchTips[current] = commits.count - 1
        return id
    }

    func branches() async throws -> [AgentBranch] {
        branchTips.keys.sorted().map { AgentBranch(name: $0, isCurrent: $0 == current) }
    }

    func createBranch(_ name: String, checkout: Bool) async throws {
        if branchTips[name] != nil { throw AgentWorkspaceError("A branch named \(name) already exists.") }
        branchTips[name] = branchTips[current] ?? 0
        if checkout { current = name }
    }

    func checkout(_ name: String) async throws {
        guard let tip = branchTips[name] else { throw AgentWorkspaceError("No branch named \(name).") }
        current = name
        working = commits[tip].files
        index = commits[tip].files
    }
}

/// A provider that replays a script, so loop behaviour can be tested without
/// a network or a model.
@MainActor
final class ScriptedProvider: AgentProvider {
    var script: [AgentTurn]
    var sentHistories: [[JSONValue]] = []
    var calls = 0
    var failures: [AgentProviderException] = []

    init(_ script: [AgentTurn]) {
        self.script = script
    }

    var id: String { "scripted" }
    var model: String { "scripted-1" }

    func send(systemPrompt: String, history: [JSONValue], tools: [AgentTool], client: HTTPClient) async throws -> AgentTurn {
        sentHistories.append(history)
        if !failures.isEmpty { throw failures.removeFirst() }
        if calls >= script.count {
            // Past the script the model simply stops asking for tools.
            return AgentTurn(text: "done", toolCalls: [])
        }
        defer { calls += 1 }
        return script[calls]
    }

    func appendAssistant(_ history: inout [JSONValue], _ turn: AgentTurn) {
        history.append(["role": "assistant", "turn": .string(turn.text)])
    }

    func appendToolResults(_ history: inout [JSONValue], _ results: [AgentToolResult]) {
        history.append(["role": "tool", "results": .array(results.map { .string($0.output) })])
    }

    func appendUser(_ history: inout [JSONValue], _ text: String) {
        history.append(["role": "user", "content": .string(text)])
    }
}

func toolTurn(_ name: String, _ args: JSONObject = [:], id: String = "t1") -> AgentTurn {
    AgentTurn(text: "", toolCalls: [AgentToolCall(id: id, name: name, arguments: args)])
}

/// A fresh directory under the system temp dir, removed by `cleanUp`.
final class TempDirectory {
    let url: URL

    init() {
        url = FileManager.default.temporaryDirectory
            .appendingPathComponent("fmagents-\(UUID().uuidString)", isDirectory: true)
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    }

    func cleanUp() {
        try? FileManager.default.removeItem(at: url)
    }
}

let tinyModel = LocalAIModel(
    id: "tiny-it-4bit",
    repo: "test-org/tiny-it-4bit",
    revision: "abc123",
    displayName: "Tiny",
    summary: "A test model.",
    approxDownloadBytes: 30,
    approxMemoryBytes: 1_000,
    minimumPhysicalMemory: 1_000_000,
    recommendedPhysicalMemory: 2_000_000,
    contextLength: 4096
)

let bigDevice = DeviceProfile(physicalMemory: 12_000_000_000)

/// A manager over `engine` with `tinyModel` downloaded (a hand-written
/// manifest) and the switch set to `enabled`.
@MainActor
func localModels(_ engine: FakeLocalChatEngine, in temp: TempDirectory, downloaded: Bool = true, enabled: Bool = true) throws -> LocalModelManager {
    let store = LocalModelStore(root: temp.url)
    if downloaded {
        let directory = store.directory(for: tinyModel)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try Data("{}".utf8).write(to: directory.appendingPathComponent("config.json"))
        let manifest = SnapshotManifest(
            repo: tinyModel.repo, revision: tinyModel.revision, files: [HubFile(path: "config.json", size: 2)], completedAtMs: 0
        )
        try Data(JSON.encode(.object(manifest.json)).utf8).write(to: store.manifestURL(for: tinyModel))
    }
    let downloader = HuggingFaceSnapshotDownloader(http: FakeHTTPClient(), files: FakeFileDownloader(), store: store)
    let manager = LocalModelManager(
        engine: engine, store: store, downloader: downloader, prefs: InMemoryKeyValueStore(), device: bigDevice, catalog: [tinyModel]
    )
    manager.setEnabled(enabled)
    manager.refresh()
    return manager
}

/// A mutable cell a listener closure can write to.
final class Box<Value>: @unchecked Sendable {
    var value: Value
    init(_ value: Value) { self.value = value }
}
