import FMApi
import FMBase
import Foundation
import Observation

public struct NoteComment: Equatable, Identifiable, Sendable {
    public let id: String
    public let parentId: String
    public let author: String
    public let body: String
    public let quote: String
    public let prefix: String
    public let suffix: String
    public let start: Int
    public let end: Int
    public let mentions: [String]
    public let createdAt: Int64
    public let updatedAt: Int64
    public let editedAt: Int64
    public let resolvedAt: Int64
    public let resolvedBy: String

    public init(
        id: String, parentId: String = "", author: String = "", body: String = "", quote: String = "",
        prefix: String = "", suffix: String = "", start: Int = 0, end: Int = 0, mentions: [String] = [],
        createdAt: Int64 = 0, updatedAt: Int64 = 0, editedAt: Int64 = 0, resolvedAt: Int64 = 0, resolvedBy: String = ""
    ) {
        self.id = id
        self.parentId = parentId
        self.author = author
        self.body = body
        self.quote = quote
        self.prefix = prefix
        self.suffix = suffix
        self.start = start
        self.end = end
        self.mentions = mentions
        self.createdAt = createdAt
        self.updatedAt = updatedAt
        self.editedAt = editedAt
        self.resolvedAt = resolvedAt
        self.resolvedBy = resolvedBy
    }

    public init(json: JSONObject) {
        self.init(
            id: ToolsJSON.text(json["id"], ""), parentId: ToolsJSON.text(json["parentId"], ""),
            author: ToolsJSON.text(json["author"], ""), body: ToolsJSON.text(json["body"], ""),
            quote: ToolsJSON.text(json["quote"], ""), prefix: ToolsJSON.text(json["prefix"], ""),
            suffix: ToolsJSON.text(json["suffix"], ""), start: ToolsJSON.int(json["start"]), end: ToolsJSON.int(json["end"]),
            mentions: ToolsJSON.strings(json["mentions"]), createdAt: ToolsJSON.epoch(json["createdAt"]),
            updatedAt: ToolsJSON.epoch(json["updatedAt"]), editedAt: ToolsJSON.epoch(json["editedAt"]),
            resolvedAt: ToolsJSON.epoch(json["resolvedAt"]), resolvedBy: ToolsJSON.text(json["resolvedBy"], ""))
    }

    public var isThread: Bool { parentId.isEmpty }
    public var resolved: Bool { resolvedAt > 0 }
    public var edited: Bool { editedAt > 0 }
}

public struct NoteQuote: Equatable, Sendable {
    public let quote: String
    public let prefix: String
    public let suffix: String
    public let start: Int
    public let end: Int

    public init(quote: String, prefix: String, suffix: String, start: Int, end: Int) {
        self.quote = quote
        self.prefix = prefix
        self.suffix = suffix
        self.start = start
        self.end = end
    }

    public var body: JSONObject {
        [
            "quote": .string(quote), "prefix": .string(prefix), "suffix": .string(suffix),
            "start": .int(Int64(start)), "end": .int(Int64(end)),
        ]
    }
}

public enum NoteAnchor {
    public static let contextLength = 32
    public static let maxQuote = 1000

    static func isLow(_ unit: UInt16) -> Bool { unit >= 0xDC00 && unit <= 0xDFFF }
    static func string(_ units: ArraySlice<UInt16>) -> String { String(decoding: Array(units), as: UTF16.self) }

    public static func capture(_ markdown: String, selection: Range<Int>) -> NoteQuote? {
        let units = Array(markdown.utf16)
        var start = min(max(0, selection.lowerBound), units.count)
        var end = min(max(start, selection.upperBound), units.count)
        if start < units.count && start > 0 && isLow(units[start]) { start += 1 }
        if end - start > maxQuote { end = start + maxQuote }
        if end < units.count && end > start && isLow(units[end]) { end -= 1 }
        guard end > start else { return nil }
        let quote = string(units[start..<end])
        guard !quote.dartTrimmed.isEmpty else { return nil }
        var before = max(0, start - contextLength)
        if before > 0 && isLow(units[before]) { before += 1 }
        var after = min(units.count, end + contextLength)
        if after < units.count && isLow(units[after]) { after -= 1 }
        return NoteQuote(
            quote: quote, prefix: string(units[before..<start]), suffix: string(units[end..<after]), start: start, end: end)
    }

    public static func locate(quote: String, prefix: String, suffix: String, start: Int, in markdown: String) -> Range<Int>? {
        locate(Array(quote.utf16), Array(prefix.utf16), Array(suffix.utf16), start: start, in: Array(markdown.utf16))
    }

    public static func locate(_ comment: NoteComment, in markdown: String) -> Range<Int>? {
        locate(quote: comment.quote, prefix: comment.prefix, suffix: comment.suffix, start: comment.start, in: markdown)
    }

    static func locate(_ quote: [UInt16], _ prefix: [UInt16], _ suffix: [UInt16], start: Int, in text: [UInt16]) -> Range<Int>? {
        guard !quote.isEmpty, quote.count <= text.count else { return nil }
        var best: (at: Int, score: Int, distance: Int)?
        let first = quote[0]
        var at = 0
        let last = text.count - quote.count
        while at <= last {
            if text[at] == first && matches(text, quote, at: at) {
                var score = 0
                if at >= prefix.count && matches(text, prefix, at: at - prefix.count) { score += 2 }
                let tail = at + quote.count
                if tail + suffix.count <= text.count && matches(text, suffix, at: tail) { score += 2 }
                let distance = abs(at - start)
                if let current = best {
                    if score > current.score || (score == current.score && distance < current.distance) {
                        best = (at, score, distance)
                    }
                } else {
                    best = (at, score, distance)
                }
            }
            at += 1
        }
        return best.map { $0.at..<($0.at + quote.count) }
    }

    static func matches(_ text: [UInt16], _ part: [UInt16], at: Int) -> Bool {
        if part.isEmpty { return true }
        if at < 0 || at + part.count > text.count { return false }
        var index = 0
        while index < part.count {
            if text[at + index] != part[index] { return false }
            index += 1
        }
        return true
    }
}

public enum NoteThreadPlace: Equatable, Sendable {
    case whole
    case inline(Range<Int>)
    case detached
}

public struct NoteThread: Equatable, Identifiable, Sendable {
    public let comment: NoteComment
    public let replies: [NoteComment]
    public let place: NoteThreadPlace

    public init(comment: NoteComment, replies: [NoteComment], place: NoteThreadPlace) {
        self.comment = comment
        self.replies = replies
        self.place = place
    }

    public var id: String { comment.id }
    public var resolved: Bool { comment.resolved }
    public var detached: Bool { place == .detached }

    public var range: Range<Int>? {
        if case let .inline(range) = place { return range }
        return nil
    }

    var rank: Int {
        switch place {
        case .whole: return 0
        case .inline: return 1
        case .detached: return 2
        }
    }
}

public struct NoteThreadList: Equatable, Sendable {
    public let open: [NoteThread]
    public let resolved: [NoteThread]

    public init(open: [NoteThread], resolved: [NoteThread]) {
        self.open = open
        self.resolved = resolved
    }

    public static let empty = NoteThreadList(open: [], resolved: [])

    public var all: [NoteThread] { open + resolved }

    public static func build(_ comments: [NoteComment], markdown: String) -> NoteThreadList {
        let text = Array(markdown.utf16)
        var replies: [String: [NoteComment]] = [:]
        for comment in comments where !comment.isThread {
            replies[comment.parentId, default: []].append(comment)
        }
        let threads = comments.filter(\.isThread).map { (root: NoteComment) -> NoteThread in
            let place: NoteThreadPlace
            if root.quote.isEmpty {
                place = .whole
            } else if let range = NoteAnchor.locate(
                Array(root.quote.utf16), Array(root.prefix.utf16), Array(root.suffix.utf16), start: root.start, in: text) {
                place = .inline(range)
            } else {
                place = .detached
            }
            let answers = (replies[root.id] ?? []).enumerated().sorted { (a, b) in
                a.element.createdAt != b.element.createdAt ? a.element.createdAt < b.element.createdAt : a.offset < b.offset
            }.map(\.element)
            return NoteThread(comment: root, replies: answers, place: place)
        }
        let ordered = threads.sorted(by: inDocumentOrder)
        return NoteThreadList(open: ordered.filter { !$0.resolved }, resolved: ordered.filter(\.resolved))
    }

    static func inDocumentOrder(_ a: NoteThread, _ b: NoteThread) -> Bool {
        if a.rank != b.rank { return a.rank < b.rank }
        if let left = a.range, let right = b.range, left.lowerBound != right.lowerBound {
            return left.lowerBound < right.lowerBound
        }
        if a.comment.createdAt != b.comment.createdAt { return a.comment.createdAt < b.comment.createdAt }
        return a.comment.id < b.comment.id
    }
}

public struct NoteMentionSpan: Equatable, Sendable {
    public let text: String
    public let mention: Bool

    public init(_ text: String, mention: Bool = false) {
        self.text = text
        self.mention = mention
    }
}

public enum NoteMentions {
    public static let maxSuggestions = 5
    static let maxName = 63

    static func isNameUnit(_ c: Character) -> Bool {
        c.isASCII && (c.isLetter || c.isNumber || c == "-")
    }

    static func isWordUnit(_ c: Character) -> Bool {
        c.isLetter || c.isNumber || c == "_" || c == "@"
    }

    static func name(after at: String.Index, in text: String) -> Range<String.Index>? {
        let begin = text.index(after: at)
        guard begin < text.endIndex, text[begin].isASCII, text[begin].isLetter else { return nil }
        var end = begin
        var count = 0
        while end < text.endIndex, isNameUnit(text[end]), count < maxName {
            end = text.index(after: end)
            count += 1
        }
        while end > begin, text[text.index(before: end)] == "-" {
            end = text.index(before: end)
        }
        return begin..<end
    }

    public static func names(in body: String) -> [String] {
        var found: [String] = []
        for range in ranges(in: body) {
            let name = String(body[range].dropFirst()).lowercased()
            if !found.contains(name) { found.append(name) }
        }
        return found
    }

    static func ranges(in text: String) -> [Range<String.Index>] {
        var out: [Range<String.Index>] = []
        var index = text.startIndex
        var previous: Character?
        while index < text.endIndex {
            let c = text[index]
            if c == "@", previous.map({ !isWordUnit($0) }) ?? true, let name = name(after: index, in: text) {
                out.append(index..<name.upperBound)
                previous = text[text.index(before: name.upperBound)]
                index = name.upperBound
                continue
            }
            previous = c
            index = text.index(after: index)
        }
        return out
    }

    public static func segments(_ body: String, mentions: [String]) -> [NoteMentionSpan] {
        let kept = Set(mentions.map { $0.lowercased() })
        var out: [NoteMentionSpan] = []
        var cursor = body.startIndex
        for range in ranges(in: body) {
            let name = String(body[body.index(after: range.lowerBound)..<range.upperBound]).lowercased()
            guard kept.contains(name) else { continue }
            if cursor < range.lowerBound { out.append(NoteMentionSpan(String(body[cursor..<range.lowerBound]))) }
            out.append(NoteMentionSpan(String(body[range]), mention: true))
            cursor = range.upperBound
        }
        if cursor < body.endIndex { out.append(NoteMentionSpan(String(body[cursor...]))) }
        return out
    }

    public static func query(_ text: String) -> String? {
        guard let at = text.lastIndex(of: "@") else { return nil }
        let partial = text[text.index(after: at)...]
        guard partial.allSatisfy(isNameUnit), partial.count <= maxName else { return nil }
        if at > text.startIndex, isWordUnit(text[text.index(before: at)]) { return nil }
        return partial.lowercased()
    }

    public static func suggestions(_ people: [String], for text: String, me: String = "") -> [String] {
        guard let partial = query(text) else { return [] }
        let self_ = me.lowercased()
        let names = people.map { $0.lowercased() }.filter { !$0.isEmpty && $0 != self_ && $0 != partial }
        let starts = names.filter { $0.hasPrefix(partial) }
        let contains = partial.isEmpty ? [] : names.filter { !$0.hasPrefix(partial) && $0.contains(partial) }
        return Array((starts + contains).prefix(maxSuggestions))
    }

    public static func complete(_ text: String, with name: String) -> String {
        guard query(text) != nil, let at = text.lastIndex(of: "@") else { return text }
        return String(text[..<at]) + "@\(name) "
    }
}

@MainActor
@Observable
public final class NoteCommentsModel {
    public static let title = "Comments"
    public static let reloadDelayMillis = 300
    public static let maxBody = 4000
    public static let emptyMessage = "No comments yet. Select text in the note and choose Comment, or comment on the whole note."
    public static let detachedNote = "This text is no longer in the note."
    public static let wholeNote = "On the whole note"
    public static let unavailable = "Comments are unavailable right now."
    public static let deleteConfirmation = ToolsConfirmation(
        title: "Delete comment?", message: "This removes the comment for everyone.",
        confirm: "Delete", cancel: "Cancel", destructive: true)

    @ObservationIgnored private let call: ToolsApiCall
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored private let sleep: ToolsSleep
    @ObservationIgnored var reloadTask: Task<Void, Never>?
    @ObservationIgnored private var reloadGeneration = 0
    public let noteId: String
    public let me: String
    public var isOwner: Bool

    public private(set) var comments: [NoteComment] = []
    public private(set) var people: [String] = []
    public private(set) var canResolveAll = false
    public private(set) var loaded = false
    public private(set) var error = ""
    public private(set) var busy = false
    public var composing = false
    public var draft = ""
    public private(set) var draftQuote: NoteQuote?
    public var replies: [String: String] = [:]
    public private(set) var editingId: String?
    public var editText = ""
    public var focusedId: String?
    public var showResolved = false

    public init(
        noteId: String, me: String = "", isOwner: Bool = false, call: @escaping ToolsApiCall,
        clock: Clock = SystemClock(), sleep: @escaping ToolsSleep = toolsTaskSleep
    ) {
        self.noteId = noteId
        self.me = me.lowercased()
        self.isOwner = isOwner
        self.call = call
        self.clock = clock
        self.sleep = sleep
    }

    var path: String { "/\(noteId)/comments" }

    public var openCount: Int { comments.filter { $0.isThread && !$0.resolved }.count }

    public func threads(markdown: String) -> NoteThreadList {
        NoteThreadList.build(comments, markdown: markdown)
    }

    public var focusedThreadId: String? {
        guard let focusedId, let comment = comments.first(where: { $0.id == focusedId }) else { return nil }
        return comment.isThread ? comment.id : comment.parentId
    }

    public func range(of threadId: String, in markdown: String) -> Range<Int>? {
        guard let root = comments.first(where: { $0.id == threadId && $0.isThread }), !root.quote.isEmpty else { return nil }
        return NoteAnchor.locate(root, in: markdown)
    }

    public func focus(_ commentId: String?) {
        focusedId = commentId
        revealFocused()
    }

    private func revealFocused() {
        guard let id = focusedThreadId, comments.first(where: { $0.id == id })?.resolved == true else { return }
        showResolved = true
    }

    public func load() async {
        do {
            apply(try await call("GET", path, nil))
            error = ""
        } catch {
            if !loaded { self.error = notesErrorText(error, Self.unavailable) }
        }
    }

    public func reloadSoon() {
        reloadGeneration += 1
        let generation = reloadGeneration
        let sleep = self.sleep
        reloadTask = Task { [weak self] in
            do { try await sleep(Self.reloadDelayMillis) } catch { return }
            guard let self, self.reloadGeneration == generation else { return }
            await self.load()
        }
    }

    func apply(_ data: JSONObject) {
        comments = ToolsJSON.maps(data["comments"]).map(NoteComment.init(json:)).filter { !$0.id.isEmpty }
        people = ToolsJSON.strings(data["people"])
        canResolveAll = data["canResolve"]?.boolValue ?? false
        loaded = true
        let alive = Set(comments.map(\.id))
        replies = replies.filter { alive.contains($0.key) }
        if let editingId, !alive.contains(editingId) { self.editingId = nil }
        revealFocused()
    }

    public func isMine(_ comment: NoteComment) -> Bool { !me.isEmpty && comment.author.lowercased() == me }
    public func canEdit(_ comment: NoteComment) -> Bool { isMine(comment) }
    public func canDelete(_ comment: NoteComment) -> Bool { isMine(comment) || isOwner }
    public func canResolve(_ thread: NoteThread) -> Bool { canResolveAll || isMine(thread.comment) }

    public func replyCount(_ comment: NoteComment) -> Int {
        comment.isThread ? comments.filter { $0.parentId == comment.id }.count : 0
    }

    public func deleteConfirmation(_ comment: NoteComment) -> ToolsConfirmation {
        let replies = replyCount(comment)
        guard replies > 0 else { return Self.deleteConfirmation }
        let message = replies == 1
            ? "This removes the comment and its reply for everyone."
            : "This removes the comment and its \(replies) replies for everyone."
        return ToolsConfirmation(title: "Delete thread?", message: message, confirm: "Delete", cancel: "Cancel", destructive: true)
    }

    public func authorName(_ comment: NoteComment) -> String { comment.author.isEmpty ? "Someone" : comment.author }

    public func when(_ comment: NoteComment) -> String {
        let time = Format.relativeTime(comment.createdAt, nowMillis: clock.nowMillis)
        return comment.edited ? "\(time) · edited" : time
    }

    public func resolvedLine(_ thread: NoteThread) -> String {
        let time = Format.relativeTime(thread.comment.resolvedAt, nowMillis: clock.nowMillis)
        let who = thread.comment.resolvedBy
        return who.isEmpty ? "Resolved \(time)" : "Resolved by \(who) · \(time)"
    }

    public func resolvedTitle(_ list: NoteThreadList) -> String { "Resolved (\(list.resolved.count))" }

    public var buttonLabel: String {
        let count = openCount
        return count == 0 ? "Comments" : "Comments, \(count) open"
    }

    public func startComment(_ quote: NoteQuote?) {
        draftQuote = quote
        draft = ""
        composing = true
    }

    public func cancelComment() {
        composing = false
        draft = ""
        draftQuote = nil
    }

    static func cleaned(_ text: String) -> (body: String?, failure: String?) {
        let body = text.dartTrimmed
        if body.isEmpty { return (nil, nil) }
        if body.count > maxBody { return (nil, "A comment can be at most \(maxBody) characters.") }
        return (body, nil)
    }

    private func send(_ method: String, _ suffix: String, _ body: JSONObject?, fallback: String) async -> (data: JSONObject?, outcome: ToolsOutcome) {
        busy = true
        defer { busy = false }
        do {
            let data = try await call(method, path + suffix, body)
            apply(data)
            return (data, ToolsOutcome())
        } catch {
            return (nil, .failed(notesErrorText(error, fallback)))
        }
    }

    public func post() async -> ToolsOutcome {
        let checked = Self.cleaned(draft)
        if let failure = checked.failure { return .failed(failure) }
        guard let text = checked.body else { return ToolsOutcome() }
        var body: JSONObject = ["body": .string(text)]
        if let quote = draftQuote {
            for (key, value) in quote.body { body[key] = value }
        }
        let result = await send("POST", "", body, fallback: "The comment was not added.")
        guard let data = result.data else { return result.outcome }
        if let id = ToolsJSON.object(data["comment"])["id"]?.stringValue { focusedId = id }
        cancelComment()
        return ToolsOutcome()
    }

    public func reply(to threadId: String) async -> ToolsOutcome {
        let checked = Self.cleaned(replies[threadId] ?? "")
        if let failure = checked.failure { return .failed(failure) }
        guard let text = checked.body else { return ToolsOutcome() }
        let result = await send("POST", "", ["body": .string(text), "parentId": .string(threadId)], fallback: "The reply was not added.")
        if result.data != nil { replies[threadId] = nil }
        return result.outcome
    }

    public func startEdit(_ comment: NoteComment) {
        guard canEdit(comment) else { return }
        editingId = comment.id
        editText = comment.body
    }

    public func cancelEdit() {
        editingId = nil
        editText = ""
    }

    public func saveEdit() async -> ToolsOutcome {
        guard let id = editingId else { return ToolsOutcome() }
        let checked = Self.cleaned(editText)
        if let failure = checked.failure { return .failed(failure) }
        guard let text = checked.body else { return ToolsOutcome() }
        let result = await send("PATCH", "/\(id)", ["body": .string(text)], fallback: "The comment was not changed.")
        if result.data != nil { cancelEdit() }
        return result.outcome
    }

    public func setResolved(_ thread: NoteThread, _ resolved: Bool) async -> ToolsOutcome {
        let result = await send(
            "PATCH", "/\(thread.id)", ["resolved": .bool(resolved)],
            fallback: resolved ? "The thread was not resolved." : "The thread was not reopened.")
        return result.data == nil ? result.outcome : .done(resolved ? "Thread resolved" : "Thread reopened")
    }

    public func delete(_ comment: NoteComment) async -> ToolsOutcome {
        let result = await send("DELETE", "/\(comment.id)", nil, fallback: "The comment was not deleted.")
        if result.data != nil, focusedId == comment.id { focusedId = nil }
        return result.data == nil ? result.outcome : .done(comment.isThread ? "Thread deleted" : "Comment deleted")
    }

    public func suggestions(for text: String) -> [String] {
        NoteMentions.suggestions(people, for: text, me: me)
    }
}
