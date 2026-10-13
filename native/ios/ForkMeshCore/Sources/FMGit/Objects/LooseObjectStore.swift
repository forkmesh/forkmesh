import Foundation

public final class LooseObjectStore {
    public let gitDirectory: String
    public let limits: GitResourceLimits
    public let detector: GitSha1CollisionDetector?

    public init(_ gitDirectory: String, _ limits: GitResourceLimits, detector: GitSha1CollisionDetector? = nil) {
        self.gitDirectory = gitDirectory
        self.limits = limits
        self.detector = detector
    }

    public func fileFor(_ id: GitObjectId) -> String {
        let hex = id.hex
        return GitFS.join(GitFS.join(GitFS.join(gitDirectory, "objects"), String(hex.prefix(2))), String(hex.dropFirst(2)))
    }

    @discardableResult
    public func write(_ type: GitObjectType, _ body: [UInt8]) throws -> GitObjectId {
        if body.count > limits.maxObjectBytes {
            throw GitException(.resourceLimitExceeded, "object exceeds configured limit")
        }
        let frame = GitObjectFrame.encode(type, body)
        let id = GitObjectId.sha1Of(frame)
        let destination = fileFor(id)
        try GitFS.createDirectories(GitFS.parent(destination))
        if GitFS.fileExists(destination) { return id }
        let temporary = "\(destination).tmp-\(GitFS.nowMicros())"
        defer {
            if GitFS.fileExists(temporary) { try? GitFS.deleteFile(temporary) }
        }
        try GitFS.writeBytesFlushed(temporary, try GitZlib.deflate(frame))
        do {
            try GitFS.rename(temporary, destination)
        } catch {
            // Another writer may have installed the same object first.
            if !GitFS.fileExists(destination) { throw error }
        }
        return id
    }

    public func read(
        _ expectedId: GitObjectId,
        origin: GitSha1ObjectOrigin = .trustedLocal,
        cancellation: GitCancellationToken? = nil
    ) throws -> GitObjectFrame {
        let source = fileFor(expectedId)
        if !GitFS.fileExists(source) {
            throw GitException(.invalidObject, "object is absent")
        }
        let compressed = try GitFS.readBytes(source)
        let frameBytes = try inflateBounded(compressed, limits.maxObjectBytes + 128)
        let frame = try GitObjectFrame.decode(frameBytes, limits)
        let screening = try Sha1CollisionScreen(detector: detector).screenFrame(
            frame: frameBytes,
            objectType: frame.type,
            declaredBodySize: frame.body.count,
            origin: origin,
            limits: limits,
            cancellation: cancellation
        )
        try screening.requireApproved()
        let actualId = try GitObjectId.fromSha1Bytes(screening.digest)
        if actualId != expectedId {
            throw GitException(.objectHashMismatch, "loose object path does not match its contents")
        }
        return frame
    }
}

/// Inflates a whole zlib stream, failing with `resourceLimitExceeded` as soon
/// as the output would pass `maximumBytes`.
private func inflateBounded(_ compressed: [UInt8], _ maximumBytes: Int) throws -> [UInt8] {
    do {
        let inflated = try GitZlib.inflate(compressed, maximumOutput: maximumBytes)
        // Bytes after the stream would be data no Git ever wrote there.
        if inflated.consumed != compressed.count {
            throw GitException(.invalidObject, "object zlib stream is invalid")
        }
        return inflated.output
    } catch GitZlibFailure.outputLimitExceeded {
        throw GitException(.resourceLimitExceeded, "inflated object exceeds configured limit")
    } catch is GitZlibFailure {
        throw GitException(.invalidObject, "object zlib stream is invalid")
    }
}
