import Foundation

public struct GitResourceLimits: Sendable {
    public static let defaultMaxObjectBytes = 64 * 1024 * 1024
    public static let hardMaxObjectBytes = 256 * 1024 * 1024
    public static let defaultMaxInflatedBytes = 768 * 1024 * 1024
    public static let hardMaxInflatedBytes = 1024 * 1024 * 1024
    public static let defaultMaxRefNameBytes = 255
    public static let hardMaxRefNameBytes = 1024
    public static let defaultMaxRefCount = 25000
    public static let hardMaxRefCount = 100_000
    public static let defaultMaxPktLines = 50000
    public static let hardMaxPktLines = 200_000
    public static let defaultMaxAdvertisementBytes = 1024 * 1024
    public static let hardMaxAdvertisementBytes = 4 * 1024 * 1024
    public static let defaultMaxCompressedPackBytes = 512 * 1024 * 1024
    public static let hardMaxCompressedPackBytes = 1024 * 1024 * 1024
    public static let defaultMaxResponseHeaderBytes = 64 * 1024
    public static let hardMaxResponseHeaderBytes = 128 * 1024
    public static let defaultMaxSidebandBytes = 1024 * 1024
    public static let hardMaxSidebandBytes = 4 * 1024 * 1024
    public static let defaultConnectTimeout: TimeInterval = 15
    public static let hardConnectTimeout: TimeInterval = 30
    public static let defaultIdleReadTimeout: TimeInterval = 30
    public static let hardIdleReadTimeout: TimeInterval = 60
    public static let defaultOperationDeadline: TimeInterval = 5 * 60
    public static let hardOperationDeadline: TimeInterval = 30 * 60
    public static let defaultMaxNegotiationRounds = 8
    public static let hardMaxNegotiationRounds = 32
    public static let defaultMaxCommitWalk = 250_000
    public static let hardMaxCommitWalk = 1_000_000

    public let maxObjectBytes: Int
    public let maxInflatedBytes: Int
    public let maxRefNameBytes: Int
    public let maxRefCount: Int
    /// Maximum pkt-lines accepted in one protocol response.
    public let maxPktLines: Int
    /// Maximum HTTP body accepted for a service advertisement or a receive-pack report.
    public let maxAdvertisementBytes: Int
    /// Maximum compressed pack bytes streamed from one fetch response.
    public let maxCompressedPackBytes: Int
    /// Maximum HTTP response header bytes accepted before processing.
    public let maxResponseHeaderBytes: Int
    /// Maximum sideband progress bytes retained per operation.
    public let maxSidebandBytes: Int
    /// Connect, TLS, or first-byte wait per HTTP request (seconds).
    public let connectTimeout: TimeInterval
    /// Maximum wait between two body chunks while streaming (seconds).
    public let idleReadTimeout: TimeInterval
    /// End-to-end clock for one clone, fetch, or push operation (seconds).
    public let operationDeadline: TimeInterval
    /// Maximum stateless fetch negotiation rounds before `done` is forced.
    public let maxNegotiationRounds: Int
    /// Maximum commits visited by one reachability walk.
    public let maxCommitWalk: Int

    /// The defaults; the throwing initializer only fails for out-of-range values.
    public static let standard = try! GitResourceLimits()

    public init(
        maxObjectBytes: Int = GitResourceLimits.defaultMaxObjectBytes,
        maxInflatedBytes: Int = GitResourceLimits.defaultMaxInflatedBytes,
        maxRefNameBytes: Int = GitResourceLimits.defaultMaxRefNameBytes,
        maxRefCount: Int = GitResourceLimits.defaultMaxRefCount,
        maxPktLines: Int = GitResourceLimits.defaultMaxPktLines,
        maxAdvertisementBytes: Int = GitResourceLimits.defaultMaxAdvertisementBytes,
        maxCompressedPackBytes: Int = GitResourceLimits.defaultMaxCompressedPackBytes,
        maxResponseHeaderBytes: Int = GitResourceLimits.defaultMaxResponseHeaderBytes,
        maxSidebandBytes: Int = GitResourceLimits.defaultMaxSidebandBytes,
        connectTimeout: TimeInterval = GitResourceLimits.defaultConnectTimeout,
        idleReadTimeout: TimeInterval = GitResourceLimits.defaultIdleReadTimeout,
        operationDeadline: TimeInterval = GitResourceLimits.defaultOperationDeadline,
        maxNegotiationRounds: Int = GitResourceLimits.defaultMaxNegotiationRounds,
        maxCommitWalk: Int = GitResourceLimits.defaultMaxCommitWalk
    ) throws {
        self.maxObjectBytes = maxObjectBytes
        self.maxInflatedBytes = maxInflatedBytes
        self.maxRefNameBytes = maxRefNameBytes
        self.maxRefCount = maxRefCount
        self.maxPktLines = maxPktLines
        self.maxAdvertisementBytes = maxAdvertisementBytes
        self.maxCompressedPackBytes = maxCompressedPackBytes
        self.maxResponseHeaderBytes = maxResponseHeaderBytes
        self.maxSidebandBytes = maxSidebandBytes
        self.connectTimeout = connectTimeout
        self.idleReadTimeout = idleReadTimeout
        self.operationDeadline = operationDeadline
        self.maxNegotiationRounds = maxNegotiationRounds
        self.maxCommitWalk = maxCommitWalk
        try Self.validateRange(maxObjectBytes, "maxObjectBytes", Self.hardMaxObjectBytes)
        try Self.validateRange(maxInflatedBytes, "maxInflatedBytes", Self.hardMaxInflatedBytes)
        try Self.validateRange(maxRefNameBytes, "maxRefNameBytes", Self.hardMaxRefNameBytes)
        try Self.validateRange(maxRefCount, "maxRefCount", Self.hardMaxRefCount)
        try Self.validateRange(maxPktLines, "maxPktLines", Self.hardMaxPktLines)
        try Self.validateRange(maxAdvertisementBytes, "maxAdvertisementBytes", Self.hardMaxAdvertisementBytes)
        try Self.validateRange(maxCompressedPackBytes, "maxCompressedPackBytes", Self.hardMaxCompressedPackBytes)
        try Self.validateRange(maxResponseHeaderBytes, "maxResponseHeaderBytes", Self.hardMaxResponseHeaderBytes)
        try Self.validateRange(maxSidebandBytes, "maxSidebandBytes", Self.hardMaxSidebandBytes)
        try Self.validateDuration(connectTimeout, "connectTimeout", Self.hardConnectTimeout)
        try Self.validateDuration(idleReadTimeout, "idleReadTimeout", Self.hardIdleReadTimeout)
        try Self.validateDuration(operationDeadline, "operationDeadline", Self.hardOperationDeadline)
        try Self.validateRange(maxNegotiationRounds, "maxNegotiationRounds", Self.hardMaxNegotiationRounds)
        try Self.validateRange(maxCommitWalk, "maxCommitWalk", Self.hardMaxCommitWalk)
    }

    private static func validateRange(_ value: Int, _ name: String, _ hardMaximum: Int) throws {
        if value <= 0 || value > hardMaximum {
            throw GitArgumentError(name, "must be in 1..\(hardMaximum)")
        }
    }

    private static func validateDuration(_ value: TimeInterval, _ name: String, _ hard: TimeInterval) throws {
        if !(value > 0) || value > hard {
            throw GitArgumentError(name, "must be positive and <= \(hard)s")
        }
    }
}
