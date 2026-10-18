import Foundation

public struct LineChangeCount: Equatable, Sendable {
    public let added: Int
    public let deleted: Int

    public init(added: Int, deleted: Int) {
        self.added = added
        self.deleted = deleted
    }
}

public enum LineDiffCount {
    public static let binarySniffBytes = 8000
    public static let maxEditDistance = 4096

    public static func isBinary(_ bytes: [UInt8]) -> Bool {
        bytes.prefix(binarySniffBytes).contains(0)
    }

    public static func lines(_ bytes: [UInt8]) -> [ArraySlice<UInt8>] {
        var result: [ArraySlice<UInt8>] = []
        var start = 0
        var index = 0
        let count = bytes.count
        while index < count {
            if bytes[index] == 10 {
                result.append(bytes[start...index])
                start = index + 1
            }
            index += 1
        }
        if start < count { result.append(bytes[start..<count]) }
        return result
    }

    public static func lineCount(_ bytes: [UInt8]) -> Int {
        if bytes.isEmpty { return 0 }
        var count = 0
        for byte in bytes where byte == 10 { count += 1 }
        return bytes.last == 10 ? count : count + 1
    }

    public static func added(_ bytes: [UInt8]) -> LineChangeCount? {
        isBinary(bytes) ? nil : LineChangeCount(added: lineCount(bytes), deleted: 0)
    }

    public static func deleted(_ bytes: [UInt8]) -> LineChangeCount? {
        isBinary(bytes) ? nil : LineChangeCount(added: 0, deleted: lineCount(bytes))
    }

    public static func count(old: [UInt8], new: [UInt8]) -> LineChangeCount? {
        if isBinary(old) || isBinary(new) { return nil }
        if old == new { return LineChangeCount(added: 0, deleted: 0) }
        var ids: [ArraySlice<UInt8>: Int32] = [:]
        func intern(_ lines: [ArraySlice<UInt8>]) -> [Int32] {
            lines.map { line in
                if let id = ids[line] { return id }
                let id = Int32(ids.count)
                ids[line] = id
                return id
            }
        }
        let a = intern(lines(old))
        let b = intern(lines(new))
        var prefix = 0
        while prefix < a.count, prefix < b.count, a[prefix] == b[prefix] { prefix += 1 }
        var suffix = 0
        while suffix < a.count - prefix, suffix < b.count - prefix, a[a.count - 1 - suffix] == b[b.count - 1 - suffix] {
            suffix += 1
        }
        let midA = Array(a[prefix..<(a.count - suffix)])
        let midB = Array(b[prefix..<(b.count - suffix)])
        let common = commonLength(midA, midB, symbols: ids.count)
        return LineChangeCount(added: midB.count - common, deleted: midA.count - common)
    }

    static func commonLength(_ a: [Int32], _ b: [Int32], symbols: Int) -> Int {
        if a.isEmpty || b.isEmpty { return 0 }
        var inA = [Bool](repeating: false, count: symbols)
        var inB = [Bool](repeating: false, count: symbols)
        for id in a { inA[Int(id)] = true }
        for id in b { inB[Int(id)] = true }
        let x = a.filter { inB[Int($0)] }
        let y = b.filter { inA[Int($0)] }
        return longestCommon(x, y)
    }

    static func longestCommon(_ a: [Int32], _ b: [Int32]) -> Int {
        let n = a.count
        let m = b.count
        if n == 0 || m == 0 { return 0 }
        let maxD = min(n + m, maxEditDistance)
        let offset = maxD + 1
        var v = [Int](repeating: 0, count: 2 * maxD + 3)
        for d in 0...maxD {
            var k = -d
            while k <= d {
                let ki = k + offset
                var x: Int
                if k == -d || (k != d && v[ki - 1] < v[ki + 1]) {
                    x = v[ki + 1]
                } else {
                    x = v[ki - 1] + 1
                }
                var y = x - k
                while x < n, y < m, a[x] == b[y] {
                    x += 1
                    y += 1
                }
                v[ki] = x
                if x >= n && y >= m { return (n + m - d) / 2 }
                k += 2
            }
        }
        return 0
    }
}
