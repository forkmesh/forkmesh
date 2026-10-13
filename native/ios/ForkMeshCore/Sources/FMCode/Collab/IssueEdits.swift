import FMApi
import FMBase
import Foundation

public struct IssueEdit: Equatable, Sendable {
    public var type: String
    public var fields: JSONObject

    public init(type: String, fields: JSONObject = [:]) {
        self.type = type
        self.fields = fields
    }

    public static let maxTitle = 240
    public static let maxLabels = 30
    public static let maxAssignees = 20
    public static let maxLabelLength = 50

    public static func title(_ title: String) -> IssueEdit {
        IssueEdit(type: "title", fields: ["title": .string(String(title.dartTrimmed.prefix(maxTitle)))])
    }

    public static func status(open: Bool) -> IssueEdit {
        IssueEdit(type: "status", fields: ["status": .string(open ? "open" : "closed")])
    }

    public static func labels(_ labels: [String]) -> IssueEdit {
        IssueEdit(type: "labels", fields: ["labels": JSONValue(IssueFields.cleanList(labels, max: maxLabels, length: maxLabelLength))])
    }

    public static func assignees(_ names: [String]) -> IssueEdit {
        IssueEdit(type: "assignees", fields: [
            "assignees": JSONValue(IssueFields.cleanList(names.map { $0.lowercased() }, max: maxAssignees, length: 64)),
        ])
    }

    public static func priority(_ value: Int) -> IssueEdit {
        IssueEdit(type: "priority", fields: ["priority": .int(Int64(IssueFields.clampPriority(value)))])
    }

    public static func progress(_ value: Int) -> IssueEdit {
        IssueEdit(type: "progress", fields: ["progress": .int(Int64(min(max(value, 0), 100)))])
    }

    public static func milestone(_ name: String) -> IssueEdit {
        IssueEdit(type: "milestone", fields: ["milestone": .string(name.dartTrimmed)])
    }

    public static func dates(startMs: Int64, endMs: Int64) -> IssueEdit {
        IssueEdit(type: "dates", fields: ["startDate": .int(max(0, startMs)), "endDate": .int(max(0, endMs))])
    }

    public static func comment(_ body: String) -> IssueEdit {
        IssueEdit(type: "comment", fields: ["body": .string(body), "attachments": .array([])])
    }

    public static func editBody(target: String, body: String) -> IssueEdit {
        IssueEdit(type: "edit", fields: ["target": .string(target), "body": .string(body), "attachments": .array([])])
    }

    public static func react(target: String, reaction: String) -> IssueEdit {
        IssueEdit(type: "reaction", fields: ["target": .string(target), "reaction": .string(reaction)])
    }

    public static func retract(eventId: String) -> IssueEdit {
        IssueEdit(type: "delete", fields: ["target": .string(eventId)])
    }

    public static func deleteOwnIssue() -> IssueEdit {
        IssueEdit(type: "delete", fields: ["target": "self"])
    }
}

public enum IssueFields {
    public static let priorities: [(Int, String)] = [(0, "None"), (1, "Low"), (2, "Medium"), (3, "High"), (4, "Urgent")]

    public static func clampPriority(_ value: Int) -> Int { min(max(value, 0), 4) }

    public static func priorityLabel(_ value: Int) -> String {
        priorities.first { $0.0 == clampPriority(value) }?.1 ?? "None"
    }

    public static func cleanList(_ values: [String], max: Int, length: Int) -> [String] {
        var seen = Set<String>()
        var out: [String] = []
        for value in values {
            let clean = String(value.dartTrimmed.prefix(length))
            if clean.isEmpty || seen.contains(clean.lowercased()) { continue }
            seen.insert(clean.lowercased())
            out.append(clean)
            if out.count >= max { break }
        }
        return out
    }

    public static func parseList(_ text: String) -> [String] {
        text.split(whereSeparator: { $0 == "," || $0 == "\n" }).map { String($0).dartTrimmed }.filter { !$0.isEmpty }
    }

    public static func diff(_ issue: Issue, title: String? = nil, labels: [String]? = nil, assignees: [String]? = nil,
                            priority: Int? = nil, progress: Int? = nil, milestone: String? = nil, open: Bool? = nil) -> [IssueEdit] {
        var edits: [IssueEdit] = []
        if let title, title.dartTrimmed != issue.title.dartTrimmed, !title.dartTrimmed.isEmpty { edits.append(.title(title)) }
        if let labels {
            let clean = cleanList(labels, max: IssueEdit.maxLabels, length: IssueEdit.maxLabelLength)
            if clean != issue.labels { edits.append(.labels(clean)) }
        }
        if let assignees {
            let clean = cleanList(assignees.map { $0.lowercased() }, max: IssueEdit.maxAssignees, length: 64)
            if clean != issue.assignees { edits.append(.assignees(clean)) }
        }
        if let priority, clampPriority(priority) != issue.priority { edits.append(.priority(priority)) }
        if let progress, min(max(progress, 0), 100) != issue.progress { edits.append(.progress(progress)) }
        if let milestone, milestone.dartTrimmed != issue.milestone { edits.append(.milestone(milestone)) }
        if let open, open != issue.isOpen { edits.append(.status(open: open)) }
        return edits
    }

    public static func canDeleteOwn(_ issue: Issue, author: String) -> Bool {
        let creator = issue.events.first { $0.type == "open" }?.author ?? issue.author
        return !author.isEmpty && creator == author
    }
}

public struct IssueChecklistItem: Equatable, Sendable, Identifiable {
    public var id: Int { lineIndex }
    public let lineIndex: Int
    public let text: String
    public let done: Bool
}

public enum IssueChecklist {
    static func parse(line: String) -> (prefix: String, done: Bool, text: String)? {
        var rest = Substring(line)
        let indent = rest.prefix { $0 == " " || $0 == "\t" }
        rest = rest.dropFirst(indent.count)
        guard let bullet = rest.first, bullet == "-" || bullet == "*" || bullet == "+" else { return nil }
        rest = rest.dropFirst()
        guard rest.hasPrefix(" [") else { return nil }
        rest = rest.dropFirst(2)
        guard let mark = rest.first, mark == " " || mark == "x" || mark == "X" else { return nil }
        rest = rest.dropFirst()
        guard rest.hasPrefix("]") else { return nil }
        rest = rest.dropFirst()
        guard rest.isEmpty || rest.hasPrefix(" ") else { return nil }
        return ("\(indent)\(bullet) [", mark != " ", String(rest.dropFirst(rest.isEmpty ? 0 : 1)))
    }

    public static func items(_ body: String) -> [IssueChecklistItem] {
        var out: [IssueChecklistItem] = []
        var inFence = false
        for (index, line) in body.components(separatedBy: "\n").enumerated() {
            if line.dartTrimmed.hasPrefix("```") { inFence.toggle(); continue }
            if inFence { continue }
            if let item = parse(line: line) { out.append(IssueChecklistItem(lineIndex: index, text: item.text, done: item.done)) }
        }
        return out
    }

    public static func progress(_ body: String) -> (done: Int, total: Int) {
        let all = items(body)
        return (all.filter(\.done).count, all.count)
    }

    public static func toggled(_ body: String, lineIndex: Int) -> String? {
        var lines = body.components(separatedBy: "\n")
        guard lines.indices.contains(lineIndex), items(body).contains(where: { $0.lineIndex == lineIndex }),
              let item = parse(line: lines[lineIndex]) else { return nil }
        lines[lineIndex] = "\(item.prefix)\(item.done ? " " : "x")]" + (item.text.isEmpty ? "" : " \(item.text)")
        return lines.joined(separator: "\n")
    }
}

public enum IssueLinks {
    static let closingVerbs = ["close", "closes", "closed", "fix", "fixes", "fixed", "resolve", "resolves", "resolved"]

    public static func linkedIssueNumbers(_ text: String) -> [Int] {
        var out: [Int] = []
        let words = text.replacingOccurrences(of: "\n", with: " ").split(separator: " ", omittingEmptySubsequences: true)
        for (i, word) in words.enumerated() where i + 1 < words.count {
            let verb = word.lowercased().trimmingCharacters(in: CharacterSet(charactersIn: ":"))
            guard closingVerbs.contains(verb) else { continue }
            let target = words[i + 1].trimmingCharacters(in: CharacterSet(charactersIn: ".,;:)"))
            guard target.hasPrefix("#"), let number = Int(target.dropFirst()), number > 0, !out.contains(number) else { continue }
            out.append(number)
        }
        return out
    }

    public static func linkComment(pull: Int) -> String { "Linked to pull request #\(pull)." }

    public static func closingLine(issue: Int) -> String { "Closes #\(issue)" }

    public static func bodyLinking(_ body: String, issue: Int) -> String {
        if linkedIssueNumbers(body).contains(issue) { return body }
        let trimmed = body.dartTrimmed
        return trimmed.isEmpty ? closingLine(issue: issue) : "\(trimmed)\n\n\(closingLine(issue: issue))"
    }
}

public enum IssueThreadText {
    public static func copyText(repo: String, issue: Issue, includeThread: Bool) -> String {
        var parts = ["\(repo)#\(issue.number): \(issue.title)", ""]
        if !issue.body.dartTrimmed.isEmpty { parts.append(issue.body.dartTrimmed) }
        if includeThread {
            for event in issue.events where event.type == "comment" && !event.body.dartTrimmed.isEmpty {
                parts.append("")
                parts.append("— \(event.displayAuthor):")
                parts.append(event.body.dartTrimmed)
            }
        }
        return parts.joined(separator: "\n")
    }

    public static func promptText(repo: String, issue: Issue) -> String {
        "Work on \(repo)#\(issue.number): \(issue.title)\n\n\(issue.body.dartTrimmed)"
    }
}
