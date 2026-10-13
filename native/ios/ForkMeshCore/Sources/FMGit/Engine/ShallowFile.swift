import Foundation

enum GitShallowFile {
    static func path(_ gitDirectory: String) -> String { EngineFS.join(gitDirectory, "shallow") }

    static func read(_ gitDirectory: String) throws -> Set<GitObjectId> {
        let file = path(gitDirectory)
        if !EngineFS.fileExists(file) { return [] }
        var ids = Set<GitObjectId>()
        for line in try EngineFS.readString(file).split(separator: "\n") {
            let text = String(line).trimmingCharacters(in: .whitespaces)
            if text.isEmpty { continue }
            do {
                ids.insert(try GitObjectId.parseSha1(text))
            } catch {
                throw GitException(.invalidRepository, "shallow file is invalid")
            }
        }
        return ids
    }

    static func write(_ gitDirectory: String, _ ids: Set<GitObjectId>) throws {
        let file = path(gitDirectory)
        if ids.isEmpty {
            if EngineFS.fileExists(file) { try EngineFS.deleteFile(file) }
            return
        }
        let lock = "\(file).lock"
        if !EngineFS.createFileExclusive(lock) {
            throw GitException(.refLocked, "shallow lock already exists")
        }
        defer {
            if EngineFS.fileExists(lock) { try? EngineFS.deleteFile(lock) }
        }
        let text = ids.map(\.hex).sorted().map { "\($0)\n" }.joined()
        try EngineFS.writeStringFlushed(lock, text)
        try EngineFS.rename(lock, file)
    }
}

extension GitRepository {
    public func shallowCommits() throws -> Set<GitObjectId> { try GitShallowFile.read(gitDirectory) }

    public var isShallow: Bool { EngineFS.fileExists(GitShallowFile.path(gitDirectory)) }

    func walkParents(_ id: GitObjectId, _ commit: GitCommit, _ shallow: Set<GitObjectId>) -> [GitObjectId] {
        shallow.contains(id) ? [] : commit.parents
    }

    public func ancestry(
        _ ancestor: GitObjectId, _ descendant: GitObjectId, _ cancellation: GitCancellationToken? = nil
    ) throws -> GitAncestry {
        if ancestor == descendant { return .ancestor }
        let shallow = try shallowCommits()
        let below = try boundedWalk(descendant, shallow, cancellation)
        if below.visited.contains(ancestor) { return .ancestor }
        if below.edges.isEmpty && below.missing.isEmpty { return .notAncestor }
        if !below.missing.isEmpty { return .unknown }
        let above = try boundedWalk(ancestor, shallow, cancellation)
        return below.edges.isSubset(of: above.visited) ? .notAncestor : .unknown
    }

    private func boundedWalk(
        _ tip: GitObjectId, _ shallow: Set<GitObjectId>, _ cancellation: GitCancellationToken?
    ) throws -> (visited: Set<GitObjectId>, edges: Set<GitObjectId>, missing: Set<GitObjectId>) {
        var visited = Set<GitObjectId>()
        var edges = Set<GitObjectId>()
        var missing = Set<GitObjectId>()
        var stack = [tip]
        while let id = stack.popLast() {
            try cancellation?.throwIfCancelled()
            if !visited.insert(id).inserted { continue }
            if visited.count > limits.maxCommitWalk {
                throw GitException(.resourceLimitExceeded, "ancestry walk exceeded the configured limit")
            }
            guard try hasObject(id), let commit = try readCommitOrTagTarget(id) else {
                missing.insert(id)
                continue
            }
            if shallow.contains(id) {
                edges.insert(id)
                continue
            }
            stack.append(contentsOf: commit.parents)
        }
        return (visited, edges, missing)
    }
}
