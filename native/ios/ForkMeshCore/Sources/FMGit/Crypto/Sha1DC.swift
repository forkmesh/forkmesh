import CSha1DC

public final class GitSha1DCDetector: GitSha1CollisionDetector, @unchecked Sendable {
    public init() {}

    public func start() throws -> GitSha1CollisionDetectorSession { GitSha1DCSession() }
}

final class GitSha1DCSession: GitSha1CollisionDetectorSession {
    private var context: UnsafeMutablePointer<SHA1_CTX>?

    init() {
        let context = UnsafeMutablePointer<SHA1_CTX>.allocate(capacity: 1)
        SHA1DCInit(context)
        SHA1DCSetSafeHash(context, 0)
        self.context = context
    }

    deinit { try? dispose() }

    func add(_ bytes: [UInt8]) throws {
        try add(bytes[...])
    }

    func add(_ bytes: ArraySlice<UInt8>) throws {
        guard let context else { throw GitStateError("SHA-1 screening session is closed") }
        bytes.withUnsafeBytes { buffer in
            guard let base = buffer.baseAddress else { return }
            SHA1DCUpdate(context, base.assumingMemoryBound(to: CChar.self), buffer.count)
        }
    }

    func close() throws -> GitSha1CollisionDetectorStatus {
        guard let context else { return .failed }
        var digest = [UInt8](repeating: 0, count: 20)
        let collision = SHA1DCFinal(&digest, context)
        try dispose()
        return collision == 0 ? .clear : .detected
    }

    func dispose() throws {
        guard let context else { return }
        context.deallocate()
        self.context = nil
    }
}
