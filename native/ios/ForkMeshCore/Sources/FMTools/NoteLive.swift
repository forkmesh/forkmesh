import FMApi
import FMBase
import Foundation
import Observation

public enum NoteLiveTab: String, CaseIterable, Identifiable, Sendable {
    case history, activity, chat

    public var id: String { rawValue }

    public var label: String {
        switch self {
        case .history: return "History"
        case .activity: return "Activity"
        case .chat: return "Chat"
        }
    }
}

public struct NoteActivityEntry: Equatable, Sendable {
    public let kind: String
    public let actor: String
    public let at: Int64
    public let count: Int
    public let version: Int
    public let fromVersion: Int
    public let readers: Int
    public let detail: JSONObject

    public init(
        kind: String, actor: String = "", at: Int64 = 0, count: Int = 1, version: Int = 0, fromVersion: Int = 0,
        readers: Int = 0, detail: JSONObject = [:]
    ) {
        self.kind = kind
        self.actor = actor
        self.at = at
        self.count = count
        self.version = version
        self.fromVersion = fromVersion
        self.readers = readers
        self.detail = detail
    }

    public init?(json: JSONObject) {
        let kind = ToolsJSON.text(json["kind"], "").dartTrimmed
        guard !kind.isEmpty else { return nil }
        let version = ToolsJSON.int(json["version"])
        self.init(
            kind: kind, actor: ToolsJSON.text(json["actor"], "").dartTrimmed, at: ToolsJSON.epoch(json["at"]),
            count: max(1, ToolsJSON.int(json["count"])), version: version, fromVersion: version,
            detail: ToolsJSON.object(json["detail"]))
    }

    public var icon: String { NoteActivity.icon(kind) }
    public var text: String { NoteActivity.text(self) }

    public func actorLabel(me: String) -> String {
        if kind == "web" { return "" }
        if !actor.isEmpty && actor.lowercased() == me.dartTrimmed.lowercased() { return "You" }
        return actor.isEmpty ? "Someone" : actor
    }

    public func line(me: String) -> String {
        [actorLabel(me: me), text].filter { !$0.isEmpty }.joined(separator: " ")
    }

    func with(count: Int, fromVersion: Int) -> NoteActivityEntry {
        NoteActivityEntry(
            kind: kind, actor: actor, at: at, count: count, version: version, fromVersion: fromVersion, readers: readers,
            detail: detail)
    }
}

public enum NoteActivity {
    public static let editGroupMillis: Int64 = 30 * 60 * 1000
    public static let reloadDelayMillis = 300
    public static let emptyMessage = "Nothing yet."
    public static let unavailable = "Activity is unavailable right now."

    static let icons: [String: String] = [
        "view": "visibility", "web": "public", "edit": "edit", "create": "note_add_outlined",
        "restore": "restore", "share": "person_add", "role": "manage_accounts", "unshare": "person_remove",
        "publish": "public", "unpublish": "visibility_off_outlined", "link": "link", "unlink": "link_off",
        "export": "download_outlined", "copy-link": "content_copy", "task": "task_alt", "issue": "adjust",
        "comment": "chat_bubble_outline", "resolve": "check_circle", "reopen": "undo",
    ]

    public static func icon(_ kind: String) -> String { icons[kind] ?? "history" }

    public static func count(_ value: Int, _ one: String, _ many: String) -> String { "\(value) \(value == 1 ? one : many)" }

    public static func summary(_ data: JSONObject) -> String {
        let counts = ToolsJSON.object(data["counts"])
        return [
            count(ToolsJSON.int(counts["views"]), "view", "views"),
            count(ToolsJSON.int(counts["versions"]), "version", "versions"),
            count(ToolsJSON.int(counts["actions"]), "action", "actions"),
        ].joined(separator: " · ")
    }

    public static func entries(_ data: JSONObject) -> [NoteActivityEntry] {
        let counts = ToolsJSON.object(data["counts"])
        var events = ToolsJSON.maps(data["events"]).compactMap(NoteActivityEntry.init(json:))
        let webViews = ToolsJSON.int(counts["webViews"])
        if webViews > 0 {
            events.append(NoteActivityEntry(
                kind: "web", at: ToolsJSON.epoch(counts["webLastViewedAt"]), count: webViews,
                readers: ToolsJSON.int(counts["webReaders"])))
        }
        let sorted = events.enumerated().sorted { (a, b) in
            a.element.at != b.element.at ? a.element.at > b.element.at : a.offset < b.offset
        }.map(\.element)
        var grouped: [NoteActivityEntry] = []
        var oldestAt: Int64 = 0
        for entry in sorted {
            if entry.kind == "edit", let last = grouped.last, last.kind == "edit", last.actor == entry.actor,
               oldestAt - entry.at <= editGroupMillis {
                grouped[grouped.count - 1] = last.with(count: last.count + 1, fromVersion: entry.version)
            } else {
                grouped.append(entry.kind == "edit" ? entry.with(count: 1, fromVersion: entry.version) : entry)
            }
            oldestAt = entry.at
        }
        return grouped
    }

    static func ref(_ detail: JSONObject) -> String {
        let owner = ToolsJSON.text(detail["owner"], "")
        let repo = ToolsJSON.text(detail["repo"], "")
        return "\(owner.isEmpty ? repo : "\(owner)/\(repo)")#\(ToolsJSON.int(detail["number"]))"
    }

    public static func text(_ entry: NoteActivityEntry) -> String {
        let detail = entry.detail
        let named = ToolsJSON.text(detail["name"], "")
        let name = named.isEmpty ? "someone" : named
        let role = detail["role"]?.stringValue == "editor" ? "can edit" : "can view"
        let count = entry.count
        switch entry.kind {
        case "view": return count > 1 ? "viewed it \(count) times" : "viewed it"
        case "web":
            return "\(Self.count(count, "read", "reads")) of the published page" + (entry.readers > 1 ? " by \(entry.readers) readers" : "")
        case "edit":
            return count > 1 ? "edited it · v\(entry.fromVersion)–v\(entry.version), \(count) saves" : "edited it · v\(entry.version)"
        case "create": return "created the note"
        case "restore":
            let target = ToolsJSON.int(detail["as"])
            return "restored version \(ToolsJSON.int(detail["version"]))" + (target > 0 ? " as v\(target)" : "")
        case "share": return "shared it with \(name) · \(role)"
        case "role": return "changed \(name) to \(role)"
        case "unshare": return "stopped sharing with \(name)"
        case "publish": return "published it to the web"
        case "unpublish": return "unpublished it"
        case "link": return "linked \(ref(detail))"
        case "unlink": return "unlinked \(ref(detail))"
        case "export": return "downloaded the Markdown"
        case "copy-link": return "copied the link"
        case "task": return "turned it into a task"
        case "issue": return ToolsJSON.text(detail["repo"], "").isEmpty ? "filed an issue from it" : "filed issue \(ref(detail)) from it"
        case "comment": return detail["reply"]?.boolValue == true ? "replied to a comment" : "commented"
        case "resolve": return "resolved a comment"
        case "reopen": return "reopened a comment"
        default: return entry.kind
        }
    }
}

@MainActor
@Observable
public final class NoteActivityModel {
    public static let title = "Activity"

    @ObservationIgnored private let call: ToolsApiCall
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored private let sleep: ToolsSleep
    @ObservationIgnored var reloadTask: Task<Void, Never>?
    @ObservationIgnored private var reloadGeneration = 0
    public let noteId: String
    public let me: String

    public private(set) var entries: [NoteActivityEntry] = []
    public private(set) var summary = ""
    public private(set) var loaded = false
    public private(set) var error = ""

    public init(noteId: String, me: String = "", call: @escaping ToolsApiCall, clock: Clock = SystemClock(), sleep: @escaping ToolsSleep = toolsTaskSleep) {
        self.noteId = noteId
        self.me = me
        self.call = call
        self.clock = clock
        self.sleep = sleep
    }

    public func load() async {
        do {
            let data = try await call("GET", "/\(noteId)/activity", nil)
            entries = NoteActivity.entries(data)
            summary = NoteActivity.summary(data)
            loaded = true
            error = ""
        } catch {
            if !loaded { self.error = notesErrorText(error, NoteActivity.unavailable) }
        }
    }

    public func reloadSoon() {
        reloadGeneration += 1
        let generation = reloadGeneration
        let sleep = self.sleep
        reloadTask = Task { [weak self] in
            do { try await sleep(NoteActivity.reloadDelayMillis) } catch { return }
            guard let self, self.reloadGeneration == generation else { return }
            await self.load()
        }
    }

    public func line(_ entry: NoteActivityEntry) -> String { entry.line(me: me) }

    public func when(_ entry: NoteActivityEntry) -> String {
        entry.at > 0 ? Format.relativeTime(entry.at, nowMillis: clock.nowMillis) : ""
    }
}

public struct NoteChatMessage: Equatable, Identifiable, Sendable {
    public let id: String
    public let author: String
    public let body: String
    public let createdAt: Int64

    public init(id: String, author: String = "", body: String = "", createdAt: Int64 = 0) {
        self.id = id
        self.author = author
        self.body = body
        self.createdAt = createdAt
    }

    public init?(json: JSONObject) {
        let id = ToolsJSON.text(json["id"], "").dartTrimmed
        guard !id.isEmpty else { return nil }
        self.init(
            id: id, author: ToolsJSON.text(json["author"], "").dartTrimmed, body: ToolsJSON.text(json["body"], ""),
            createdAt: ToolsJSON.epoch(json["createdAt"]))
    }

    public func isBy(_ me: String) -> Bool {
        let name = me.dartTrimmed.lowercased()
        return !name.isEmpty && author.lowercased() == name
    }

    public func authorLabel(me: String) -> String {
        if isBy(me) { return "You" }
        return author.isEmpty ? "Someone" : "@\(author)"
    }
}

public enum NoteChat {
    public static let maxBody = 2000
    public static let reloadDelayMillis = 300
    public static let emptyMessage = "No messages yet. Say hello to everyone in this note."
    public static let unavailable = "Chat is unavailable right now."
    public static let hint = "Message everyone in this note"

    public static func messages(_ data: JSONObject) -> [NoteChatMessage] {
        ToolsJSON.maps(data["messages"]).compactMap(NoteChatMessage.init(json:)).enumerated().sorted { (a, b) in
            a.element.createdAt != b.element.createdAt ? a.element.createdAt < b.element.createdAt : a.offset < b.offset
        }.map(\.element)
    }

    public static func unread(_ messages: [NoteChatMessage], me: String, seenAt: Int64) -> Int {
        messages.filter { !$0.isBy(me) && $0.createdAt > seenAt }.count
    }

    public static func badge(_ unread: Int) -> String {
        if unread <= 0 { return "" }
        return unread > 99 ? "99+" : "\(unread)"
    }

    public static func buttonLabel(_ unread: Int) -> String {
        if unread <= 0 { return "Chat" }
        return unread == 1 ? "Chat, 1 unread message" : "Chat, \(unread) unread messages"
    }

    static func cleaned(_ text: String) -> (body: String?, failure: String?) {
        let body = text.dartTrimmed
        if body.isEmpty { return (nil, nil) }
        if body.count > maxBody { return (nil, "Write a message of up to \(maxBody) characters.") }
        return (body, nil)
    }

    public static func deleteConfirmation(_ message: NoteChatMessage, me: String) -> ToolsConfirmation {
        let words = message.isBy(me) || message.author.isEmpty
            ? "This message will be removed for everyone."
            : "\(message.author)'s message will be removed for everyone."
        return ToolsConfirmation(title: "Delete message?", message: words, confirm: "Delete", cancel: "Cancel", destructive: true)
    }
}

@MainActor
public final class NoteChatSeen {
    public static let shared = NoteChatSeen(defaults: .standard)
    static let key = "fm.notes.chatSeen"
    static let maxNotes = 200

    private let defaults: UserDefaults?
    private var seen: [String: Int64]
    private var order: [String]

    public init(defaults: UserDefaults? = nil) {
        self.defaults = defaults
        let stored = defaults?.dictionary(forKey: Self.key) ?? [:]
        var loaded: [String: Int64] = [:]
        for (id, value) in stored {
            if let number = Self.millis(value) { loaded[id] = number }
        }
        seen = loaded
        order = loaded.sorted { $0.value < $1.value }.map(\.key)
    }

    static func millis(_ value: Any) -> Int64? {
        switch value {
        case let number as Int64: return number
        case let number as Int: return Int64(number)
        case let number as Double: return Int64(number)
        case let text as String: return Int64(text)
        case let number as NSNumber: return number.int64Value
        default: return nil
        }
    }

    public func seenAt(_ noteId: String) -> Int64 { seen[noteId] ?? 0 }

    public func record(_ noteId: String, at: Int64) {
        guard !noteId.isEmpty, at > seenAt(noteId) else { return }
        seen[noteId] = at
        order.removeAll { $0 == noteId }
        order.append(noteId)
        while order.count > Self.maxNotes {
            seen[order.removeFirst()] = nil
        }
        defaults?.set(seen.mapValues { String($0) }, forKey: Self.key)
    }
}

@MainActor
@Observable
public final class NoteChatModel {
    public static let title = "Chat"

    @ObservationIgnored private let call: ToolsApiCall
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored private let sleep: ToolsSleep
    @ObservationIgnored private let seen: NoteChatSeen
    @ObservationIgnored var reloadTask: Task<Void, Never>?
    @ObservationIgnored private var reloadGeneration = 0
    public let noteId: String
    public let me: String

    public private(set) var messages: [NoteChatMessage] = []
    public private(set) var canModerate = false
    public private(set) var loaded = false
    public private(set) var error = ""
    public private(set) var busy = false
    public private(set) var seenAt: Int64
    public var draft = ""
    public private(set) var visible = false

    public init(
        noteId: String, me: String = "", call: @escaping ToolsApiCall, clock: Clock = SystemClock(),
        sleep: @escaping ToolsSleep = toolsTaskSleep, seen: NoteChatSeen = .shared
    ) {
        self.noteId = noteId
        self.me = me
        self.call = call
        self.clock = clock
        self.sleep = sleep
        self.seen = seen
        seenAt = seen.seenAt(noteId)
    }

    var path: String { "/\(noteId)/chat" }

    public var latestAt: Int64 { messages.map(\.createdAt).max() ?? 0 }
    public var unread: Int { visible ? 0 : NoteChat.unread(messages, me: me, seenAt: seenAt) }
    public var buttonLabel: String { NoteChat.buttonLabel(unread) }
    public var badge: String { NoteChat.badge(unread) }
    public var canSend: Bool { !busy && loaded && !draft.dartTrimmed.isEmpty }

    public func isMine(_ message: NoteChatMessage) -> Bool { message.isBy(me) }
    public func canDelete(_ message: NoteChatMessage) -> Bool { canModerate || message.isBy(me) }
    public func authorLabel(_ message: NoteChatMessage) -> String { message.authorLabel(me: me) }
    public func when(_ message: NoteChatMessage) -> String { Format.relativeTime(message.createdAt, nowMillis: clock.nowMillis) }
    public func deleteConfirmation(_ message: NoteChatMessage) -> ToolsConfirmation { NoteChat.deleteConfirmation(message, me: me) }

    func apply(_ data: JSONObject) {
        messages = NoteChat.messages(data)
        canModerate = data["canModerate"]?.boolValue ?? false
        loaded = true
        if visible { markSeen() }
    }

    public func setVisible(_ shown: Bool) {
        visible = shown
        if shown { markSeen() }
    }

    public func markSeen() {
        let latest = latestAt
        guard latest > seenAt else { return }
        seenAt = latest
        seen.record(noteId, at: latest)
    }

    public func load() async {
        do {
            apply(try await call("GET", path, nil))
            error = ""
        } catch {
            if !loaded { self.error = notesErrorText(error, NoteChat.unavailable) }
        }
    }

    public func reloadSoon() {
        reloadGeneration += 1
        let generation = reloadGeneration
        let sleep = self.sleep
        reloadTask = Task { [weak self] in
            do { try await sleep(NoteChat.reloadDelayMillis) } catch { return }
            guard let self, self.reloadGeneration == generation else { return }
            await self.load()
        }
    }

    private func send(_ method: String, _ suffix: String, _ body: JSONObject?, fallback: String) async -> ToolsOutcome? {
        busy = true
        defer { busy = false }
        do {
            apply(try await call(method, path + suffix, body))
            return nil
        } catch {
            return .failed(notesErrorText(error, fallback))
        }
    }

    public func post() async -> ToolsOutcome {
        let checked = NoteChat.cleaned(draft)
        if let failure = checked.failure { return .failed(failure) }
        guard let text = checked.body else { return ToolsOutcome() }
        if let failure = await send("POST", "", ["body": .string(text)], fallback: "The message was not sent.") { return failure }
        draft = ""
        return ToolsOutcome()
    }

    public func delete(_ message: NoteChatMessage) async -> ToolsOutcome {
        if let failure = await send("DELETE", "/\(message.id)", nil, fallback: "The message was not deleted.") { return failure }
        return .done("Message deleted")
    }
}

public struct NoteVersionSnapshot: Equatable, Sendable {
    public let version: Int
    public let createdAt: Int64
    public let author: String
    public let title: String
    public let markdown: String
    public let emoji: String

    public init(json: JSONObject) {
        version = ToolsJSON.int(json["version"])
        createdAt = ToolsJSON.epoch(json["createdAt"])
        author = ToolsJSON.text(json["author"], "").dartTrimmed
        title = ToolsJSON.text(json["title"], "")
        markdown = ToolsJSON.text(json["markdown"], "")
        emoji = ToolsJSON.text(json["emoji"], "")
    }

    public var heading: String {
        let name = title.dartTrimmed.isEmpty ? "Untitled note" : title
        return emoji.isEmpty ? name : "\(emoji) \(name)"
    }
}
