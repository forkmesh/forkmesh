// The state and decisions of the Notes screen: the notes list,
// one note, its history sheet and its share sheet, over `/api/notes`
// (ApiService.notesApi). The Worker encrypts each note at rest; the phone reads and writes plain
// JSON over the signed-in session.

import FMApi
import FMBase
import FMNet
import Foundation
import Observation

/// One call under an account-package API: (method, suffix, body) -> the answer object. Errors are
/// FMApi's `OrgApiException` for a refusal, anything else for no answer.
public typealias ToolsApiCall = @MainActor (_ method: String, _ suffix: String, _ body: JSONObject?) async throws -> JSONObject

/// JSON helpers shared by the notes and tasks screens.
enum ToolsJSON {
    /// `_maps`: the objects of a list, or none.
    static func maps(_ value: JSONValue?) -> [JSONObject] { value?.arrayValue?.compactMap(\.objectValue) ?? [] }

    /// `_strings`: `'$item'` of each entry, empties dropped.
    static func strings(_ value: JSONValue?) -> [String] {
        (value?.arrayValue ?? []).map { DartCore.toString($0) }.filter { !$0.isEmpty }
    }

    /// `_int` of notes/tasks: `value is num ? value.toInt() : int.tryParse('$value') ?? 0`.
    static func int(_ value: JSONValue?) -> Int {
        if let v = value, case .int(let i) = v { return Int(i) }
        if let v = value, case .double(let d) = v { return d.isFinite ? Int(d.rounded(.towardZero)) : 0 }
        return DartCore.intTryParse(DartCore.toString(value)) ?? 0
    }

    /// `'${value ?? fallback}'`.
    static func text(_ value: JSONValue?, _ fallback: String) -> String {
        guard let value, !value.isNull else { return fallback }
        return DartCore.toString(value)
    }

    /// `'$value'` (null prints "null").
    static func show(_ value: JSONValue?) -> String { DartCore.toString(value) }

    /// `Map<String, dynamic>.from(data[key] as Map? ?? const {})`.
    static func object(_ value: JSONValue?) -> JSONObject { value?.objectValue ?? [:] }

    static func epoch(_ value: JSONValue?) -> Int64 { Int64(int(value)) }
}

/// "Private", "Public", or "Shared with 2": how far a note reaches.
public func noteReach(_ note: JSONObject) -> String {
    if note["visibility"]?.stringValue == "public" { return "Public" }
    let shares = ToolsJSON.maps(note["shares"]).count
    return shares > 0 ? "Shared with \(shares)" : "Private"
}

/// The notes screen's error text: the Worker's message for a refusal, else `fallback`.
func notesErrorText(_ error: Error, _ fallback: String) -> String {
    guard let refusal = error as? OrgApiException else { return fallback }
    return noteErrorMessages[refusal.code] ?? refusal.message
}

let noteErrorMessages: [String: String] = [
    "principal_not_found": "No user or organization has that name.",
    "invalid_principal": "Enter a valid username or organization name.",
    "repository_not_found": "That repository doesn't exist, or you don't have access to it.",
    "invalid_link": "Paste an issue or pull request URL, or type owner/repo#123.",
    "owner_required": "Only the note's owner can do that.",
    "editor_required": "You need edit access to do that.",
    "note_not_found": "This note no longer exists, or you no longer have access to it.",
    "invalid_comment": "Write a comment of at most 4000 characters.",
    "comment_not_found": "That comment no longer exists.",
    "too_many_comments": "This note already has 1000 comments, the most it can hold.",
    "comment_author_required": "Only the comment's author can do that.",
    "resolve_not_allowed": "Only the note's owner, an editor or the thread's author can resolve it.",
]

public typealias ToolsSleep = @Sendable (_ millis: Int) async throws -> Void

public let toolsTaskSleep: ToolsSleep = { millis in try await Task.sleep(nanoseconds: UInt64(max(0, millis)) * 1_000_000) }

public typealias ToolsWebLink = @MainActor (_ path: String) -> URL?

public enum NotesFilter: String, CaseIterable, Identifiable, Sendable {
    case all, mine, shared
    case published = "public"

    public var id: String { rawValue }

    public var label: String {
        switch self {
        case .all: return "All"
        case .mine: return "Mine"
        case .shared: return "Shared"
        case .published: return "Public"
        }
    }

    public func matches(_ note: JSONObject) -> Bool {
        let owner = ToolsJSON.text(note["role"], "owner") == "owner"
        switch self {
        case .all: return true
        case .mine: return owner
        case .shared: return !owner
        case .published: return note["visibility"]?.stringValue == "public"
        }
    }
}

/// One row of the notes list.
public struct NoteRow: Equatable, Identifiable, Sendable {
    public let id: String
    public let title: String
    public let subtitle: String
    /// "public" or "sticky_note_2_outlined".
    public let icon: String
    public var unread: Bool = false
    public var reach: NoteReachBadge = .privateNote
    public var people: [NoteAvatar] = []
    public var morePeople: Int = 0
    public var openComments: Int = 0
    public var isOwner: Bool = true
}

@MainActor
@Observable
public final class NotesListModel {
    public static let title = "Notes"
    public static let emptyTitle = "No notes yet"
    public static let emptyMessage =
        "Notes are private markdown documents that follow your account across web, desktop, and mobile."
    public static let unavailableTitle = "Notes are unavailable"

    public static let searchDelayMillis = 250
    public static let maxSearchLength = 200
    public static let searchingMessage = "Searching notes…"

    @ObservationIgnored public let call: ToolsApiCall
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored private let sleep: ToolsSleep
    @ObservationIgnored var searchTask: Task<Void, Never>?
    @ObservationIgnored private var searchGeneration = 0

    public private(set) var notes: [JSONObject] = []
    public private(set) var serverMatches: [JSONObject]?
    public private(set) var answeredQuery = ""
    public private(set) var searching = false
    public private(set) var loading = true
    public private(set) var error = ""
    public var query = ""
    public var filter: NotesFilter = .all

    @ObservationIgnored private let me: String
    @ObservationIgnored private let calendar: Calendar
    @ObservationIgnored private let seen: NoteViewsSeen
    @ObservationIgnored private let webLink: ToolsWebLink
    public private(set) var seenGeneration = 0

    public init(
        call: @escaping ToolsApiCall, clock: Clock = SystemClock(), sleep: @escaping ToolsSleep = toolsTaskSleep,
        me: String = "", calendar: Calendar = .current, seen: NoteViewsSeen = .shared,
        webLink: @escaping ToolsWebLink = { _ in nil }
    ) {
        self.call = call
        self.clock = clock
        self.sleep = sleep
        self.me = me
        self.calendar = calendar
        self.seen = seen
        self.webLink = webLink
    }

    public convenience init(api: ApiService, me: String = "") {
        self.init(
            call: { method, suffix, body in try await api.notesApi(method, suffix, body) }, me: me,
            webLink: { path in api.webLink(path) })
    }

    func note(_ id: String) -> JSONObject? {
        (serverMatches ?? []).first { ToolsJSON.show($0["id"]) == id } ?? notes.first { ToolsJSON.show($0["id"]) == id }
    }

    public func accessLines(_ id: String) -> [String] {
        note(id).map { NoteAccessHeader.lines($0, me: me, calendar: calendar) } ?? []
    }

    public func accessPath(_ id: String) -> String { "/dashboard/notes?note=\(id)" }

    public func accessLink(_ id: String) -> URL? { webLink(accessPath(id)) }

    public func shareModel(_ id: String) -> NoteShareModel? {
        guard let note = note(id), ToolsJSON.text(note["role"], "owner") == "owner" else { return nil }
        return NoteShareModel(noteId: id, initial: ToolsJSON.maps(note["shares"]), call: call, sleep: sleep, webLink: webLink)
    }

    public func delete(_ id: String) async -> ToolsOutcome {
        guard let note = note(id), ToolsJSON.text(note["role"], "owner") == "owner" else { return ToolsOutcome() }
        do {
            _ = try await call("DELETE", "/\(id)", nil)
            notes.removeAll { ToolsJSON.show($0["id"]) == id }
            serverMatches?.removeAll { ToolsJSON.show($0["id"]) == id }
            return .done("Note deleted")
        } catch {
            return .failed(notesErrorText(error, "The note was not deleted."))
        }
    }

    public func markViewed(_ id: String) {
        guard let note = note(id) else { return }
        seen.record(note)
        seenGeneration += 1
    }

    public func load() async {
        loading = true
        error = ""
        do {
            let data = try await call("GET", "", nil)
            notes = ToolsJSON.maps(data["notes"])
            loading = false
            if !searchText.isEmpty {
                answeredQuery = ""
                queryChanged()
            }
        } catch {
            loading = false
            self.error = notesErrorText(error, "Notes are unavailable right now.")
        }
    }

    public var showsSpinner: Bool { loading && notes.isEmpty }
    public var showsError: Bool { !error.isEmpty && notes.isEmpty }

    public static func matches(_ note: JSONObject, query: String) -> Bool {
        let q = query.dartTrimmed.lowercased()
        if q.isEmpty { return true }
        return ToolsJSON.text(note["title"], "").lowercased().contains(q)
            || ToolsJSON.text(note["markdown"], "").lowercased().contains(q)
    }

    var searchText: String { query.dartTrimmed.lowercased() }

    private var searched: [JSONObject] {
        let text = searchText
        if !text.isEmpty, let serverMatches, answeredQuery == text { return serverMatches }
        return notes.filter { Self.matches($0, query: text) }
    }

    public func queryChanged() {
        searchGeneration += 1
        let text = searchText
        if text.isEmpty || text.count > Self.maxSearchLength {
            serverMatches = nil
            answeredQuery = ""
            searching = false
            return
        }
        if serverMatches != nil && answeredQuery == text { return }
        searching = true
        let generation = searchGeneration
        let sleep = self.sleep
        searchTask = Task { [weak self] in
            do { try await sleep(Self.searchDelayMillis) } catch { return }
            guard let self, self.searchGeneration == generation else { return }
            await self.search(text, generation: generation)
        }
    }

    func search(_ text: String, generation: Int) async {
        let encoded = text.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? text
        let answer = try? await call("GET", "?query=\(encoded)", nil)
        guard generation == searchGeneration else { return }
        searching = false
        if let answer {
            serverMatches = ToolsJSON.maps(answer["notes"])
            answeredQuery = text
        }
    }

    public var visible: [JSONObject] { searched.filter(filter.matches) }

    public func count(_ filter: NotesFilter) -> Int { searched.filter(filter.matches).count }

    public var filterIndex: Int { NotesFilter.allCases.firstIndex(of: filter) ?? 0 }

    public var emptyFilterMessage: String {
        let q = query.dartTrimmed
        if !q.isEmpty { return searching ? Self.searchingMessage : "No notes match \u{201C}\(q)\u{201D}." }
        switch filter {
        case .shared: return "Nobody has shared a note with you yet."
        case .published: return "You haven't published any notes."
        case .mine: return "You haven't written any notes yet."
        case .all: return Self.emptyTitle
        }
    }

    public var rows: [NoteRow] { visible.map(row) }

    public func row(_ note: JSONObject) -> NoteRow {
        _ = seenGeneration
        let edited = NoteEditTime.line(
            ToolsJSON.epoch(note["updatedAt"]), editedBy: ToolsJSON.text(note["lastEditedBy"], ""), me: me,
            nowMillis: clock.nowMillis, calendar: calendar)
        let avatars = NoteAvatar.of(note)
        return NoteRow(
            id: ToolsJSON.show(note["id"]),
            title: ToolsJSON.text(note["title"], "Untitled note"),
            subtitle: edited,
            icon: note["visibility"]?.stringValue == "public" ? "public" : "sticky_note_2_outlined",
            unread: seen.unread(note),
            reach: NoteReachBadge.of(note),
            people: avatars.shown, morePeople: avatars.more,
            openComments: max(0, ToolsJSON.int(note["openComments"])),
            isOwner: ToolsJSON.text(note["role"], "owner") == "owner")
    }
}

/// What a note editor action tells the screen: a notice to show inline, and whether to leave.
public struct ToolsOutcome: Equatable, Sendable {
    public var notice: ToolsNotice?
    public var close: Bool

    public init(notice: ToolsNotice? = nil, close: Bool = false) {
        self.notice = notice
        self.close = close
    }

    public static func done(_ message: String) -> ToolsOutcome { ToolsOutcome(notice: ToolsNotice(message)) }

    public static func failed(_ message: String) -> ToolsOutcome { ToolsOutcome(notice: ToolsNotice(message, isError: true)) }
}

/// A confirmation dialog's words.
public struct ToolsConfirmation: Equatable, Sendable {
    public let title: String
    public let message: String
    public let confirm: String
    public let cancel: String
    public let destructive: Bool
}

public enum NoteMode: String, CaseIterable, Identifiable, Sendable {
    case write, preview

    public var id: String { rawValue }
    public var label: String { self == .write ? "Write" : "Preview" }

    public func label(canEdit: Bool) -> String { self == .write && !canEdit ? "Markdown" : label }
}

public struct NoteSelectionRequest: Equatable, Sendable {
    public let range: Range<Int>
    public let serial: Int

    public init(range: Range<Int>, serial: Int) {
        self.range = range
        self.serial = serial
    }
}

public enum NoteFormat: String, CaseIterable, Identifiable, Sendable {
    case heading, bold, italic, list, task, link, code

    public var id: String { rawValue }

    public var label: String {
        switch self {
        case .heading: return "Heading"
        case .bold: return "Bold"
        case .italic: return "Italic"
        case .list: return "List"
        case .task: return "Task"
        case .link: return "Link"
        case .code: return "Code"
        }
    }

    public var glyph: String {
        switch self {
        case .heading: return "H"
        case .bold: return "B"
        case .italic: return "I"
        case .list: return "\u{2022}"
        case .task: return "\u{2610}"
        case .link: return "Link"
        case .code: return "</>"
        }
    }

    var wrap: (before: String, after: String, placeholder: String)? {
        switch self {
        case .bold: return ("**", "**", "bold text")
        case .italic: return ("_", "_", "italic text")
        case .code: return ("`", "`", "code")
        case .link: return ("[", "](https://)", "link text")
        default: return nil
        }
    }

    var prefix: String {
        switch self {
        case .heading: return "## "
        case .task: return "- [ ] "
        default: return "- "
        }
    }
}

public enum NoteFormatting {
    public static func apply(_ kind: NoteFormat, to text: String, selection: Range<Int>) -> (text: String, selection: Range<Int>) {
        let units = Array(text.utf16)
        let start = min(max(0, selection.lowerBound), units.count)
        let end = min(max(start, selection.upperBound), units.count)
        func string(_ slice: ArraySlice<UInt16>) -> String { String(decoding: Array(slice), as: UTF16.self) }
        if let wrap = kind.wrap {
            let chosen = string(units[start..<end])
            let body = chosen.isEmpty ? wrap.placeholder : chosen
            let next = string(units[..<start]) + wrap.before + body + wrap.after + string(units[end...])
            let cursor = start + wrap.before.utf16.count
            return (next, cursor..<(cursor + body.utf16.count))
        }
        var lineStart = start
        while lineStart > 0 && units[lineStart - 1] != 10 { lineStart -= 1 }
        let prefix = kind.prefix
        let next = string(units[..<lineStart]) + prefix + string(units[lineStart...])
        let shift = prefix.utf16.count
        return (next, (start + shift)..<(end + shift))
    }
}

public struct NoteLinkRef: Equatable, Sendable {
    public let owner: String
    public let repo: String
    public let kind: String
    public let number: Int

    public static let kinds: [(value: String, label: String)] = [("issue", "Issue"), ("pull", "PR"), ("discussion", "Discussion")]

    public init(owner: String, repo: String, kind: String, number: Int) {
        self.owner = owner
        self.repo = repo
        self.kind = kind
        self.number = number
    }

    public init(json: JSONObject) {
        self.init(
            owner: ToolsJSON.text(json["owner"], ""), repo: ToolsJSON.text(json["repo"], ""),
            kind: ToolsJSON.text(json["kind"], "issue"), number: ToolsJSON.int(json["number"]))
    }

    public var body: JSONObject {
        ["owner": .string(owner), "repo": .string(repo), "kind": .string(kind), "number": .int(Int64(number))]
    }

    public var title: String { "\(repo)#\(number)" }

    public var subtitle: String {
        let label = Self.kinds.first { $0.value == kind }?.label ?? ToolsText.cap(kind)
        return "\(owner)/\(repo) · \(label)"
    }

    public var icon: String { kind == "pull" ? "call_merge" : kind == "discussion" ? "forum_outlined" : "adjust" }

    public var path: String {
        let section = kind == "pull" ? "pulls" : "\(kind)s"
        return "/\(owner)/\(repo)?tab=\(section)#\(number)"
    }

    static let pathKinds = ["issues": "issue", "issue": "issue", "pull": "pull", "pulls": "pull", "discussions": "discussion", "discussion": "discussion"]

    static func isName(_ value: Substring) -> Bool {
        !value.isEmpty && value.unicodeScalars.allSatisfy {
            ($0.isASCII && (CharacterSet.alphanumerics.contains($0))) || $0 == "_" || $0 == "." || $0 == "-"
        }
    }

    static func number(_ value: Substring) -> Int? {
        guard !value.isEmpty, value.allSatisfy({ $0.isASCII && $0.isNumber }), let n = Int(value), n >= 1, n <= 999_999_999 else { return nil }
        return n
    }

    public static func parse(_ text: String, fallbackKind: String) -> NoteLinkRef? {
        let trimmed = text.dartTrimmed
        if !trimmed.contains("://"), !trimmed.hasPrefix("/"), let hash = trimmed.firstIndex(of: "#") {
            let parts = trimmed[..<hash].split(separator: "/", omittingEmptySubsequences: false)
            if parts.count == 2, isName(parts[0]), isName(parts[1]), let n = number(trimmed[trimmed.index(after: hash)...]) {
                return NoteLinkRef(owner: String(parts[0]), repo: String(parts[1]), kind: fallbackKind, number: n)
            }
            return nil
        }
        guard let components = URLComponents(string: trimmed) else { return nil }
        let parts = components.path.split(separator: "/")
        if parts.count >= 4, isName(parts[0]), isName(parts[1]), let kind = pathKinds[String(parts[2])], let n = number(parts[3]) {
            return NoteLinkRef(owner: String(parts[0]), repo: String(parts[1]), kind: kind, number: n)
        }
        let tab = components.queryItems?.first { $0.name == "tab" }?.value ?? ""
        if parts.count == 2, isName(parts[0]), isName(parts[1]), let kind = pathKinds[tab],
           let n = number(Substring(components.fragment ?? "")) {
            return NoteLinkRef(owner: String(parts[0]), repo: String(parts[1]), kind: kind, number: n)
        }
        return nil
    }
}

@MainActor
@Observable
public final class NoteEditorModel {
    public static let deleteConfirmation = ToolsConfirmation(
        title: "Delete note?",
        message: "This removes the note and its history for everyone it is shared with.",
        confirm: "Delete", cancel: "Cancel", destructive: true)
    public static let discardConfirmation = ToolsConfirmation(
        title: "Discard changes?", message: "Your edits to this note have not been saved.",
        confirm: "Discard", cancel: "Keep editing", destructive: true)
    public static let titleMaxLength = 200
    public static let autosaveDelayMillis = 1500
    public static let conflictMessage = "Someone else saved this note first. Keep your version or take theirs."
    public static let linkCopied = "Link copied"
    public static let deletedMessage = "This note was deleted."

    @ObservationIgnored public let call: ToolsApiCall
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored private let sleep: ToolsSleep
    @ObservationIgnored private let webLink: ToolsWebLink
    @ObservationIgnored private let openSocket: NoteSocketOpen?
    @ObservationIgnored private let me: String
    @ObservationIgnored private var openComment: String?
    @ObservationIgnored private var viewRecorded = false
    @ObservationIgnored private let seen: NoteViewsSeen
    @ObservationIgnored var autosaveTask: Task<Void, Never>?
    @ObservationIgnored private var autosaveGeneration = 0
    @ObservationIgnored private var saveQueued = false
    public let noteId: String?

    public private(set) var note: JSONObject?
    public private(set) var loading: Bool
    public private(set) var saving = false
    public private(set) var error = ""
    public private(set) var conflict: JSONObject?
    public private(set) var saveFailure = ""
    public private(set) var linkBusy = false
    public private(set) var room: NoteRoomModel?
    public private(set) var comments: NoteCommentsModel?
    public private(set) var selectionRequest: NoteSelectionRequest?
    @ObservationIgnored private var requestSerial = 0
    public var commentsOpen = false
    public var selection: Range<Int>?
    public var title = ""
    public var markdown = ""
    public var mode: NoteMode = .write
    public var linkText = ""
    public var linkKind = "issue"
    private var savedTitle = ""
    private var savedMarkdown = ""

    public init(
        noteId: String?, call: @escaping ToolsApiCall, clock: Clock = SystemClock(),
        sleep: @escaping ToolsSleep = toolsTaskSleep, webLink: @escaping ToolsWebLink = { _ in nil },
        openSocket: NoteSocketOpen? = nil, me: String = "", openComment: String? = nil, seen: NoteViewsSeen = .shared
    ) {
        self.seen = seen
        self.noteId = noteId
        self.call = call
        self.clock = clock
        self.sleep = sleep
        self.webLink = webLink
        self.openSocket = openSocket
        self.me = me
        self.openComment = openComment.flatMap { $0.isEmpty ? nil : $0 }
        loading = noteId != nil
    }

    public convenience init(
        noteId: String?, api: ApiService, connector: WebSocketConnector? = nil, me: String = "", openComment: String? = nil
    ) {
        self.init(
            noteId: noteId, call: { method, suffix, body in try await api.notesApi(method, suffix, body) },
            webLink: { path in api.webLink(path) },
            openSocket: connector.map { (sockets: WebSocketConnector) -> NoteSocketOpen in
                { id in try await api.noteSocket(id, connector: sockets) }
            },
            me: me, openComment: openComment)
    }

    public var id: String? { note == nil ? noteId : ToolsJSON.show(note?["id"]) }
    public var role: String { ToolsJSON.text(note?["role"], "owner") }
    public var isOwner: Bool { role == "owner" }
    public var canEdit: Bool { role == "owner" || role == "editor" }
    public var dirty: Bool { title != savedTitle || markdown != savedMarkdown }
    public var existing: Bool { id != nil }
    public var version: Int { ToolsJSON.int(note?["version"]) }
    public var isPublic: Bool { note?["visibility"]?.stringValue == "public" }
    public var shares: [JSONObject] { ToolsJSON.maps(note?["shares"]) }
    public var links: [NoteLinkRef] { ToolsJSON.maps(note?["links"]).map(NoteLinkRef.init(json:)) }

    public var screenTitle: String { existing ? (canEdit ? "Edit note" : "Note") : "New note" }
    public var showsSave: Bool { canEdit && !loading && error.isEmpty }
    public var saveEnabled: Bool { !(saving || conflict != nil || (existing && !dirty)) }
    public var saveLabel: String { saving ? "Saving…" : "Save" }
    public var showsMenu: Bool { existing && note != nil }
    /// Leaving needs the discard confirmation.
    public var confirmsLeave: Bool { dirty && canEdit && (conflict != nil || !saveFailure.isEmpty) }
    public var shownMode: NoteMode { mode }
    public var showsLinks: Bool { existing && note != nil && (canEdit || !links.isEmpty) }

    public var saveState: String {
        if loading || !error.isEmpty { return "" }
        if !canEdit { return "View only" }
        if saving { return "Saving…" }
        if conflict != nil { return "Not saved — resolve the conflict" }
        if !saveFailure.isEmpty { return "Not saved" }
        if dirty { return "Unsaved changes" }
        return existing ? "Saved" : ""
    }

    public var counts: String {
        let words = markdown.split(whereSeparator: { $0.isWhitespace || $0.isNewline }).count
        return "\(words) \(words == 1 ? "word" : "words") · \(max(1, Int((Double(words) / 220).rounded()))) min read"
    }

    /// The line under the title.
    public var meta: String {
        guard let note else { return "Only you can see a new note until you share or publish it." }
        var parts = [
            "Version \(ToolsJSON.int(note["version"]))",
            "edited \(Format.relativeTime(ToolsJSON.epoch(note["updatedAt"]), nowMillis: clock.nowMillis))",
            noteReach(note),
        ]
        if !canEdit { parts.append("read only") }
        return parts.joined(separator: " · ")
    }

    public var accessPath: String? { id.map { "/dashboard/notes?note=\($0)" } }
    public var publicPath: String? { isPublic ? id.map { "/notes/\($0)" } : nil }
    public var accessLink: URL? { accessPath.flatMap { webLink($0) } }
    public var publicLink: URL? { publicPath.flatMap { webLink($0) } }

    /// The overflow menu: (value, label) in display order.
    public var menuItems: [(value: String, label: String)] {
        var items: [(value: String, label: String)] = [("history", "History"), ("copy_link", "Copy link")]
        if isPublic { items.append(("open_public", "Open published page")) }
        items.append(("export", "Export Markdown"))
        if isOwner {
            items.append(("share", "Share"))
            items.append(isPublic ? ("unpublish", "Make private") : ("publish", "Publish"))
            items.append(("delete", "Delete"))
        }
        return items
    }

    private func apply(_ note: JSONObject) {
        self.note = note
        savedTitle = ToolsJSON.text(note["title"], "")
        savedMarkdown = ToolsJSON.text(note["markdown"], "")
        title = savedTitle
        markdown = savedMarkdown
        comments?.isOwner = isOwner
        seen.record(note)
    }

    /// Takes a fresh copy of the note's metadata without replacing what was typed but not saved.
    func applyMeta(_ data: JSONObject) {
        let typedTitle = title, typedMarkdown = markdown
        apply(ToolsJSON.object(data["note"]))
        title = typedTitle
        markdown = typedMarkdown
    }

    /// A restored version from the history sheet replaces the text.
    public func applyRestored(_ note: JSONObject) -> ToolsOutcome {
        conflict = nil
        saveFailure = ""
        apply(note)
        return .done("Version restored")
    }

    public func recordView() async {
        guard !viewRecorded, note != nil, error.isEmpty, let id else { return }
        viewRecorded = true
        note?["unread"] = false
        if let note { seen.record(note) }
        _ = try? await call("POST", "/\(id)/activity", ["kind": "view"])
    }

    public func load() async {
        guard let noteId else { return }
        loading = true
        error = ""
        do {
            let data = try await call("GET", "/\(noteId)", nil)
            apply(ToolsJSON.object(data["note"]))
            if !canEdit { mode = .preview }
            loading = false
            joinRoom()
            joinComments()
        } catch {
            loading = false
            self.error = notesErrorText(error, "This note could not be opened.")
        }
    }

    public func edited() {
        autosaveGeneration += 1
        guard canEdit, !loading, error.isEmpty, conflict == nil, dirty else { return }
        room?.draftChanged(title: title, markdown: markdown, baseVersion: version)
        if !existing && title.dartTrimmed.isEmpty && markdown.dartTrimmed.isEmpty { return }
        let generation = autosaveGeneration
        let sleep = self.sleep
        autosaveTask = Task { [weak self] in
            do { try await sleep(Self.autosaveDelayMillis) } catch { return }
            guard let self, self.autosaveGeneration == generation else { return }
            _ = await self.save()
        }
    }

    public func flush() async {
        autosaveGeneration += 1
        guard canEdit, dirty, conflict == nil else { return }
        _ = await save()
    }

    public func save() async -> ToolsOutcome {
        if !canEdit { return ToolsOutcome() }
        if saving {
            saveQueued = true
            return ToolsOutcome()
        }
        if conflict != nil { return .failed(Self.conflictMessage) }
        saving = true
        saveFailure = ""
        let sentTitle = title, sentMarkdown = markdown
        do {
            let data: JSONObject
            if let id {
                data = try await call("PATCH", "/\(id)", [
                    "baseVersion": .int(Int64(version)),
                    "title": .string(sentTitle.dartTrimmed),
                    "markdown": .string(sentMarkdown),
                ])
            } else {
                data = try await call("POST", "", [
                    "title": .string(sentTitle.dartTrimmed),
                    "markdown": .string(sentMarkdown),
                    "visibility": "private",
                ])
            }
            let typedTitle = title, typedMarkdown = markdown
            apply(ToolsJSON.object(data["note"]))
            if typedTitle != sentTitle || typedMarkdown != sentMarkdown {
                savedTitle = sentTitle
                savedMarkdown = sentMarkdown
                title = typedTitle
                markdown = typedMarkdown
            }
            saving = false
            joinRoom()
            joinComments()
            if saveQueued || dirty {
                saveQueued = false
                edited()
            }
            return .done("Note saved")
        } catch let error as OrgApiException {
            saving = false
            saveQueued = false
            if error.code == "version_conflict" {
                if let id, let latest = try? await call("GET", "/\(id)", nil) {
                    conflict = ToolsJSON.object(latest["note"])
                }
                return .failed(Self.conflictMessage)
            }
            saveFailure = notesErrorText(error, "The note could not be saved.")
            return .failed(saveFailure)
        } catch {
            saving = false
            saveQueued = false
            saveFailure = "The note could not be saved."
            return .failed(saveFailure)
        }
    }

    private func joinRoom() {
        guard room == nil, error.isEmpty, let id, let openSocket else { return }
        let joined = NoteRoomModel(noteId: id, open: openSocket, clock: clock, sleep: sleep) { [weak self] (event: NoteRoomEvent) in
            self?.roomEvent(event)
        }
        room = joined
        joined.start()
    }

    private func joinComments() {
        guard comments == nil, error.isEmpty, let id else { return }
        let model = NoteCommentsModel(noteId: id, me: me, isOwner: isOwner, call: call, clock: clock, sleep: sleep)
        comments = model
        if let openComment {
            self.openComment = nil
            model.focus(openComment)
            commentsOpen = true
        }
    }

    public func leave() {
        room?.stop()
        room = nil
    }

    public var highlightRange: Range<Int>? {
        guard let comments, let id = comments.focusedThreadId else { return nil }
        return comments.range(of: id, in: markdown)
    }

    private func requestSelection(_ range: Range<Int>) {
        requestSerial += 1
        selection = range
        selectionRequest = NoteSelectionRequest(range: range, serial: requestSerial)
    }

    public func pick(_ thread: NoteThread) {
        guard let comments else { return }
        comments.focus(thread.id)
        guard let range = thread.range else { return }
        mode = .write
        requestSelection(range)
    }

    public func selectionChanged(_ range: Range<Int>) {
        selection = range
        guard let shown = highlightRange else { return }
        if range.lowerBound < shown.lowerBound || range.upperBound > shown.upperBound {
            comments?.focus(nil)
        }
    }

    public var selectedQuote: NoteQuote? {
        guard let selection, !selection.isEmpty else { return nil }
        return NoteAnchor.capture(markdown, selection: selection)
    }

    public var canComment: Bool { comments != nil }

    public func commentOnSelection() {
        guard let comments else { return }
        comments.startComment(shownMode == .write ? selectedQuote : nil)
        commentsOpen = true
    }

    func roomEvent(_ event: NoteRoomEvent) {
        switch event {
        case let .draft(draftTitle, draftMarkdown, baseVersion):
            guard baseVersion == version, !dirty, !saving, conflict == nil else { return }
            savedTitle = draftTitle
            savedMarkdown = draftMarkdown
            title = draftTitle
            markdown = draftMarkdown
        case .commentsChanged:
            comments?.reloadSoon()
        case let .updated(newVersion):
            guard newVersion > version, !dirty, !saving else { return }
            Task { await reload() }
        case .deleted:
            autosaveGeneration += 1
            savedTitle = title
            savedMarkdown = markdown
            error = Self.deletedMessage
            leave()
        case .accessChanged:
            let sleep = self.sleep
            Task { [weak self] in
                try? await sleep(NoteRoomModel.accessReconnectMillis)
                await self?.reload()
            }
        }
    }

    func reload() async {
        guard let id else { return }
        do {
            let couldEdit = canEdit
            let data = try await call("GET", "/\(id)", nil)
            if dirty || saving {
                applyMeta(data)
            } else {
                apply(ToolsJSON.object(data["note"]))
            }
            if couldEdit && !canEdit { mode = .preview }
        } catch let refusal as OrgApiException {
            if refusal.code == "note_not_found" {
                autosaveGeneration += 1
                error = notesErrorText(refusal, "")
                leave()
            }
        } catch {}
    }

    public func keepMine() async -> ToolsOutcome {
        guard let latest = conflict else { return ToolsOutcome() }
        conflict = nil
        let typedTitle = title, typedMarkdown = markdown
        apply(latest)
        title = typedTitle
        markdown = typedMarkdown
        return await save()
    }

    public func takeTheirs() -> ToolsOutcome {
        guard let latest = conflict else { return ToolsOutcome() }
        conflict = nil
        saveFailure = ""
        apply(latest)
        return .done("Showing the latest saved version")
    }

    public func format(_ kind: NoteFormat) {
        guard canEdit else { return }
        let end = markdown.utf16.count
        let result = NoteFormatting.apply(kind, to: markdown, selection: selection ?? end..<end)
        markdown = result.text
        if selection != nil { requestSelection(result.selection) }
        edited()
    }

    public var exportFileName: String {
        let source = title.dartTrimmed.isEmpty ? "Untitled note" : title.dartTrimmed
        var slug = ""
        var dash = false
        for scalar in source.unicodeScalars {
            let keep = scalar.isASCII && (CharacterSet.alphanumerics.contains(scalar) || scalar == "_" || scalar == "." || scalar == "-")
            if keep {
                slug.unicodeScalars.append(scalar)
                dash = false
            } else if !dash {
                slug.append("-")
                dash = true
            }
        }
        let trimmed = slug.drop(while: { $0 == "-" }).reversed().drop(while: { $0 == "-" }).reversed()
        let name = String(trimmed).lowercased()
        return "\(name.isEmpty ? "note" : name).md"
    }

    public var exportText: String {
        "# \(title.dartTrimmed.isEmpty ? "Untitled note" : title.dartTrimmed)\n\n\(markdown)"
    }

    public func exportFile(in directory: URL) throws -> URL {
        try Self.writeExport(fileName: exportFileName, text: exportText, in: directory)
    }

    public nonisolated static func writeExport(fileName: String, text: String, in directory: URL) throws -> URL {
        let target = directory.appendingPathComponent(fileName)
        try Data(text.utf8).write(to: target, options: .atomic)
        return target
    }

    private func updateLinks(_ method: String, _ link: NoteLinkRef) async throws {
        guard let id else { return }
        let data = try await call(method, "/\(id)/links", link.body)
        note?["links"] = data["links"] ?? .array([])
    }

    public func addLink() async -> ToolsOutcome {
        guard canEdit, existing else { return ToolsOutcome() }
        guard let link = NoteLinkRef.parse(linkText, fallbackKind: linkKind) else {
            return .failed(noteErrorMessages["invalid_link"] ?? "")
        }
        linkBusy = true
        defer { linkBusy = false }
        do {
            try await updateLinks("POST", link)
            linkText = ""
            return .done("Linked \(link.title)")
        } catch {
            return .failed(notesErrorText(error, "The link was not added."))
        }
    }

    public func removeLink(_ link: NoteLinkRef) async -> ToolsOutcome {
        guard canEdit, existing else { return ToolsOutcome() }
        linkBusy = true
        defer { linkBusy = false }
        do {
            try await updateLinks("DELETE", link)
            return .done("Unlinked \(link.title)")
        } catch {
            return .failed(notesErrorText(error, "The link was not removed."))
        }
    }

    public func linkURL(_ link: NoteLinkRef) -> URL? { webLink(link.path) }

    public func setVisibility(_ visibility: String) async -> ToolsOutcome {
        guard let id else { return ToolsOutcome() }
        do {
            let data = try await call("PATCH", "/\(id)", ["baseVersion": .int(Int64(version)), "visibility": .string(visibility)])
            applyMeta(data)
            return .done(visibility == "public" ? "Published. Anyone with the link can read it." : "Note is private again")
        } catch {
            return .failed(notesErrorText(error, "Visibility was not changed."))
        }
    }

    /// After the person confirmed `deleteConfirmation`.
    public func delete() async -> ToolsOutcome {
        guard let id else { return ToolsOutcome() }
        do {
            autosaveGeneration += 1
            _ = try await call("DELETE", "/\(id)", nil)
            leave()
            savedTitle = title
            savedMarkdown = markdown
            return ToolsOutcome(close: true)
        } catch {
            return .failed(notesErrorText(error, "The note was not deleted."))
        }
    }

    /// After the discard confirmation: the text counts as saved so leaving is not asked again.
    public func discard() {
        autosaveGeneration += 1
        savedTitle = title
        savedMarkdown = markdown
    }

    /// After the share sheet closed: refresh the reach label; unsaved typing stays.
    public func refreshMeta() async {
        guard let id else { return }
        if let data = try? await call("GET", "/\(id)", nil) { applyMeta(data) }
    }

    public func historyModel() -> NoteHistoryModel? {
        guard let id else { return nil }
        return NoteHistoryModel(noteId: id, currentVersion: version, canRestore: canEdit, call: call, clock: clock)
    }

    public func shareModel() -> NoteShareModel? {
        guard let id else { return nil }
        return NoteShareModel(noteId: id, initial: shares, call: call, sleep: sleep, webLink: webLink)
    }
}

/// Stored versions, newest first. Restoring writes the old text as a new version.
@MainActor
@Observable
public final class NoteHistoryModel {
    public static let title = "History"

    @ObservationIgnored private let call: ToolsApiCall
    @ObservationIgnored private let clock: Clock
    public let noteId: String
    public let currentVersion: Int
    public let canRestore: Bool
    public private(set) var versions: [JSONObject]?
    public private(set) var error = ""
    public private(set) var busy = false

    public init(noteId: String, currentVersion: Int, canRestore: Bool, call: @escaping ToolsApiCall, clock: Clock = SystemClock()) {
        self.noteId = noteId
        self.currentVersion = currentVersion
        self.canRestore = canRestore
        self.call = call
        self.clock = clock
    }

    public func load() async {
        do {
            versions = ToolsJSON.maps(try await call("GET", "/\(noteId)/versions", nil)["versions"])
        } catch {
            self.error = notesErrorText(error, "History is unavailable.")
        }
    }

    public func version(_ entry: JSONObject) -> Int { ToolsJSON.int(entry["version"]) }
    public func title(_ entry: JSONObject) -> String { "Version \(version(entry))" }
    public func subtitle(_ entry: JSONObject) -> String {
        let when = Format.relativeTime(ToolsJSON.epoch(entry["createdAt"]), nowMillis: clock.nowMillis)
        let author = entry["author"]?.stringValue ?? ""
        return author.isEmpty ? when : "\(when) · by @\(author)"
    }
    public func isCurrent(_ entry: JSONObject) -> Bool { version(entry) == currentVersion }

    /// The restored note to hand back to the editor, or nil with the failure to show.
    public func restore(_ version: Int) async -> (note: JSONObject?, failure: String?) {
        busy = true
        do {
            let data = try await call("POST", "/\(noteId)/versions", [
                "version": .int(Int64(version)), "baseVersion": .int(Int64(currentVersion)),
            ])
            return (ToolsJSON.object(data["note"]), nil)
        } catch {
            busy = false
            return (nil, notesErrorText(error, "That version was not restored."))
        }
    }
}

public struct NoteShareSuggestion: Equatable, Identifiable, Sendable {
    public let type: String
    public let name: String

    public init(type: String, name: String) {
        self.type = type
        self.name = name
    }

    public var id: String { "\(type):\(name)" }
    public var typeLabel: String { type == "organization" ? "Organization" : "User" }
    public var icon: String { type == "organization" ? "groups_outlined" : "person_outline" }
}

/// Owner-only sharing: people or organizations, as viewers or editors.
@MainActor
@Observable
public final class NoteShareModel {
    public static let title = "Share"
    public static let subtitle = "Viewers can read; editors can change the text."
    public static let notShared = "Not shared with anyone yet."
    public static let accessTitle = "Link for people with access"
    public static let accessHint = "Only people this note is shared with can open it."
    public static let suggestDelayMillis = 150
    public static let maxQueryLength = 63
    public static let roles: [(value: String, label: String)] = [("editor", "Can edit"), ("viewer", "Can view")]

    @ObservationIgnored private let call: ToolsApiCall
    @ObservationIgnored private let sleep: ToolsSleep
    @ObservationIgnored private let webLink: ToolsWebLink
    @ObservationIgnored var suggestTask: Task<Void, Never>?
    @ObservationIgnored private var generation = 0
    @ObservationIgnored private var picked: String?
    public let noteId: String
    public private(set) var shares: [JSONObject]
    public private(set) var suggestions: [NoteShareSuggestion] = []
    public var name = ""
    /// "user" or "organization".
    public var type = "user"
    /// "viewer" or "editor".
    public var role = "viewer"
    public private(set) var busy = false

    public init(
        noteId: String, initial: [JSONObject], call: @escaping ToolsApiCall,
        sleep: @escaping ToolsSleep = toolsTaskSleep, webLink: @escaping ToolsWebLink = { _ in nil }
    ) {
        self.noteId = noteId
        shares = initial
        self.call = call
        self.sleep = sleep
        self.webLink = webLink
    }

    public func shareTitle(_ share: JSONObject) -> String { ToolsJSON.show(share["name"]) }

    public func shareSubtitle(_ share: JSONObject) -> String {
        share["type"]?.stringValue == "organization" ? "Organization" : "User"
    }

    public func shareRole(_ share: JSONObject) -> String { share["role"]?.stringValue == "editor" ? "editor" : "viewer" }

    public func shareIcon(_ share: JSONObject) -> String {
        share["type"]?.stringValue == "organization" ? "groups_outlined" : "person_outline"
    }

    public var showsAccessLink: Bool { !shares.isEmpty }
    public var accessPath: String { "/dashboard/notes?note=\(noteId)" }
    public var accessLink: URL? { webLink(accessPath) }

    public var query: String {
        String(ToolsText.dropLeadingAts(name.dartTrimmed).lowercased().unicodeScalars.filter {
            $0.isASCII && (CharacterSet.alphanumerics.contains($0) || $0 == "-" || $0 == "_" || $0 == ".")
        }.map(Character.init))
    }

    public func nameChanged() {
        generation += 1
        let current = generation
        let text = query
        if text.isEmpty || text.count > Self.maxQueryLength || text == picked {
            suggestions = []
            return
        }
        let sleep = self.sleep
        suggestTask = Task { [weak self] in
            do { try await sleep(Self.suggestDelayMillis) } catch { return }
            guard let self, self.generation == current else { return }
            await self.suggest(text, generation: current)
        }
    }

    func suggest(_ text: String, generation current: Int) async {
        let answer = try? await call("GET", "/\(noteId)/shares?query=\(text)", nil)
        guard generation == current else { return }
        let taken = Set(shares.map { "\(ToolsJSON.show($0["type"])):\(ToolsJSON.show($0["name"]).lowercased())" })
        suggestions = ToolsJSON.maps(answer?["suggestions"]).compactMap { (item: JSONObject) -> NoteShareSuggestion? in
            let name = ToolsJSON.text(item["name"], "").lowercased()
            let type = item["type"]?.stringValue == "organization" ? "organization" : "user"
            return name.isEmpty ? nil : NoteShareSuggestion(type: type, name: name)
        }.filter { !taken.contains($0.id) }
    }

    public func pick(_ suggestion: NoteShareSuggestion) {
        generation += 1
        picked = suggestion.name
        name = suggestion.name
        type = suggestion.type
        suggestions = []
    }

    public func dismissSuggestions() {
        generation += 1
        suggestions = []
    }

    /// Returns the failure to show, if any.
    private func send(_ method: String, _ body: JSONObject) async -> String? {
        busy = true
        do {
            let data = try await call(method, "/\(noteId)/shares", body)
            shares = ToolsJSON.maps(data["shares"])
            busy = false
            return nil
        } catch {
            busy = false
            let code = (error as? OrgApiException)?.code ?? ""
            return code == "principal_not_found"
                ? "No \(type == "user" ? "user" : "organization") has that name."
                : notesErrorText(error, "Sharing was not changed.")
        }
    }

    public func add() async -> ToolsOutcome {
        let cleaned = ToolsText.dropLeadingAts(name.dartTrimmed).lowercased()
        if cleaned.isEmpty { return ToolsOutcome() }
        dismissSuggestions()
        if let failure = await send("POST", ["type": .string(type), "name": .string(cleaned), "role": .string(role)]) {
            return .failed(failure)
        }
        name = ""
        return .done("Shared with \(cleaned). Copy the link below to send it.")
    }

    public func setRole(_ share: JSONObject, _ role: String) async -> ToolsOutcome {
        let who = ToolsJSON.show(share["name"])
        if let failure = await send("POST", ["type": share["type"] ?? .null, "name": share["name"] ?? .null, "role": .string(role)]) {
            return .failed(failure)
        }
        return .done("\(who) can now \(role == "editor" ? "edit" : "view")")
    }

    public func stop(_ share: JSONObject) async -> ToolsOutcome {
        let who = ToolsJSON.show(share["name"])
        if let failure = await send("DELETE", ["type": share["type"] ?? .null, "name": share["name"] ?? .null]) {
            return .failed(failure)
        }
        return .done("Removed \(who)")
    }
}

enum ToolsText {
    /// `replaceFirst(RegExp(r'^@+'), '')`.
    static func dropLeadingAts(_ text: String) -> String { String(text.drop(while: { $0 == "@" })) }

    /// `_cap`: the first character upper-cased.
    static func cap(_ value: String) -> String {
        guard let first = value.first else { return value }
        return String(first).uppercased() + value.dropFirst()
    }
}
