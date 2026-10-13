import FMApi
import FMBase
import Foundation

public struct IssueBoardColumn: Equatable, Sendable, Identifiable {
    public var id: String { name }
    public let name: String
    public let issues: [Issue]
    public let isDone: Bool
}

public struct IssueBoardMove: Equatable, Sendable {
    public let number: Int
    public let from: String
    public let to: String
    public let edits: [IssueEdit]

    public var summary: String { "Moved #\(number) to \(to)." }
}

public enum IssueBoard {
    public static let defaultColumns = ["Backlog", "Todo", "In Progress", "Done"]
    public static let maxColumns = 12
    public static let tooFewColumns = "A board needs at least two columns."

    public static func statusLabel(_ column: String) -> String { "status:" + column.dartTrimmed.lowercased() }

    public static func isStatusLabel(_ label: String) -> Bool { label.lowercased().hasPrefix("status:") }

    public static func cleanColumns(_ columns: [String]) -> [String] {
        var out: [String] = []
        for column in columns {
            let name = String(column.dartTrimmed.prefix(40))
            if name.isEmpty || out.contains(where: { $0.lowercased() == name.lowercased() }) { continue }
            out.append(name)
            if out.count >= maxColumns { break }
        }
        return out
    }

    public static func columns(saved: [String]) -> [String] {
        let clean = cleanColumns(saved)
        return clean.isEmpty ? defaultColumns : clean
    }

    public static func validate(_ columns: [String]) -> String? {
        cleanColumns(columns).count < 2 ? tooFewColumns : nil
    }

    public static func column(of issue: Issue, columns: [String]) -> String {
        guard let first = columns.first, let last = columns.last else { return "" }
        if !issue.isOpen { return last }
        for column in columns {
            let want = statusLabel(column)
            if issue.labels.contains(where: { $0.lowercased() == want }) { return column }
        }
        return first
    }

    public static func layout(_ issues: [Issue], columns: [String], filter: (Issue) -> Bool = { _ in true }) -> [IssueBoardColumn] {
        var buckets: [String: [Issue]] = [:]
        for issue in issues where filter(issue) {
            buckets[column(of: issue, columns: columns), default: []].append(issue)
        }
        return columns.enumerated().map { index, name in
            IssueBoardColumn(
                name: name,
                issues: (buckets[name] ?? []).sorted { a, b in
                    a.priority != b.priority ? a.priority > b.priority : a.number > b.number
                },
                isDone: index == columns.count - 1)
        }
    }

    public static func move(_ issue: Issue, to target: String, columns: [String]) -> IssueBoardMove? {
        guard let last = columns.last,
              let column = columns.first(where: { $0.lowercased() == target.lowercased() }) else { return nil }
        let current = self.column(of: issue, columns: columns)
        if current.lowercased() == column.lowercased() { return nil }
        let toDone = column.lowercased() == last.lowercased()
        var labels = issue.labels.filter { !isStatusLabel($0) }
        if !toDone { labels.append(statusLabel(column)) }
        var edits: [IssueEdit] = []
        if labels != issue.labels { edits.append(.labels(labels)) }
        if issue.isOpen == toDone { edits.append(.status(open: !toDone)) }
        return IssueBoardMove(number: issue.number, from: current, to: column, edits: edits)
    }

    public static func neighbours(of column: String, columns: [String]) -> (left: String?, right: String?) {
        guard let index = columns.firstIndex(where: { $0.lowercased() == column.lowercased() }) else { return (nil, nil) }
        return (index > 0 ? columns[index - 1] : nil, index + 1 < columns.count ? columns[index + 1] : nil)
    }

    public static func storageKey(owner: String, name: String) -> String { "issueBoard/columns/\(owner)/\(name)" }

    public static func load(_ store: KeyValueStore, owner: String, name: String) -> [String] {
        columns(saved: store.stringList(forKey: storageKey(owner: owner, name: name)) ?? [])
    }

    public static func save(_ store: KeyValueStore, owner: String, name: String, columns: [String]) {
        store.set(cleanColumns(columns), forKey: storageKey(owner: owner, name: name))
    }
}
