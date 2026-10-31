import Foundation

enum FaceStreamBits {
    static let probOne: UInt16 = 4096
    static let probHalf: UInt16 = 2048
    static let top: UInt32 = 1 << 24
    static let maxLength = 16
    static let maxInt = 65535
    static let maxTrim = 8
}

struct FaceStreamIntModel {
    var zero: [UInt16]
    var sign: [UInt16] = [FaceStreamBits.probHalf]
    var length = [UInt16](repeating: FaceStreamBits.probHalf, count: FaceStreamBits.maxLength)
    var mant = [UInt16](repeating: FaceStreamBits.probHalf, count: FaceStreamBits.maxLength)

    init(zeroPrior: UInt16 = FaceStreamBits.probHalf) {
        zero = [zeroPrior, zeroPrior]
    }
}

func faceStreamClamp(_ value: Int, _ low: Int, _ high: Int) -> Int {
    value < low ? low : value > high ? high : value
}

struct FaceStreamRangeEncoder {
    private var low: UInt64 = 0
    private var range: UInt32 = 0xFFFF_FFFF
    private var cache: UInt8 = 0
    private var cacheSize = 1
    private var started = false
    var out: [UInt8]

    init(prefix: [UInt8] = []) {
        out = prefix
    }

    private mutating func emit(_ value: UInt8) {
        if !started {
            started = true
            return
        }
        out.append(value)
    }

    private mutating func shiftLow() {
        if UInt32(truncatingIfNeeded: low) < 0xFF00_0000 || low >> 32 != 0 {
            let carry = UInt8(truncatingIfNeeded: low >> 32)
            var temp = cache
            repeat {
                emit(temp &+ carry)
                temp = 0xFF
                cacheSize -= 1
            } while cacheSize != 0
            cache = UInt8(truncatingIfNeeded: UInt32(truncatingIfNeeded: low) >> 24)
        }
        cacheSize += 1
        low = UInt64(UInt32(truncatingIfNeeded: low) << 8)
    }

    private mutating func normalize() {
        while range < FaceStreamBits.top {
            range <<= 8
            shiftLow()
        }
    }

    mutating func bit(_ probs: inout [UInt16], _ at: Int, _ bit: Int) {
        let p = probs[at]
        let bound = (range >> 12) * UInt32(p)
        if bit == 0 {
            range = bound
            probs[at] = p + ((FaceStreamBits.probOne - p) >> 4)
        } else {
            low += UInt64(bound)
            range -= bound
            probs[at] = p - (p >> 4)
        }
        normalize()
    }

    mutating func direct(_ value: UInt32, _ count: Int) {
        var i = count - 1
        while i >= 0 {
            range >>= 1
            if (value >> UInt32(i)) & 1 == 1 { low += UInt64(range) }
            normalize()
            i -= 1
        }
    }

    mutating func finish() -> [UInt8] {
        for k in [UInt64(32), UInt64(24)] {
            let mask = (UInt64(1) << k) - 1
            let value = (low + mask) & ~mask
            if value < low + UInt64(range) {
                low = value
                break
            }
        }
        for _ in 0..<5 { shiftLow() }
        let full = out.count
        while let last = out.last, last == 0, full - out.count < FaceStreamBits.maxTrim { out.removeLast() }
        return out
    }

    private mutating func magnitude(_ model: inout FaceStreamIntModel, _ a: UInt32) {
        var n = 0
        var v = a
        while v != 0 {
            n += 1
            v >>= 1
        }
        if n > 1 {
            for i in 1..<n { bit(&model.length, i - 1, 1) }
        }
        if n < FaceStreamBits.maxLength { bit(&model.length, n - 1, 0) }
        if n >= 2 {
            bit(&model.mant, n - 2, Int((a >> UInt32(n - 2)) & 1))
            if n >= 3 { direct(a & ((1 << UInt32(n - 2)) - 1), n - 2) }
        }
    }

    mutating func uint(_ model: inout FaceStreamIntModel, _ ctx: Int, _ value: Int) {
        let v = faceStreamClamp(value, 0, FaceStreamBits.maxInt)
        if v == 0 {
            bit(&model.zero, ctx, 0)
            return
        }
        bit(&model.zero, ctx, 1)
        magnitude(&model, UInt32(v))
    }

    mutating func int(_ model: inout FaceStreamIntModel, _ ctx: Int, _ value: Int) {
        var v = faceStreamClamp(value, -FaceStreamBits.maxInt, FaceStreamBits.maxInt)
        if v == 0 {
            bit(&model.zero, ctx, 0)
            return
        }
        bit(&model.zero, ctx, 1)
        if v < 0 {
            bit(&model.sign, 0, 1)
            v = -v
        } else {
            bit(&model.sign, 0, 0)
        }
        magnitude(&model, UInt32(v))
    }
}

struct FaceStreamRangeDecoder {
    private var code: UInt32 = 0
    private var range: UInt32 = 0xFFFF_FFFF
    private let data: [UInt8]
    private var pos = 0

    init(_ data: [UInt8]) {
        self.data = data
        for _ in 0..<4 { code = code << 8 | UInt32(next()) }
    }

    private mutating func next() -> UInt8 {
        let value: UInt8 = pos < data.count ? data[pos] : 0
        pos += 1
        return value
    }

    private mutating func normalize() {
        while range < FaceStreamBits.top {
            range <<= 8
            code = code << 8 | UInt32(next())
        }
    }

    mutating func bit(_ probs: inout [UInt16], _ at: Int) -> Int {
        let p = probs[at]
        let bound = (range >> 12) * UInt32(p)
        if code < bound {
            range = bound
            probs[at] = p + ((FaceStreamBits.probOne - p) >> 4)
            normalize()
            return 0
        }
        code -= bound
        range -= bound
        probs[at] = p - (p >> 4)
        normalize()
        return 1
    }

    mutating func direct(_ count: Int) -> UInt32 {
        var value: UInt32 = 0
        for _ in 0..<count {
            range >>= 1
            var bit: UInt32 = 0
            if code >= range {
                code -= range
                bit = 1
            }
            value = value << 1 | bit
            normalize()
        }
        return value
    }

    var overrun: Bool { pos > data.count + FaceStreamBits.maxTrim }

    private mutating func magnitude(_ model: inout FaceStreamIntModel) -> Int {
        var n = 1
        while n < FaceStreamBits.maxLength && bit(&model.length, n - 1) == 1 { n += 1 }
        var a: UInt32 = 1
        if n >= 2 {
            a = a << 1 | UInt32(bit(&model.mant, n - 2))
            if n >= 3 { a = a << UInt32(n - 2) | direct(n - 2) }
        }
        return Int(a)
    }

    mutating func uint(_ model: inout FaceStreamIntModel, _ ctx: Int) -> Int {
        if bit(&model.zero, ctx) == 0 { return 0 }
        return magnitude(&model)
    }

    mutating func int(_ model: inout FaceStreamIntModel, _ ctx: Int) -> Int {
        if bit(&model.zero, ctx) == 0 { return 0 }
        let negative = bit(&model.sign, 0) == 1
        let a = magnitude(&model)
        return negative ? -a : a
    }
}
