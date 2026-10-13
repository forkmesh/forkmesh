import Foundation

/// How an operation reaches the network.
///
/// Supply a ready `transport` (for example a test double) or let the engine
/// build a fail-closed `GitNetHttpTransport` from `headerProvider`. The
/// engine never persists any of these values. (Certificate challenges and
/// extra trusted roots are not supported: HTTPClient has no hook for them;
/// see NetHttpTransport.swift.)
public struct GitRemoteAccess {
    public let transport: GitHttpTransport?
    public let headerProvider: GitHttpHeaderProvider?

    public init(transport: GitHttpTransport? = nil, headerProvider: GitHttpHeaderProvider? = nil) {
        self.transport = transport
        self.headerProvider = headerProvider
    }
}

public struct GitCloneRequest {
    public let url: GitRemoteUrl
    public let worktree: String
    public let remoteName: String

    /// Branch to check out; defaults to the remote `HEAD` symref target.
    public let branch: String?

    public let depth: Int?
    public let singleBranch: Bool
    public let access: GitRemoteAccess
    public let limits: GitResourceLimits?
    public let detector: GitSha1CollisionDetector?
    public let cancellation: GitCancellationToken?

    public init(
        url: GitRemoteUrl,
        worktree: String,
        remoteName: String = "origin",
        branch: String? = nil,
        depth: Int? = nil,
        singleBranch: Bool = false,
        access: GitRemoteAccess = .init(),
        limits: GitResourceLimits? = nil,
        detector: GitSha1CollisionDetector? = nil,
        cancellation: GitCancellationToken? = nil
    ) throws {
        try GitRemoteName.validate(remoteName)
        if let branch { _ = try GitBranchName(branch) }
        if let depth, depth < 1 || depth > GitResourceLimits.hardMaxCommitWalk {
            throw GitException(.unsupportedRepositoryFormat, "shallow clone depth must be in 1..\(GitResourceLimits.hardMaxCommitWalk)")
        }
        self.url = url
        self.worktree = worktree
        self.remoteName = remoteName
        self.branch = branch
        self.depth = depth
        self.singleBranch = singleBranch
        self.access = access
        self.limits = limits
        self.detector = detector
        self.cancellation = cancellation
    }
}

public struct GitCloneResult: Sendable {
    public let repository: GitRepository

    /// The branch checked out, or nil when the remote was empty.
    public let checkedOutBranch: String?
    public let fetch: GitFetchResult
}

public struct GitFetchRequest {
    /// Configured remote name whose URL and refspec are used.
    public let remote: String

    /// Optional one-off URL override; it is never written to config.
    public let url: GitRemoteUrl?
    public let refspec: GitFetchRefspec?
    public let deepen: GitDeepen?
    public let access: GitRemoteAccess
    public let cancellation: GitCancellationToken?

    public init(
        remote: String = "origin",
        url: GitRemoteUrl? = nil,
        refspec: GitFetchRefspec? = nil,
        deepen: GitDeepen? = nil,
        access: GitRemoteAccess = .init(),
        cancellation: GitCancellationToken? = nil
    ) throws {
        try GitRemoteName.validate(remote)
        try deepen?.validate()
        self.remote = remote
        self.url = url
        self.refspec = refspec
        self.deepen = deepen
        self.access = access
        self.cancellation = cancellation
    }
}

public enum GitDeepen: Equatable, Sendable {
    case relative(Int)
    case since(Date)
    case unshallow

    public static let infiniteDepth = 0x7fff_ffff

    func validate() throws {
        switch self {
        case .relative(let depth):
            if depth < 1 || depth > GitResourceLimits.hardMaxCommitWalk {
                throw GitException(.unsupportedRepositoryFormat, "deepen depth must be in 1..\(GitResourceLimits.hardMaxCommitWalk)")
            }
        case .since(let date):
            if date.timeIntervalSince1970 < 0 {
                throw GitException(.unsupportedRepositoryFormat, "deepen-since must not be before 1970")
            }
        case .unshallow:
            break
        }
    }

    var depth: Int? {
        switch self {
        case .relative(let depth): return depth
        case .since: return nil
        case .unshallow: return GitDeepen.infiniteDepth
        }
    }

    var isRelative: Bool {
        if case .relative = self { return true }
        return false
    }

    var since: Int64? {
        if case .since(let date) = self { return Int64(date.timeIntervalSince1970) }
        return nil
    }
}

public enum GitAncestry: Equatable, Sendable {
    case ancestor
    case notAncestor
    case unknown
}

public enum GitFastForwardOutcome: Equatable, Sendable {
    case upToDate
    case fastForwarded
    case diverged
    case undetermined
}

/// One remote-tracking ref (or tag) written by a fetch.
public struct GitRefUpdateRecord: Sendable {
    public let ref: String
    public let oldId: GitObjectId?
    public let newId: GitObjectId

    public var isNew: Bool { oldId == nil }
    public var isChanged: Bool { oldId != newId }
}

public struct GitFetchResult: Sendable {
    public let updates: [GitRefUpdateRecord]
    public let packBytes: Int
    public let objects: Int

    /// Remote `HEAD` symref target (for example `refs/heads/main`), if any.
    public let remoteHead: String?
    public let negotiationRounds: Int
}

public struct GitPushRequest {
    public let remote: String

    /// Branches to push; empty means the current branch to its upstream or
    /// same-named remote branch. Deletions and force are not expressible.
    public let refspecs: [GitPushRefspec]
    public let access: GitRemoteAccess
    public let cancellation: GitCancellationToken?

    public init(
        remote: String = "origin",
        refspecs: [GitPushRefspec] = [],
        access: GitRemoteAccess = .init(),
        cancellation: GitCancellationToken? = nil
    ) throws {
        try GitRemoteName.validate(remote)
        if refspecs.count > 1000 {
            throw GitException(.resourceLimitExceeded, "too many push refspecs")
        }
        self.remote = remote
        self.refspecs = refspecs
        self.access = access
        self.cancellation = cancellation
    }
}

public enum GitPushRefOutcome: String, Sendable {
    case updated, upToDate, rejected
}

public struct GitPushRefResult: Sendable {
    public let localRef: String
    public let remoteRef: String

    /// Remote value before the push (nil when the ref did not exist).
    public let oldId: GitObjectId?
    public let newId: GitObjectId
    public let outcome: GitPushRefOutcome
    public let code: GitErrorCode?
    public let message: String?

    public init(
        localRef: String,
        remoteRef: String,
        oldId: GitObjectId?,
        newId: GitObjectId,
        outcome: GitPushRefOutcome,
        code: GitErrorCode? = nil,
        message: String? = nil
    ) {
        self.localRef = localRef
        self.remoteRef = remoteRef
        self.oldId = oldId
        self.newId = newId
        self.outcome = outcome
        self.code = code
        self.message = message
    }
}

public struct GitPushResult: Sendable {
    public let refs: [GitPushRefResult]
    public let packBytes: Int
    public let objects: Int
}
