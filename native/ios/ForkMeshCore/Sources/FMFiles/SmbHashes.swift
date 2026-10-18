import Foundation

private func rotl(_ value: UInt32, _ shift: UInt32) -> UInt32 { (value << shift) | (value >> (32 - shift)) }

private func padded(_ message: [UInt8]) -> [UInt8] {
    var data = message
    let bits = UInt64(message.count) &* 8
    data.append(0x80)
    while data.count % 64 != 56 { data.append(0) }
    for shift in stride(from: 0, to: 64, by: 8) { data.append(UInt8((bits >> UInt64(shift)) & 0xff)) }
    return data
}

private func words(_ data: [UInt8], _ block: Int) -> [UInt32] {
    (0..<16).map { index in
        let base = block + index * 4
        return UInt32(data[base]) | UInt32(data[base + 1]) << 8 | UInt32(data[base + 2]) << 16 | UInt32(data[base + 3]) << 24
    }
}

private func littleEndian(_ state: [UInt32]) -> [UInt8] {
    state.flatMap { value in (0..<4).map { UInt8((value >> UInt32($0 * 8)) & 0xff) } }
}

public enum Md4 {
    public static func hash(_ message: [UInt8]) -> [UInt8] {
        let data = padded(message)
        var state: [UInt32] = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476]
        let order2 = [0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15]
        let order3 = [0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15]
        for block in stride(from: 0, to: data.count, by: 64) {
            let x = words(data, block)
            var v = state
            for step in 0..<48 {
                let target = (4 - step % 4) % 4
                let b = v[(target + 1) % 4], c = v[(target + 2) % 4], d = v[(target + 3) % 4]
                let round = step / 16
                let index = step % 16
                let mix: UInt32
                let word: UInt32
                let shift: UInt32
                switch round {
                case 0:
                    mix = (b & c) | (~b & d)
                    word = x[index]
                    shift = [3, 7, 11, 19][step % 4]
                case 1:
                    mix = ((b & c) | (b & d) | (c & d)) &+ 0x5A827999
                    word = x[order2[index]]
                    shift = [3, 5, 9, 13][step % 4]
                default:
                    mix = (b ^ c ^ d) &+ 0x6ED9EBA1
                    word = x[order3[index]]
                    shift = [3, 9, 11, 15][step % 4]
                }
                v[target] = rotl(v[target] &+ mix &+ word, shift)
            }
            for index in 0..<4 { state[index] = state[index] &+ v[index] }
        }
        return littleEndian(state)
    }
}

public enum Md5 {
    private static let shifts: [UInt32] = [
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    ]

    private static let table: [UInt32] = [
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    ]

    public static func hash(_ message: [UInt8]) -> [UInt8] {
        let data = padded(message)
        var state: [UInt32] = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476]
        for block in stride(from: 0, to: data.count, by: 64) {
            let m = words(data, block)
            var a = state[0], b = state[1], c = state[2], d = state[3]
            for step in 0..<64 {
                var f: UInt32
                let g: Int
                switch step / 16 {
                case 0:
                    f = (b & c) | (~b & d)
                    g = step
                case 1:
                    f = (d & b) | (~d & c)
                    g = (5 * step + 1) % 16
                case 2:
                    f = b ^ c ^ d
                    g = (3 * step + 5) % 16
                default:
                    f = c ^ (b | ~d)
                    g = (7 * step) % 16
                }
                f = f &+ a &+ table[step] &+ m[g]
                a = d
                d = c
                c = b
                b = b &+ rotl(f, shifts[step])
            }
            state[0] = state[0] &+ a
            state[1] = state[1] &+ b
            state[2] = state[2] &+ c
            state[3] = state[3] &+ d
        }
        return littleEndian(state)
    }
}

public enum HmacMd5 {
    public static func mac(key: [UInt8], _ parts: [UInt8]...) -> [UInt8] {
        var block = key.count > 64 ? Md5.hash(key) : key
        block += [UInt8](repeating: 0, count: 64 - block.count)
        let inner = block.map { $0 ^ 0x36 }
        let outer = block.map { $0 ^ 0x5c }
        return Md5.hash(outer + Md5.hash(inner + parts.flatMap { $0 }))
    }
}
