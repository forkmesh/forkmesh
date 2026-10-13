import Foundation

extension UnifiedDiff {
    /// The LCS table is quadratic; long minified lines are left unmarked.
    static let maxIntraLineCells = 40000

    /// Token spans (UTF-16 ranges) that differ between a removed line and the
    /// added line paired with it, or nil when the lines share too little for
    /// marks to help.
    public static func intraLineChanges(_ a: String, _ b: String) -> (removed: [DiffRange], added: [DiffRange])? {
        let ua = U16.units(a)
        let ub = U16.units(b)
        // Swift's `==` is canonical equivalence, not code units; the
        // units are what get marked, so they decide.
        if ua == ub { return nil }
        let ta = tokens(ua)
        let tb = tokens(ub)
        if ta.isEmpty || tb.isEmpty || ta.count * tb.count > maxIntraLineCells { return nil }

        func same(_ i: Int, _ j: Int) -> Bool {
            let x = ta[i], y = tb[j]
            return x.length == y.length && ua[x.start..<x.end].elementsEqual(ub[y.start..<y.end])
        }

        // Trim the common prefix and suffix first: most edits touch one spot.
        var prefix = 0
        while prefix < ta.count, prefix < tb.count, same(prefix, prefix) { prefix += 1 }
        var suffix = 0
        while suffix < ta.count - prefix, suffix < tb.count - prefix,
              same(ta.count - 1 - suffix, tb.count - 1 - suffix) {
            suffix += 1
        }
        let na = ta.count - prefix - suffix
        let nb = tb.count - prefix - suffix
        var keepA = [Bool](repeating: true, count: ta.count)
        var keepB = [Bool](repeating: true, count: tb.count)
        if na > 0 || nb > 0 {
            let width = nb + 1
            var table = [Int32](repeating: 0, count: (na + 1) * width)
            if na > 0 && nb > 0 {
                for i in stride(from: na - 1, through: 0, by: -1) {
                    for j in stride(from: nb - 1, through: 0, by: -1) {
                        if same(prefix + i, prefix + j) {
                            table[i * width + j] = table[(i + 1) * width + j + 1] + 1
                        } else {
                            table[i * width + j] = max(table[(i + 1) * width + j], table[i * width + j + 1])
                        }
                    }
                }
            }
            for k in 0..<na { keepA[prefix + k] = false }
            for k in 0..<nb { keepB[prefix + k] = false }
            var i = 0
            var j = 0
            while i < na && j < nb {
                if same(prefix + i, prefix + j) {
                    keepA[prefix + i] = true
                    keepB[prefix + j] = true
                    i += 1
                    j += 1
                } else if table[(i + 1) * width + j] >= table[i * width + j + 1] {
                    i += 1
                } else {
                    j += 1
                }
            }
        }

        let ra = ranges(ta, keepA)
        let rb = ranges(tb, keepB)
        // When nearly everything changed the marks are noise: the whole line
        // is already tinted as removed/added.
        let changedA = Double(covered(ra)) / Double(ua.isEmpty ? 1 : ua.count)
        let changedB = Double(covered(rb)) / Double(ub.isEmpty ? 1 : ub.count)
        if changedA > 0.7 && changedB > 0.7 { return nil }
        return (ra, rb)
    }

    /// `\w+|\s+|[^\w\s]`: words, whitespace runs, and single symbols.
    ///
    /// Unlike a regex working per code unit, a surrogate pair is one token, so
    /// a mark never splits an emoji into two halves that cannot be rendered.
    static func tokens(_ text: Units) -> [DiffRange] {
        var out: [DiffRange] = []
        var i = 0
        let count = text.count
        while i < count {
            let start = i
            let unit = text[i]
            if U16.isWord(unit) {
                while i < count, U16.isWord(text[i]) { i += 1 }
            } else if U16.isRegexSpace(unit) {
                while i < count, U16.isRegexSpace(text[i]) { i += 1 }
            } else if U16.isHighSurrogate(unit), i + 1 < count, U16.isLowSurrogate(text[i + 1]) {
                i += 2
            } else {
                i += 1
            }
            out.append(DiffRange(start, i))
        }
        return out
    }

    private static func ranges(_ tokens: [DiffRange], _ keep: [Bool]) -> [DiffRange] {
        var out: [DiffRange] = []
        for k in tokens.indices where !keep[k] {
            if let last = out.last, last.end == tokens[k].start {
                out[out.count - 1] = DiffRange(last.start, tokens[k].end)
            } else {
                out.append(tokens[k])
            }
        }
        return out
    }

    private static func covered(_ ranges: [DiffRange]) -> Int {
        ranges.reduce(0) { $0 + $1.length }
    }
}
