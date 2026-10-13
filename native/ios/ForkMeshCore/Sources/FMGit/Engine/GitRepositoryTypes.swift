import Foundation

public struct GitRepositoryInit {
    public let worktree: String
    public let limits: GitResourceLimits?
    public let detector: GitSha1CollisionDetector?
    public let initialBranch: GitBranchName?

    public init(
        _ worktree: String, limits: GitResourceLimits? = nil, detector: GitSha1CollisionDetector? = nil,
        initialBranch: GitBranchName? = nil
    ) {
        self.worktree = worktree
        self.limits = limits
        self.detector = detector
        self.initialBranch = initialBranch
    }
}

public struct GitRepositoryOpen {
    public let worktree: String
    public let limits: GitResourceLimits?
    public let detector: GitSha1CollisionDetector?

    public init(_ worktree: String, limits: GitResourceLimits? = nil, detector: GitSha1CollisionDetector? = nil) {
        self.worktree = worktree
        self.limits = limits
        self.detector = detector
    }
}

public enum GitChangeKind: String, Sendable {
    case added, modified, deleted, untracked
}

public struct GitStatusEntry: Sendable {
    public let path: GitWorktreePath
    public let kind: GitChangeKind

    public init(_ path: GitWorktreePath, _ kind: GitChangeKind) {
        self.path = path
        self.kind = kind
    }

    public init(path: GitWorktreePath, kind: GitChangeKind) { self.init(path, kind) }
}

public struct GitStatus: Sendable {
    public let staged: [GitStatusEntry]
    public let worktree: [GitStatusEntry]

    public init(staged: [GitStatusEntry], worktree: [GitStatusEntry]) {
        self.staged = staged
        self.worktree = worktree
    }

    public var isClean: Bool { staged.isEmpty && worktree.isEmpty }
}

public struct GitIdentity: Sendable {
    public let name: String
    public let email: String
    public let timestampSeconds: Int
    public let timezoneOffsetMinutes: Int

    public init(name: String, email: String, timestampSeconds: Int, timezoneOffsetMinutes: Int) throws {
        if name.isEmpty || email.isEmpty || !Self.isSafeField(name) || !Self.isSafeField(email)
            || timezoneOffsetMinutes < -14 * 60 || timezoneOffsetMinutes > 14 * 60
        {
            throw GitException(.invalidObject, "commit identity is invalid")
        }
        self.name = name
        self.email = email
        self.timestampSeconds = timestampSeconds
        self.timezoneOffsetMinutes = timezoneOffsetMinutes
    }

    public var gitText: String {
        let absoluteMinutes = abs(timezoneOffsetMinutes)
        let sign = timezoneOffsetMinutes < 0 ? "-" : "+"
        let hours = absoluteMinutes / 60
        let minutes = absoluteMinutes % 60
        let hh = hours < 10 ? "0\(hours)" : "\(hours)"
        let mm = minutes < 10 ? "0\(minutes)" : "\(minutes)"
        return "\(name) <\(email)> \(timestampSeconds) \(sign)\(hh)\(mm)"
    }

    /// Identity fields are UTF-8 per the specification. Angle brackets, NUL,
    /// line breaks, and other ASCII control characters are rejected because
    /// they would corrupt or forge the `name <email> time tz` header framing.
    private static func isSafeField(_ value: String) -> Bool {
        !value.utf8.contains { $0 <= 0x1f || $0 == 0x7f || $0 == 0x3c || $0 == 0x3e }
    }
}

public struct GitCommitRequest: Sendable {
    public let message: String
    public let author: GitIdentity
    public let committer: GitIdentity

    public init(message: String, author: GitIdentity, committer: GitIdentity) throws {
        if message.utf8.contains(0) {
            throw GitException(.invalidObject, "commit message contains NUL")
        }
        self.message = message
        self.author = author
        self.committer = committer
    }
}

public struct GitBranchName: Hashable, Sendable {
    public let value: String

    public init(_ value: String) throws {
        if value.isEmpty || value.engHasPrefix("/") || value.engHasSuffix("/") {
            throw GitException(.invalidRef, "branch name is invalid")
        }
        _ = try GitRefName.branch("refs/heads/\(value)")
        self.value = value
    }

    /// `refs/heads/<value>`; validated at construction, so it does not fail.
    public func ref() throws -> GitRefName { try GitRefName.branch("refs/heads/\(value)") }

    public static func == (left: GitBranchName, right: GitBranchName) -> Bool { left.value.engSame(right.value) }

    public func hash(into hasher: inout Hasher) { hasher.combine(Array(value.utf8)) }
}

public struct GitCheckoutRequest: Sendable {
    public let branch: GitBranchName

    public static func branch(_ branch: GitBranchName) -> GitCheckoutRequest { GitCheckoutRequest(branch: branch) }
}

/// A bounded history walk request for `GitRepository.log`.
public struct GitLogRequest: Sendable {
    public static let maxLimit = 10000

    /// Full ref name (for example `refs/heads/main`); nil walks from `HEAD`.
    public let ref: String?

    /// Maximum commits returned.
    public let limit: Int

    public init(ref: String? = nil, limit: Int = 200) throws {
        if limit <= 0 || limit > Self.maxLimit {
            throw GitArgumentError("limit", "must be in 1..\(Self.maxLimit)")
        }
        if let ref { _ = try GitRefName.parse(ref) }
        self.ref = ref
        self.limit = limit
    }
}

/// One commit produced by `GitRepository.log`.
public struct GitLogEntry: Sendable {
    public let id: GitObjectId
    public let commit: GitCommit

    public init(_ id: GitObjectId, _ commit: GitCommit) {
        self.id = id
        self.commit = commit
    }

    public init(id: GitObjectId, commit: GitCommit) { self.init(id, commit) }
}

/// A direct ref (branch or tag) returned by enumeration APIs.
public struct GitRefEntry: Sendable {
    /// Full validated ref name, for example `refs/heads/main`.
    public let name: GitRefName
    public let objectId: GitObjectId

    public init(_ name: GitRefName, _ objectId: GitObjectId) {
        self.name = name
        self.objectId = objectId
    }

    public init(name: GitRefName, objectId: GitObjectId) { self.init(name, objectId) }

    /// The name without its `refs/<namespace>/` prefix.
    public var shortName: String {
        let value = name.value
        let second = value.engIndexOf("/", from: "refs/".utf8.count)
        return value.engSubstring(second + 1)
    }
}
