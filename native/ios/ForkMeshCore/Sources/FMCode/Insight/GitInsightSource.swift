import FMCodeInsight
import FMDiff
import FMGit
import Foundation

public final class GitInsightSource: InsightSource, @unchecked Sendable {
    public let name: String
    public let description: String
    public let head: String
    public let branch: String
    public let tip: GitObjectId?
    public let workingRoot: URL?
    public var git: Bool { true }

    let repository: GitRepository
    let now: @Sendable () -> Date
    private let lock = NSLock()
    private var blobs: [String: GitObjectId] = [:]
    private var listed = false

    public init(
        repository: GitRepository,
        name: String,
        description: String,
        tip: GitObjectId?,
        branch: String,
        workingRoot: URL? = nil,
        now: @escaping @Sendable () -> Date = { Date() }
    ) {
        self.repository = repository
        self.name = name
        self.description = description
        self.tip = tip
        self.head = tip?.hex ?? ""
        self.branch = branch
        self.workingRoot = workingRoot
        self.now = now
    }

    public static func open(
        worktree: URL,
        name: String,
        description: String,
        tipRef: String? = nil,
        readsWorkingFiles: Bool,
        limits: GitResourceLimits? = nil,
        now: @escaping @Sendable () -> Date = { Date() }
    ) throws -> GitInsightSource {
        let repository = try GitRepository.open(GitRepositoryOpen(worktree.path, limits: limits))
        var tip: GitObjectId?
        if let tipRef, let ref = try? GitRefName.parse(tipRef) {
            tip = try? RefStore(repository.gitDirectory).readDirect(ref)
        }
        if tip == nil { tip = try repository.readHeadObjectId() }
        var branch = "HEAD"
        if let target = try? repository.readHead().target {
            branch = target.hasPrefix("refs/heads/") ? String(target.dropFirst("refs/heads/".count)) : target
        }
        return GitInsightSource(
            repository: repository, name: name, description: description, tip: tip, branch: branch,
            workingRoot: readsWorkingFiles ? worktree : nil, now: now)
    }

    public static func analysable(_ path: String) -> Bool {
        !Insight.languageForPath(path).isEmpty && !Insight.isVendoredPath(path)
    }

    public func entries() throws -> [InsightEntry] {
        lock.lock()
        defer { lock.unlock() }
        return try listLocked()
    }

    private func listLocked() throws -> [InsightEntry] {
        var found: [String: GitObjectId] = [:]
        var result: [InsightEntry] = []
        if let tip {
            let commit = try repository.readCommit(tip)
            try walkTree(commit.tree, prefix: "") { path, entry in
                found[path] = entry.objectId
                result.append(InsightEntry(path: path, size: self.workingSize(path)))
            }
        }
        blobs = found
        listed = true
        return result
    }

    public func read(_ path: String, limit: Int64) throws -> [UInt8]? {
        if let workingRoot {
            let url = workingRoot.appendingPathComponent(path)
            if let attributes = try? FileManager.default.attributesOfItem(atPath: url.path),
               (attributes[.type] as? FileAttributeType) == .typeRegular {
                let size = (attributes[.size] as? NSNumber)?.int64Value ?? 0
                if size > limit { return nil }
                if let data = FileManager.default.contents(atPath: url.path) { return [UInt8](data) }
            }
        }
        lock.lock()
        defer { lock.unlock() }
        if !listed { _ = try listLocked() }
        guard let id = blobs[path] else { return nil }
        let bytes = try repository.readBlob(id)
        if Int64(bytes.count) > limit { return nil }
        return bytes
    }

    public func history(_ options: InsightOptions, cancel: @escaping () -> Bool) throws -> [InsightCommit]? {
        guard let tip else { return [] }
        lock.lock()
        defer { lock.unlock() }
        let cutoff = Int64(now().timeIntervalSince1970) - Int64(options.historyDays) * 86400
        let shallow = (try? repository.shallowCommits()) ?? []
        var queue = CommitQueue()
        var seen = Set<GitObjectId>()
        var output: [InsightCommit] = []
        seen.insert(tip)
        queue.push(tip, try repository.readCommit(tip))
        while output.count < options.maxCommits, let next = queue.pop() {
            if cancel() { throw CancellationError() }
            let commit = next.commit
            if committerTime(commit) < cutoff { continue }
            if shallow.contains(next.id) { continue }
            var parents: [GitCommit] = []
            var complete = true
            for parent in commit.parents {
                guard try repository.hasObject(parent) else {
                    complete = false
                    break
                }
                parents.append(try repository.readCommit(parent))
            }
            if !complete { continue }
            for (index, parent) in commit.parents.enumerated() where seen.insert(parent).inserted {
                queue.push(parent, parents[index])
            }
            if commit.parents.count > 1 { continue }
            let files = try changedFiles(parents.first?.tree, commit.tree, cancel: cancel)
            output.append(InsightCommit(
                hash: next.id.hex, author: authorName(commit), time: authorTime(commit),
                subject: GitInsightSource.subject(commit.message), files: files))
        }
        return output
    }

    public func trendVersions(_ path: String, samples: Int = 16, maxCommits: Int = 400, cancel: () -> Bool = { false }) throws -> [InsightVersion] {
        guard let tip else { return [] }
        lock.lock()
        defer { lock.unlock() }
        let shallow = (try? repository.shallowCommits()) ?? []
        let components = path.split(separator: "/", omittingEmptySubsequences: false).map { Array($0.utf8) }
        var queue = CommitQueue()
        var seen = Set<GitObjectId>()
        var touching: [(id: GitObjectId, commit: GitCommit, blob: GitObjectId?)] = []
        seen.insert(tip)
        queue.push(tip, try repository.readCommit(tip))
        while touching.count < maxCommits, let next = queue.pop() {
            if cancel() { throw CancellationError() }
            let commit = next.commit
            if shallow.contains(next.id) { continue }
            var parents: [(id: GitObjectId, commit: GitCommit)] = []
            var complete = true
            for parent in commit.parents {
                guard try repository.hasObject(parent) else {
                    complete = false
                    break
                }
                parents.append((parent, try repository.readCommit(parent)))
            }
            if !complete { continue }
            let mine = try blob(at: components, in: commit.tree)
            if parents.count > 1 {
                var followed = false
                for parent in parents {
                    if try blob(at: components, in: parent.commit.tree) != mine { continue }
                    if seen.insert(parent.id).inserted { queue.push(parent.id, parent.commit) }
                    followed = true
                    break
                }
                if !followed {
                    for parent in parents where seen.insert(parent.id).inserted { queue.push(parent.id, parent.commit) }
                }
                continue
            }
            for parent in parents where seen.insert(parent.id).inserted { queue.push(parent.id, parent.commit) }
            let before = try parents.first.flatMap { try blob(at: components, in: $0.commit.tree) }
            if mine != before { touching.append((next.id, commit, mine)) }
        }
        var versions: [InsightVersion] = []
        for index in Insight.trendPicks(commitCount: touching.count, samples: samples) {
            if cancel() { throw CancellationError() }
            let pick = touching[index]
            guard let blob = pick.blob else { continue }
            let bytes = try repository.readBlob(blob)
            versions.append(InsightVersion(time: authorTime(pick.commit), commit: pick.id.hex, author: authorName(pick.commit), bytes: bytes))
        }
        return versions
    }

    public func trend(_ path: String, samples: Int = 16, cancel: () -> Bool = { false }) throws -> [InsightTrendPoint] {
        Insight.trend(path, versions: try trendVersions(path, samples: samples, cancel: cancel))
    }

    public static func subject(_ message: [UInt8]) -> String {
        let text = String(decoding: message, as: UTF8.self)
        var parts: [String] = []
        var started = false
        for line in text.split(separator: "\n", omittingEmptySubsequences: false) {
            let blank = line.allSatisfy { $0 == " " || $0 == "\t" || $0 == "\r" }
            if blank {
                if started { break }
                continue
            }
            started = true
            parts.append(String(line))
        }
        return parts.joined(separator: " ")
    }

    func authorName(_ commit: GitCommit) -> String {
        if let author = commit.author { return author.name.trimmingCharacters(in: .whitespacesAndNewlines) }
        let raw = commit.headers["author"]?.first ?? ""
        let name = raw.components(separatedBy: " <").first ?? raw
        return name.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    func authorTime(_ commit: GitCommit) -> Int64 {
        Int64(commit.author?.timestampSeconds ?? commit.committer?.timestampSeconds ?? 0)
    }

    func committerTime(_ commit: GitCommit) -> Int64 {
        Int64(commit.committer?.timestampSeconds ?? commit.author?.timestampSeconds ?? 0)
    }

    private func workingSize(_ path: String) -> Int64 {
        guard let workingRoot else { return -1 }
        let url = workingRoot.appendingPathComponent(path)
        guard let attributes = try? FileManager.default.attributesOfItem(atPath: url.path),
              (attributes[.type] as? FileAttributeType) == .typeRegular,
              let size = attributes[.size] as? NSNumber
        else { return -1 }
        return size.int64Value
    }

    private func walkTree(_ id: GitObjectId, prefix: String, _ visit: (String, GitTreeEntry) throws -> Void) throws {
        var stack: [(GitObjectId, String)] = [(id, prefix)]
        var pending: [(String, GitTreeEntry)] = []
        while let (tree, base) = stack.popLast() {
            for entry in try repository.readTree(tree).entries {
                let path = base.isEmpty ? entry.nameText : base + "/" + entry.nameText
                if entry.mode == .directory {
                    stack.append((entry.objectId, path))
                } else if entry.mode != .symlink {
                    pending.append((path, entry))
                }
            }
        }
        pending.sort { $0.0.utf8.lexicographicallyPrecedes($1.0.utf8) }
        for (path, entry) in pending { try visit(path, entry) }
    }

    private func blob(at components: [[UInt8]], in tree: GitObjectId) throws -> GitObjectId? {
        var current = tree
        for (index, name) in components.enumerated() {
            guard let entry = try repository.readTree(current).entries.first(where: { $0.name == name }) else { return nil }
            if index == components.count - 1 {
                return entry.mode == .directory ? nil : entry.objectId
            }
            if entry.mode != .directory { return nil }
            current = entry.objectId
        }
        return nil
    }

    func changedFiles(_ before: GitObjectId?, _ after: GitObjectId?, cancel: () -> Bool) throws -> [InsightCommitFile] {
        var files: [InsightCommitFile] = []
        try diffTrees(before, after, prefix: "", into: &files, cancel: cancel)
        return files
    }

    private func diffTrees(_ before: GitObjectId?, _ after: GitObjectId?, prefix: String, into files: inout [InsightCommitFile], cancel: () -> Bool) throws {
        if before == after { return }
        if cancel() { throw CancellationError() }
        let left = try before.map { try repository.readTree($0).entries } ?? []
        let right = try after.map { try repository.readTree($0).entries } ?? []
        var keys: [(key: [UInt8], name: String, directory: Bool)] = []
        var leftByKey: [[UInt8]: GitTreeEntry] = [:]
        var rightByKey: [[UInt8]: GitTreeEntry] = [:]
        for entry in left {
            let key = entry.mode == .directory ? entry.name + [0x2f] : entry.name
            leftByKey[key] = entry
            keys.append((key, entry.nameText, entry.mode == .directory))
        }
        for entry in right {
            let key = entry.mode == .directory ? entry.name + [0x2f] : entry.name
            if leftByKey[key] == nil { keys.append((key, entry.nameText, entry.mode == .directory)) }
            rightByKey[key] = entry
        }
        keys.sort { $0.key.lexicographicallyPrecedes($1.key) }
        for item in keys {
            let path = prefix.isEmpty ? item.name : prefix + "/" + item.name
            let old = leftByKey[item.key]
            let new = rightByKey[item.key]
            if item.directory {
                try diffTrees(old?.objectId, new?.objectId, prefix: path, into: &files, cancel: cancel)
                continue
            }
            if let old, let new, old.objectId == new.objectId, old.mode == new.mode { continue }
            if !GitInsightSource.analysable(path) { continue }
            if let old, let new, old.objectId == new.objectId {
                files.append(InsightCommitFile(path: path, added: 0, deleted: 0))
                continue
            }
            let oldBytes = try old.map { try repository.readBlob($0.objectId) }
            let newBytes = try new.map { try repository.readBlob($0.objectId) }
            let counted: LineChangeCount?
            switch (oldBytes, newBytes) {
            case let (.some(a), .some(b)): counted = LineDiffCount.count(old: a, new: b)
            case let (.none, .some(b)): counted = LineDiffCount.added(b)
            case let (.some(a), .none): counted = LineDiffCount.deleted(a)
            case (.none, .none): continue
            }
            files.append(InsightCommitFile(path: path, added: counted?.added, deleted: counted?.deleted))
        }
    }
}

struct CommitQueue {
    struct Item {
        let id: GitObjectId
        let commit: GitCommit
        let time: Int64
        let order: Int
    }

    private var heap: [Item] = []
    private var counter = 0

    mutating func push(_ id: GitObjectId, _ commit: GitCommit) {
        let time = Int64(commit.committer?.timestampSeconds ?? commit.author?.timestampSeconds ?? 0)
        heap.append(Item(id: id, commit: commit, time: time, order: counter))
        counter += 1
        var child = heap.count - 1
        while child > 0 {
            let parent = (child - 1) / 2
            if !before(heap[child], heap[parent]) { break }
            heap.swapAt(child, parent)
            child = parent
        }
    }

    mutating func pop() -> Item? {
        guard !heap.isEmpty else { return nil }
        heap.swapAt(0, heap.count - 1)
        let top = heap.removeLast()
        var parent = 0
        while true {
            let left = parent * 2 + 1
            let right = left + 1
            var best = parent
            if left < heap.count, before(heap[left], heap[best]) { best = left }
            if right < heap.count, before(heap[right], heap[best]) { best = right }
            if best == parent { break }
            heap.swapAt(parent, best)
            parent = best
        }
        return top
    }

    private func before(_ a: Item, _ b: Item) -> Bool {
        if a.time != b.time { return a.time > b.time }
        return a.order < b.order
    }
}
