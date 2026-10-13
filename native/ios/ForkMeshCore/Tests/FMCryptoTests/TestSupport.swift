import XCTest
@testable import FMCrypto

func unhex(_ text: String, file: StaticString = #filePath, line: UInt = #line) -> [UInt8] {
    guard let bytes = CryptoBytes.fromHex(text) else {
        XCTFail("bad hex literal", file: file, line: line)
        return []
    }
    return bytes
}

func ascii(_ text: String) -> [UInt8] { Array(text.utf8) }

func assertHex(_ expected: String, _ actual: [UInt8], _ message: String = "", file: StaticString = #filePath, line: UInt = #line) {
    XCTAssertEqual(expected, CryptoBytes.hex(actual), message, file: file, line: line)
}

/// `java.util.Random`, so vectors the Kotlin tests derive from a seeded generator carry over.
struct JavaRandom {
    private var seed: UInt64

    init(_ seed: Int64) {
        self.seed = (UInt64(bitPattern: seed) ^ 0x5DEECE66D) & ((1 << 48) - 1)
    }

    mutating func next(_ bits: Int) -> Int32 {
        seed = (seed &* 0x5DEECE66D &+ 0xB) & ((1 << 48) - 1)
        return Int32(truncatingIfNeeded: Int64(seed >> UInt64(48 - bits)))
    }

    mutating func nextInt(_ bound: Int32) -> Int {
        if bound & (bound - 1) == 0 { return Int((Int64(bound) * Int64(next(31))) >> 31) }
        while true {
            let bits = next(31)
            let value = bits % bound
            if bits - value + (bound - 1) >= 0 { return Int(value) }
        }
    }

    mutating func nextBytes(_ count: Int) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: count)
        var i = 0
        while i < count {
            var rnd = next(32)
            var n = min(count - i, 4)
            while n > 0 {
                out[i] = UInt8(truncatingIfNeeded: rnd)
                rnd >>= 8
                i += 1
                n -= 1
            }
        }
        return out
    }
}
