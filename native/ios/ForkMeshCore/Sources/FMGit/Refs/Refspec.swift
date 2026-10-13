/// A validated fetch refspec of the form
/// `[+]refs/heads/<pattern>:refs/remotes/<remote>/<pattern>` where both
/// sides are either the same literal branch or end in a single `*`.
///
/// Only the remote-tracking namespace may be written by a fetch refspec, so
/// a refspec can never rewrite `refs/heads/*` or `refs/tags/*` locally.
public struct GitFetchRefspec: Sendable {
    public let source: String
    public let destination: String

    /// Fetch refspecs are always applied as forced remote-tracking updates;
    /// this records whether the `+` prefix was written.
    public let force: Bool

    public var isWildcard: Bool { source.u8HasSuffix("/*") }

    private init(_ source: String, _ destination: String, _ force: Bool) {
        self.source = source
        self.destination = destination
        self.force = force
    }

    public static func parse(_ value: String) throws -> GitFetchRefspec {
        var text = value
        var force = false
        if text.u8HasPrefix("+") {
            force = true
            text = text.u8Substring(1)
        }
        let bytes = Array(text.utf8)
        let separator = bytes.indexOfByte(0x3a)
        let colons = bytes.filter { $0 == 0x3a }.count
        if separator <= 0 || colons != 1 {
            throw GitException(.invalidRef, "fetch refspec must have one source and one destination")
        }
        let source = text.u8Substring(0, separator)
        let destination = text.u8Substring(separator + 1)
        let sourceWildcard = source.u8HasSuffix("/*")
        let destinationWildcard = destination.u8HasSuffix("/*")
        if sourceWildcard != destinationWildcard
            || !source.u8HasPrefix("refs/heads/")
            || !destination.u8HasPrefix("refs/remotes/")
            || source.utf8.filter({ $0 == 0x2a }).count > 1
            || destination.utf8.filter({ $0 == 0x2a }).count > 1
        {
            throw GitException(.invalidRef, "fetch refspec must map refs/heads to refs/remotes")
        }
        let sourceBase = sourceWildcard ? source.u8Substring(0, source.utf8.count - 2) : source
        let destinationBase = destinationWildcard
            ? destination.u8Substring(0, destination.utf8.count - 2) : destination
        _ = try GitRefName.parse(sourceWildcard ? "\(sourceBase)/x" : sourceBase)
        _ = try GitRefName.parse(destinationWildcard ? "\(destinationBase)/x" : destinationBase)
        let remotesPrefix = "refs/remotes/".utf8.count
        if destinationWildcard
            && (destinationBase.utf8.count <= remotesPrefix
                || destinationBase.u8Substring(remotesPrefix).u8Contains("/"))
        {
            throw GitException(.invalidRef, "fetch refspec destination must be refs/remotes/<remote>/*")
        }
        return GitFetchRefspec(source, destination, force)
    }

    /// The standard refspec for `remote`.
    public static func standard(_ remote: String) throws -> GitFetchRefspec {
        try GitRemoteName.validate(remote)
        return GitFetchRefspec("refs/heads/*", "refs/remotes/\(remote)/*", true)
    }

    /// Maps a remote ref name to the local tracking ref, or nil when the
    /// refspec does not match.
    public func destinationFor(_ remoteRef: String) -> String? {
        if !isWildcard { return remoteRef.sameText(source) ? destination : nil }
        let prefix = source.u8Substring(0, source.utf8.count - 1)
        if !remoteRef.u8HasPrefix(prefix) { return nil }
        let tail = remoteRef.u8Substring(prefix.utf8.count)
        if tail.utf8.isEmpty { return nil }
        return destination.u8Substring(0, destination.utf8.count - 1) + tail
    }

    public var value: String { "\(force ? "+" : "")\(source):\(destination)" }
}

/// A validated remote name: `[A-Za-z0-9][A-Za-z0-9._-]{0,63}` without `..`.
public enum GitRemoteName {
    public static func validate(_ name: String) throws {
        let bytes = Array(name.utf8)
        func alphanumeric(_ byte: UInt8) -> Bool {
            (byte >= 0x30 && byte <= 0x39) || (byte >= 0x41 && byte <= 0x5a) || (byte >= 0x61 && byte <= 0x7a)
        }
        let shaped = !bytes.isEmpty && bytes.count <= 64 && alphanumeric(bytes[0])
            && bytes.dropFirst().allSatisfy { alphanumeric($0) || $0 == 0x2e || $0 == 0x5f || $0 == 0x2d }
        if !shaped || name.u8Contains("..") || name.u8HasSuffix(".lock") {
            throw GitException(.invalidRef, "remote name is invalid")
        }
    }
}

/// A push refspec mapping a local branch to a remote branch.
///
/// Only `refs/heads/<local>:refs/heads/<remote>` is expressible; deletions
/// (an empty source) and force markers are rejected by construction.
public struct GitPushRefspec: Sendable {
    public let local: GitRefName
    public let remote: GitRefName

    public init(localBranch: String, remoteBranch: String? = nil) throws {
        local = try GitRefName.branch("refs/heads/\(localBranch)")
        remote = try GitRefName.branch("refs/heads/\(remoteBranch ?? localBranch)")
    }

    public static func parse(_ value: String) throws -> GitPushRefspec {
        if value.u8HasPrefix("+") || value.u8HasPrefix(":") {
            throw GitException(.invalidRef, "push refspec may not force or delete")
        }
        let separator = value.u8IndexOf(":")
        let source = separator == -1 ? value : value.u8Substring(0, separator)
        let destination = separator == -1 ? value : value.u8Substring(separator + 1)
        if source.utf8.isEmpty || destination.utf8.isEmpty || source.u8Contains("*") || destination.u8Contains("*") {
            throw GitException(.invalidRef, "push refspec must name one local and one remote branch")
        }
        return try GitPushRefspec(localBranch: stripHeads(source), remoteBranch: stripHeads(destination))
    }

    private static func stripHeads(_ value: String) -> String {
        value.u8HasPrefix("refs/heads/") ? value.u8Substring(11) : value
    }

    public var localBranch: String { local.value.u8Substring(11) }
    public var remoteBranch: String { remote.value.u8Substring(11) }
}
