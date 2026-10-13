// A parsed `git diff` / `git format-patch` document, shaped for display.
//
// The mirror's compare endpoint answers with raw text: the net patch from the
// merge base to the branch tip, and the branch's commits as a format-patch
// series. Both are parsed here once, into plain values the diff viewer can
// lay out in either unified or side-by-side form without reparsing.

import Foundation

public enum DiffFileStatus: String, Sendable, CaseIterable {
    case added, modified, deleted, renamed, copied
}

public enum DiffLineKind: String, Sendable, CaseIterable {
    case context, added, removed
}

public struct DiffLine: Equatable, Sendable, Identifiable {
    public let kind: DiffLineKind

    /// The line without its `+`/`-`/` ` marker, tabs expanded for display.
    public let text: String

    /// One-based line numbers; zero on the side the line does not exist on.
    public let oldNumber: Int
    public let newNumber: Int

    /// The file has no trailing newline after this line.
    public var noNewlineAtEnd: Bool

    /// UTF-16 ranges that changed against the paired line on the other side.
    /// Filled in by `DiffFile.prepare()`; empty means either the whole line is
    /// new or the pair was too different to be worth marking.
    public var emphasis: [DiffRange]

    /// Position of the line among all lines of its file, in patch order. Values
    /// have no identity, so this is the key of per-line caches (and the
    /// SwiftUI id).
    public let ordinal: Int

    /// `text.utf16.count`, kept from the parse so sizing never rescans.
    public let utf16Length: Int

    public var id: Int { ordinal }

    public init(
        kind: DiffLineKind,
        text: String,
        oldNumber: Int = 0,
        newNumber: Int = 0,
        noNewlineAtEnd: Bool = false,
        emphasis: [DiffRange] = [],
        ordinal: Int = 0
    ) {
        self.kind = kind
        self.text = text
        self.oldNumber = oldNumber
        self.newNumber = newNumber
        self.noNewlineAtEnd = noNewlineAtEnd
        self.emphasis = emphasis
        self.ordinal = ordinal
        self.utf16Length = text.utf16.count
    }
}

public struct DiffHunk: Equatable, Sendable {
    public let oldStart: Int
    public let oldCount: Int
    public let newStart: Int
    public let newCount: Int

    /// The function or heading git printed after the second `@@`.
    public let section: String
    public var lines: [DiffLine]

    public init(
        oldStart: Int,
        oldCount: Int,
        newStart: Int,
        newCount: Int,
        section: String = "",
        lines: [DiffLine] = []
    ) {
        self.oldStart = oldStart
        self.oldCount = oldCount
        self.newStart = newStart
        self.newCount = newCount
        self.section = section
        self.lines = lines
    }

    public var header: String {
        "@@ -\(oldStart),\(oldCount) +\(newStart),\(newCount) @@" + (section.isEmpty ? "" : " \(section)")
    }
}

/// One row of the side-by-side layout: a line on the left, the right, or both.
public struct DiffSplitRow: Equatable, Sendable, Identifiable {
    /// Row position within the file's split layout.
    public let id: Int
    public let left: DiffLine?
    public let right: DiffLine?

    /// Set on the row that introduces a hunk (an index into `DiffFile.hunks`);
    /// its lines follow.
    public let hunkIndex: Int?

    public init(id: Int = 0, left: DiffLine? = nil, right: DiffLine? = nil, hunkIndex: Int? = nil) {
        self.id = id
        self.left = left
        self.right = right
        self.hunkIndex = hunkIndex
    }
}

public struct DiffFile: Equatable, Sendable {
    public let oldPath: String
    public let newPath: String
    public let status: DiffFileStatus
    public let binary: Bool
    public let oldMode: String
    public let newMode: String

    /// Rename/copy similarity percentage, or -1 when git did not print one.
    public let similarity: Int
    public private(set) var hunks: [DiffHunk]

    /// This file's section of the patch, verbatim, for copying.
    public let raw: String

    public init(
        oldPath: String,
        newPath: String,
        status: DiffFileStatus,
        binary: Bool = false,
        oldMode: String = "",
        newMode: String = "",
        similarity: Int = -1,
        hunks: [DiffHunk] = [],
        raw: String = ""
    ) {
        self.oldPath = oldPath
        self.newPath = newPath
        self.status = status
        self.binary = binary
        self.oldMode = oldMode
        self.newMode = newMode
        self.similarity = similarity
        self.hunks = hunks
        self.raw = raw
    }

    /// The path a reader cares about: the new one, unless the file is gone.
    public var path: String { status == .deleted ? oldPath : newPath }

    public var modeChanged: Bool {
        !oldMode.isEmpty && !newMode.isEmpty && oldMode != newMode
    }

    public var additions: Int { count(.added) }
    public var deletions: Int { count(.removed) }

    private func count(_ kind: DiffLineKind) -> Int {
        var total = 0
        for hunk in hunks {
            for line in hunk.lines where line.kind == kind { total += 1 }
        }
        return total
    }

    /// Lines across all hunks; ordinals run `0..<lineCount`.
    public var lineCount: Int { hunks.reduce(0) { $0 + $1.lines.count } }

    /// The largest line number on either side, for sizing the gutter.
    public var maxLineNumber: Int {
        var largest = 0
        for hunk in hunks {
            largest = max(largest, hunk.oldStart + hunk.oldCount, hunk.newStart + hunk.newCount)
        }
        return largest
    }

    /// The longest displayed line in UTF-16 units, for sizing a non-wrapping canvas.
    public var longestLine: Int {
        var longest = 0
        for hunk in hunks {
            for line in hunk.lines where line.utf16Length > longest { longest = line.utf16Length }
        }
        return longest
    }

    public private(set) var isPrepared = false
    private var preparedRows: [DiffSplitRow] = []

    /// Rows for the side-by-side layout. Cached once `prepare()` has run;
    /// computed on the spot (same result) when it has not.
    public var splitRows: [DiffSplitRow] {
        if isPrepared { return preparedRows }
        var copy = self
        copy.prepare()
        return copy.preparedRows
    }

    /// Pairs removed and added lines and marks what changed inside each pair.
    ///
    /// Done on first display rather than at parse time: a 4 MiB comparison can
    /// hold thousands of files, and most of them are never opened on a phone.
    public mutating func prepare() {
        if isPrepared { return }
        isPrepared = true
        var rows: [DiffSplitRow] = []
        for h in hunks.indices {
            rows.append(DiffSplitRow(id: rows.count, hunkIndex: h))
            let count = hunks[h].lines.count
            var i = 0
            while i < count {
                if hunks[h].lines[i].kind == .context {
                    let line = hunks[h].lines[i]
                    rows.append(DiffSplitRow(id: rows.count, left: line, right: line))
                    i += 1
                    continue
                }
                var removed: [Int] = []
                var added: [Int] = []
                while i < count, hunks[h].lines[i].kind == .removed {
                    removed.append(i)
                    i += 1
                }
                while i < count, hunks[h].lines[i].kind == .added {
                    added.append(i)
                    i += 1
                }
                for p in 0..<max(removed.count, added.count) {
                    if p < removed.count, p < added.count,
                       let marks = UnifiedDiff.intraLineChanges(
                           hunks[h].lines[removed[p]].text,
                           hunks[h].lines[added[p]].text
                       ) {
                        hunks[h].lines[removed[p]].emphasis = marks.removed
                        hunks[h].lines[added[p]].emphasis = marks.added
                    }
                    rows.append(DiffSplitRow(
                        id: rows.count,
                        left: p < removed.count ? hunks[h].lines[removed[p]] : nil,
                        right: p < added.count ? hunks[h].lines[added[p]] : nil
                    ))
                }
            }
        }
        preparedRows = rows
    }

    /// The file with `prepare()` applied, for callers holding a `let`.
    public func prepared() -> DiffFile {
        var copy = self
        copy.prepare()
        return copy
    }

    public static func == (lhs: DiffFile, rhs: DiffFile) -> Bool {
        lhs.oldPath == rhs.oldPath && lhs.newPath == rhs.newPath && lhs.status == rhs.status
            && lhs.binary == rhs.binary && lhs.oldMode == rhs.oldMode && lhs.newMode == rhs.newMode
            && lhs.similarity == rhs.similarity && lhs.raw == rhs.raw && lhs.hunks == rhs.hunks
    }
}

public enum UnifiedDiff {
    /// Parses a unified diff as produced by `git diff` (with or without
    /// `--binary`) into files, hunks and numbered lines.
    public static func parse(_ patch: String) -> [DiffFile] {
        let units = U16.units(patch)
        if U16.isBlank(units) { return [] }
        var parser = Parser(lines: U16.splitLines(units))
        return parser.run()
    }

    // MARK: - Parser

    private struct Parser {
        let lines: [ArraySlice<UInt16>]
        var files: [DiffFile] = []

        var oldPath: String?
        var newPath: String?
        var status = DiffFileStatus.modified
        var binary = false
        var oldMode = ""
        var newMode = ""
        var similarity = -1
        var hunks: [DiffHunk] = []
        var started = false
        var rawStart = 0
        var inHunk = false
        var oldLeft = 0
        var newLeft = 0
        var oldNo = 0
        var newNo = 0
        var skippingBinary = false
        var ordinal = 0

        init(lines: [ArraySlice<UInt16>]) {
            self.lines = lines
        }

        mutating func flush(_ endLine: Int) {
            guard started else { return }
            let o = oldPath ?? newPath ?? ""
            let n = newPath ?? oldPath ?? ""
            if o.isEmpty && n.isEmpty { return }
            var raw = Units()
            for index in rawStart..<endLine {
                if index > rawStart { raw.append(U16.lf) }
                raw.append(contentsOf: lines[index])
            }
            files.append(DiffFile(
                oldPath: o,
                newPath: n,
                status: status,
                binary: binary,
                oldMode: oldMode,
                newMode: newMode,
                similarity: similarity,
                hunks: hunks,
                raw: U16.string(raw)
            ))
        }

        mutating func begin(_ index: Int) {
            flush(index)
            started = true
            rawStart = index
            oldPath = nil
            newPath = nil
            status = .modified
            binary = false
            oldMode = ""
            newMode = ""
            similarity = -1
            hunks = []
            inHunk = false
            oldLeft = 0
            newLeft = 0
            skippingBinary = false
            ordinal = 0
        }

        mutating func markNoNewline() {
            guard let h = hunks.indices.last, let l = hunks[h].lines.indices.last else { return }
            hunks[h].lines[l].noNewlineAtEnd = true
        }

        mutating func add(_ kind: DiffLineKind, _ text: String, old: Int = 0, new: Int = 0) {
            hunks[hunks.count - 1].lines.append(DiffLine(
                kind: kind, text: text, oldNumber: old, newNumber: new, ordinal: ordinal
            ))
            ordinal += 1
        }

        mutating func run() -> [DiffFile] {
            for index in lines.indices {
                let line = lines[index]
                let first = line.first

                // Inside a hunk the counts decide what a line is: a removed line
                // whose text begins with "-- " must not be mistaken for a file
                // header.
                if inHunk && (oldLeft > 0 || newLeft > 0) {
                    if first == U16.backslash {
                        markNoNewline()
                        continue
                    }
                    let marker = first ?? U16.space
                    if marker == U16.plus || marker == U16.minus || marker == U16.space {
                        let text = expandTabs(line.dropFirst())
                        if marker == U16.plus {
                            add(.added, text, new: newNo)
                            newNo += 1
                            newLeft -= 1
                        } else if marker == U16.minus {
                            add(.removed, text, old: oldNo)
                            oldNo += 1
                            oldLeft -= 1
                        } else {
                            add(.context, text, old: oldNo, new: newNo)
                            oldNo += 1
                            newNo += 1
                            oldLeft -= 1
                            newLeft -= 1
                        }
                        continue
                    }
                    // Anything else ends a truncated hunk; fall through and reinterpret.
                    oldLeft = 0
                    newLeft = 0
                }

                if first == U16.backslash && inHunk {
                    markNoNewline()
                    continue
                }
                if U16.hasPrefix(line, Prefix.diffGit) {
                    begin(index)
                    let paths = gitHeaderPaths(line)
                    oldPath = paths.0
                    newPath = paths.1
                    continue
                }
                if !started && (U16.hasPrefix(line, Prefix.minus3) || U16.hasPrefix(line, Prefix.diff)) {
                    // A plain `diff -u` document with no git header.
                    begin(index)
                }
                if !started { continue }
                if U16.hasPrefix(line, Prefix.minus3), !hunks.isEmpty, index + 1 < lines.count,
                   U16.hasPrefix(lines[index + 1], Prefix.plus3) {
                    // The next file of a plain `diff -u` document.
                    begin(index)
                }
                if skippingBinary { continue }

                if let header = hunkHeader(line) {
                    hunks.append(header)
                    inHunk = true
                    oldLeft = header.oldCount
                    newLeft = header.newCount
                    oldNo = header.oldStart
                    newNo = header.newStart
                    continue
                }
                interpretHeader(line)
            }
            flush(lines.count)
            return files
        }

        mutating func interpretHeader(_ line: ArraySlice<UInt16>) {
            func rest(_ prefix: Units) -> ArraySlice<UInt16>? {
                U16.hasPrefix(line, prefix) ? line.dropFirst(prefix.count) : nil
            }
            if let path = rest(Prefix.minus3) {
                if U16.trim(path).elementsEqual(Prefix.devNull) {
                    status = .added
                } else {
                    oldPath = stripPrefix(path)
                }
            } else if let path = rest(Prefix.plus3) {
                if U16.trim(path).elementsEqual(Prefix.devNull) {
                    status = .deleted
                } else {
                    newPath = stripPrefix(path)
                }
            } else if let mode = rest(Prefix.newFileMode) {
                status = .added
                newMode = U16.string(U16.trim(mode))
            } else if let mode = rest(Prefix.deletedFileMode) {
                status = .deleted
                oldMode = U16.string(U16.trim(mode))
            } else if let mode = rest(Prefix.oldMode) {
                oldMode = U16.string(U16.trim(mode))
            } else if let mode = rest(Prefix.newMode) {
                newMode = U16.string(U16.trim(mode))
            } else if let path = rest(Prefix.renameFrom) {
                status = .renamed
                oldPath = stripPrefix(path)
            } else if let path = rest(Prefix.renameTo) {
                status = .renamed
                newPath = stripPrefix(path)
            } else if let path = rest(Prefix.copyFrom) {
                status = .copied
                oldPath = stripPrefix(path)
            } else if let path = rest(Prefix.copyTo) {
                status = .copied
                newPath = stripPrefix(path)
            } else if let value = rest(Prefix.similarity) {
                let digits = U16.trim(ArraySlice(value.filter { $0 != 0x25 }))
                similarity = U16.parseInt(digits) ?? -1
            } else if U16.hasPrefix(line, Prefix.index) {
                // `index abc..def 100644` carries the mode when it did not change.
                let parts = line.split(separator: U16.space, omittingEmptySubsequences: false)
                if parts.count >= 3 && oldMode.isEmpty && newMode.isEmpty {
                    oldMode = U16.string(parts[2])
                    newMode = oldMode
                }
            } else if U16.hasPrefix(line, Prefix.binaryFiles) {
                binary = true
            } else if line.elementsEqual(Prefix.gitBinaryPatch) {
                binary = true
                skippingBinary = true
            }
        }
    }

    private enum Prefix {
        static let diffGit = U16.units("diff --git ")
        static let diff = U16.units("diff ")
        static let minus3 = U16.units("--- ")
        static let plus3 = U16.units("+++ ")
        static let devNull = U16.units("/dev/null")
        static let newFileMode = U16.units("new file mode ")
        static let deletedFileMode = U16.units("deleted file mode ")
        static let oldMode = U16.units("old mode ")
        static let newMode = U16.units("new mode ")
        static let renameFrom = U16.units("rename from ")
        static let renameTo = U16.units("rename to ")
        static let copyFrom = U16.units("copy from ")
        static let copyTo = U16.units("copy to ")
        static let similarity = U16.units("similarity index ")
        static let index = U16.units("index ")
        static let binaryFiles = U16.units("Binary files ")
        static let gitBinaryPatch = U16.units("GIT binary patch")
        static let aSlash = U16.units("a/")
        static let bSlash = U16.units("b/")
        static let spaceB = U16.units(" b/")
        static let quoteSpace = U16.units("\" ")
    }

    /// Expands tabs so columns line up in a monospace canvas. Columns count
    /// code points, so a surrogate pair is one column.
    static func expandTabs(_ line: ArraySlice<UInt16>) -> String {
        guard line.contains(U16.tab) else { return U16.string(line) }
        var out = Units()
        out.reserveCapacity(line.count + 16)
        var column = 0
        for unit in line {
            if unit == U16.tab {
                let pad = 4 - (column % 4)
                out.append(contentsOf: repeatElement(U16.space, count: pad))
                column += pad
            } else {
                out.append(unit)
                if !U16.isLowSurrogate(unit) { column += 1 }
            }
        }
        return U16.string(out)
    }

    /// `^@@+ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@+ ?(.*)$`
    private static func hunkHeader(_ line: ArraySlice<UInt16>) -> DiffHunk? {
        var i = line.startIndex
        let end = line.endIndex
        func peek(_ unit: UInt16) -> Bool { i < end && line[i] == unit }
        func ats() -> Bool {
            let begin = i
            while peek(U16.at) { i += 1 }
            return i - begin >= 2
        }
        func number() -> Int? {
            let begin = i
            while i < end, U16.isDigit(line[i]) { i += 1 }
            return U16.parseInt(line[begin..<i])
        }
        func range(_ sign: UInt16) -> (Int, Int)? {
            guard peek(sign) else { return nil }
            i += 1
            guard let start = number() else { return nil }
            var count = 1
            if peek(U16.comma), i + 1 < end, U16.isDigit(line[i + 1]) {
                i += 1
                guard let parsed = number() else { return nil }
                count = parsed
            }
            return (start, count)
        }
        guard ats(), peek(U16.space) else { return nil }
        i += 1
        guard let old = range(U16.minus), peek(U16.space) else { return nil }
        i += 1
        guard let new = range(U16.plus), peek(U16.space) else { return nil }
        i += 1
        guard ats() else { return nil }
        if peek(U16.space) { i += 1 }
        let section = line[i..<end]
        // `.` stops at a line separator, and then `$` cannot hold.
        if section.contains(where: U16.isLineTerminator) { return nil }
        return DiffHunk(
            oldStart: old.0,
            oldCount: old.1,
            newStart: new.0,
            newCount: new.1,
            section: U16.string(U16.trim(section))
        )
    }

    /// `a/foo` → `foo`, with git's C-style quoting undone.
    static func stripPrefix(_ raw: ArraySlice<UInt16>) -> String {
        var path = U16.trim(raw)
        // A tab separates the path from an optional timestamp in plain diff(1).
        if let tab = path.firstIndex(of: U16.tab) { path = path[path.startIndex..<tab] }
        var units = Units(path)
        if units.count >= 2, units.first == U16.quote, units.last == U16.quote {
            units = U16.units(unquote(units[1..<(units.count - 1)]))
        }
        if U16.hasPrefix(units, Prefix.aSlash) || U16.hasPrefix(units, Prefix.bSlash) {
            units.removeFirst(2)
        }
        return U16.string(units)
    }

    static func unquote(_ quoted: ArraySlice<UInt16>) -> String {
        var bytes: [UInt8] = []
        var literal = Units()
        func flushLiteral() {
            guard !literal.isEmpty else { return }
            bytes.append(contentsOf: Array(U16.string(literal).utf8))
            literal.removeAll(keepingCapacity: true)
        }
        var i = quoted.startIndex
        let end = quoted.endIndex
        while i < end {
            let unit = quoted[i]
            if unit != U16.backslash || i + 1 >= end {
                literal.append(unit)
                i += 1
                continue
            }
            i += 1
            let next = quoted[i]
            flushLiteral()
            switch next {
            case 0x6E: bytes.append(0x0A) // n
            case 0x74: bytes.append(0x09) // t
            case U16.quote: bytes.append(0x22)
            case U16.backslash: bytes.append(0x5C)
            default:
                if i + 2 < end, quoted[i...(i + 2)].allSatisfy({ $0 >= 0x30 && $0 <= 0x37 }) {
                    let value = quoted[i...(i + 2)].reduce(0) { $0 * 8 + Int($1 - 0x30) }
                    bytes.append(UInt8(truncatingIfNeeded: value))
                    i += 2
                } else {
                    literal.append(next)
                }
            }
            i += 1
        }
        flushLiteral()
        return String(decoding: bytes, as: UTF8.self)
    }

    /// `diff --git a/x b/y` → (`x`, `y`). Ambiguous when a path contains ` b/`;
    /// the `---`/`+++` and rename lines that follow override it when present.
    static func gitHeaderPaths(_ line: ArraySlice<UInt16>) -> (String, String) {
        let rest = Units(line.dropFirst(Prefix.diffGit.count))
        if rest.first == U16.quote {
            let close = U16.indexOf(rest, Prefix.quoteSpace, from: 1)
            if close > 0 {
                return (stripPrefix(rest[0...close]), stripPrefix(rest[(close + 2)...]))
            }
        }
        if U16.hasPrefix(rest, Prefix.aSlash) {
            // Prefer a split where both halves name the same path, which is
            // every plain modification.
            let half = (rest.count - 1) / 2
            if rest.count % 2 == 1, half >= 2, half + 3 <= rest.count,
               U16.hasPrefix(rest, Prefix.spaceB, at: half),
               rest[2..<half].elementsEqual(rest[(half + 3)...]) {
                let same = U16.string(rest[2..<half])
                return (same, same)
            }
            let split = U16.indexOf(rest, Prefix.spaceB, from: 0)
            if split > 0 {
                return (U16.string(rest[2..<split]), stripPrefix(rest[(split + 1)...]))
            }
        }
        let whole = U16.string(rest)
        return (whole, whole)
    }
}
