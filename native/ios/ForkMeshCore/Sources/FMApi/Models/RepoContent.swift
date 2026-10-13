// The endpoints answer in a few historical shapes (desktop hosts, the Worker,
// mirrors), so every reader accepts each shape's key aliases.

import Foundation
import FMBase

public struct RepoTreeEntry: Equatable, Hashable, Sendable, Identifiable {
    public var name: String
    public var path: String
    /// "file" or "dir" once parsed (symlink/submodule rows read as files).
    public var type: String
    public var size: Int
    public var sha: String
    /// Last commit touching this entry, in epoch milliseconds (0 if unknown).
    public var updatedMs: Int
    /// Subject line of the last commit touching this entry ("" if unknown).
    public var lastMessage: String

    public var id: String { path }

    public init(
        name: String, path: String, type: String, size: Int = 0, sha: String = "", updatedMs: Int = 0,
        lastMessage: String = ""
    ) {
        self.name = name
        self.path = path
        self.type = type
        self.size = size
        self.sha = sha
        self.updatedMs = updatedMs
        self.lastMessage = lastMessage
    }

    public init(json j: JSONObject, timeZone: TimeZone = .current) {
        let rawMessage = j.firstStr("subject", "commitMessage", "message")
        let rawPath = j.firstStr("path", "name")
        let name: String
        if let value = ModelJSON.coalesce(j["name"]) {
            name = value.text
        } else {
            name = rawPath.split(separator: "/").last.map(String.init) ?? rawPath
        }
        let rawType = j.firstStr("type", "kind", "mode").lowercased()
        let isDir = rawType.contains("tree") || rawType == "directory" || rawType == "folder" || rawType == "dir"
        self.init(
            name: name,
            path: rawPath,
            type: isDir ? "dir" : "file",
            size: ModelJSON.int(ModelJSON.coalesce(j["size"], j["bytes"])),
            sha: j.firstStr("sha", "hash", "oid"),
            updatedMs: Self.epochMs(ModelJSON.coalesce(j["date"], j["updatedAt"], j["mtime"]), timeZone),
            lastMessage: (rawMessage.components(separatedBy: "\n").first ?? "").dartTrimmed
        )
    }

    /// Seconds or milliseconds (anything up to 10^11 is seconds), a numeric
    /// string, or an ISO date.
    private static func epochMs(_ value: JSONValue?, _ timeZone: TimeZone) -> Int {
        guard let value else { return 0 }
        if value.doubleValue != nil {
            let n = ModelJSON.int(value)
            return n > 100_000_000_000 ? n : n * 1_000
        }
        let text = DartCore.toString(value).dartTrimmed
        if text.isEmpty { return 0 }
        if let n = DartCore.intTryParse(text) { return n > 100_000_000_000 ? n : n * 1_000 }
        return DartCore.dateTimeTryParseMs(text, timeZone: timeZone) ?? 0
    }

    public var isDirectory: Bool { type == "dir" || type == "tree" || type == "folder" }

    public var updatedAt: Date? { updatedMs > 0 ? Date(epochMillis: Int64(updatedMs)) : nil }

    public func copyWith(path: String? = nil) -> RepoTreeEntry {
        var copy = self
        copy.path = path ?? self.path
        return copy
    }
}

public struct RepoTree: Equatable, Hashable, Sendable {
    public var path: String
    public var entries: [RepoTreeEntry]
    public var source: String
    public var cachedAt: Int
    /// The immutable commit this listing was served from, when the mirror
    /// reports one. Reads that must pin an exact revision (resolving the
    /// `forkmesh/pulls` ledger tip before a merge, above all) need this and
    /// have no other way to learn it.
    public var commit: String
    public var pullRefs: [String: String]

    public init(
        path: String, entries: [RepoTreeEntry], source: String = "", cachedAt: Int = 0, commit: String = "",
        pullRefs: [String: String] = [:]
    ) {
        self.path = path
        self.entries = entries
        self.source = source
        self.cachedAt = cachedAt
        self.commit = commit
        self.pullRefs = pullRefs
    }

    /// A bare entry list (desktop hosts) or an object wrapping one under
    /// `entries`, `tree`, `items`, `files` or `data` (the Worker). Directories
    /// sort first, then names case-insensitively; bare child names are
    /// qualified with `path`.
    public init(json data: JSONValue?, path requested: String = "") {
        var path = requested
        var rawEntries: [JSONValue] = []
        var source = ""
        var cachedAt = 0
        var commit = ""
        var pullRefs: [String: String] = [:]
        if let list = data?.arrayValue {
            rawEntries = list
        } else if let object = data?.objectValue {
            for key in ["entries", "tree", "items", "files", "data"] {
                if let list = object[key]?.arrayValue {
                    rawEntries = list
                    break
                }
            }
            source = object.firstStr("source", "servedBy")
            cachedAt = ModelJSON.int(ModelJSON.coalesce(object["cachedAt"], object["ts"]))
            path = object.str("path", path)
            let analysisCommit = object["analysis"]?.objectValue?["commit"]
            commit = ModelJSON.coalesce(object["commit"], analysisCommit)?.text ?? ""
            for (key, value) in object["pullRefs"]?.objectValue ?? JSONObject() {
                let ref = value.stringValue?.dartTrimmed ?? ""
                if !key.isEmpty, key.allSatisfy({ $0.isASCII && $0.isNumber }), !ref.isEmpty {
                    pullRefs[key] = ref
                }
            }
        }
        let base = path
        let entries = rawEntries.compactMap(\.objectValue)
            .map { RepoTreeEntry(json: $0) }
            .map { entry -> RepoTreeEntry in
                if base.isEmpty || entry.path.contains("/") { return entry }
                return entry.copyWith(path: "\(base)/\(entry.path)")
            }
            .filter { !$0.name.isEmpty }
            .sorted { a, b in
                if a.isDirectory != b.isDirectory { return a.isDirectory }
                return DartCore.lessThan(a.name.lowercased(), b.name.lowercased())
            }
        self.init(path: path, entries: entries, source: source, cachedAt: cachedAt, commit: commit, pullRefs: pullRefs)
    }
}

public struct RepoBlob: Equatable, Hashable, Sendable {
    public var path: String
    public var content: String
    public var encoding: String
    public var size: Int
    public var source: String

    public init(path: String, content: String, encoding: String = "", size: Int = 0, source: String = "") {
        self.path = path
        self.content = content
        self.encoding = encoding
        self.size = size
        self.source = source
    }

    /// A raw string body or an object wrapping `content` / `text` / `data`.
    public init(json data: JSONValue?, path: String = "") {
        if let text = data?.stringValue {
            self.init(path: path, content: text, size: DartCore.length(text))
        } else if let object = data?.objectValue {
            self.init(
                path: object.str("path", path),
                content: object.firstStr("content", "text", "data"),
                encoding: object.str("encoding"),
                size: ModelJSON.int(ModelJSON.coalesce(object["size"], object["bytes"])),
                source: object.firstStr("source", "servedBy")
            )
        } else {
            self.init(path: path, content: "")
        }
    }
}

public struct RepoCommitFile: Equatable, Hashable, Sendable {
    public var path: String
    public var adds: String
    public var dels: String

    public init(path: String, adds: String = "", dels: String = "") {
        self.path = path
        self.adds = adds
        self.dels = dels
    }

    public init(json j: JSONObject) {
        self.init(
            path: j.firstStr("path", "file"),
            adds: j.firstStr("adds", "additions"),
            dels: j.firstStr("dels", "deletions")
        )
    }
}

public struct RepoCommitDetail: Equatable, Hashable, Sendable {
    public var commit: JSONObject
    public var files: [RepoCommitFile]
    public var diff: String
    public var truncated: Bool
    public var source: String

    public init(
        commit: JSONObject, files: [RepoCommitFile] = [], diff: String = "", truncated: Bool = false,
        source: String = ""
    ) {
        self.commit = commit
        self.files = files
        self.diff = diff
        self.truncated = truncated
        self.source = source
    }

    public init(json data: JSONValue?, fallbackCommit: JSONObject = JSONObject()) {
        guard let object = data?.objectValue else {
            self.init(commit: fallbackCommit)
            return
        }
        self.init(
            commit: object["commit"]?.objectValue ?? fallbackCommit,
            files: object.maps("files").map(RepoCommitFile.init(json:)).filter { !$0.path.isEmpty },
            diff: object.str("diff"),
            truncated: object.bool("truncated"),
            source: object.firstStr("source", "servedBy")
        )
    }
}

/// `GET /api/repo/<owner>/<name>/compare?base=&head=`: what head adds on top
/// of base, as the mirror's git computed it.
public struct RepoComparison: Equatable, Hashable, Sendable {
    public var baseOid: String
    public var headOid: String
    public var mergeBaseOid: String
    /// `git rev-list --count base..head`.
    public var commitCount: Int
    /// Net change from the merge base to the head, as a unified diff.
    public var patch: String
    /// The same range as a `git format-patch --stdout` series; empty when the
    /// range holds a merge commit, which format-patch cannot represent.
    public var commits: String

    public init(
        baseOid: String = "", headOid: String = "", mergeBaseOid: String = "", commitCount: Int = 0,
        patch: String = "", commits: String = ""
    ) {
        self.baseOid = baseOid
        self.headOid = headOid
        self.mergeBaseOid = mergeBaseOid
        self.commitCount = commitCount
        self.patch = patch
        self.commits = commits
    }

    public init(json data: JSONValue?) {
        guard let object = data?.objectValue else {
            self.init()
            return
        }
        self.init(
            baseOid: object.str("baseOid"),
            headOid: object.str("headOid"),
            mergeBaseOid: object.str("mergeBaseOid"),
            commitCount: ModelJSON.int(object["commitCount"]),
            patch: object.str("patch"),
            commits: object.str("commits")
        )
    }
}

public struct RepoBranch: Equatable, Hashable, Sendable, Identifiable {
    public var name: String
    public var sha: String
    public var isDefault: Bool
    public var worktreePath: String

    public var id: String { name }

    public init(name: String, sha: String = "", isDefault: Bool = false, worktreePath: String = "") {
        self.name = name
        self.sha = sha
        self.isDefault = isDefault
        self.worktreePath = worktreePath
    }

    public init(json j: JSONObject) {
        self.init(
            name: j.firstStr("name", "branch"),
            sha: j.firstStr("sha", "hash", "commit"),
            isDefault: j.bool("default") || j.bool("isDefault"),
            worktreePath: j.firstStr("worktree", "worktreePath")
        )
    }

    public var hasWorktree: Bool { !worktreePath.isEmpty }
}

public struct RepoCodeSearchMatch: Equatable, Hashable, Sendable {
    public var path: String
    public var line: Int
    public var text: String

    public init(path: String, line: Int = 0, text: String = "") {
        self.path = path
        self.line = line
        self.text = text
    }

    public init(json j: JSONObject) {
        self.init(
            path: j.firstStr("path", "file"),
            line: ModelJSON.int(ModelJSON.coalesce(j["line"], j["lineNumber"])),
            text: j.firstStr("text", "snippet", "match")
        )
    }
}

public struct RepoSearchResults: Equatable, Hashable, Sendable {
    public var code: [RepoCodeSearchMatch]

    public init(code: [RepoCodeSearchMatch] = []) {
        self.code = code
    }

    public init(json data: JSONValue?) {
        let object = data?.objectValue ?? JSONObject()
        self.init(code: object.maps("code").map(RepoCodeSearchMatch.init(json:)).filter { !$0.path.isEmpty })
    }
}

public struct RepoMirror: Equatable, Hashable, Sendable {
    public var owner: String
    public var name: String
    public var online: Bool
    public var lastSeenMs: Int
    public var firstHostedMs: Int
    public var node: String = ""
    public var mirrorUrl: String = ""
    public var endpoint: String = ""
    public var integrity: String = ""
    public var lastSyncMs: Int = 0
    public var cloneAvailable: Bool = false
    public var endpointHealthy: Bool = false
    public var endpointFresh: Bool = false
    public var abuseBlocked: Bool = false
    public var behind: Bool = false
    public var latencyMs: Int = 0

    public init(owner: String, name: String, online: Bool = false, lastSeenMs: Int = 0, firstHostedMs: Int = 0) {
        self.owner = owner
        self.name = name
        self.online = online
        self.lastSeenMs = lastSeenMs
        self.firstHostedMs = firstHostedMs
    }

    public init(json j: JSONObject) {
        self.init(
            owner: j.firstStr("owner", "node", "name"),
            name: j.firstStr("repo", "repository", "name"),
            online: j.bool("online") || j.bool("live") || j["status"] == .string("online"),
            lastSeenMs: ModelJSON.int(ModelJSON.coalesce(j["lastSeen"], j["lastSeenAt"], j["ts"])),
            firstHostedMs: ModelJSON.int(ModelJSON.coalesce(j["firstHosted"], j["firstHostedAt"]))
        )
        node = j.firstStr("node", "owner")
        mirrorUrl = j.str("mirrorUrl").dartTrimmed
        endpoint = j.str("endpoint").dartTrimmed
        integrity = j.str("integrity").dartTrimmed.lowercased()
        lastSyncMs = ModelJSON.int(j["lastSync"])
        cloneAvailable = j.bool("cloneAvailable")
        endpointHealthy = j.bool("endpointHealthy")
        endpointFresh = j.bool("endpointFresh")
        abuseBlocked = j.bool("abuseBlocked")
        behind = j.bool("behind")
        latencyMs = ModelJSON.int(ModelJSON.coalesce(j["directLatencyMs"], j["latencyMs"]))
        if latencyMs == 0 { latencyMs = ModelJSON.int(j["latencyMs"]) }
    }

    public var label: String { owner.isEmpty ? name : "\(owner)/\(name)" }

    public var isHealthy: Bool {
        online && integrity == "ok" && lastSyncMs > 0 && cloneAvailable && endpointHealthy && endpointFresh
            && !abuseBlocked
    }
}

/// A repository's mirrors and its forkmesh-sync-status-v1 status map.
public struct RepoMirrorSet: Equatable, Hashable, Sendable {
    public var mirrors: [RepoMirror]
    public var syncStatus: JSONObject?

    public init(mirrors: [RepoMirror], syncStatus: JSONObject? = nil) {
        self.mirrors = mirrors
        self.syncStatus = syncStatus
    }
}
