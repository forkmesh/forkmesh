import FMApi
import FMBase
import Foundation
import Observation

public enum NoteEmoji {
    public static let maxScalars = 16
    public static let maxBytes = 64
    public static let invalidMessage = "Use a single emoji, such as 📝 or 🇫🇷."
    public static let common: [String] = [
        "📝", "📌", "📎", "📚", "📖", "💡", "✅", "⭐",
        "🔥", "🚀", "🎯", "📅", "⏰", "🗂️", "📁", "🧠",
        "💬", "❓", "❗", "⚠️", "🐛", "🔧", "🔒", "🔑",
        "🌱", "🌍", "🏠", "💼", "💰", "📈", "🎉", "❤️",
        "👍", "👀", "🎨", "🎵", "☕", "🍕", "✈️", "🏁",
    ]

    static let allowedCategories: Set<Unicode.GeneralCategory> = [
        .mathSymbol, .currencySymbol, .modifierSymbol, .otherSymbol,
        .nonspacingMark, .spacingMark, .enclosingMark, .format,
    ]

    public static func clean(_ text: String) -> String? {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { return "" }
        let scalars = Array(trimmed.unicodeScalars)
        if scalars.count > maxScalars || trimmed.utf8.count > maxBytes { return nil }
        for (index, scalar) in scalars.enumerated() {
            let category = scalar.properties.generalCategory
            if scalar.properties.isWhitespace || category == .control || category == .spaceSeparator
                || category == .lineSeparator || category == .paragraphSeparator {
                return nil
            }
            if scalar.isASCII {
                let keycapBase = ("0"..."9").contains(scalar) || scalar == "#" || scalar == "*"
                let next = index + 1 < scalars.count ? scalars[index + 1].value : 0
                let after = index + 2 < scalars.count ? scalars[index + 2].value : 0
                let keycap = next == 0x20E3 || (next == 0xFE0F && after == 0x20E3)
                if !keycapBase || !keycap { return nil }
                continue
            }
            if !allowedCategories.contains(category) { return nil }
        }
        return trimmed
    }
}

public enum NotePageAccent: String, CaseIterable, Identifiable, Sendable {
    case blue, purple, pink, red, orange, yellow, teal, gray

    public var id: String { rawValue }
    public var label: String { ToolsText.cap(rawValue) }

    public init(json value: JSONValue?) {
        self = NotePageAccent(rawValue: value?.stringValue ?? "") ?? .blue
    }
}

public enum NotePageLayout: String, CaseIterable, Identifiable, Sendable {
    case outline, continuous

    public var id: String { rawValue }
    public var label: String { self == .outline ? "Outline" : "Continuous" }

    public init(json value: JSONValue?) {
        self = NotePageLayout(rawValue: value?.stringValue ?? "") ?? .outline
    }
}

public enum NotePageEntryKind: String, Sendable {
    case note, section, link
}

public struct NotePageEntry: Equatable, Identifiable, Sendable {
    public static let maxDepth = 3

    public var id: String
    public var kind: NotePageEntryKind
    public var depth: Int
    public var label: String
    public var noteId: String
    public var url: String
    public var collapsed: Bool

    public init(
        id: String = NotePageEntry.makeId(), kind: NotePageEntryKind, depth: Int = 0, label: String = "",
        noteId: String = "", url: String = "", collapsed: Bool = false
    ) {
        self.id = id
        self.kind = kind
        self.depth = depth
        self.label = label
        self.noteId = noteId
        self.url = url
        self.collapsed = collapsed
    }

    public init?(json: JSONObject) {
        guard let kind = NotePageEntryKind(rawValue: ToolsJSON.text(json["kind"], "")) else { return nil }
        let id = ToolsJSON.text(json["id"], "")
        guard !id.isEmpty else { return nil }
        self.init(
            id: id, kind: kind, depth: min(max(0, ToolsJSON.int(json["depth"])), Self.maxDepth),
            label: ToolsJSON.text(json["label"], ""), noteId: ToolsJSON.text(json["noteId"], ""),
            url: ToolsJSON.text(json["url"], ""), collapsed: json["collapsed"]?.boolValue ?? false)
    }

    public var json: JSONObject {
        [
            "id": .string(id), "kind": .string(kind.rawValue), "depth": .int(Int64(depth)), "label": .string(label),
            "noteId": .string(noteId), "url": .string(url), "collapsed": .bool(collapsed),
        ]
    }

    public static func makeId() -> String {
        String(UUID().uuidString.lowercased().replacingOccurrences(of: "-", with: "").prefix(16))
    }
}

public struct NotePageNote: Equatable, Sendable {
    public let id: String
    public let title: String
    public let emoji: String
    public let updatedAt: Int64
    public let lastEditedBy: String
    public let unread: Bool
    public let role: String
    public let markdown: String?

    public init(json: JSONObject) {
        id = ToolsJSON.text(json["id"], "")
        title = ToolsJSON.text(json["title"], "Untitled note")
        emoji = ToolsJSON.text(json["emoji"], "")
        updatedAt = ToolsJSON.epoch(json["updatedAt"])
        lastEditedBy = ToolsJSON.text(json["lastEditedBy"], "")
        unread = json["unread"]?.boolValue ?? false
        role = ToolsJSON.text(json["role"], "viewer")
        markdown = json["markdown"]?.stringValue
    }
}

public struct NotePage: Equatable, Sendable {
    public let id: String
    public let title: String
    public let emoji: String
    public let description: String
    public let accent: NotePageAccent
    public let layout: NotePageLayout
    public let showEmoji: Bool
    public let role: String
    public let version: Int
    public let updatedAt: Int64
    public let entries: Int
    public let shares: [JSONObject]
    public let outline: [NotePageEntry]

    public init(json: JSONObject) {
        id = ToolsJSON.text(json["id"], "")
        title = ToolsJSON.text(json["title"], "").isEmpty ? "Untitled page" : ToolsJSON.text(json["title"], "")
        emoji = ToolsJSON.text(json["emoji"], "")
        description = ToolsJSON.text(json["description"], "")
        accent = NotePageAccent(json: json["accent"])
        layout = NotePageLayout(json: json["layout"])
        showEmoji = json["showEmoji"]?.boolValue ?? true
        role = ToolsJSON.text(json["role"], "viewer")
        version = ToolsJSON.int(json["version"])
        updatedAt = ToolsJSON.epoch(json["updatedAt"])
        outline = ToolsJSON.maps(json["outline"]).compactMap(NotePageEntry.init(json:))
        entries = json["entries"] == nil ? outline.count : ToolsJSON.int(json["entries"])
        shares = ToolsJSON.maps(json["shares"])
    }

    public var canEdit: Bool { role == "owner" || role == "editor" }
    public var isOwner: Bool { role == "owner" }

    public var entriesLabel: String {
        entries == 1 ? "1 entry" : "\(max(0, entries)) entries"
    }
}

public enum NotePageRowKind: Equatable, Sendable {
    case section(collapsed: Bool)
    case note(id: String)
    case locked
    case link(url: String)
}

public struct NotePageRow: Equatable, Identifiable, Sendable {
    public let id: String
    public let depth: Int
    public let kind: NotePageRowKind
    public let title: String
    public let emoji: String
    public let unread: Bool

    public var noteId: String? {
        if case let .note(id) = kind { return id }
        return nil
    }
}

public enum NotePageOutline {
    public static let maxEntries = 300
    public static let maxUrl = 2000
    public static let lockedText = "You don't have access to this note"

    @MainActor
    public static func rows(
        _ outline: [NotePageEntry], notes: [String: NotePageNote], showEmoji: Bool, collapsed: Set<String> = [],
        seen: NoteViewsSeen? = nil
    ) -> [NotePageRow] {
        var out: [NotePageRow] = []
        var hiddenBelow: Int?
        for entry in outline {
            if let limit = hiddenBelow {
                if entry.kind == .section && entry.depth <= limit {
                    hiddenBelow = nil
                } else {
                    continue
                }
            }
            switch entry.kind {
            case .section:
                let closed = collapsed.contains(entry.id)
                out.append(NotePageRow(id: entry.id, depth: entry.depth, kind: .section(collapsed: closed), title: entry.label, emoji: "", unread: false))
                if closed { hiddenBelow = entry.depth }
            case .link:
                out.append(NotePageRow(
                    id: entry.id, depth: entry.depth, kind: .link(url: entry.url),
                    title: entry.label.isEmpty ? String(entry.url.dropFirst(8)) : entry.label, emoji: "", unread: false))
            case .note:
                guard let note = notes[entry.noteId] else {
                    out.append(NotePageRow(
                        id: entry.id, depth: entry.depth, kind: .locked,
                        title: entry.label.isEmpty ? lockedText : entry.label, emoji: "", unread: false))
                    continue
                }
                let unread = seen?.unread(id: note.id, flagged: note.unread, updatedAt: note.updatedAt) ?? note.unread
                out.append(NotePageRow(
                    id: entry.id, depth: entry.depth, kind: .note(id: note.id),
                    title: entry.label.isEmpty ? note.title : entry.label, emoji: showEmoji ? note.emoji : "", unread: unread))
            }
        }
        return out
    }

    public static func validLink(_ url: String) -> String? {
        let trimmed = url.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmed.hasPrefix("https://"), trimmed.count > 8, trimmed.count <= maxUrl,
              !trimmed.unicodeScalars.contains(where: { $0.properties.isWhitespace || $0.value < 32 }) else { return nil }
        return trimmed
    }

    public static func indent(_ outline: [NotePageEntry], _ id: String, by step: Int) -> [NotePageEntry] {
        outline.map { (entry: NotePageEntry) -> NotePageEntry in
            guard entry.id == id else { return entry }
            var next = entry
            next.depth = min(max(0, entry.depth + step), NotePageEntry.maxDepth)
            return next
        }
    }

    public static func move(_ outline: [NotePageEntry], from offsets: IndexSet, to destination: Int) -> [NotePageEntry] {
        let valid = offsets.filter { $0 >= 0 && $0 < outline.count }
        guard !valid.isEmpty else { return outline }
        let moving = valid.sorted().map { outline[$0] }
        var rest = outline
        for index in valid.sorted(by: >) { rest.remove(at: index) }
        let target = min(max(0, destination - valid.filter { $0 < destination }.count), rest.count)
        rest.insert(contentsOf: moving, at: target)
        return rest
    }

    public static func step(_ outline: [NotePageEntry], _ id: String, by delta: Int) -> [NotePageEntry] {
        guard let index = outline.firstIndex(where: { $0.id == id }) else { return outline }
        let target = index + delta
        guard target >= 0, target < outline.count else { return outline }
        var next = outline
        next.swapAt(index, target)
        return next
    }
}

extension NoteViewsSeen {
    public func unread(id: String, flagged: Bool, updatedAt: Int64) -> Bool {
        unread(["id": .string(id), "unread": .bool(flagged), "updatedAt": .int(updatedAt)])
    }
}

@MainActor
@Observable
public final class NotePagesModel {
    public static let title = "Pages"

    @ObservationIgnored private let call: ToolsApiCall
    public private(set) var pages: [NotePage] = []
    public private(set) var loaded = false
    public private(set) var busy = false

    public init(call: @escaping ToolsApiCall) {
        self.call = call
    }

    public func load() async {
        guard let data = try? await call("GET", "/pages", nil) else { return }
        pages = ToolsJSON.maps(data["pages"]).map(NotePage.init(json:)).filter { !$0.id.isEmpty }
        loaded = true
    }

    public func create() async -> (id: String?, outcome: ToolsOutcome) {
        busy = true
        defer { busy = false }
        do {
            let data = try await call("POST", "/pages", ["title": ""])
            let page = NotePage(json: ToolsJSON.object(data["page"]))
            guard !page.id.isEmpty else { return (nil, .failed("The page was not created.")) }
            pages.insert(page, at: 0)
            return (page.id, ToolsOutcome())
        } catch {
            return (nil, .failed(notesErrorText(error, "The page was not created.")))
        }
    }
}

public struct NotePageDraft: Equatable, Sendable {
    public var title: String
    public var emoji: String
    public var description: String
    public var accent: NotePageAccent
    public var layout: NotePageLayout
    public var showEmoji: Bool
    public var outline: [NotePageEntry]

    public init(_ page: NotePage) {
        title = page.title == "Untitled page" ? "" : page.title
        emoji = page.emoji
        description = page.description
        accent = page.accent
        layout = page.layout
        showEmoji = page.showEmoji
        outline = page.outline
    }
}

@MainActor
@Observable
public final class NotePageModel {
    public static let maxDescription = 2000
    public static let conflictMessage = "Someone else changed this page. Save again to keep your version."
    public static let deleteConfirmation = ToolsConfirmation(
        title: "Delete page?", message: "This removes the page for everyone it is shared with. The notes on it are not deleted.",
        confirm: "Delete", cancel: "Cancel", destructive: true)

    @ObservationIgnored private let call: ToolsApiCall
    @ObservationIgnored private let sleep: ToolsSleep
    @ObservationIgnored private let webLink: ToolsWebLink
    @ObservationIgnored private let seen: NoteViewsSeen
    public let pageId: String
    public private(set) var seenTick = 0

    public private(set) var page: NotePage?
    public private(set) var notes: [String: NotePageNote] = [:]
    public private(set) var bodies: [String: String] = [:]
    public private(set) var loading = true
    public private(set) var error = ""
    public private(set) var busy = false
    public var collapsed: Set<String> = []
    public var selectedNoteId: String?
    public var draft: NotePageDraft?
    public var noteQuery = ""
    public private(set) var myNotes: [JSONObject] = []

    public init(
        pageId: String, selectedNoteId: String? = nil, call: @escaping ToolsApiCall,
        sleep: @escaping ToolsSleep = toolsTaskSleep, webLink: @escaping ToolsWebLink = { _ in nil },
        seen: NoteViewsSeen = .shared
    ) {
        self.pageId = pageId
        self.selectedNoteId = selectedNoteId.flatMap { $0.isEmpty ? nil : $0 }
        self.call = call
        self.sleep = sleep
        self.webLink = webLink
        self.seen = seen
    }

    public convenience init(pageId: String, selectedNoteId: String? = nil, api: ApiService) {
        self.init(
            pageId: pageId, selectedNoteId: selectedNoteId,
            call: { method, suffix, body in try await api.notesApi(method, suffix, body) },
            webLink: { path in api.webLink(path) })
    }

    var path: String { "/pages/\(pageId)" }

    public func load() async {
        do {
            let data = try await call("GET", "\(path)?content=1", nil)
            let fresh = NotePage(json: ToolsJSON.object(data["page"]))
            if page == nil { collapsed = Set(fresh.outline.filter { $0.kind == .section && $0.collapsed }.map(\.id)) }
            page = fresh
            var map: [String: NotePageNote] = [:]
            for (key, value) in ToolsJSON.object(data["notes"]) {
                guard let object = value.objectValue else { continue }
                let note = NotePageNote(json: object)
                map[note.id.isEmpty ? key : note.id] = note
                if let markdown = note.markdown { bodies[note.id.isEmpty ? key : note.id] = markdown }
            }
            notes = map
            error = ""
        } catch {
            if page == nil { self.error = notesErrorText(error, "This page could not be opened.") }
        }
        loading = false
    }

    public var rows: [NotePageRow] {
        _ = seenTick
        guard let page else { return [] }
        return NotePageOutline.rows(page.outline, notes: notes, showEmoji: page.showEmoji, collapsed: collapsed, seen: seen)
    }

    public var allRows: [NotePageRow] {
        _ = seenTick
        guard let page else { return [] }
        return NotePageOutline.rows(page.outline, notes: notes, showEmoji: page.showEmoji, seen: seen)
    }

    public func toggle(_ sectionId: String) {
        if collapsed.contains(sectionId) { collapsed.remove(sectionId) } else { collapsed.insert(sectionId) }
    }

    public var canEdit: Bool { page?.canEdit ?? false }
    public var isOwner: Bool { page?.isOwner ?? false }
    public var accessPath: String { "/dashboard/notes?page=\(pageId)" }
    public var accessLink: URL? { webLink(accessPath) }

    public func note(_ id: String) -> NotePageNote? { notes[id] }

    public func body(_ id: String) -> String? { bodies[id] }

    public func ensureBody(_ id: String) async {
        guard notes[id] != nil, bodies[id] == nil else { return }
        guard let data = try? await call("GET", "/\(id)", nil) else { return }
        bodies[id] = ToolsJSON.text(ToolsJSON.object(data["note"])["markdown"], "")
    }

    public func ensureAllBodies() async {
        for row in allRows {
            if let id = row.noteId { await ensureBody(id) }
        }
    }

    public func opened(_ noteId: String) {
        guard let note = notes[noteId] else { return }
        seen.record(noteId, updatedAt: note.updatedAt)
        selectedNoteId = noteId
        seenTick += 1
    }

    public func startCustomize() {
        guard let page, page.canEdit else { return }
        draft = NotePageDraft(page)
        noteQuery = ""
    }

    public func loadMyNotes() async {
        guard myNotes.isEmpty, let data = try? await call("GET", "", nil) else { return }
        myNotes = ToolsJSON.maps(data["notes"])
    }

    public var noteChoices: [(id: String, title: String, emoji: String)] {
        let taken = Set((draft?.outline ?? []).filter { $0.kind == .note }.map(\.noteId))
        let query = noteQuery.dartTrimmed.lowercased()
        return myNotes.compactMap { (note: JSONObject) -> (id: String, title: String, emoji: String)? in
            let id = ToolsJSON.show(note["id"])
            let title = ToolsJSON.text(note["title"], "Untitled note")
            guard !taken.contains(id), query.isEmpty || title.lowercased().contains(query) else { return nil }
            return (id, title, ToolsJSON.text(note["emoji"], ""))
        }
    }

    public func addNote(_ id: String) {
        guard draft != nil, (draft?.outline.count ?? 0) < NotePageOutline.maxEntries else { return }
        draft?.outline.append(NotePageEntry(kind: .note, noteId: id))
    }

    public func addSection(_ label: String) -> Bool {
        let clean = label.dartTrimmed
        guard draft != nil, !clean.isEmpty, (draft?.outline.count ?? 0) < NotePageOutline.maxEntries else { return false }
        draft?.outline.append(NotePageEntry(kind: .section, label: String(clean.prefix(NoteEditorModel.titleMaxLength))))
        return true
    }

    public func addLink(label: String, url: String) -> ToolsOutcome {
        guard draft != nil else { return ToolsOutcome() }
        guard let clean = NotePageOutline.validLink(url) else { return .failed("Enter a link that starts with https://") }
        guard (draft?.outline.count ?? 0) < NotePageOutline.maxEntries else { return .failed(noteErrorMessages["too_many_entries"] ?? "") }
        draft?.outline.append(NotePageEntry(kind: .link, label: String(label.dartTrimmed.prefix(NoteEditorModel.titleMaxLength)), url: clean))
        return ToolsOutcome()
    }

    public func rename(_ id: String, _ label: String) {
        guard let index = draft?.outline.firstIndex(where: { $0.id == id }) else { return }
        draft?.outline[index].label = String(label.dartTrimmed.prefix(NoteEditorModel.titleMaxLength))
    }

    public func indent(_ id: String) { if let outline = draft?.outline { draft?.outline = NotePageOutline.indent(outline, id, by: 1) } }
    public func outdent(_ id: String) { if let outline = draft?.outline { draft?.outline = NotePageOutline.indent(outline, id, by: -1) } }
    public func moveUp(_ id: String) { if let outline = draft?.outline { draft?.outline = NotePageOutline.step(outline, id, by: -1) } }
    public func moveDown(_ id: String) { if let outline = draft?.outline { draft?.outline = NotePageOutline.step(outline, id, by: 1) } }

    public func move(from offsets: IndexSet, to destination: Int) {
        if let outline = draft?.outline { draft?.outline = NotePageOutline.move(outline, from: offsets, to: destination) }
    }

    public func remove(_ id: String) { draft?.outline.removeAll { $0.id == id } }

    public func entryTitle(_ entry: NotePageEntry) -> String {
        switch entry.kind {
        case .section: return entry.label
        case .link: return entry.label.isEmpty ? String(entry.url.dropFirst(8)) : entry.label
        case .note:
            if !entry.label.isEmpty { return entry.label }
            if let note = notes[entry.noteId] { return note.title }
            if let mine = myNotes.first(where: { ToolsJSON.show($0["id"]) == entry.noteId }) {
                return ToolsJSON.text(mine["title"], "Untitled note")
            }
            return NotePageOutline.lockedText
        }
    }

    public func saveCustomize() async -> ToolsOutcome {
        guard let page, let draft else { return ToolsOutcome() }
        guard let emoji = NoteEmoji.clean(draft.emoji) else { return .failed(NoteEmoji.invalidMessage) }
        if draft.description.count > Self.maxDescription {
            return .failed("A description can be at most \(Self.maxDescription) characters.")
        }
        if draft.outline.count > NotePageOutline.maxEntries { return .failed(noteErrorMessages["too_many_entries"] ?? "") }
        busy = true
        defer { busy = false }
        let body: JSONObject = [
            "baseVersion": .int(Int64(page.version)),
            "title": .string(String(draft.title.dartTrimmed.prefix(NoteEditorModel.titleMaxLength))),
            "emoji": .string(emoji),
            "description": .string(draft.description),
            "accent": .string(draft.accent.rawValue),
            "layout": .string(draft.layout.rawValue),
            "showEmoji": .bool(draft.showEmoji),
            "outline": .array(draft.outline.map { .object($0.json) }),
        ]
        do {
            _ = try await call("PATCH", path, body)
            self.draft = nil
            await load()
            return .done("Page saved")
        } catch let refusal as OrgApiException where refusal.code == "version_conflict" {
            await load()
            return .failed(Self.conflictMessage)
        } catch {
            return .failed(notesErrorText(error, "The page was not saved."))
        }
    }

    public func delete() async -> ToolsOutcome {
        guard isOwner else { return ToolsOutcome() }
        do {
            _ = try await call("DELETE", path, nil)
            return ToolsOutcome(close: true)
        } catch {
            return .failed(notesErrorText(error, "The page was not deleted."))
        }
    }

    public func shareModel() -> NoteShareModel? {
        guard let page, page.isOwner else { return nil }
        return NoteShareModel(noteId: pageId, initial: page.shares, call: call, sleep: sleep, webLink: webLink, page: true)
    }
}
