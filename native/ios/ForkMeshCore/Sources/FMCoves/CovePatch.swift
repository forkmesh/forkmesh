import FMBase
import Foundation

public enum CoveSha1 {
    public static func hash(_ message: [UInt8]) -> [UInt8] {
        var h0: UInt32 = 0x6745_2301
        var h1: UInt32 = 0xEFCD_AB89
        var h2: UInt32 = 0x98BA_DCFE
        var h3: UInt32 = 0x1032_5476
        var h4: UInt32 = 0xC3D2_E1F0
        var data = message
        let bitLength = UInt64(message.count) * 8
        data.append(0x80)
        while data.count % 64 != 56 { data.append(0) }
        for shift in stride(from: 56, through: 0, by: -8) { data.append(UInt8(truncatingIfNeeded: bitLength >> UInt64(shift))) }
        var w = [UInt32](repeating: 0, count: 80)
        var chunk = 0
        while chunk < data.count {
            for i in 0..<16 {
                let b = chunk + i * 4
                w[i] = UInt32(data[b]) << 24 | UInt32(data[b + 1]) << 16 | UInt32(data[b + 2]) << 8 | UInt32(data[b + 3])
            }
            for i in 16..<80 {
                let x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]
                w[i] = (x << 1) | (x >> 31)
            }
            var a = h0, b = h1, c = h2, d = h3, e = h4
            for i in 0..<80 {
                let f: UInt32
                let k: UInt32
                switch i {
                case 0..<20:
                    f = (b & c) | (~b & d)
                    k = 0x5A82_7999
                case 20..<40:
                    f = b ^ c ^ d
                    k = 0x6ED9_EBA1
                case 40..<60:
                    f = (b & c) | (b & d) | (c & d)
                    k = 0x8F1B_BCDC
                default:
                    f = b ^ c ^ d
                    k = 0xCA62_C1D6
                }
                let temp = ((a << 5) | (a >> 27)) &+ f &+ e &+ k &+ w[i]
                e = d
                d = c
                c = (b << 30) | (b >> 2)
                b = a
                a = temp
            }
            h0 = h0 &+ a
            h1 = h1 &+ b
            h2 = h2 &+ c
            h3 = h3 &+ d
            h4 = h4 &+ e
            chunk += 64
        }
        var out: [UInt8] = []
        for word in [h0, h1, h2, h3, h4] {
            out.append(contentsOf: [UInt8(word >> 24), UInt8(truncatingIfNeeded: word >> 16), UInt8(truncatingIfNeeded: word >> 8), UInt8(truncatingIfNeeded: word)])
        }
        return out
    }

    public static func gitBlobId(_ bytes: [UInt8]) -> String {
        Hex.encode(hash(Array("blob \(bytes.count)\0".utf8) + bytes))
    }
}

public enum CovePatch {
    private static func lines(_ bytes: [UInt8]) -> (lines: [String], endsWithNewline: Bool) {
        if bytes.isEmpty { return ([], true) }
        var result: [String] = []
        var start = 0
        for (index, byte) in bytes.enumerated() where byte == 0x0A {
            result.append(String(decoding: bytes[start..<index], as: UTF8.self))
            start = index + 1
        }
        let ends = start == bytes.count
        if !ends { result.append(String(decoding: bytes[start...], as: UTF8.self)) }
        return (result, ends)
    }

    public static func replaceFile(path: String, before: [UInt8]?, after: [UInt8]) -> String {
        if let before, before == after { return "" }
        let newId = CoveSha1.gitBlobId(after)
        let (newLines, newEnds) = lines(after)
        var out = "diff --git a/\(path) b/\(path)\n"
        if let before {
            let oldId = CoveSha1.gitBlobId(before)
            let (oldLines, oldEnds) = lines(before)
            out += "index \(oldId)..\(newId)\n--- a/\(path)\n+++ b/\(path)\n"
            out += "@@ -\(range(oldLines.count)) +\(range(newLines.count)) @@\n"
            for (index, line) in oldLines.enumerated() {
                out += "-\(line)\n"
                if index == oldLines.count - 1 && !oldEnds { out += "\\ No newline at end of file\n" }
            }
        } else {
            out += "new file mode 100644\nindex 0000000000000000000000000000000000000000..\(newId)\n--- /dev/null\n+++ b/\(path)\n"
            out += "@@ -0,0 +\(range(newLines.count)) @@\n"
        }
        for (index, line) in newLines.enumerated() {
            out += "+\(line)\n"
            if index == newLines.count - 1 && !newEnds { out += "\\ No newline at end of file\n" }
        }
        return out
    }

    private static func range(_ count: Int) -> String {
        if count == 0 { return "0,0" }
        return count == 1 ? "1" : "1,\(count)"
    }

    public static func head(account: String, slug: String, now: Date) -> String {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(identifier: "UTC") ?? calendar.timeZone
        let parts = calendar.dateComponents([.month, .day, .hour, .minute, .second], from: now)
        func two(_ value: Int?) -> String {
            let text = String(value ?? 0)
            return text.count < 2 ? "0" + text : text
        }
        let stamp = "\(two(parts.month))\(two(parts.day))-\(two(parts.hour))\(two(parts.minute))\(two(parts.second))"
        let branch = "mobile-cove/\(slug.prefix(12))-\(stamp)"
        let owner = CoveStore.normalizedAccount(account)
        return owner.isEmpty ? branch : "\(owner):\(branch)"
    }
}
