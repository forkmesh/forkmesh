import FMBase
import FMDiff
import FMGit
import Foundation
import Observation

public struct DiffReviewSection: Sendable {
    public let title: String
    public let files: [DiffFile]
    public let group: String

    public init(title: String = "", files: [DiffFile], group: String = "") {
        self.title = title
        self.files = files
        self.group = group
    }
}

public struct DiffReviewFile: Sendable {
    public let index: Int
    public let section: Int
    public let key: String
    public let file: DiffFile
    public let fingerprint: String
}

public enum DiffReviewRow: Identifiable, Equatable, Sendable {
    case section(Int)
    case header(Int)
    case notice(Int, Int, String)
    case hunk(Int, Int)
    case line(Int, DiffLine)
    case pair(Int, DiffSplitRow)
    case end(Int)
    case preview(Int)

    static func makeId(_ owner: Int, _ tag: Int, _ local: Int) -> Int { (owner << 24) | (tag << 21) | (local & 0x1F_FFFF) }

    public static func headerId(_ file: Int) -> Int { makeId(file, 0, 0) }
    public static func endId(_ file: Int) -> Int { makeId(file, 5, 0) }

    public var id: Int {
        switch self {
        case .header(let f): return DiffReviewRow.makeId(f, 0, 0)
        case .notice(let f, let i, _): return DiffReviewRow.makeId(f, 1, i)
        case .hunk(let f, let h): return DiffReviewRow.makeId(f, 2, h)
        case .line(let f, let line): return DiffReviewRow.makeId(f, 3, line.ordinal)
        case .pair(let f, let row): return DiffReviewRow.makeId(f, 4, row.id)
        case .end(let f): return DiffReviewRow.makeId(f, 5, 0)
        case .section(let s): return DiffReviewRow.makeId(s, 6, 0)
        case .preview(let f): return DiffReviewRow.makeId(f, 7, 0)
        }
    }

    public var file: Int? {
        switch self {
        case .header(let f), .notice(let f, _, _), .hunk(let f, _), .line(let f, _), .pair(let f, _), .end(let f),
            .preview(let f):
            return f
        case .section:
            return nil
        }
    }
}

public struct DiffScrollPass: Equatable, Sendable {
    public var lines = 0
    public var changed = 0
    public var flagged = 0

    public init(lines: Int = 0, changed: Int = 0, flagged: Int = 0) {
        self.lines = lines
        self.changed = changed
        self.flagged = flagged
    }

    mutating func count(_ row: DiffReviewRow, _ flags: (Int, DiffLine) -> [DiffLineFlag]) {
        switch row {
        case let .line(file, line): count(file, [line], flags)
        case let .pair(file, pair): count(file, [pair.left, pair.right].compactMap { $0 }, flags)
        case .hunk: lines += 1
        default: break
        }
    }

    private mutating func count(_ file: Int, _ sides: [DiffLine], _ flags: (Int, DiffLine) -> [DiffLineFlag]) {
        lines += 1
        if sides.contains(where: { $0.kind != .context }) { changed += 1 }
        if sides.contains(where: { !flags(file, $0).isEmpty }) { flagged += 1 }
    }
}

public struct DiffFindMatch: Equatable, Sendable {
    public let row: Int
    public let right: Bool
    public let start: Int
    public let length: Int
}

public enum DiffReviewText {
    public static func viewedCount(_ seen: Int, _ total: Int) -> String { "\(seen) of \(total) files viewed" }

    public static func kinds(_ files: [DiffFile]) -> String {
        var added = 0, removed = 0, renamed = 0, binaries = 0
        for file in files {
            if file.binary { binaries += 1 }
            switch file.status {
            case .added: added += 1
            case .deleted: removed += 1
            case .renamed: renamed += 1
            default: break
            }
        }
        var parts: [String] = []
        let modified = files.count - added - removed - renamed
        if modified > 0 { parts.append("\(modified) modified") }
        if added > 0 { parts.append("\(added) added") }
        if removed > 0 { parts.append("\(removed) removed") }
        if renamed > 0 { parts.append("\(renamed) renamed") }
        if binaries > 0 { parts.append("\(binaries) binary") }
        return parts.joined(separator: ", ")
    }

    public static func remaining(_ left: Int) -> String { left == 0 ? "every file reviewed" : "\(left) left to review" }

    public static func countsTooltip(_ file: DiffFile) -> String {
        let total = file.additions + file.deletions
        return "+\(file.additions) −\(file.deletions) · \(total) changed line\(total == 1 ? "" : "s")"
    }

    public static func viewedHint(_ viewed: Bool) -> String { viewed ? "Reopen this file" : "Collapse this file and mark it viewed" }
    public static func foldHint(_ folded: Bool) -> String { folded ? "Expand this file (it stays marked viewed)" : "Collapse this file" }

    public static func findCount(_ index: Int, _ total: Int) -> String { total == 0 ? "No results" : "\(index + 1)/\(total)" }

    public static let findHint = "Find in diff…"
    public static let nextPage = "Next page."
    public static let previousPage = "Previous page."
    public static let diffEnd = "That's the end of the diff."
    public static let diffTop = "You're at the top of the diff."
    public static let lastFile = "That's the last file."
    public static let firstFile = "That's the first file."

    public static func nextFile(_ file: DiffFile) -> String { "Next file, \(DiffFileRowText.name(file))." }
    public static func previousFile(_ file: DiffFile) -> String { "Previous file, \(DiffFileRowText.name(file))." }
    public static let emptyTitle = "No changes"
}

@MainActor
@Observable
public final class DiffReviewModel {
    public static let maxMatches = 5000
    public static let maxScopes = 200
    public static let fingerprintChars = 16
    static let scopesKey = "diffViewed/scopes"

    public let sections: [DiffReviewSection]
    public let files: [DiffReviewFile]
    public let scope: String
    public let git: DiffHealthGit
    public let longestColumns: Int
    public let gutterDigits: Int

    public private(set) var viewed: Set<String> = []
    public private(set) var expanded: Set<String> = []
    public private(set) var rows: [DiffReviewRow] = []
    public var split: Bool { didSet { if split != oldValue { rebuild() } } }
    public var query: String = "" { didSet { if query != oldValue { refind() } } }
    public private(set) var matches: [DiffFindMatch] = []
    public private(set) var matchIndex: Int = -1
    public private(set) var imageResults: [DiffImageRequest: DiffImageResult] = [:]

    @ObservationIgnored private let store: KeyValueStore?
    @ObservationIgnored private let images: DiffImageLoader?
    @ObservationIgnored private var positions: [Int: Int] = [:]
    @ObservationIgnored private var hunkPositions: [Int] = []
    @ObservationIgnored private var layouts: [Int: DiffFileLayout] = [:]
    @ObservationIgnored private var flagCache: [Int: [Int: [DiffLineFlag]]] = [:]
    @ObservationIgnored private var healthCache: [DiffHealthMetric]?
    @ObservationIgnored private var matchRows: [Int: [DiffFindMatch]] = [:]
    @ObservationIgnored private var pendingEnds: Set<Int> = []

    public init(
        sections: [DiffReviewSection], scope: String, split: Bool = false, git: DiffHealthGit = .unknown,
        store: KeyValueStore? = nil, images: DiffImageLoader? = nil
    ) {
        let shown = sections.filter { !$0.files.isEmpty }
        self.sections = shown
        self.scope = scope
        self.split = split
        self.git = git
        self.store = store
        self.images = images
        var list: [DiffReviewFile] = []
        for (s, section) in shown.enumerated() {
            for file in section.files {
                let key = shown.count > 1 ? "\(s):\(file.path)" : file.path
                list.append(DiffReviewFile(
                    index: list.count, section: s, key: key, file: file.prepared(),
                    fingerprint: DiffReviewModel.fingerprint(file.raw)))
            }
        }
        files = list
        longestColumns = min(max(list.map(\.file.longestLine).max() ?? 0, 0), 400)
        gutterDigits = min(max(String(list.map(\.file.maxLineNumber).max() ?? 0).count, 2), 7)
        restore()
        rebuild()
    }

    public convenience init(
        files: [DiffFile], scope: String, split: Bool = false, git: DiffHealthGit = .unknown, store: KeyValueStore? = nil,
        images: DiffImageLoader? = nil
    ) {
        self.init(sections: [DiffReviewSection(files: files)], scope: scope, split: split, git: git, store: store, images: images)
    }

    public static func fingerprint(_ raw: String) -> String {
        String(GitObjectId.sha1Of(Array(raw.utf8)).hex.prefix(fingerprintChars))
    }

    public static func patchScope(_ patch: String) -> String { "patch/" + fingerprint(patch) }

    public var allFiles: [DiffFile] { files.map(\.file) }

    // MARK: Viewed

    public func isViewed(_ index: Int) -> Bool { index >= 0 && index < files.count && viewed.contains(files[index].key) }

    public func isFolded(_ index: Int) -> Bool {
        guard index >= 0 && index < files.count else { return false }
        let key = files[index].key
        return viewed.contains(key) && !expanded.contains(key)
    }

    public var seen: Int { files.filter { viewed.contains($0.key) }.count }
    public var progress: Double { files.isEmpty ? 0 : Double(seen) / Double(files.count) }
    public var viewedLabel: String { DiffReviewText.viewedCount(seen, files.count) }
    public var remainingLabel: String { DiffReviewText.remaining(files.count - seen) }
    public var kindsLabel: String { DiffReviewText.kinds(allFiles) }
    public var additions: Int { files.reduce(0) { $0 + $1.file.additions } }
    public var deletions: Int { files.reduce(0) { $0 + $1.file.deletions } }

    @discardableResult
    public func toggleViewed(_ index: Int) -> Int? {
        guard index >= 0 && index < files.count else { return nil }
        let key = files[index].key
        expanded.remove(key)
        if viewed.remove(key) != nil {
            persist()
            rebuild()
            return nil
        }
        viewed.insert(key)
        persist()
        rebuild()
        return index + 1 < files.count ? index + 1 : nil
    }

    public func toggleFold(_ index: Int) {
        guard isViewed(index) else { return }
        let key = files[index].key
        if expanded.remove(key) == nil { expanded.insert(key) }
        rebuild()
    }

    @discardableResult
    public func autoMark(_ index: Int) -> Bool {
        guard index >= 0 && index < files.count, !isViewed(index) else { return false }
        let key = files[index].key
        viewed.insert(key)
        expanded.insert(key)
        persist()
        return true
    }

    public func endAppeared(_ index: Int) { pendingEnds.insert(index) }
    public func endDisappeared(_ index: Int) { pendingEnds.remove(index) }

    @discardableResult
    public func settleScroll() -> [Int] {
        pendingEnds.sorted().filter { autoMark($0) }
    }

    @discardableResult
    public func autoMarkPassed(from old: Int?, to new: Int?) -> [Int] {
        guard let old, let new else { return [] }
        let start = positions[old] ?? -1
        let stop = min(positions[new] ?? -1, rows.count - 1)
        guard stop > start else { return [] }
        var marked: [Int] = []
        for p in (start + 1)...stop {
            if case .end(let file) = rows[p], autoMark(file) { marked.append(file) }
        }
        return marked
    }

    public func scrollPass(from old: Int?, to new: Int?) -> DiffScrollPass {
        var pass = DiffScrollPass()
        guard let old, let new, let a = positions[old], let b = positions[new], a != b else { return pass }
        for p in (min(a, b) + 1)...max(a, b) { pass.count(rows[p], flags) }
        return pass
    }

    public func scrollPass(crossing ids: [Int]) -> DiffScrollPass {
        var pass = DiffScrollPass()
        for id in ids { if let p = positions[id] { pass.count(rows[p], flags) } }
        return pass
    }

    public func nextUnviewed(after index: Int) -> Int? {
        let count = files.count
        if count == 0 { return nil }
        for step in 1...count {
            let candidate = (index + step) % count
            if !isViewed(candidate) && candidate != index { return candidate }
        }
        return nil
    }

    // MARK: Rows

    public func position(of rowId: Int) -> Int? { positions[rowId] }

    public func fileAt(rowId: Int?) -> Int? {
        guard let rowId, let position = positions[rowId] else { return nil }
        var p = position
        while p >= 0 {
            if let file = rows[p].file { return file }
            p -= 1
        }
        return files.isEmpty ? nil : 0
    }

    public func nextChange(from rowId: Int?) -> Int? {
        let current = rowId.flatMap { positions[$0] } ?? -1
        guard let target = hunkPositions.first(where: { $0 > current }) else { return nil }
        return rows[target].id
    }

    public func previousChange(from rowId: Int?) -> Int? {
        guard let rowId, let current = positions[rowId] else { return nil }
        guard let target = hunkPositions.last(where: { $0 < current }) else { return nil }
        return rows[target].id
    }

    public func pageDown(top: Int?, bottom: Int?) -> Int? {
        let current = top.flatMap { positions[$0] } ?? -1
        let target = max(bottom.flatMap { positions[$0] } ?? -1, current + 1)
        return target < rows.count - 1 ? rows[target].id : nil
    }

    public func pageUp(top: Int?) -> Int? {
        guard let top, positions[top] != nil else { return nil }
        return top
    }

    public func nextFile(from rowId: Int?) -> Int? {
        let next = rowId.flatMap { positions[$0] } == nil ? 0 : (fileAt(rowId: rowId) ?? -1) + 1
        return next < files.count ? next : nil
    }

    public func previousFile(from rowId: Int?) -> Int? {
        guard rowId.flatMap({ positions[$0] }) != nil, let file = fileAt(rowId: rowId), file > 0 else { return nil }
        return file - 1
    }

    private func rebuild() {
        var out: [DiffReviewRow] = []
        var lastSection = -1
        for entry in files {
            if sections.count > 1 && entry.section != lastSection {
                lastSection = entry.section
                out.append(.section(entry.section))
            }
            let i = entry.index
            out.append(.header(i))
            if isFolded(i) { continue }
            let previewed = showsPreview(i)
            for (n, note) in DiffFileLayout.notices(entry.file, previewed: previewed).enumerated() { out.append(.notice(i, n, note)) }
            if previewed { out.append(.preview(i)) }
            if split {
                for row in entry.file.splitRows {
                    if let h = row.hunkIndex { out.append(.hunk(i, h)) } else { out.append(.pair(i, row)) }
                }
            } else {
                for (h, hunk) in entry.file.hunks.enumerated() {
                    out.append(.hunk(i, h))
                    for line in hunk.lines { out.append(.line(i, line)) }
                }
            }
            out.append(.end(i))
        }
        rows = out
        var map: [Int: Int] = [:]
        map.reserveCapacity(out.count)
        var hunks: [Int] = []
        for (p, row) in out.enumerated() {
            map[row.id] = p
            if case .hunk = row { hunks.append(p) }
        }
        positions = map
        hunkPositions = hunks
        refind()
    }

    // MARK: Images

    public func showsPreview(_ index: Int) -> Bool {
        images != nil && index >= 0 && index < files.count && DiffImages.previewable(files[index].file)
    }

    public func imageRequests(_ index: Int) -> [DiffImageRequest] {
        guard showsPreview(index) else { return [] }
        let entry = files[index]
        return DiffImages.requests(entry, group: sections[entry.section].group)
    }

    public func loadImages(_ index: Int) async {
        guard let images else { return }
        for request in imageRequests(index) {
            switch imageResults[request] {
            case .loading, .loaded: continue
            case .failed, nil: break
            }
            imageResults[request] = .loading
            do {
                let data = try await images(request)
                imageResults[request] = data.isEmpty ? .failed(DiffImages.unreadable) : .loaded(data)
            } catch is CancellationError {
                imageResults[request] = nil
                return
            } catch {
                imageResults[request] = .failed(DiffImages.describe(error))
            }
        }
    }

    // MARK: Layout, flags, health

    public func layout(_ index: Int) -> DiffFileLayout {
        if let cached = layouts[index] { return cached }
        let made = DiffFileLayout(files[index].file)
        layouts[index] = made
        return made
    }

    public func marks(_ index: Int, _ line: DiffLine) -> [DiffLineMark] {
        DiffLineMark.of(flags(index, line))
    }

    public func flags(_ index: Int, _ line: DiffLine) -> [DiffLineFlag] {
        guard line.kind == .added, index >= 0, index < files.count else { return [] }
        if let cached = flagCache[index]?[line.ordinal] { return cached }
        let found = DiffHealth.flags(path: files[index].file.path, text: line.text)
        flagCache[index, default: [:]][line.ordinal] = found
        return found
    }

    public var health: [DiffHealthMetric] {
        if let healthCache { return healthCache }
        let made = DiffHealth.report(allFiles, git: git)
        healthCache = made
        return made
    }

    // MARK: Find

    public func matches(in rowId: Int, right: Bool) -> [DiffFindMatch] {
        (matchRows[rowId] ?? []).filter { $0.right == right }
    }

    public var currentMatch: DiffFindMatch? { matchIndex >= 0 && matchIndex < matches.count ? matches[matchIndex] : nil }

    public var findLabel: String { DiffReviewText.findCount(matchIndex, matches.count) }

    @discardableResult
    public func stepMatch(_ forward: Bool) -> Int? {
        if matches.isEmpty { return nil }
        matchIndex = forward ? (matchIndex + 1) % matches.count : (matchIndex - 1 + matches.count) % matches.count
        return matches[matchIndex].row
    }

    private func refind() {
        let needle = query
        var found: [DiffFindMatch] = []
        var byRow: [Int: [DiffFindMatch]] = [:]
        if !needle.isEmpty {
            func scan(_ text: String, _ row: Int, _ right: Bool) {
                let ns = text as NSString
                var from = 0
                while from < ns.length && found.count < DiffReviewModel.maxMatches {
                    let hit = ns.range(of: needle, options: [.caseInsensitive], range: NSRange(location: from, length: ns.length - from))
                    if hit.location == NSNotFound || hit.length == 0 { break }
                    let match = DiffFindMatch(row: row, right: right, start: hit.location, length: hit.length)
                    found.append(match)
                    byRow[row, default: []].append(match)
                    from = hit.location + hit.length
                }
            }
            for row in rows where found.count < DiffReviewModel.maxMatches {
                switch row {
                case .line(_, let line):
                    scan(line.text, row.id, true)
                case .pair(_, let pair):
                    if let left = pair.left, left.kind != .context { scan(left.text, row.id, false) }
                    if let right = pair.right { scan(right.text, row.id, true) }
                default:
                    break
                }
            }
        }
        matches = found
        matchRows = byRow
        matchIndex = found.isEmpty ? -1 : 0
    }

    // MARK: Persistence

    private var storeKey: String { "diffViewed/" + scope }

    private func restore() {
        guard let store, !scope.isEmpty, let marks = store.stringList(forKey: storeKey) else { return }
        let current = Dictionary(files.map { ($0.key, $0.fingerprint) }, uniquingKeysWith: { first, _ in first })
        var kept: Set<String> = []
        for mark in marks {
            guard let space = mark.firstIndex(of: " ") else { continue }
            let fp = String(mark[..<space])
            let key = String(mark[mark.index(after: space)...])
            if current[key] == fp { kept.insert(key) }
        }
        viewed = kept
    }

    private func persist() {
        guard let store, !scope.isEmpty else { return }
        let marks = files.filter { viewed.contains($0.key) }.map { "\($0.fingerprint) \($0.key)" }
        let previous = store.stringList(forKey: storeKey) ?? []
        let known = Set(files.map(\.key))
        let others = previous.filter { mark in
            guard let space = mark.firstIndex(of: " ") else { return false }
            return !known.contains(String(mark[mark.index(after: space)...]))
        }
        let all = marks + others
        if all.isEmpty { store.remove(storeKey) } else { store.set(all, forKey: storeKey) }
        var scopes = (store.stringList(forKey: DiffReviewModel.scopesKey) ?? []).filter { $0 != scope }
        scopes.insert(scope, at: 0)
        while scopes.count > DiffReviewModel.maxScopes {
            store.remove("diffViewed/" + scopes.removeLast())
        }
        store.set(scopes, forKey: DiffReviewModel.scopesKey)
    }
}
