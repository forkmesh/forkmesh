/// Typed failure codes; `wireName` is the spelling shared with the Kotlin
/// engine (logs, telemetry and the agent tool results carry it).
public enum GitErrorCode: String, CaseIterable, Sendable {
    case invalidRepository
    case unsupportedRepositoryFormat
    case invalidObject
    case objectHashMismatch
    case invalidTree
    case invalidRef
    case refLocked
    case indexCorrupt
    case packCorrupt
    case packChecksumMismatch
    case deltaDepthExceeded
    case resourceLimitExceeded
    case pathEscapesWorktree
    case symlinkEscape
    case networkUnavailable
    case tlsRejected
    case authenticationFailed
    case protocolUnsupported
    case remoteRejected
    case nonFastForward
    case cancelled
    case operationInterrupted
    case sha1CollisionDetected
    case hashScreeningUnavailable
    case hashImplementationFailure
    case quarantineRejected

    public var wireName: String { rawValue }
}

public struct GitException: Error, Equatable, CustomStringConvertible, Sendable {
    public let code: GitErrorCode
    public let message: String

    public init(_ code: GitErrorCode, _ message: String) {
        self.code = code
        self.message = message
    }

    public var description: String {
        "GitException(code: \(code.wireName), message: \(message))"
    }
}

/// A caller passed a value the API does not accept.
/// Kept distinct from `GitException` because tests and callers
/// tell programmer errors from repository or remote failures.
public struct GitArgumentError: Error, Equatable, CustomStringConvertible, Sendable {
    public let name: String
    public let message: String

    public init(_ name: String, _ message: String = "invalid argument") {
        self.name = name
        self.message = message
    }

    public var description: String { "GitArgumentError(\(name)): \(message)" }
}

/// An object was used after it was closed.
public struct GitStateError: Error, Equatable, CustomStringConvertible, Sendable {
    public let message: String

    public init(_ message: String) { self.message = message }

    public var description: String { "GitStateError: \(message)" }
}

/// A decoder rejected its input; not mapped to a `GitException` code.
public struct GitFormatError: Error, Equatable, CustomStringConvertible, Sendable {
    public let message: String

    public init(_ message: String) { self.message = message }

    public var description: String { "GitFormatError: \(message)" }
}
