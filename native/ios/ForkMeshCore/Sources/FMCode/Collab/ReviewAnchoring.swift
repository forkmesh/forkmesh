import Foundation

public let reviewAnchorMaxLines = 12

public enum DiffSide: String, Codable, Hashable, Sendable, CaseIterable {
    case old, new

    public init(wire: String) { self = wire == "old" ? .old : .new }
}

public enum ReviewThreadPlacement: String, Codable, Hashable, Sendable, CaseIterable {
    case current, moved, outdated, legacy
}

public struct DiffSideIndex: Equatable, Sendable {
    public var lines: [Int] = []
    public var texts: [String] = []

    public init(lines: [Int] = [], texts: [String] = []) {
        self.lines = lines
        self.texts = texts
    }

    public func indexOfLine(_ line: Int) -> Int {
        var lo = 0
        var hi = lines.count - 1
        while lo <= hi {
            let mid = (lo + hi) / 2
            if lines[mid] == line { return mid }
            if lines[mid] < line { lo = mid + 1 } else { hi = mid - 1 }
        }
        return -1
    }

    public func contains(line: Int) -> Bool { indexOfLine(line) >= 0 }
}

public struct DiffSideIndexes: Sendable {
    private var sides: [String: DiffSideIndex] = [:]

    public init(patch: String) {
        var path = ""
        var inHunk = false
        var binary = false
        var oldNo = 0
        var newNo = 0
        var oldSide = DiffSideIndex()
        var newSide = DiffSideIndex()
        func flush() {
            guard !path.isEmpty else { return }
            sides[Self.key(path, .old), default: DiffSideIndex()].lines += oldSide.lines
            sides[Self.key(path, .old), default: DiffSideIndex()].texts += oldSide.texts
            sides[Self.key(path, .new), default: DiffSideIndex()].lines += newSide.lines
            sides[Self.key(path, .new), default: DiffSideIndex()].texts += newSide.texts
        }
        for raw in patch.split(separator: "\n", omittingEmptySubsequences: false) {
            var line = Substring(raw)
            if line.hasSuffix("\r") { line = line.dropLast() }
            if line.hasPrefix("diff --git ") {
                flush()
                oldSide = DiffSideIndex()
                newSide = DiffSideIndex()
                if let marker = line.range(of: " b/") {
                    path = String(line[marker.upperBound...])
                } else {
                    path = ""
                }
                inHunk = false
                binary = false
                continue
            }
            if path.isEmpty || binary { continue }
            if line.hasPrefix("@@") {
                if let (oldStart, newStart) = Self.hunkStarts(line) {
                    oldNo = oldStart
                    newNo = newStart
                    inHunk = true
                }
            } else if line.hasPrefix("GIT binary patch") || line.hasPrefix("Binary files ") {
                binary = true
                inHunk = false
            } else if inHunk && !line.isEmpty {
                let marker = line.first!
                if marker == "+" && !line.hasPrefix("+++ ") {
                    newSide.lines.append(newNo)
                    newSide.texts.append(String(line.dropFirst()))
                    newNo += 1
                } else if marker == "-" && !line.hasPrefix("--- ") {
                    oldSide.lines.append(oldNo)
                    oldSide.texts.append(String(line.dropFirst()))
                    oldNo += 1
                } else if marker == " " {
                    let context = String(line.dropFirst())
                    oldSide.lines.append(oldNo)
                    oldSide.texts.append(context)
                    newSide.lines.append(newNo)
                    newSide.texts.append(context)
                    oldNo += 1
                    newNo += 1
                } else if marker != "\\" {
                    inHunk = false
                }
            } else if inHunk && line.isEmpty {
                oldSide.lines.append(oldNo)
                oldSide.texts.append("")
                newSide.lines.append(newNo)
                newSide.texts.append("")
                oldNo += 1
                newNo += 1
            }
        }
        flush()
    }

    public func side(_ path: String, _ side: DiffSide) -> DiffSideIndex? { sides[Self.key(path, side)] }

    static func key(_ path: String, _ side: DiffSide) -> String { path + "\u{1F}" + side.rawValue }

    static func hunkStarts(_ line: Substring) -> (Int, Int)? {
        var rest = line.dropFirst(2)
        guard rest.hasPrefix(" -") else { return nil }
        rest = rest.dropFirst(2)
        func number(_ text: inout Substring) -> Int? {
            let digits = text.prefix { $0.isASCII && $0.isNumber }
            guard !digits.isEmpty, let value = Int(digits) else { return nil }
            text = text.dropFirst(digits.count)
            return value
        }
        func skipCount(_ text: inout Substring) -> Bool {
            guard text.hasPrefix(",") else { return true }
            text = text.dropFirst()
            return number(&text) != nil
        }
        guard let oldStart = number(&rest), skipCount(&rest), rest.hasPrefix(" +") else { return nil }
        rest = rest.dropFirst(2)
        guard let newStart = number(&rest), skipCount(&rest), rest.hasPrefix(" @@") else { return nil }
        return (oldStart, newStart)
    }
}

@inline(__always)
func sameText(_ a: String, _ b: String) -> Bool { a.utf8.elementsEqual(b.utf8) }

public enum ReviewAnchoring {
    public static func anchorLines(_ side: DiffSideIndex, lineStart: Int, lineEnd: Int) -> String {
        if lineStart <= 0 { return "" }
        let end = max(lineEnd, lineStart)
        let first = side.indexOfLine(lineStart)
        if first < 0 { return "" }
        var out: [String] = []
        let wanted = min(end - lineStart + 1, reviewAnchorMaxLines)
        for i in 0..<wanted {
            let at = first + i
            if at >= side.lines.count || side.lines[at] != lineStart + i { break }
            out.append(side.texts[at])
        }
        return out.joined(separator: "\n")
    }

    public static func anchorLines(patch: String, path: String, side: DiffSide, lineStart: Int, lineEnd: Int) -> String {
        if path.isEmpty || lineStart <= 0 { return "" }
        guard let index = DiffSideIndexes(patch: patch).side(path, side) else { return "" }
        return anchorLines(index, lineStart: lineStart, lineEnd: lineEnd)
    }

    public static func nearestMatch(_ side: DiffSideIndex, needle: String, preferred: Int) -> Int {
        let want = needle.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        if want.isEmpty { return -1 }
        var best = -1
        var i = 0
        while i + want.count <= side.texts.count {
            defer { i += 1 }
            if !sameText(side.texts[i], want[0]) { continue }
            var ok = true
            for k in 1..<want.count {
                if !sameText(side.texts[i + k], want[k]) || side.lines[i + k] != side.lines[i] + k {
                    ok = false
                    break
                }
            }
            if !ok { continue }
            let line = side.lines[i]
            if best < 0 || abs(line - preferred) < abs(best - preferred) { best = line }
            if line >= preferred { break }
        }
        return best
    }

    public struct Anchor: Equatable, Sendable {
        public var path: String
        public var side: DiffSide
        public var lineStart: Int
        public var lineEnd: Int
        public var patchset: String
        public var anchorLines: String

        public init(path: String, side: DiffSide, lineStart: Int, lineEnd: Int, patchset: String = "", anchorLines: String = "") {
            self.path = path
            self.side = side
            self.lineStart = lineStart
            self.lineEnd = lineEnd
            self.patchset = patchset
            self.anchorLines = anchorLines
        }
    }

    public struct Placement: Equatable, Sendable {
        public var placement: ReviewThreadPlacement
        public var currentLineStart: Int
        public var currentLineEnd: Int

        public init(_ placement: ReviewThreadPlacement, _ start: Int, _ end: Int) {
            self.placement = placement
            self.currentLineStart = start
            self.currentLineEnd = end
        }

        public var isOutdated: Bool { placement == .outdated }
    }

    public static func place(_ anchor: Anchor, side: DiffSideIndex?, currentPatchset: String) -> Placement {
        if anchor.path.isEmpty || anchor.lineStart <= 0 { return Placement(.legacy, 0, 0) }
        let span = max(0, anchor.lineEnd - anchor.lineStart)
        func at(_ placement: ReviewThreadPlacement, _ start: Int) -> Placement {
            Placement(placement, start, start + span)
        }
        let outdated = Placement(.outdated, 0, 0)
        let lineExists = side?.contains(line: anchor.lineStart) ?? false
        if anchor.patchset.isEmpty && anchor.anchorLines.isEmpty {
            return lineExists ? at(.legacy, anchor.lineStart) : outdated
        }
        if !anchor.patchset.isEmpty && !currentPatchset.isEmpty
            && anchor.patchset.lowercased() == currentPatchset.lowercased()
        {
            return at(.current, anchor.lineStart)
        }
        if !anchor.anchorLines.isEmpty {
            let found = side.map { nearestMatch($0, needle: anchor.anchorLines, preferred: anchor.lineStart) } ?? -1
            if found < 0 { return outdated }
            return at(found == anchor.lineStart ? .current : .moved, found)
        }
        return currentPatchset.isEmpty && lineExists ? at(.current, anchor.lineStart) : outdated
    }

    public static func label(_ placement: ReviewThreadPlacement, lineStart: Int, lineEnd: Int) -> String {
        switch placement {
        case .outdated: return "Outdated"
        case .moved:
            return lineEnd > lineStart ? "Moved from lines \(lineStart)–\(lineEnd)" : "Moved from line \(lineStart)"
        case .current, .legacy: return ""
        }
    }
}
