// The diff viewer logic: view settings, change kinds, the stat summary's
// proportion bar, file row lines, emphasis, file body rows, notices, hunk
// labels and span cache, and sizing inputs. The drawing is the app's.

import FMApi
import FMBase
import FMDiff
import Foundation
import Observation

/// How diffs are laid out. One shared instance, so a reader who switches to
/// side-by-side or a larger size keeps it from file to file and screen to
/// screen; `attach` keeps it across launches.
@MainActor
@Observable
public final class DiffViewSettings {
    public static let shared = DiffViewSettings()

    public static let minFontSize: Double = 9
    public static let maxFontSize: Double = 18
    public static let defaultFontSize: Double = 12

    public var split = false { didSet { save("diff/split", split) } }
    public var wrap = true { didSet { save("diff/wrap", wrap) } }
    public var syntax = true { didSet { save("diff/syntax", syntax) } }
    public var autoViewed = true { didSet { save("diff/autoViewed", autoViewed) } }
    public var findings = false { didSet { save("diff/findings", findings) } }
    public var scrollSound = true { didSet { save("diff/scrollSound", scrollSound) } }
    public var scrollHaptic = true { didSet { save("diff/scrollHaptic", scrollHaptic) } }
    public var pageButtons = true { didSet { save("diff/pageButtons", pageButtons) } }
    public private(set) var fontSize: Double = DiffViewSettings.defaultFontSize

    @ObservationIgnored private var store: KeyValueStore?

    public init() {}

    public func attach(_ store: KeyValueStore?) {
        guard let store, self.store !== store else { return }
        self.store = nil
        split = store.bool(forKey: "diff/split") ?? split
        wrap = store.bool(forKey: "diff/wrap") ?? wrap
        syntax = store.bool(forKey: "diff/syntax") ?? syntax
        autoViewed = store.bool(forKey: "diff/autoViewed") ?? autoViewed
        findings = store.bool(forKey: "diff/findings") ?? findings
        scrollSound = store.bool(forKey: "diff/scrollSound") ?? scrollSound
        scrollHaptic = store.bool(forKey: "diff/scrollHaptic") ?? scrollHaptic
        pageButtons = store.bool(forKey: "diff/pageButtons") ?? pageButtons
        if let size = store.double(forKey: "diff/fontSize") { fontSize = min(max(size, DiffViewSettings.minFontSize), DiffViewSettings.maxFontSize) }
        self.store = store
    }

    private func save(_ key: String, _ value: Bool) {
        store?.set(value, forKey: key)
    }

    /// Clamped to `minFontSize...maxFontSize`.
    public func setFontSize(_ value: Double) {
        let clamped = min(max(value, DiffViewSettings.minFontSize), DiffViewSettings.maxFontSize)
        if clamped != fontSize {
            fontSize = clamped
            store?.set(clamped, forKey: "diff/fontSize")
        }
    }

    public var canShrink: Bool { fontSize > DiffViewSettings.minFontSize }
    public var canGrow: Bool { fontSize < DiffViewSettings.maxFontSize }

    public func reset() {
        split = false
        wrap = true
        syntax = true
        autoViewed = true
        findings = false
        scrollSound = true
        scrollHaptic = true
        pageButtons = true
        fontSize = DiffViewSettings.defaultFontSize
    }

    public var layoutToggleLabel: String { split ? "Unified view" : "Side-by-side view" }
    public var layoutToggleIcon: String { split ? "view_agenda_outlined" : "vertical_split_outlined" }
}

/// `diffChangeKind`: the badge letter of a file's status.
public func diffChangeKind(_ status: DiffFileStatus) -> PatchChangeKind {
    switch status {
    case .added, .copied: return .added
    case .deleted: return .deleted
    case .renamed: return .renamed
    case .modified: return .modified
    }
}

/// One square of DiffStatSummary's five-square bar.
public enum DiffStatSquare: String, Sendable {
    case added, removed, empty
}

/// GitHub's five-square proportion bar: green for the additions' share
/// (rounded half away from zero), red for the rest.
public func diffStatBar(additions: Int, deletions: Int) -> [DiffStatSquare] {
    let total = additions + deletions
    let green = total == 0 ? 0 : Int((Double(additions) * 5 / Double(total)).rounded(.toNearestOrAwayFromZero))
    let red = total == 0 ? 0 : min(max(5 - green, 0), 5)
    return (0..<5).map { $0 < green ? .added : ($0 < green + red ? .removed : .empty) }
}

/// DiffFileRow's two lines.
public enum DiffFileRowText {
    public static func name(_ file: DiffFile) -> String {
        guard let slash = file.path.lastIndex(of: "/") else { return file.path }
        return String(file.path[file.path.index(after: slash)...])
    }

    public static func directory(_ file: DiffFile) -> String {
        guard let slash = file.path.lastIndex(of: "/") else { return "" }
        return String(file.path[...slash])
    }

    public static func isRenamed(_ file: DiffFile) -> Bool {
        (file.status == .renamed || file.status == .copied) && file.oldPath != file.newPath
    }

    /// The second line: "old/path →" for a rename, else the directory ("" for none).
    public static func detail(_ file: DiffFile) -> String {
        isRenamed(file) ? "\(file.oldPath) →" : directory(file)
    }

    public static func viewedLabel(_ viewed: Bool) -> String { viewed ? "Mark as not viewed" : "Mark as viewed" }
}

/// A coloured piece of a diff line, with `emphasized` set where the line
/// changed against its pair.
public struct DiffRun: Equatable, Sendable {
    public let text: String
    public let kind: TokenKind
    public let emphasized: Bool

    public init(_ text: String, _ kind: TokenKind, emphasized: Bool = false) {
        self.text = text
        self.kind = kind
        self.emphasized = emphasized
    }
}

/// `_applyEmphasis`: splits the pieces at `ranges` (UTF-16, sorted) and marks
/// those parts.
public func applyDiffEmphasis(_ pieces: [(text: String, kind: TokenKind)], _ ranges: [DiffRange]) -> [DiffRun] {
    if ranges.isEmpty { return pieces.map { DiffRun($0.text, $0.kind) } }
    var out: [DiffRun] = []
    var offset = 0
    var r = 0
    for piece in pieces {
        let units = Array(piece.text.utf16)
        let end = offset + units.count
        var cursor = offset
        func slice(_ from: Int, _ to: Int) -> String {
            String(decoding: units[(from - offset)..<(to - offset)], as: UTF16.self)
        }
        while cursor < end {
            while r < ranges.count && ranges[r].end <= cursor { r += 1 }
            let range: DiffRange? = r < ranges.count ? ranges[r] : nil
            if range == nil || range!.start >= end {
                out.append(DiffRun(slice(cursor, end), piece.kind))
                cursor = end
            } else if range!.start > cursor {
                out.append(DiffRun(slice(cursor, range!.start), piece.kind))
                cursor = range!.start
            } else {
                let stop = min(range!.end, end)
                out.append(DiffRun(slice(cursor, stop), piece.kind, emphasized: true))
                cursor = stop
            }
        }
        offset = end
    }
    return out
}

/// A row of the unified layout: a hunk header (index into `hunks`) or a line.
public enum DiffUnifiedRow: Identifiable, Equatable, Sendable {
    case hunk(Int)
    case line(DiffLine)

    public var id: String {
        switch self {
        case .hunk(let index): return "h\(index)"
        case .line(let line): return "l\(line.ordinal)"
        }
    }
}

/// One file's state in the review stream: the prepared file, its unified rows, the
/// block-comment starts per side and the highlighted runs, cached per line.
@MainActor
public final class DiffFileLayout {
    public let file: DiffFile
    public let unifiedRows: [DiffUnifiedRow]
    public let splitRows: [DiffSplitRow]
    private let highlighter: CodeHighlighter
    private var startsInComment: Set<Int> = []
    private var cache: [Int: [DiffRun]] = [:]
    private var cachedSyntax: Bool?

    public init(_ file: DiffFile) {
        let prepared = file.prepared()
        self.file = prepared
        highlighter = CodeHighlighter(CodeLang.forPath(prepared.path))
        var rows: [DiffUnifiedRow] = []
        for (index, hunk) in prepared.hunks.enumerated() {
            rows.append(.hunk(index))
            rows.append(contentsOf: hunk.lines.map { DiffUnifiedRow.line($0) })
        }
        unifiedRows = rows
        splitRows = prepared.splitRows
        // Block comments are tracked per side, since a removed line and the
        // added line replacing it belong to different versions of the file.
        for side in [DiffLineKind.removed, DiffLineKind.added] {
            let lines = prepared.hunks.flatMap { $0.lines }.filter { $0.kind == .context || $0.kind == side }
            let starts = highlighter.blockCommentStarts(lines.map(\.text))
            for (i, line) in lines.enumerated() where starts[i] { startsInComment.insert(line.ordinal) }
        }
    }

    public var hunks: [DiffHunk] { file.hunks }

    /// "⋯ 12 unchanged lines" before a hunk, or "".
    public func hunkLabel(_ index: Int) -> String {
        let hunks = file.hunks
        guard index >= 0, index < hunks.count else { return "" }
        let hunk = hunks[index]
        let hidden: Int
        if index > 0 {
            let previous = hunks[index - 1]
            hidden = hunk.newStart - (previous.newStart + previous.newCount)
        } else {
            hidden = hunk.newStart - 1
        }
        return hidden > 0 ? "⋯ \(hidden) unchanged \(hidden == 1 ? "line" : "lines")" : ""
    }

    public func hunkHeader(_ index: Int) -> String {
        index >= 0 && index < file.hunks.count ? file.hunks[index].header : ""
    }

    /// The notes above the first hunk (binary, rename, mode, empty file).
    public var notices: [String] { DiffFileLayout.notices(file) }

    public nonisolated static func notices(_ file: DiffFile, previewed: Bool = false) -> [String] {
        var notes: [String] = []
        if file.binary && !previewed { notes.append("Binary file — contents are not shown.") }
        if DiffFileRowText.isRenamed(file) {
            notes.append(
                "\(file.status == .copied ? "Copied" : "Renamed") from \(file.oldPath)"
                    + (file.similarity >= 0 ? " (\(file.similarity)% similar)" : ""))
        }
        if file.modeChanged { notes.append("Mode changed \(file.oldMode) → \(file.newMode)") }
        if file.status == .added && file.hunks.isEmpty { notes.append("New empty file.") }
        if file.status == .deleted && file.hunks.isEmpty { notes.append("Deleted empty file.") }
        return notes
    }

    /// Shown when there is nothing else to show.
    public static let noLineChanges = "No line changes to show."

    /// The line in pieces: highlighted (or plain), with the changed parts of
    /// an added or removed line marked.
    public func runs(_ line: DiffLine, syntax: Bool) -> [DiffRun] {
        if cachedSyntax != syntax {
            cachedSyntax = syntax
            cache.removeAll()
        }
        if let cached = cache[line.ordinal] { return cached }
        let pieces: [(text: String, kind: TokenKind)] = syntax
            ? highlighter.pieces(line.text, startsInBlockComment: startsInComment.contains(line.ordinal))
            : [(line.text, TokenKind.plain)]
        let runs = line.kind == .context ? pieces.map { DiffRun($0.text, $0.kind) } : applyDiffEmphasis(pieces, line.emphasis)
        cache[line.ordinal] = runs
        return runs
    }

    public static func sign(_ kind: DiffLineKind) -> String {
        switch kind {
        case .added: return "+"
        case .removed: return "−"
        case .context: return " "
        }
    }

    /// Gutter width in digits: `'${maxLineNumber}'.length.clamp(2, 7)`.
    public var gutterDigits: Int { min(max(String(file.maxLineNumber).count, 2), 7) }

    /// Past a few hundred columns a sideways canvas stops being readable;
    /// longer lines clip, and wrapping shows them in full.
    public var longestColumns: Int { min(max(file.longestLine, 0), 400) }
}
