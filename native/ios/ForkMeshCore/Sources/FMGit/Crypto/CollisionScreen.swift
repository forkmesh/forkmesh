public enum GitSha1ObjectOrigin: Sendable { case trustedLocal, untrustedTransport }

public enum GitSha1CollisionDetectorStatus: Sendable { case clear, detected, unavailable, failed }

public protocol GitSha1CollisionDetector: AnyObject {
    func start() throws -> GitSha1CollisionDetectorSession
}

public protocol GitSha1CollisionDetectorSession: AnyObject {
    func add(_ bytes: [UInt8]) throws
    func add(_ bytes: ArraySlice<UInt8>) throws
    func close() throws -> GitSha1CollisionDetectorStatus
    func dispose() throws
}

extension GitSha1CollisionDetectorSession {
    public func add(_ bytes: ArraySlice<UInt8>) throws { try add(Array(bytes)) }
}

public enum GitSha1ScreeningDecision: Sendable {
    case approved
    case collisionDetected
    case malformedFrame
    case resourceLimitExceeded
    case screeningUnavailable
    case cancelled
}

public struct GitSha1ScreeningResult: Sendable {
    public let decision: GitSha1ScreeningDecision
    public let digest: [UInt8]
    public let inspectedBytes: Int

    fileprivate init(_ decision: GitSha1ScreeningDecision, digest: [UInt8], inspectedBytes: Int) {
        self.decision = decision
        self.digest = digest
        self.inspectedBytes = inspectedBytes
    }

    public var approved: Bool { decision == .approved }

    /// The typed error of a rejected result. Asking an approved result for
    /// its error is a programmer error (`GitStateError`).
    public func toException() throws -> GitException {
        switch decision {
        case .approved:
            throw GitStateError("approved screening result has no error")
        case .collisionDetected:
            return GitException(.sha1CollisionDetected, "SHA-1 collision screening rejected the object")
        case .malformedFrame:
            return GitException(.invalidObject, "object frame does not match screening metadata")
        case .resourceLimitExceeded:
            return GitException(.resourceLimitExceeded, "object frame exceeds screening limit")
        case .screeningUnavailable:
            return GitException(.hashScreeningUnavailable, "untrusted SHA-1 screening is unavailable")
        case .cancelled:
            return GitException(.cancelled, "SHA-1 screening was cancelled")
        }
    }

    /// Throws the rejection; returns normally only when approved.
    func requireApproved() throws {
        if !approved { throw try toException() }
    }
}

/// Bounded full-frame SHA-1 screening boundary.
///
/// Untrusted SHA-1 data returns `screeningUnavailable` unless an injected
/// detector reports a clear result for the complete canonical frame.
public final class Sha1CollisionScreen {
    private static let screeningChunkBytes = 64 * 1024

    public let detector: GitSha1CollisionDetector?

    public init(detector: GitSha1CollisionDetector? = nil) { self.detector = detector }

    public var hasGeneralizedDetector: Bool { detector != nil }

    public func start(
        objectType: GitObjectType,
        declaredBodySize: Int,
        origin: GitSha1ObjectOrigin,
        limits: GitResourceLimits,
        cancellation: GitCancellationToken? = nil
    ) -> GitSha1ScreeningSession {
        GitSha1ScreeningSession(
            objectType: objectType,
            declaredBodySize: declaredBodySize,
            origin: origin,
            limits: limits,
            cancellation: cancellation,
            detector: detector
        )
    }

    public func screenFrame(
        frame: [UInt8],
        objectType: GitObjectType,
        declaredBodySize: Int,
        origin: GitSha1ObjectOrigin,
        limits: GitResourceLimits,
        cancellation: GitCancellationToken? = nil
    ) throws -> GitSha1ScreeningResult {
        let session = start(
            objectType: objectType,
            declaredBodySize: declaredBodySize,
            origin: origin,
            limits: limits,
            cancellation: cancellation
        )
        var offset = 0
        while offset < frame.count {
            let end = min(offset + Self.screeningChunkBytes, frame.count)
            try session.add(frame[offset..<end])
            offset = end
            if cancellation?.isCancelled ?? false { break }
        }
        return try session.close()
    }

    public func screenObject(
        type: GitObjectType,
        body: [UInt8],
        origin: GitSha1ObjectOrigin,
        limits: GitResourceLimits,
        cancellation: GitCancellationToken? = nil
    ) throws -> GitSha1ScreeningResult {
        let session = start(
            objectType: type,
            declaredBodySize: body.count,
            origin: origin,
            limits: limits,
            cancellation: cancellation
        )
        try session.add(Array("\(type.name) \(body.count)\u{0}".utf8))
        var offset = 0
        while offset < body.count {
            if cancellation?.isCancelled ?? false { break }
            let end = min(offset + Self.screeningChunkBytes, body.count)
            try session.add(body[offset..<end])
            offset = end
        }
        return try session.close()
    }
}

public final class GitSha1ScreeningSession {
    public let objectType: GitObjectType
    public let declaredBodySize: Int
    public let origin: GitSha1ObjectOrigin
    public let limits: GitResourceLimits
    public let cancellation: GitCancellationToken?
    public let detector: GitSha1CollisionDetector?
    private let digest = ForkMeshSha1()
    private var frame = [UInt8]()
    private var detectorSession: GitSha1CollisionDetectorSession?
    private var inspectedBytes = 0
    private var limitExceeded = false
    private var detectorFailed = false
    private var closed = false

    fileprivate init(
        objectType: GitObjectType,
        declaredBodySize: Int,
        origin: GitSha1ObjectOrigin,
        limits: GitResourceLimits,
        cancellation: GitCancellationToken?,
        detector: GitSha1CollisionDetector?
    ) {
        self.objectType = objectType
        self.declaredBodySize = declaredBodySize
        self.origin = origin
        self.limits = limits
        self.cancellation = cancellation
        self.detector = detector
        do {
            detectorSession = try detector?.start()
        } catch {
            detectorFailed = true
        }
    }

    public func add(_ bytes: [UInt8]) throws {
        try add(bytes[...])
    }

    public func add(_ bytes: ArraySlice<UInt8>) throws {
        if closed { throw GitStateError("screening session is closed") }
        if cancellation?.isCancelled ?? false { return }
        let maximumFrameBytes = limits.maxObjectBytes + 128
        if bytes.count > maximumFrameBytes - inspectedBytes {
            limitExceeded = true
            return
        }
        try digest.add(bytes)
        // Only the header is compared at close; keeping the whole frame would
        // double the memory of every screened object.
        if frame.count < 64 { frame.append(contentsOf: bytes.prefix(64 - frame.count)) }
        do {
            try detectorSession?.add(bytes)
        } catch {
            detectorFailed = true
        }
        inspectedBytes += bytes.count
    }

    public func close() throws -> GitSha1ScreeningResult {
        if closed { throw GitStateError("screening session is closed") }
        closed = true
        defer {
            // A failed native cleanup cannot turn a rejected object into approval.
            try? detectorSession?.dispose()
        }
        if cancellation?.isCancelled ?? false {
            return GitSha1ScreeningResult(.cancelled, digest: [], inspectedBytes: inspectedBytes)
        }
        if limitExceeded || declaredBodySize > limits.maxObjectBytes {
            return GitSha1ScreeningResult(.resourceLimitExceeded, digest: [], inspectedBytes: inspectedBytes)
        }
        let digestBytes = try digest.close()
        let header = Array("\(objectType.name) \(declaredBodySize)\u{0}".utf8)
        if declaredBodySize < 0
            || inspectedBytes != header.count + declaredBodySize
            || !sameHeader(frame, header)
        {
            return GitSha1ScreeningResult(.malformedFrame, digest: digestBytes, inspectedBytes: inspectedBytes)
        }
        let detectorStatus = finishDetector()
        if detectorStatus == .detected {
            return GitSha1ScreeningResult(.collisionDetected, digest: digestBytes, inspectedBytes: inspectedBytes)
        }
        if origin == .untrustedTransport && detectorStatus != .clear {
            return GitSha1ScreeningResult(.screeningUnavailable, digest: digestBytes, inspectedBytes: inspectedBytes)
        }
        return GitSha1ScreeningResult(.approved, digest: digestBytes, inspectedBytes: inspectedBytes)
    }

    private func finishDetector() -> GitSha1CollisionDetectorStatus {
        if detectorFailed { return .failed }
        guard let session = detectorSession else { return .unavailable }
        do {
            return try session.close()
        } catch {
            return .failed
        }
    }
}

private func sameHeader(_ frame: [UInt8], _ header: [UInt8]) -> Bool {
    if frame.count < header.count { return false }
    var different: UInt8 = 0
    for index in 0..<header.count { different |= frame[index] ^ header[index] }
    return different == 0
}
