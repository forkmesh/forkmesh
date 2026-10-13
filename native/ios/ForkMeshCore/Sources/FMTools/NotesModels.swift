// The state and decisions of the Notes screen: the notes list,
// one note, its history sheet and its share sheet, over `/api/notes`
// (ApiService.notesApi). The Worker encrypts each note at rest; the phone reads and writes plain
// JSON over the signed-in session.

import FMApi
import FMBase
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
    (error as? OrgApiException)?.message ?? fallback
}

/// One row of the notes list.
public struct NoteRow: Equatable, Identifiable, Sendable {
    public let id: String
    public let title: String
    public let subtitle: String
    /// "public" or "sticky_note_2_outlined".
    public let icon: String
}

@MainActor
@Observable
public final class NotesListModel {
    public static let title = "Notes"
    public static let emptyTitle = "No notes yet"
    public static let emptyMessage =
        "Notes are private markdown documents that follow your account across web, desktop, and mobile."
    public static let unavailableTitle = "Notes are unavailable"

    @ObservationIgnored public let call: ToolsApiCall
    @ObservationIgnored private let clock: Clock

    public private(set) var notes: [JSONObject] = []
    public private(set) var loading = true
    public private(set) var error = ""
    public var query = ""

    public init(call: @escaping ToolsApiCall, clock: Clock = SystemClock()) {
        self.call = call
        self.clock = clock
    }

    public convenience init(api: ApiService) {
        self.init(call: { method, suffix, body in try await api.notesApi(method, suffix, body) })
    }

    public func load() async {
        loading = true
        error = ""
        do {
            let data = try await call("GET", "", nil)
            notes = ToolsJSON.maps(data["notes"])
            loading = false
        } catch {
            loading = false
            self.error = notesErrorText(error, "Notes are unavailable right now.")
        }
    }

    public var showsSpinner: Bool { loading && notes.isEmpty }
    public var showsError: Bool { !error.isEmpty && notes.isEmpty }

    public var visible: [JSONObject] {
        let q = query.dartTrimmed.lowercased()
        if q.isEmpty { return notes }
        return notes.filter { ToolsJSON.show($0["title"]).lowercased().contains(q) }
    }

    public var rows: [NoteRow] { visible.map(row) }

    public func row(_ note: JSONObject) -> NoteRow {
        let role = ToolsJSON.text(note["role"], "owner")
        var parts = ["Edited \(Format.relativeTime(ToolsJSON.epoch(note["updatedAt"]), nowMillis: clock.nowMillis))", noteReach(note)]
        if role != "owner" { parts.append(role == "editor" ? "Can edit" : "Read only") }
        return NoteRow(
            id: ToolsJSON.show(note["id"]),
            title: ToolsJSON.text(note["title"], "Untitled note"),
            subtitle: parts.joined(separator: " · "),
            icon: note["visibility"]?.stringValue == "public" ? "public" : "sticky_note_2_outlined")
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

    @ObservationIgnored public let call: ToolsApiCall
    @ObservationIgnored private let clock: Clock
    public let noteId: String?

    public private(set) var note: JSONObject?
    public private(set) var loading: Bool
    public private(set) var saving = false
    public private(set) var error = ""
    public var title = ""
    public var markdown = ""
    private var savedTitle = ""
    private var savedMarkdown = ""

    public init(noteId: String?, call: @escaping ToolsApiCall, clock: Clock = SystemClock()) {
        self.noteId = noteId
        self.call = call
        self.clock = clock
        loading = noteId != nil
    }

    public convenience init(noteId: String?, api: ApiService) {
        self.init(noteId: noteId, call: { method, suffix, body in try await api.notesApi(method, suffix, body) })
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

    public var screenTitle: String { existing ? (canEdit ? "Edit note" : "Note") : "New note" }
    public var showsSave: Bool { canEdit && !loading && error.isEmpty }
    public var saveEnabled: Bool { !(saving || (existing && !dirty)) }
    public var saveLabel: String { saving ? "Saving…" : "Save" }
    public var showsMenu: Bool { existing && note != nil }
    /// Leaving needs the discard confirmation.
    public var confirmsLeave: Bool { dirty && canEdit }

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

    /// The overflow menu: (value, label) in display order.
    public var menuItems: [(value: String, label: String)] {
        var items: [(value: String, label: String)] = [("history", "History")]
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
        apply(note)
        return .done("Version restored")
    }

    public func load() async {
        guard let noteId else { return }
        loading = true
        error = ""
        do {
            let data = try await call("GET", "/\(noteId)", nil)
            apply(ToolsJSON.object(data["note"]))
            loading = false
        } catch {
            loading = false
            self.error = notesErrorText(error, "This note could not be opened.")
        }
    }

    public func save() async -> ToolsOutcome {
        if saving || !canEdit { return ToolsOutcome() }
        saving = true
        do {
            let data: JSONObject
            if let id {
                data = try await call("PATCH", "/\(id)", [
                    "baseVersion": .int(Int64(version)),
                    "title": .string(title.dartTrimmed),
                    "markdown": .string(markdown),
                ])
            } else {
                data = try await call("POST", "", [
                    "title": .string(title.dartTrimmed),
                    "markdown": .string(markdown),
                    "visibility": "private",
                ])
            }
            apply(ToolsJSON.object(data["note"]))
            saving = false
            return .done("Note saved")
        } catch let error as OrgApiException {
            saving = false
            if error.code == "version_conflict" {
                return .failed("Someone else saved this note first. Copy your changes, then refresh to see theirs.")
            }
            return .failed(error.message)
        } catch {
            saving = false
            return .failed("The note could not be saved.")
        }
    }

    public func setVisibility(_ visibility: String) async -> ToolsOutcome {
        guard let id else { return ToolsOutcome() }
        do {
            let data = try await call("PATCH", "/\(id)", ["baseVersion": .int(Int64(version)), "visibility": .string(visibility)])
            applyMeta(data)
            return .done(visibility == "public" ? "Note published" : "Note is private again")
        } catch {
            return .failed(notesErrorText(error, "Visibility was not changed."))
        }
    }

    /// After the person confirmed `deleteConfirmation`.
    public func delete() async -> ToolsOutcome {
        guard let id else { return ToolsOutcome() }
        do {
            _ = try await call("DELETE", "/\(id)", nil)
            savedTitle = title
            savedMarkdown = markdown
            return ToolsOutcome(close: true)
        } catch {
            return .failed(notesErrorText(error, "The note was not deleted."))
        }
    }

    /// After the discard confirmation: the text counts as saved so leaving is not asked again.
    public func discard() {
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
        return NoteShareModel(noteId: id, initial: shares, call: call)
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

/// Owner-only sharing: people or organizations, as viewers or editors.
@MainActor
@Observable
public final class NoteShareModel {
    public static let title = "Share"
    public static let subtitle = "Viewers can read; editors can change the text."
    public static let notShared = "Not shared with anyone yet."

    @ObservationIgnored private let call: ToolsApiCall
    public let noteId: String
    public private(set) var shares: [JSONObject]
    public var name = ""
    /// "user" or "organization".
    public var type = "user"
    /// "viewer" or "editor".
    public var role = "viewer"
    public private(set) var busy = false

    public init(noteId: String, initial: [JSONObject], call: @escaping ToolsApiCall) {
        self.noteId = noteId
        shares = initial
        self.call = call
    }

    public func shareTitle(_ share: JSONObject) -> String { ToolsJSON.show(share["name"]) }

    public func shareSubtitle(_ share: JSONObject) -> String {
        "\(share["type"]?.stringValue == "organization" ? "Organization" : "User") · \(share["role"]?.stringValue == "editor" ? "Editor" : "Viewer")"
    }

    public func shareIcon(_ share: JSONObject) -> String {
        share["type"]?.stringValue == "organization" ? "groups_outlined" : "person_outline"
    }

    /// Returns the failure to show, if any.
    private func send(_ method: String, _ body: JSONObject) async -> String? {
        busy = true
        do {
            let data = try await call(method, "/\(noteId)/shares", body)
            shares = ToolsJSON.maps(data["shares"])
            busy = false
            if method == "POST" { name = "" }
            return nil
        } catch {
            busy = false
            let code = (error as? OrgApiException)?.code ?? ""
            return code == "principal_not_found"
                ? "No \(type == "user" ? "user" : "organization") has that name."
                : notesErrorText(error, "Sharing was not changed.")
        }
    }

    public func add() async -> String? {
        let cleaned = ToolsText.dropLeadingAts(name.dartTrimmed)
        if cleaned.isEmpty { return nil }
        return await send("POST", ["type": .string(type), "name": .string(cleaned.lowercased()), "role": .string(role)])
    }

    public func stop(_ share: JSONObject) async -> String? {
        await send("DELETE", ["type": share["type"] ?? .null, "name": share["name"] ?? .null])
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
