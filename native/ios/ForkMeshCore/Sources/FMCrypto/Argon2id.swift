// Argon2id version 0x13 (RFC 9106) in pure Swift, as the vault uses it to stretch the vault
// password. There is no Kotlin counterpart (native/android does not port the vault) and no OS
// implementation, so this is the path on every platform.
//
// Lanes are filled one after another on the calling thread; the result does not depend on the
// order. Callers run it off the main thread: at the vault's default
// 64 MiB and 3 passes it is seconds of work.

public enum Argon2id {
    /// The tag of `password` under `salt`. `memoryKiB` is the block count (1 KiB blocks),
    /// `iterations` the pass count. `secret` and `associatedData` are RFC 9106's K and X, which
    /// the vault leaves empty.
    public static func deriveKey(
        password: [UInt8],
        salt: [UInt8],
        parallelism: Int,
        memoryKiB: Int,
        iterations: Int,
        hashLength: Int,
        secret: [UInt8] = [],
        associatedData: [UInt8] = []
    ) throws -> [UInt8] {
        guard parallelism >= 1 && parallelism < (1 << 24) else { throw CryptoArgumentError("Argon2 parallelism out of range") }
        guard memoryKiB >= 8 * parallelism && memoryKiB <= Int(UInt32.max) else { throw CryptoArgumentError("Argon2 memory out of range") }
        guard iterations >= 1 && iterations <= Int(UInt32.max) else { throw CryptoArgumentError("Argon2 iterations out of range") }
        guard hashLength >= 4 && hashLength <= Int(UInt32.max) else { throw CryptoArgumentError("Argon2 hash length out of range") }
        guard salt.count >= 8 else { throw CryptoArgumentError("Argon2 salt must be at least 8 bytes") }

        let lanes = parallelism
        let segmentLength = memoryKiB / (4 * lanes)
        let laneLength = segmentLength * 4
        let blockCount = laneLength * lanes

        var h0 = Blake2b(outputLength: 64)
        for value in [lanes, hashLength, memoryKiB, iterations, 0x13, 2] { h0.update(le32(value)) }
        for field in [password, salt, secret, associatedData] {
            h0.update(le32(field.count))
            h0.update(field)
        }
        var seed = h0.finalize()

        let words = blockCount * 128
        let memory = UnsafeMutablePointer<UInt64>.allocate(capacity: words)
        memory.initialize(repeating: 0, count: words)
        // Scratch: R, the saved input, and for data-independent addressing the zero, input and
        // address blocks.
        let scratch = UnsafeMutablePointer<UInt64>.allocate(capacity: 5 * 128)
        scratch.initialize(repeating: 0, count: 5 * 128)
        defer {
            memory.update(repeating: 0, count: words)
            memory.deallocate()
            scratch.update(repeating: 0, count: 5 * 128)
            scratch.deallocate()
        }

        for lane in 0..<lanes {
            for index in 0..<2 {
                var block = longHash(1024, seed + le32(index) + le32(lane))
                load(block, into: memory + (lane * laneLength + index) * 128)
                CryptoBytes.wipe(&block)
            }
        }
        CryptoBytes.wipe(&seed)

        let zero = scratch + 2 * 128
        let input = scratch + 3 * 128
        let address = scratch + 4 * 128
        for pass in 0..<iterations {
            for slice in 0..<4 {
                // Argon2id: the first half of the first pass is addressed independently of the data.
                let independent = pass == 0 && slice < 2
                for lane in 0..<lanes {
                    if independent {
                        input.update(repeating: 0, count: 128)
                        input[0] = UInt64(pass)
                        input[1] = UInt64(lane)
                        input[2] = UInt64(slice)
                        input[3] = UInt64(blockCount)
                        input[4] = UInt64(iterations)
                        input[5] = 2
                    }
                    var start = 0
                    if pass == 0 && slice == 0 {
                        start = 2
                        if independent { nextAddresses(address, input, zero, scratch) }
                    }
                    var current = lane * laneLength + slice * segmentLength + start
                    var previous = current % laneLength == 0 ? current + laneLength - 1 : current - 1
                    var i = start
                    while i < segmentLength {
                        if current % laneLength == 1 { previous = current - 1 }
                        let random: UInt64
                        if independent {
                            if i % 128 == 0 { nextAddresses(address, input, zero, scratch) }
                            random = address[i % 128]
                        } else {
                            random = memory[previous * 128]
                        }
                        var referenceLane = Int((random >> 32) % UInt64(lanes))
                        if pass == 0 && slice == 0 { referenceLane = lane }
                        let sameLane = referenceLane == lane
                        var area: Int
                        if pass == 0 {
                            if slice == 0 {
                                area = i - 1
                            } else if sameLane {
                                area = slice * segmentLength + i - 1
                            } else {
                                area = slice * segmentLength + (i == 0 ? -1 : 0)
                            }
                        } else if sameLane {
                            area = laneLength - segmentLength + i - 1
                        } else {
                            area = laneLength - segmentLength + (i == 0 ? -1 : 0)
                        }
                        let j1 = random & 0xffff_ffff
                        let x = (j1 &* j1) >> 32
                        let relative = UInt64(area) &- 1 &- ((UInt64(area) &* x) >> 32)
                        let startPosition = pass != 0 && slice != 3 ? (slice + 1) * segmentLength : 0
                        let reference = (startPosition + Int(relative)) % laneLength
                        fillBlock(
                            previous: memory + previous * 128,
                            reference: memory + (referenceLane * laneLength + reference) * 128,
                            next: memory + current * 128,
                            withXor: pass != 0,
                            scratch: scratch)
                        i += 1
                        current += 1
                        previous += 1
                    }
                }
            }
        }

        let final = scratch
        for w in 0..<128 { final[w] = 0 }
        for lane in 0..<lanes {
            let last = memory + (lane * laneLength + laneLength - 1) * 128
            for w in 0..<128 { final[w] ^= last[w] }
        }
        var bytes = [UInt8](repeating: 0, count: 1024)
        for w in 0..<128 {
            for b in 0..<8 { bytes[8 * w + b] = UInt8(truncatingIfNeeded: final[w] >> UInt64(8 * b)) }
        }
        let tag = longHash(hashLength, bytes)
        CryptoBytes.wipe(&bytes)
        return tag
    }

    private static func le32(_ value: Int) -> [UInt8] {
        let v = UInt32(truncatingIfNeeded: value)
        return [UInt8(truncatingIfNeeded: v), UInt8(truncatingIfNeeded: v >> 8), UInt8(truncatingIfNeeded: v >> 16), UInt8(truncatingIfNeeded: v >> 24)]
    }

    private static func load(_ bytes: [UInt8], into block: UnsafeMutablePointer<UInt64>) {
        for w in 0..<128 {
            var word: UInt64 = 0
            for b in 0..<8 { word |= UInt64(bytes[8 * w + b]) << UInt64(8 * b) }
            block[w] = word
        }
    }

    /// H' of RFC 9106 §3.3: BLAKE2b stretched to any length.
    static func longHash(_ length: Int, _ input: [UInt8]) -> [UInt8] {
        if length <= 64 { return Blake2b.hash(outputLength: length, le32(length), input) }
        var out = [UInt8]()
        out.reserveCapacity(length)
        var v = Blake2b.hash(outputLength: 64, le32(length), input)
        out.append(contentsOf: v[0..<32])
        var remaining = length - 32
        while remaining > 64 {
            v = Blake2b.hash(outputLength: 64, v)
            out.append(contentsOf: v[0..<32])
            remaining -= 32
        }
        out.append(contentsOf: Blake2b.hash(outputLength: remaining, v))
        CryptoBytes.wipe(&v)
        return out
    }

    private static func nextAddresses(
        _ address: UnsafeMutablePointer<UInt64>, _ input: UnsafeMutablePointer<UInt64>,
        _ zero: UnsafeMutablePointer<UInt64>, _ scratch: UnsafeMutablePointer<UInt64>
    ) {
        input[6] &+= 1
        fillBlock(previous: zero, reference: input, next: address, withXor: false, scratch: scratch)
        fillBlock(previous: zero, reference: address, next: address, withXor: false, scratch: scratch)
    }

    @inline(__always)
    private static func mix(_ v: UnsafeMutablePointer<UInt64>, _ ai: Int, _ bi: Int, _ ci: Int, _ di: Int) {
        var a = v[ai], b = v[bi], c = v[ci], d = v[di]
        a = a &+ b &+ 2 &* (a & 0xffff_ffff) &* (b & 0xffff_ffff)
        d ^= a; d = d >> 32 | d << 32
        c = c &+ d &+ 2 &* (c & 0xffff_ffff) &* (d & 0xffff_ffff)
        b ^= c; b = b >> 24 | b << 40
        a = a &+ b &+ 2 &* (a & 0xffff_ffff) &* (b & 0xffff_ffff)
        d ^= a; d = d >> 16 | d << 48
        c = c &+ d &+ 2 &* (c & 0xffff_ffff) &* (d & 0xffff_ffff)
        b ^= c; b = b >> 63 | b << 1
        v[ai] = a; v[bi] = b; v[ci] = c; v[di] = d
    }

    @inline(__always)
    private static func round(
        _ v: UnsafeMutablePointer<UInt64>,
        _ i0: Int, _ i1: Int, _ i2: Int, _ i3: Int, _ i4: Int, _ i5: Int, _ i6: Int, _ i7: Int,
        _ i8: Int, _ i9: Int, _ i10: Int, _ i11: Int, _ i12: Int, _ i13: Int, _ i14: Int, _ i15: Int
    ) {
        mix(v, i0, i4, i8, i12)
        mix(v, i1, i5, i9, i13)
        mix(v, i2, i6, i10, i14)
        mix(v, i3, i7, i11, i15)
        mix(v, i0, i5, i10, i15)
        mix(v, i1, i6, i11, i12)
        mix(v, i2, i7, i8, i13)
        mix(v, i3, i4, i9, i14)
    }

    /// The compression function G of RFC 9106 §3.5; `next` may alias `reference`.
    private static func fillBlock(
        previous: UnsafeMutablePointer<UInt64>, reference: UnsafeMutablePointer<UInt64>,
        next: UnsafeMutablePointer<UInt64>, withXor: Bool, scratch: UnsafeMutablePointer<UInt64>
    ) {
        let r = scratch
        let saved = scratch + 128
        for w in 0..<128 {
            let value = reference[w] ^ previous[w]
            r[w] = value
            saved[w] = withXor ? value ^ next[w] : value
        }
        for i in 0..<8 {
            let o = 16 * i
            round(r, o, o + 1, o + 2, o + 3, o + 4, o + 5, o + 6, o + 7, o + 8, o + 9, o + 10, o + 11, o + 12, o + 13, o + 14, o + 15)
        }
        for i in 0..<8 {
            let o = 2 * i
            round(r, o, o + 1, o + 16, o + 17, o + 32, o + 33, o + 48, o + 49, o + 64, o + 65, o + 80, o + 81, o + 96, o + 97, o + 112, o + 113)
        }
        for w in 0..<128 { next[w] = saved[w] ^ r[w] }
    }
}
