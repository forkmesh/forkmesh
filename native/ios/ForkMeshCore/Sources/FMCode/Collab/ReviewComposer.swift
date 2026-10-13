import FMBase
import FMDiff
import Foundation

public struct DiffLineSelection: Equatable, Hashable, Sendable, Codable {
    public var path: String
    public var side: DiffSide
    public var startLine: Int
    public var endLine: Int

    public init(path: String, side: DiffSide, startLine: Int, endLine: Int? = nil) {
        self.path = path
        self.side = side
        self.startLine = startLine
        self.endLine = endLine ?? startLine
    }

    public init?(path: String, line: DiffLine) {
        switch line.kind {
        case .removed:
            guard line.oldNumber > 0 else { return nil }
            self.init(path: path, side: .old, startLine: line.oldNumber)
        case .added, .context:
            guard line.newNumber > 0 else { return nil }
            self.init(path: path, side: .new, startLine: line.newNumber)
        }
    }

    public var lineStart: Int { min(startLine, endLine) }
    public var lineEnd: Int { max(startLine, endLine) }
    public var isRange: Bool { lineStart != lineEnd }

    public func extended(to line: DiffLine) -> DiffLineSelection {
        guard let other = DiffLineSelection(path: path, line: line), other.side == side else { return self }
        var copy = self
        copy.endLine = other.startLine
        if copy.lineEnd - copy.lineStart + 1 > ReviewComposer.maxSelectionLines {
            copy.endLine = copy.startLine + (other.startLine >= copy.startLine ? 1 : -1) * (ReviewComposer.maxSelectionLines - 1)
        }
        return copy
    }

    public static func tapped(_ current: DiffLineSelection?, path: String, line: DiffLine) -> DiffLineSelection? {
        guard let picked = DiffLineSelection(path: path, line: line) else { return current }
        if let current, current.path == path, current.side == picked.side, !current.isRange,
           current.startLine != picked.startLine {
            return current.extended(to: line)
        }
        return current == picked ? nil : picked
    }

    public func contains(_ line: DiffLine) -> Bool {
        guard let other = DiffLineSelection(path: path, line: line), other.side == side else { return false }
        return other.startLine >= lineStart && other.startLine <= lineEnd
    }

    public var label: String {
        let range = isRange ? "lines \(lineStart)–\(lineEnd)" : "line \(lineStart)"
        return "\(path) · \(side.rawValue) \(range)"
    }
}

public struct ReviewEventFields: Equatable, Sendable {
    public var type: String
    public var fields: JSONObject

    public init(type: String, fields: JSONObject) {
        self.type = type
        self.fields = fields
    }

    public var body: String { fields.str("body") }
}

public struct ReviewDraft: Equatable, Sendable, Identifiable, Codable {
    public var id: String
    public var selection: DiffLineSelection
    public var body: String
    public var suggestion: String

    public init(id: String, selection: DiffLineSelection, body: String, suggestion: String = "") {
        self.id = id
        self.selection = selection
        self.body = body
        self.suggestion = suggestion
    }
}

public enum ReviewVerdict: String, CaseIterable, Sendable, Codable {
    case comment
    case approve
    case requestChanges = "request-changes"

    public var title: String {
        switch self {
        case .comment: return "Comment"
        case .approve: return "Approve"
        case .requestChanges: return "Request changes"
        }
    }
}

public enum ReviewComposer {
    public static let maxSelectionLines = 200
    public static let maxDrafts = 50
    public static let maxBodyBytes = 60_000
    public static let emptyBody = "Write a comment first."
    public static let tooLong = "That comment is longer than the relay accepts."
    public static let requestChangesNeedsNote = "A note is required to request changes."
    public static let tooManyDrafts = "A review holds at most 50 comments. Submit this one first."

    public static func cleanBody(_ text: String) -> String {
        var body = text
        while let last = body.unicodeScalars.last, last == "\n" || last == "\r" { body.unicodeScalars.removeLast() }
        return body
    }

    public static func validate(_ body: String) -> String? {
        let clean = cleanBody(body)
        if clean.dartTrimmed.isEmpty { return emptyBody }
        if clean.utf8.count > maxBodyBytes { return tooLong }
        return nil
    }

    public static func eventId(type: String, ts: Int64, random: String) -> String {
        "\(type)-ios-\(ts)-\(random)"
    }

    public static func inlineComment(
        _ selection: DiffLineSelection, body: String, patch: String, patchset: String, suggestion: String = "",
        newId: () -> String
    ) -> ReviewEventFields {
        let start = selection.lineStart
        let end = selection.lineEnd
        let clean = cleanBody(body)
        let id = newId()
        var fields: JSONObject
        let type: String
        if start == end && suggestion.isEmpty {
            type = "line-comment"
            fields = [
                "id": .string(id), "path": .string(selection.path), "side": .string(selection.side.rawValue),
                "line": .int(Int64(start)), "body": .string(clean),
            ]
        } else {
            type = "thread-comment"
            fields = [
                "id": .string(id), "threadId": .string(id), "path": .string(selection.path), "side": .string(selection.side.rawValue),
                "lineStart": .int(Int64(start)), "lineEnd": .int(Int64(end)), "body": .string(clean),
            ]
            if !suggestion.isEmpty { fields["suggestionPatch"] = .string(suggestion) }
        }
        let anchorLines = ReviewAnchoring.anchorLines(
            patch: patch, path: selection.path, side: selection.side, lineStart: start, lineEnd: end)
        if !patchset.isEmpty { fields["patchset"] = .string(patchset) }
        if !anchorLines.isEmpty { fields["anchorLines"] = .string(anchorLines) }
        return ReviewEventFields(type: type, fields: fields)
    }

    public static func branchComment(
        _ selection: DiffLineSelection, body: String, patch: String, newId: () -> String
    ) -> ReviewEventFields {
        let id = newId()
        var fields: JSONObject = [
            "id": .string(id), "threadId": .string(id), "path": .string(selection.path),
            "side": .string(selection.side.rawValue), "lineStart": .int(Int64(selection.lineStart)),
            "lineEnd": .int(Int64(selection.lineEnd)), "body": .string(cleanBody(body)),
        ]
        let anchorLines = ReviewAnchoring.anchorLines(
            patch: patch, path: selection.path, side: selection.side, lineStart: selection.lineStart,
            lineEnd: selection.lineEnd)
        if !anchorLines.isEmpty { fields["anchorLines"] = .string(anchorLines) }
        return ReviewEventFields(type: "thread-comment", fields: fields)
    }

    public static func reply(to thread: ReviewThread, body: String) -> ReviewEventFields {
        ReviewEventFields(type: "thread-reply", fields: [
            "threadId": .string(thread.id), "parentId": .string(thread.lastEventId), "body": .string(cleanBody(body)),
        ])
    }

    public static func setResolved(
        _ thread: ReviewThread, resolved: Bool, note: String = "", reopened: String = "open"
    ) -> ReviewEventFields {
        ReviewEventFields(type: "thread-state", fields: [
            "threadId": .string(thread.id), "state": .string(resolved ? "resolved" : reopened),
            "body": .string(cleanBody(note)),
        ])
    }

    public static func review(_ verdict: ReviewVerdict, body: String) -> ReviewEventFields {
        ReviewEventFields(type: "review", fields: ["state": .string(verdict.rawValue), "body": .string(cleanBody(body))])
    }

    public static func comment(_ body: String) -> ReviewEventFields {
        ReviewEventFields(type: "comment", fields: ["body": .string(cleanBody(body))])
    }

    public static func validateReview(_ verdict: ReviewVerdict, body: String, drafts: [ReviewDraft]) -> String? {
        if verdict == .requestChanges && cleanBody(body).dartTrimmed.isEmpty && drafts.isEmpty {
            return requestChangesNeedsNote
        }
        if verdict == .comment && cleanBody(body).dartTrimmed.isEmpty && drafts.isEmpty { return emptyBody }
        if cleanBody(body).utf8.count > maxBodyBytes { return tooLong }
        for draft in drafts {
            if let problem = validate(draft.body) { return problem }
        }
        return nil
    }

    public static func submission(
        verdict: ReviewVerdict, body: String, drafts: [ReviewDraft], patch: String, patchset: String,
        newId: () -> String
    ) -> [ReviewEventFields] {
        var out = drafts.map {
            inlineComment($0.selection, body: $0.body, patch: patch, patchset: patchset, suggestion: $0.suggestion, newId: newId)
        }
        let clean = cleanBody(body)
        if verdict != .comment || !clean.dartTrimmed.isEmpty {
            out.append(review(verdict, body: clean))
        }
        return out
    }
}

public struct ReviewDraftBook: Equatable, Sendable {
    public private(set) var drafts: [ReviewDraft] = []

    public init(drafts: [ReviewDraft] = []) { self.drafts = drafts }

    @discardableResult
    public mutating func add(_ selection: DiffLineSelection, body: String, id: String, suggestion: String = "") -> String? {
        if let problem = ReviewComposer.validate(body) { return problem }
        if drafts.count >= ReviewComposer.maxDrafts { return ReviewComposer.tooManyDrafts }
        drafts.append(ReviewDraft(id: id, selection: selection, body: ReviewComposer.cleanBody(body), suggestion: suggestion))
        return nil
    }

    public mutating func update(_ id: String, body: String) {
        guard let index = drafts.firstIndex(where: { $0.id == id }) else { return }
        drafts[index].body = ReviewComposer.cleanBody(body)
    }

    public mutating func remove(_ id: String) { drafts.removeAll { $0.id == id } }

    public mutating func clear() { drafts.removeAll() }

    public func drafts(inFile path: String) -> [ReviewDraft] { drafts.filter { $0.selection.path == path } }

    public var isEmpty: Bool { drafts.isEmpty }
    public var count: Int { drafts.count }

    public var summary: String {
        drafts.isEmpty ? "No pending comments" : "\(drafts.count) pending comment\(drafts.count == 1 ? "" : "s")"
    }
}
