private let sha256RoundConstants: [UInt32] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]

public final class ForkMeshSha256: GitDigest {
    public static let maximumInputBytes = 0x1fff_ffff_ffff_ffff

    private var state: [UInt32] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    ]
    private var pending = [UInt8]()
    private let maxInputBytes: Int
    private var lengthBytes = 0
    private var closed = false
    private var words = [UInt32](repeating: 0, count: 64)

    public init() { maxInputBytes = Self.maximumInputBytes }

    public init(maxInputBytes: Int) throws {
        if maxInputBytes < 0 || maxInputBytes > Self.maximumInputBytes {
            throw GitArgumentError("maxInputBytes")
        }
        self.maxInputBytes = maxInputBytes
    }

    public func add(_ bytes: [UInt8]) throws {
        if closed { throw GitStateError("digest is closed") }
        if bytes.count > maxInputBytes - lengthBytes {
            throw GitException(.hashImplementationFailure, "SHA-256 input length exceeds supported accounting range")
        }
        lengthBytes += bytes.count
        bytes.withUnsafeBufferPointer { buffer in
            var offset = 0
            if !pending.isEmpty {
                let take = min(64 - pending.count, buffer.count)
                pending.append(contentsOf: UnsafeBufferPointer(rebasing: buffer[0..<take]))
                offset = take
                if pending.count == 64 {
                    pending.withUnsafeBufferPointer { compress($0.baseAddress!) }
                    pending.removeAll(keepingCapacity: true)
                }
            }
            while buffer.count - offset >= 64 {
                compress(buffer.baseAddress! + offset)
                offset += 64
            }
            if offset < buffer.count {
                pending.append(contentsOf: UnsafeBufferPointer(rebasing: buffer[offset...]))
            }
        }
    }

    private func compress(_ block: UnsafePointer<UInt8>) {
        words.withUnsafeMutableBufferPointer { w in
            for index in 0..<16 {
                let offset = index * 4
                w[index] = (UInt32(block[offset]) << 24) | (UInt32(block[offset + 1]) << 16)
                    | (UInt32(block[offset + 2]) << 8) | UInt32(block[offset + 3])
            }
            for index in 16..<64 {
                let x = w[index - 15]
                let y = w[index - 2]
                let sigma0 = rotateRight32(x, 7) ^ rotateRight32(x, 18) ^ (x >> 3)
                let sigma1 = rotateRight32(y, 17) ^ rotateRight32(y, 19) ^ (y >> 10)
                w[index] = w[index - 16] &+ sigma0 &+ w[index - 7] &+ sigma1
            }
            var a = state[0], b = state[1], c = state[2], d = state[3]
            var e = state[4], f = state[5], g = state[6], h = state[7]
            for index in 0..<64 {
                let sum1 = rotateRight32(e, 6) ^ rotateRight32(e, 11) ^ rotateRight32(e, 25)
                let choice = (e & f) ^ (~e & g)
                let temp1 = h &+ sum1 &+ choice &+ sha256RoundConstants[index] &+ w[index]
                let sum0 = rotateRight32(a, 2) ^ rotateRight32(a, 13) ^ rotateRight32(a, 22)
                let majority = (a & b) ^ (a & c) ^ (b & c)
                let temp2 = sum0 &+ majority
                h = g
                g = f
                f = e
                e = d &+ temp1
                d = c
                c = b
                b = a
                a = temp1 &+ temp2
            }
            state[0] = state[0] &+ a
            state[1] = state[1] &+ b
            state[2] = state[2] &+ c
            state[3] = state[3] &+ d
            state[4] = state[4] &+ e
            state[5] = state[5] &+ f
            state[6] = state[6] &+ g
            state[7] = state[7] &+ h
        }
    }

    public func close() throws -> [UInt8] {
        if closed { throw GitStateError("digest is closed") }
        closed = true
        var padded = pending
        padded.append(0x80)
        while (padded.count + 8) % 64 != 0 { padded.append(0) }
        let bits = UInt64(lengthBytes) &* 8
        for shift in stride(from: 56, through: 0, by: -8) {
            padded.append(UInt8(truncatingIfNeeded: bits >> UInt64(shift)))
        }
        padded.withUnsafeBufferPointer { buffer in
            var offset = 0
            while offset < buffer.count {
                compress(buffer.baseAddress! + offset)
                offset += 64
            }
        }
        var output = [UInt8]()
        for word in state { output.append(contentsOf: uint32Bytes(word)) }
        return output
    }
}
