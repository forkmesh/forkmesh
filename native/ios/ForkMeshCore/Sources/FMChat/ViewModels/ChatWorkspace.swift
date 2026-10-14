import FMAccount
import FMApi
import FMBase
import Foundation
import Observation

public struct ChatSavedItem: Codable, Equatable, Sendable, Identifiable {
    public let id: String
    public let conversation: String
    public let place: String
    public let senderName: String
    public let text: String
    public let fileName: String
    public let sentAt: Date
    public let savedAt: Date

    public init(
        id: String, conversation: String, place: String, senderName: String, text: String, fileName: String = "",
        sentAt: Date, savedAt: Date
    ) {
        self.id = id
        self.conversation = conversation
        self.place = place
        self.senderName = senderName
        self.text = text
        self.fileName = fileName
        self.sentAt = sentAt
        self.savedAt = savedAt
    }
}

public struct ChatBookmark: Codable, Equatable, Sendable, Identifiable {
    public let id: String
    public var title: String
    public var url: String
    public let addedAt: Date

    public init(id: String = UUID().uuidString, title: String, url: String, addedAt: Date) {
        self.id = id
        self.title = title
        self.url = url
        self.addedAt = addedAt
    }
}

public struct ChatCanvas: Codable, Equatable, Sendable {
    public var title: String
    public var text: String
    public var updatedAt: Date

    public init(title: String = "Canvas", text: String = "", updatedAt: Date) {
        self.title = title
        self.text = text
        self.updatedAt = updatedAt
    }
}

public enum ChatNotifyLevel: String, CaseIterable, Sendable, Identifiable {
    case all, mentions, nothing

    public var id: String { rawValue }

    public var label: String {
        switch self {
        case .all: return "All"
        case .mentions: return "Mentions"
        case .nothing: return "Nothing"
        }
    }

    public var detail: String {
        switch self {
        case .all: return "Every new message marks the channel unread."
        case .mentions: return "Only messages that mention you, @here, @channel or @everyone."
        case .nothing: return "Muted: no unread dot or badge."
        }
    }
}

@MainActor
@Observable
public final class ChatWorkspace {
    public static let maxBookmarks = 50
    public static let maxCanvasChars = 20_000
    public static let maxSaved = 200
    public static let maxRecents = 20
    public static let maxDraftChars = 4_000
    public static let maxBlocked = 500
    public static let maxRemovedConversations = 500

    @ObservationIgnored let store: KeyValueStore?
    @ObservationIgnored let scope: String
    public private(set) var saved: [ChatSavedItem] = []
    public private(set) var muted: Set<String> = []
    public private(set) var starred: [String] = []
    public private(set) var drafts: [String: String] = [:]
    public private(set) var recentPeople: [String] = []
    public private(set) var lastConversation: String = ""
    public private(set) var movedToEveryone = false
    public private(set) var mentionsOnly: Set<String> = []
    public private(set) var bookmarks: [String: [ChatBookmark]] = [:]
    public private(set) var canvases: [String: ChatCanvas] = [:]
    public private(set) var blocked: [String] = []
    public private(set) var removedConversations: [String: Date] = [:]

    public init(store: KeyValueStore? = nil, scope: String = "") {
        self.store = store
        self.scope = scope
        load()
    }

    func key(_ name: String) -> String {
        scope.isEmpty ? "chat/workspace/\(name)" : "chat/workspace/\(scope)/\(name)"
    }

    func load() {
        guard let store else { return }
        if let json = store.string(forKey: key("saved")), let data = json.data(using: .utf8),
            let decoded = try? ChatWorkspace.decoder.decode([ChatSavedItem].self, from: data)
        {
            saved = decoded
        }
        muted = Set(store.stringList(forKey: key("muted")) ?? [])
        starred = store.stringList(forKey: key("starred")) ?? []
        recentPeople = store.stringList(forKey: key("recentPeople")) ?? []
        lastConversation = store.string(forKey: key("lastConversation")) ?? ""
        movedToEveryone = store.bool(forKey: key("movedToEveryone")) ?? false
        if let json = store.string(forKey: key("drafts")), let data = json.data(using: .utf8),
            let decoded = try? JSONDecoder().decode([String: String].self, from: data)
        {
            drafts = decoded
        }
        mentionsOnly = Set(store.stringList(forKey: key("mentionsOnly")) ?? [])
        if let json = store.string(forKey: key("bookmarks")), let data = json.data(using: .utf8),
            let decoded = try? ChatWorkspace.decoder.decode([String: [ChatBookmark]].self, from: data)
        {
            bookmarks = decoded
        }
        if let json = store.string(forKey: key("canvases")), let data = json.data(using: .utf8),
            let decoded = try? ChatWorkspace.decoder.decode([String: ChatCanvas].self, from: data)
        {
            canvases = decoded
        }
        blocked = store.stringList(forKey: key("blocked")) ?? []
        if let json = store.string(forKey: key("removedConversations")), let data = json.data(using: .utf8),
            let decoded = try? ChatWorkspace.decoder.decode([String: Date].self, from: data)
        {
            removedConversations = decoded
        }
    }

    static var encoder: JSONEncoder {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .millisecondsSince1970
        encoder.outputFormatting = .sortedKeys
        return encoder
    }

    static var decoder: JSONDecoder {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .millisecondsSince1970
        return decoder
    }

    func persistSaved() {
        guard let store, let data = try? ChatWorkspace.encoder.encode(saved) else { return }
        store.set(String(decoding: data, as: UTF8.self), forKey: key("saved"))
    }

    func persistDrafts() {
        guard let store, let data = try? ChatWorkspace.encoder.encode(drafts) else { return }
        store.set(String(decoding: data, as: UTF8.self), forKey: key("drafts"))
    }

    public func isSaved(_ id: String) -> Bool { saved.contains { $0.id == id } }

    public func save(_ item: ChatSavedItem) {
        saved.removeAll { $0.id == item.id }
        saved.insert(item, at: 0)
        if saved.count > ChatWorkspace.maxSaved { saved.removeLast(saved.count - ChatWorkspace.maxSaved) }
        persistSaved()
    }

    public func unsave(_ id: String) {
        let before = saved.count
        saved.removeAll { $0.id == id }
        if saved.count != before { persistSaved() }
    }

    @discardableResult
    public func pruneSaved(_ expired: (ChatSavedItem) -> Bool) -> Bool {
        let before = saved.count
        saved.removeAll(where: expired)
        if saved.count == before { return false }
        persistSaved()
        return true
    }

    public func isMuted(_ conversation: String) -> Bool { muted.contains(conversation) }

    public func setMuted(_ conversation: String, _ on: Bool) {
        if on == muted.contains(conversation) || conversation.isEmpty { return }
        if on { muted.insert(conversation) } else { muted.remove(conversation) }
        store?.set(muted.sorted(), forKey: key("muted"))
    }

    public func isStarred(_ conversation: String) -> Bool { starred.contains(conversation) }

    public func setStarred(_ conversation: String, _ on: Bool) {
        if on == starred.contains(conversation) || conversation.isEmpty { return }
        if on { starred.append(conversation) } else { starred.removeAll { $0 == conversation } }
        store?.set(starred, forKey: key("starred"))
    }

    public func setLastConversation(_ conversation: String) {
        if conversation.isEmpty || conversation == lastConversation { return }
        lastConversation = conversation
        store?.set(conversation, forKey: key("lastConversation"))
    }

    public func markMovedToEveryone() {
        movedToEveryone = true
        store?.set(true, forKey: key("movedToEveryone"))
    }

    public func draft(for conversation: String) -> String { drafts[conversation] ?? "" }

    public func hasDraft(_ conversation: String) -> Bool { !(drafts[conversation] ?? "").dartTrimmed.isEmpty }

    public func setDraft(_ text: String, for conversation: String) {
        if conversation.isEmpty { return }
        let kept = String(text.prefix(ChatWorkspace.maxDraftChars))
        if kept.dartTrimmed.isEmpty {
            if drafts.removeValue(forKey: conversation) != nil { persistDrafts() }
            return
        }
        if drafts[conversation] == kept { return }
        drafts[conversation] = kept
        persistDrafts()
    }

    public func noteRecipients(_ recipients: [ChatRecipient]) {
        var list = recentPeople
        for recipient in recipients.reversed() where recipient.kind == .person {
            let name = recipient.handle
            list.removeAll { $0.lowercased() == name.lowercased() }
            list.insert(name, at: 0)
        }
        list = Array(list.prefix(ChatWorkspace.maxRecents))
        if list == recentPeople { return }
        recentPeople = list
        store?.set(list, forKey: key("recentPeople"))
    }

    public func forget(conversation: String) {
        setMuted(conversation, false)
        setStarred(conversation, false)
        setDraft("", for: conversation)
        setNotifyLevel(.all, for: conversation)
        if bookmarks.removeValue(forKey: conversation) != nil { persistBookmarks() }
        removeCanvas(conversation)
    }

    public static func blockKey(_ handle: String) -> String {
        handle.dartTrimmed.lowercased().trimmingCharacters(in: CharacterSet(charactersIn: "@"))
    }

    public func isBlocked(_ handle: String) -> Bool {
        let name = ChatWorkspace.blockKey(handle)
        return !name.isEmpty && blocked.contains(name)
    }

    public func setBlocked(_ handle: String, _ on: Bool) {
        let name = ChatWorkspace.blockKey(handle)
        if name.isEmpty || on == blocked.contains(name) { return }
        if on {
            blocked.append(name)
            if blocked.count > ChatWorkspace.maxBlocked { blocked.removeFirst(blocked.count - ChatWorkspace.maxBlocked) }
        } else {
            blocked.removeAll { $0 == name }
        }
        store?.set(blocked, forKey: key("blocked"))
    }

    public func removeConversation(_ conversation: String, at date: Date) {
        if conversation.isEmpty { return }
        removedConversations[conversation] = date
        if removedConversations.count > ChatWorkspace.maxRemovedConversations,
            let oldest = removedConversations.min(by: { $0.value < $1.value })?.key
        {
            removedConversations.removeValue(forKey: oldest)
        }
        if lastConversation == conversation {
            lastConversation = ""
            store?.set("", forKey: key("lastConversation"))
        }
        persistRemovedConversations()
    }

    public func restoreConversation(_ conversation: String) {
        if removedConversations.removeValue(forKey: conversation) != nil { persistRemovedConversations() }
    }

    public func isRemoved(_ conversation: String, lastActivity: Date?) -> Bool {
        guard let removedAt = removedConversations[conversation] else { return false }
        guard let lastActivity else { return true }
        return lastActivity <= removedAt
    }

    func persistRemovedConversations() {
        guard let store, let data = try? ChatWorkspace.encoder.encode(removedConversations) else { return }
        store.set(String(decoding: data, as: UTF8.self), forKey: key("removedConversations"))
    }

    public func notifyLevel(_ conversation: String) -> ChatNotifyLevel {
        if muted.contains(conversation) { return .nothing }
        if mentionsOnly.contains(conversation) { return .mentions }
        return .all
    }

    public func setNotifyLevel(_ level: ChatNotifyLevel, for conversation: String) {
        if conversation.isEmpty { return }
        setMuted(conversation, level == .nothing)
        let wanted = level == .mentions
        if wanted == mentionsOnly.contains(conversation) { return }
        if wanted { mentionsOnly.insert(conversation) } else { mentionsOnly.remove(conversation) }
        store?.set(mentionsOnly.sorted(), forKey: key("mentionsOnly"))
    }

    public func bookmarks(for conversation: String) -> [ChatBookmark] { bookmarks[conversation] ?? [] }

    @discardableResult
    public func addBookmark(_ conversation: String, title: String, url: String, now: Date) -> ChatBookmark? {
        let link = url.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !conversation.isEmpty, let parsed = URL(string: link), let scheme = parsed.scheme?.lowercased(),
            scheme == "https" || scheme == "http", parsed.host?.isEmpty == false
        else { return nil }
        var list = bookmarks[conversation] ?? []
        if list.count >= ChatWorkspace.maxBookmarks { return nil }
        let name = title.trimmingCharacters(in: .whitespacesAndNewlines)
        let bookmark = ChatBookmark(title: String((name.isEmpty ? parsed.host ?? link : name).prefix(120)), url: link, addedAt: now)
        list.append(bookmark)
        bookmarks[conversation] = list
        persistBookmarks()
        return bookmark
    }

    public func removeBookmark(_ conversation: String, id: String) {
        guard var list = bookmarks[conversation] else { return }
        let before = list.count
        list.removeAll { $0.id == id }
        if list.count == before { return }
        bookmarks[conversation] = list.isEmpty ? nil : list
        persistBookmarks()
    }

    func persistBookmarks() {
        guard let store, let data = try? ChatWorkspace.encoder.encode(bookmarks) else { return }
        store.set(String(decoding: data, as: UTF8.self), forKey: key("bookmarks"))
    }

    public func canvas(for conversation: String) -> ChatCanvas? { canvases[conversation] }

    public func setCanvas(_ conversation: String, title: String, text: String, now: Date) {
        if conversation.isEmpty { return }
        let name = title.trimmingCharacters(in: .whitespacesAndNewlines)
        let canvas = ChatCanvas(
            title: String((name.isEmpty ? "Canvas" : name).prefix(120)), text: String(text.prefix(ChatWorkspace.maxCanvasChars)),
            updatedAt: now)
        if canvases[conversation]?.title == canvas.title && canvases[conversation]?.text == canvas.text { return }
        canvases[conversation] = canvas
        persistCanvases()
    }

    public func removeCanvas(_ conversation: String) {
        if canvases.removeValue(forKey: conversation) != nil { persistCanvases() }
    }

    func persistCanvases() {
        guard let store, let data = try? ChatWorkspace.encoder.encode(canvases) else { return }
        store.set(String(decoding: data, as: UTF8.self), forKey: key("canvases"))
    }
}

public struct ChatRecipient: Equatable, Hashable, Sendable, Identifiable {
    public enum Kind: String, Sendable {
        case person
        case channel
    }

    public let kind: Kind
    public var name: String
    public var account: String
    public var conversation: String
    public var peerId: String
    public var online: Bool
    public var away: Bool
    public var detail: String

    public init(
        kind: Kind, name: String, account: String = "", conversation: String = "", peerId: String = "",
        online: Bool = false, away: Bool = false, detail: String = ""
    ) {
        self.kind = kind
        self.name = name
        self.account = account
        self.conversation = conversation
        self.peerId = peerId
        self.online = online
        self.away = away
        self.detail = detail
    }

    public static func person(_ name: String) -> ChatRecipient {
        let trimmed = name.trimmingCharacters(in: .whitespaces)
        let bare = trimmed.hasPrefix("@") ? String(trimmed.dropFirst()) : trimmed
        return ChatRecipient(kind: .person, name: bare, account: bare.lowercased())
    }

    public var handle: String { account.isEmpty ? name : account }

    public var key: String { kind == .channel ? "channel:\(conversation)" : "person:\(handle.lowercased())" }

    public var id: String { key }

    public var label: String { kind == .channel ? ChatDirectory.channelLabel(conversation, name: name) : name }
}

public struct ChatDirectoryChannel: Equatable, Sendable {
    public let conversation: String
    public let name: String
    public let detail: String
    public let isPrivate: Bool

    public init(conversation: String, name: String, detail: String = "", isPrivate: Bool = false) {
        self.conversation = conversation
        self.name = name
        self.detail = detail
        self.isPrivate = isPrivate
    }
}

public struct ChatDirectorySource: Sendable {
    public var own: [String] = []
    public var recents: [String] = []
    public var roster: [MemberGroup] = []
    public var directs: [DirectConversation] = []
    public var officeMembers: [String] = []
    public var senders: [String] = []
    public var searched: [String] = []
    public var channels: [ChatDirectoryChannel] = []

    public init() {}
}

public enum ChatDirectory {
    public static func channelLabel(_ conversation: String, name: String = "") -> String {
        if !name.isEmpty { return name.hasPrefix("#") ? name : "#\(name)" }
        if OfficeFrames.isOffice(conversation) { return "#" + conversation.dropFirst(OfficeFrames.prefix.count) }
        return conversation
    }

    static func excluded(_ name: String) -> Bool {
        let key = name.lowercased()
        return key.isEmpty || key == "forkbot" || key.hasPrefix("app:") || ChatVisitorPresence.isTransientVisitor(name: name)
    }

    public static func people(_ source: ChatDirectorySource) -> [ChatRecipient] {
        let own = Set(source.own.map { $0.lowercased() }.filter { !$0.isEmpty })
        var order: [String] = []
        var byKey: [String: ChatRecipient] = [:]
        var aliases: [String: String] = [:]
        func add(_ recipient: ChatRecipient) {
            let handle = recipient.handle.lowercased()
            let display = recipient.name.lowercased()
            if excluded(recipient.name) || own.contains(handle) || own.contains(display) { return }
            let key = aliases[handle] ?? aliases[display] ?? handle
            if var known = byKey[key] {
                if known.account.isEmpty && !recipient.account.isEmpty { known.account = recipient.account }
                if known.conversation.isEmpty { known.conversation = recipient.conversation }
                if known.peerId.isEmpty { known.peerId = recipient.peerId }
                if recipient.online && !known.online {
                    known.online = true
                    known.away = recipient.away
                }
                if known.detail.isEmpty { known.detail = recipient.detail }
                byKey[key] = known
            } else {
                order.append(key)
                byKey[key] = recipient
            }
            aliases[handle] = key
            aliases[display] = key
        }
        for group in source.roster where !group.isSelf {
            let account = group.members.first { !$0.owner.isEmpty }?.owner ?? ""
            add(
                ChatRecipient(
                    kind: .person, name: group.name, account: account.lowercased(), peerId: group.id, online: group.online,
                    away: group.online && group.userStatus.away))
        }
        for record in source.directs {
            add(ChatRecipient(kind: .person, name: record.otherUser, account: record.otherUser, conversation: record.conversation))
        }
        for name in source.recents { add(ChatRecipient.person(name)) }
        for name in source.officeMembers { add(ChatRecipient.person(name)) }
        for name in source.senders { add(ChatRecipient(kind: .person, name: name)) }
        for name in source.searched { add(ChatRecipient.person(name)) }
        let recent = source.recents.map { $0.lowercased() }
        var out = order.compactMap { byKey[$0] }.map { (recipient: ChatRecipient) -> ChatRecipient in
            var described = recipient
            if recipient.online {
                described.detail = recipient.away ? "Away" : "Online"
            } else if !recipient.conversation.isEmpty {
                described.detail = "Direct message"
            } else if recipient.detail.isEmpty, !recipient.account.isEmpty, recipient.account != recipient.name.lowercased() {
                described.detail = "@\(recipient.account)"
            }
            return described
        }
        func rank(_ recipient: ChatRecipient) -> Int {
            if let index = recent.firstIndex(of: recipient.handle.lowercased()) ?? recent.firstIndex(of: recipient.name.lowercased()) {
                return index
            }
            if recipient.online { return 100 }
            if !recipient.conversation.isEmpty { return 200 }
            return 300
        }
        let ranks = Dictionary(out.map { ($0.key, rank($0)) }, uniquingKeysWith: { first, _ in first })
        let positions = Dictionary(out.enumerated().map { ($1.key, $0) }, uniquingKeysWith: { first, _ in first })
        out.sort { (ranks[$0.key] ?? 0, positions[$0.key] ?? 0) < (ranks[$1.key] ?? 0, positions[$1.key] ?? 0) }
        return out
    }

    public static func score(_ text: String, query: String) -> Int? {
        let key = text.lowercased()
        if query.isEmpty { return 3 }
        if key.hasPrefix(query) { return 0 }
        let words = key.split(whereSeparator: { $0 == " " || $0 == "-" || $0 == "_" || $0 == "." || $0 == "/" })
        if words.contains(where: { $0.hasPrefix(query) }) { return 1 }
        if key.contains(query) { return 2 }
        return nil
    }

    public static func suggestions(
        _ source: ChatDirectorySource, query raw: String, selected: [ChatRecipient] = [], limit: Int = 12
    ) -> [ChatRecipient] {
        var query = raw.trimmingCharacters(in: .whitespaces).lowercased()
        var wantPeople = true
        var wantChannels = true
        if query.hasPrefix("@") {
            wantChannels = false
            query.removeFirst()
        } else if query.hasPrefix("#") {
            wantPeople = false
            query.removeFirst()
        }
        let taken = Set(selected.map(\.key))
        var scored: [(Int, Int, ChatRecipient)] = []
        if wantPeople {
            for (index, person) in people(source).enumerated() where !taken.contains(person.key) {
                let best = [score(person.name, query: query), score(person.account, query: query)].compactMap { $0 }.min()
                if let best { scored.append((best, index, person)) }
            }
        }
        if wantChannels && !(query.isEmpty && !raw.hasPrefix("#") && !selected.isEmpty) {
            var seen = Set<String>()
            for (index, channel) in source.channels.enumerated() where !seen.contains(channel.conversation) {
                seen.insert(channel.conversation)
                let recipient = ChatRecipient(
                    kind: .channel, name: channel.name, conversation: channel.conversation, detail: channel.detail)
                if taken.contains(recipient.key) { continue }
                let bare = channel.name.hasPrefix("#") ? String(channel.name.dropFirst()) : channel.name
                if let best = score(bare, query: query) { scored.append((best, 10_000 + index, recipient)) }
            }
        }
        scored.sort { ($0.0, $0.1) < ($1.0, $1.1) }
        return Array(scored.prefix(limit).map(\.2))
    }

    public static func exact(_ source: ChatDirectorySource, name raw: String) -> ChatRecipient? {
        let trimmed = raw.trimmingCharacters(in: .whitespaces)
        if trimmed.hasPrefix("#") {
            let key = trimmed.dropFirst().lowercased()
            guard let channel = source.channels.first(where: {
                ($0.name.hasPrefix("#") ? $0.name.dropFirst().lowercased() : $0.name.lowercased()) == key
            }) else { return nil }
            return ChatRecipient(kind: .channel, name: channel.name, conversation: channel.conversation, detail: channel.detail)
        }
        let key = (trimmed.hasPrefix("@") ? String(trimmed.dropFirst()) : trimmed).lowercased()
        if key.isEmpty { return nil }
        return people(source).first { $0.account == key || $0.name.lowercased() == key }
    }
}

public struct ChatSearchQuery: Equatable, Sendable {
    public var terms: [String] = []
    public var from = ""
    public var place = ""
    public var hasFile = false
    public var hasLink = false

    public var isEmpty: Bool { terms.isEmpty && from.isEmpty && place.isEmpty && !hasFile && !hasLink }
}

public struct ChatSearchHit: Equatable, Sendable, Identifiable {
    public let message: ChatMessage
    public let place: String

    public var id: String { "\(message.conversation)|\(message.id)" }
}

public enum ChatSearch {
    public static func parse(_ raw: String) -> ChatSearchQuery {
        var query = ChatSearchQuery()
        for word in raw.split(whereSeparator: { $0.isWhitespace }).map(String.init) {
            let lower = word.lowercased()
            if lower.hasPrefix("from:") {
                query.from = String(lower.dropFirst(5)).trimmingCharacters(in: CharacterSet(charactersIn: "@"))
            } else if lower.hasPrefix("in:") {
                query.place = String(lower.dropFirst(3)).trimmingCharacters(in: CharacterSet(charactersIn: "#@"))
            } else if lower == "has:file" || lower == "has:files" {
                query.hasFile = true
            } else if lower == "has:link" || lower == "has:links" {
                query.hasLink = true
            } else {
                query.terms.append(lower)
            }
        }
        return query
    }

    public static func matches(_ message: ChatMessage, text: String, place: String, query: ChatSearchQuery) -> Bool {
        if query.isEmpty { return false }
        if !query.from.isEmpty && !message.senderName.lowercased().contains(query.from) { return false }
        if !query.place.isEmpty {
            let bare = place.lowercased().trimmingCharacters(in: CharacterSet(charactersIn: "#@"))
            if !bare.contains(query.place) { return false }
        }
        if query.hasFile && message.fileName.isEmpty { return false }
        let lower = text.lowercased()
        if query.hasLink && !lower.contains("http://") && !lower.contains("https://") { return false }
        let haystack = lower + " " + message.fileName.lowercased()
        return query.terms.allSatisfy { haystack.contains($0) }
    }

    public static func mentions(_ text: String, names: [String]) -> Bool {
        let lower = Array(text.lowercased())
        let wanted = names.map { $0.lowercased() }.filter { !$0.isEmpty } + ["here", "channel", "everyone"]
        var index = 0
        while index < lower.count {
            if lower[index] == "@" && (index == 0 || lower[index - 1].isWhitespace || lower[index - 1] == "(") {
                for name in wanted {
                    let chars = Array(name)
                    let end = index + 1 + chars.count
                    if end <= lower.count && Array(lower[(index + 1)..<end]) == chars {
                        if end == lower.count || !(lower[end].isLetter || lower[end].isNumber || lower[end] == "_" || lower[end] == "-") {
                            return true
                        }
                    }
                }
            }
            index += 1
        }
        return false
    }

    public static func snippet(_ text: String, terms: [String], width: Int = 140) -> String {
        let flat = text.split(whereSeparator: { $0.isNewline }).joined(separator: " ")
        if flat.count <= width { return flat }
        let lower = flat.lowercased()
        guard let term = terms.first(where: { lower.contains($0) }), let range = lower.range(of: term) else {
            return String(flat.prefix(width - 1)) + "\u{2026}"
        }
        let at = lower.distance(from: lower.startIndex, to: range.lowerBound)
        let start = max(0, min(at - width / 3, flat.count - width))
        let slice = String(Array(flat)[start..<min(flat.count, start + width)])
        return (start > 0 ? "\u{2026}" : "") + slice + (start + width < flat.count ? "\u{2026}" : "")
    }
}

public enum ChatReminderOption: String, CaseIterable, Sendable, Identifiable {
    case twentyMinutes
    case oneHour
    case threeHours
    case tomorrow
    case nextWeek

    public var id: String { rawValue }

    public var label: String {
        switch self {
        case .twentyMinutes: return "In 20 minutes"
        case .oneHour: return "In 1 hour"
        case .threeHours: return "In 3 hours"
        case .tomorrow: return "Tomorrow at 9:00 AM"
        case .nextWeek: return "Next Monday at 9:00 AM"
        }
    }

    public func fireDate(from now: Date, calendar: Calendar = .current) -> Date {
        switch self {
        case .twentyMinutes: return now.addingTimeInterval(20 * 60)
        case .oneHour: return now.addingTimeInterval(60 * 60)
        case .threeHours: return now.addingTimeInterval(3 * 60 * 60)
        case .tomorrow:
            let start = calendar.startOfDay(for: now)
            let next = calendar.date(byAdding: .day, value: 1, to: start) ?? now
            return calendar.date(bySettingHour: 9, minute: 0, second: 0, of: next) ?? next
        case .nextWeek:
            let start = calendar.startOfDay(for: now)
            let weekday = calendar.component(.weekday, from: start)
            let ahead = (9 - weekday) % 7 == 0 ? 7 : (9 - weekday) % 7
            let monday = calendar.date(byAdding: .day, value: ahead, to: start) ?? now
            return calendar.date(bySettingHour: 9, minute: 0, second: 0, of: monday) ?? monday
        }
    }
}
