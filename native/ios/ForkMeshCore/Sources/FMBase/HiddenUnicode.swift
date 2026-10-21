import Foundation

public enum HiddenUnicode {
    public static let policy = "forkmesh-hidden-unicode-v1"

    private typealias Range = (UInt32, UInt32)

    private static let drop: [Range] = [
        (0x0000, 0x0008), (0x000B, 0x000C), (0x000E, 0x001F), (0x007F, 0x009F),
        (0x00AD, 0x00AD), (0x034F, 0x034F), (0x115F, 0x1160), (0x17B4, 0x17B5),
        (0x180B, 0x180F), (0x200B, 0x200B), (0x202A, 0x202E), (0x2060, 0x206F),
        (0x3164, 0x3164), (0xD800, 0xDFFF), (0xFDD0, 0xFDEF), (0xFE00, 0xFE0D),
        (0xFEFF, 0xFEFF), (0xFFA0, 0xFFA0), (0xFFF9, 0xFFFB), (0x1BCA0, 0x1BCA3),
        (0x1D173, 0x1D17A), (0xE0000, 0xE007F), (0xE0100, 0xE01EF),
    ]
    private static let emoji: [Range] = [
        (0x00A9, 0x00A9), (0x00AE, 0x00AE), (0x203C, 0x203C), (0x2049, 0x2049),
        (0x2122, 0x2122), (0x2139, 0x2139), (0x2190, 0x2BFF), (0x3030, 0x3030),
        (0x303D, 0x303D), (0x3297, 0x3297), (0x3299, 0x3299), (0x1F000, 0x1FAFF),
    ]
    private static let joining: [Range] = [
        (0x0600, 0x08FF), (0x0900, 0x0DFF), (0x0F00, 0x109F),
        (0x1780, 0x18AF), (0xFB50, 0xFDFF), (0xFE70, 0xFEFC),
    ]
    private static let rtl: [Range] = [
        (0x0590, 0x08FF), (0xFB1D, 0xFDFF), (0xFE70, 0xFEFC),
        (0x10800, 0x10FFF), (0x1E800, 0x1EFFF),
    ]

    private static let none: UInt32 = .max
    private static let zwnj: UInt32 = 0x200C
    private static let zwj: UInt32 = 0x200D
    private static let blackFlag: UInt32 = 0x1F3F4
    private static let cancelTag: UInt32 = 0xE007F

    private static func within(_ cp: UInt32, _ ranges: [Range]) -> Bool {
        if cp == none { return false }
        for (low, high) in ranges where cp >= low && cp <= high { return true }
        return false
    }

    private static func isWhitespace(_ cp: UInt32) -> Bool {
        cp == 0x09 || cp == 0x0A || cp == 0x0D || cp == 0x20
    }

    private static func isSelector(_ cp: UInt32) -> Bool {
        cp == 0xFE0E || cp == 0xFE0F
    }

    private static func candidate(_ cp: UInt32) -> Bool {
        if cp < 0x20 { return !isWhitespace(cp) }
        if cp < 0x7F { return false }
        if cp <= 0x9F { return true }
        if cp >= 0x10000 {
            return (cp >= 0xE0000 && cp <= 0xE01EF) || (cp >= 0x1BCA0 && cp <= 0x1BCA3)
                || (cp >= 0x1D173 && cp <= 0x1D17A) || (cp & 0xFFFE) == 0xFFFE
        }
        return cp == 0x00AD || cp == 0x034F || cp == 0x061C || cp == 0x115F || cp == 0x1160
            || cp == 0x17B4 || cp == 0x17B5 || (cp >= 0x180B && cp <= 0x180F)
            || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202E)
            || (cp >= 0x2060 && cp <= 0x206F) || cp == 0x3164 || (cp >= 0xFDD0 && cp <= 0xFDEF)
            || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF || cp == 0xFFA0 || cp >= 0xFFF9
    }

    private static func flagTagLength(_ cps: [UInt32], _ start: Int) -> Int {
        var index = start
        while index < cps.count && index - start < 8 {
            let cp = cps[index]
            if cp == cancelTag {
                let run = index - start
                return run >= 2 && run <= 7 ? run + 1 : 0
            }
            if !((cp >= 0xE0030 && cp <= 0xE0039) || (cp >= 0xE0061 && cp <= 0xE007A)) { return 0 }
            index += 1
        }
        return 0
    }

    public static func clean(_ text: String) -> String {
        guard text.unicodeScalars.contains(where: { candidate($0.value) }) else { return text }
        let cps = text.unicodeScalars.map(\.value)
        var out: [UInt32] = []
        out.reserveCapacity(cps.count)
        var index = 0
        while index < cps.count {
            let cp = cps[index]
            let prev = out.last ?? none
            let following = index + 1 < cps.count ? cps[index + 1] : none
            index += 1
            if cp == 0x2028 || cp == 0x2029 {
                out.append(0x0A)
                continue
            }
            if isSelector(cp) {
                if prev != none && !isWhitespace(prev) && !isSelector(prev) && prev != zwj && prev != zwnj {
                    out.append(cp)
                }
                continue
            }
            if cp == zwj {
                let emojiJoin = (prev == 0xFE0F || within(prev, emoji)) && within(following, emoji)
                let scriptJoin = within(prev, joining) && within(following, joining)
                if emojiJoin || scriptJoin { out.append(cp) }
                continue
            }
            if cp == zwnj {
                if within(prev, joining) && within(following, joining) { out.append(cp) }
                continue
            }
            if cp == 0x061C || cp == 0x200E || cp == 0x200F {
                if within(prev, rtl) || within(following, rtl) { out.append(cp) }
                continue
            }
            if cp == blackFlag {
                out.append(cp)
                let run = flagTagLength(cps, index)
                out.append(contentsOf: cps[index..<(index + run)])
                index += run
                continue
            }
            if within(cp, drop) || (cp & 0xFFFE) == 0xFFFE { continue }
            out.append(cp)
        }
        if out == cps { return text }
        var result = String.UnicodeScalarView()
        for cp in out {
            if let scalar = Unicode.Scalar(cp) { result.append(scalar) }
        }
        return String(result)
    }

    public static func hasHidden(_ text: String) -> Bool {
        clean(text) != text
    }
}
