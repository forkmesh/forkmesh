import CSha1DC

public final class ForkMeshSha1: GitDigest {
    public static let maximumInputBytes = 0x1fff_ffff_ffff_ffff

    private let maxInputBytes: Int
    private var lengthBytes = 0
    private var closed = false
    private let context: UnsafeMutablePointer<SHA1_CTX>

    public init() {
        maxInputBytes = Self.maximumInputBytes
        context = Self.plainContext()
    }

    public init(maxInputBytes: Int) throws {
        if maxInputBytes < 0 || maxInputBytes > Self.maximumInputBytes {
            throw GitArgumentError("maxInputBytes")
        }
        self.maxInputBytes = maxInputBytes
        context = Self.plainContext()
    }

    deinit {
        context.deallocate()
    }

    private static func plainContext() -> UnsafeMutablePointer<SHA1_CTX> {
        let context = UnsafeMutablePointer<SHA1_CTX>.allocate(capacity: 1)
        SHA1DCInit(context)
        SHA1DCSetSafeHash(context, 0)
        SHA1DCSetUseDetectColl(context, 0)
        SHA1DCSetUseUBC(context, 0)
        return context
    }

    public func add(_ bytes: [UInt8]) throws {
        try bytes.withUnsafeBufferPointer { try add($0) }
    }

    public func add(_ bytes: ArraySlice<UInt8>) throws {
        try bytes.withUnsafeBufferPointer { try add($0) }
    }

    private func add(_ bytes: UnsafeBufferPointer<UInt8>) throws {
        if closed { throw GitStateError("digest is closed") }
        if bytes.count > maxInputBytes - lengthBytes {
            throw GitException(.hashImplementationFailure, "SHA-1 input length exceeds supported accounting range")
        }
        lengthBytes += bytes.count
        guard let base = bytes.baseAddress, bytes.count > 0 else { return }
        SHA1DCUpdate(context, UnsafeRawPointer(base).assumingMemoryBound(to: CChar.self), bytes.count)
    }

    public func close() throws -> [UInt8] {
        if closed { throw GitStateError("digest is closed") }
        closed = true
        var digest = [UInt8](repeating: 0, count: 20)
        digest.withUnsafeMutableBufferPointer { buffer in
            _ = SHA1DCFinal(buffer.baseAddress!, context)
        }
        return digest
    }

    /// One-shot digest of `bytes`.
    public static func hash(_ bytes: [UInt8]) -> [UInt8] {
        let digest = ForkMeshSha1()
        // Neither call can fail on a fresh digest below the accounting limit.
        try! digest.add(bytes)
        return try! digest.close()
    }
}
