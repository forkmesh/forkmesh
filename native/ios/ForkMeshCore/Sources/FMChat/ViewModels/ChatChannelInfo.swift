import FMApi
import FMBase
import Foundation

public struct ChatChannelMember: Equatable, Hashable, Sendable, Identifiable {
    public var name: String
    public var account: String
    public var peerId: String
    public var online: Bool
    public var away: Bool
    public var isSelf: Bool

    public init(name: String, account: String = "", peerId: String = "", online: Bool = false, away: Bool = false, isSelf: Bool = false) {
        self.name = name
        self.account = account
        self.peerId = peerId
        self.online = online
        self.away = away
        self.isSelf = isSelf
    }

    public var key: String { (account.isEmpty ? name : account).lowercased() }
    public var id: String { key }
}

public struct ChatChannelFile: Equatable, Sendable, Identifiable {
    public enum Kind: String, Sendable { case file, link }

    public let kind: Kind
    public let messageId: String
    public let senderName: String
    public let sentAt: Date
    public let name: String
    public let url: String
    public let attachment: ChatAttachment?

    public var id: String { "\(messageId)|\(kind.rawValue)|\(url.isEmpty ? name : url)" }
}

public enum ChatBroadcast: String, CaseIterable, Sendable, Identifiable {
    case here, channel, everyone

    public var id: String { rawValue }
    public var tag: String { "@" + rawValue }

    public var summary: String {
        switch self {
        case .here: return "Notify everyone online in this channel"
        case .channel: return "Notify everyone in this channel"
        case .everyone: return "Notify everyone in this channel"
        }
    }

    public var reachesOfflinePeople: Bool { self != .here }
}

public struct ChatMentionPiece: Equatable, Sendable {
    public enum Kind: Equatable, Sendable { case text, person, you, broadcast }

    public let text: String
    public let kind: Kind

    public init(_ text: String, _ kind: Kind = .text) {
        self.text = text
        self.kind = kind
    }
}

public struct ChatBroadcastWarning: Equatable, Sendable, Identifiable {
    public let broadcast: ChatBroadcast
    public let people: Int
    public let place: String

    public var id: String { "\(broadcast.rawValue)|\(people)|\(place)" }

    public var title: String { "Notify \(people) \(people == 1 ? "person" : "people")?" }

    public var message: String {
        switch broadcast {
        case .here: return "\(broadcast.tag) alerts everyone who is online in \(place) right now."
        case .channel, .everyone: return "\(broadcast.tag) alerts every member of \(place), even people who are away."
        }
    }
}

public enum ChatSmartMentions {
    public static let warnAt = 6

    static func isNameCharacter(_ character: Character) -> Bool {
        character.isLetter || character.isNumber || character == "_" || character == "-" || character == "."
    }

    static func opensMention(_ chars: [Character], _ index: Int) -> Bool {
        guard chars[index] == "@" else { return false }
        if index == 0 { return true }
        let before = chars[index - 1]
        return before.isWhitespace || before == "(" || before == "[" || before == "\"" || before == "'"
    }

    public static func broadcasts(in text: String) -> [ChatBroadcast] {
        guard text.contains("@") else { return [] }
        var found: [ChatBroadcast] = []
        for piece in split(text, own: []) where piece.kind == .broadcast {
            if let kind = ChatBroadcast(rawValue: String(piece.text.dropFirst()).lowercased()), !found.contains(kind) {
                found.append(kind)
            }
        }
        return found
    }

    public static func strongest(in text: String) -> ChatBroadcast? {
        let found = broadcasts(in: text)
        if found.contains(.everyone) { return .everyone }
        if found.contains(.channel) { return .channel }
        return found.first
    }

    public static func suggestions(_ query: String) -> [ChatBroadcast] {
        let folded = query.lowercased()
        return ChatBroadcast.allCases.filter { folded.isEmpty || $0.rawValue.hasPrefix(folded) }
    }

    public static func split(_ text: String, own: [String]) -> [ChatMentionPiece] {
        let chars = Array(text)
        let mine = Set(own.map { $0.trimmingCharacters(in: .whitespaces).lowercased() }.filter { !$0.isEmpty })
        var pieces: [ChatMentionPiece] = []
        var buffer = ""
        var index = 0
        var inCode = false
        while index < chars.count {
            if chars[index] == "`" { inCode.toggle() }
            if !inCode && opensMention(chars, index) {
                var end = index + 1
                while end < chars.count && isNameCharacter(chars[end]) { end += 1 }
                var name = String(chars[(index + 1)..<end])
                while name.hasSuffix(".") || name.hasSuffix("-") {
                    name.removeLast()
                    end -= 1
                }
                if !name.isEmpty {
                    let lower = name.lowercased()
                    let kind: ChatMentionPiece.Kind =
                        ChatBroadcast(rawValue: lower) != nil ? .broadcast : (mine.contains(lower) ? .you : .person)
                    if !buffer.isEmpty {
                        pieces.append(ChatMentionPiece(buffer))
                        buffer = ""
                    }
                    pieces.append(ChatMentionPiece("@" + name, kind))
                    index = end
                    continue
                }
            }
            buffer.append(chars[index])
            index += 1
        }
        if !buffer.isEmpty { pieces.append(ChatMentionPiece(buffer)) }
        return pieces
    }

    public static func mentionsYou(_ text: String, own: [String]) -> Bool {
        guard text.contains("@") else { return false }
        return split(text, own: own).contains { $0.kind == .you || $0.kind == .broadcast }
    }
}

extension ChatLinkPreviewParsing {
    public static func links(in text: String) -> [String] {
        var out: [String] = []
        var inFence = false
        for line in text.split(separator: "\n", omittingEmptySubsequences: false) {
            if line.trimmingCharacters(in: .whitespaces).hasPrefix("```") {
                inFence.toggle()
                continue
            }
            if inFence { continue }
            for word in line.split(whereSeparator: { $0.isWhitespace || $0 == "<" || $0 == ">" }) {
                guard let start = word.range(of: "http", options: .caseInsensitive) else { continue }
                var candidate = String(word[start.lowerBound...])
                let lower = candidate.lowercased()
                guard lower.hasPrefix("https://") || lower.hasPrefix("http://") else { continue }
                if let pipe = candidate.firstIndex(of: "|") { candidate = String(candidate[..<pipe]) }
                while let last = candidate.last {
                    if ",.!?;:'\"*_~`".contains(last) {
                        candidate.removeLast()
                        continue
                    }
                    if last == ")" && candidate.filter({ $0 == ")" }).count > candidate.filter({ $0 == "(" }).count {
                        candidate.removeLast()
                        continue
                    }
                    if last == "]" && candidate.filter({ $0 == "]" }).count > candidate.filter({ $0 == "[" }).count {
                        candidate.removeLast()
                        continue
                    }
                    break
                }
                if candidate.count > 8 && !out.contains(candidate) { out.append(candidate) }
            }
        }
        return out
    }
}

extension ChatHub {
    public nonisolated static func firstUnreadId(_ messages: [ChatMessage], unread: Int) -> String? {
        guard unread > 0 else { return nil }
        var remaining = unread
        for message in messages.reversed() where !message.isSelf {
            remaining -= 1
            if remaining == 0 { return message.id }
        }
        return messages.first { !$0.isSelf }?.id
    }

    public func unreadCount(_ conversation: String) -> Int {
        if DirectMessageFrames.isDirect(conversation) { return direct?.record(for: conversation)?.unreadCount ?? 0 }
        if conversation == meshConversation { return 0 }
        return relay.unreadCountFor(conversation)
    }

    public func unreadMessages(_ conversation: String) -> [ChatMessage] {
        let count = unreadCount(conversation)
        guard count > 0 else { return [] }
        return Array(messages(in: conversation).filter { !$0.isSelf }.suffix(count))
    }

    public func hasUnreadMention(_ conversation: String) -> Bool {
        let names = ownNames
        return unreadMessages(conversation).contains { ChatSmartMentions.mentionsYou(extras.text(for: $0), own: names) }
    }

    public func notifyLevel(_ conversation: String) -> ChatNotifyLevel { workspace.notifyLevel(conversation) }

    public func setNotifyLevel(_ level: ChatNotifyLevel, for conversation: String) {
        let before = workspace.notifyLevel(conversation)
        workspace.setNotifyLevel(level, for: conversation)
        if before != level {
            let place = conversationLabel(conversation)
            switch level {
            case .all: post("\(place): notifications for every message.")
            case .mentions: post("\(place): notifications only for mentions.")
            case .nothing: post("\(place) is muted.")
            }
        }
    }

    public func isChannel(_ conversation: String) -> Bool {
        if conversation == meshConversation { return false }
        if conversation.hasPrefix("@") || DirectMessageFrames.isDirect(conversation) { return false }
        return conversation.hasPrefix("#") || OfficeFrames.isOffice(conversation)
    }

    public func channelMembers(_ conversation: String) -> [ChatChannelMember] {
        var byKey: [String: ChatChannelMember] = [:]
        var order: [String] = []
        let roster = conversation == meshConversation ? memberGroups : relay.rosterGroups()
        var onlineByName: [String: MemberGroup] = [:]
        for group in roster {
            onlineByName[group.name.lowercased()] = group
            let owner = group.members.first { !$0.owner.isEmpty }?.owner ?? ""
            if !owner.isEmpty { onlineByName[owner.lowercased()] = group }
        }
        func add(_ member: ChatChannelMember) {
            if member.name.isEmpty { return }
            let lower = member.name.lowercased()
            if lower == "forkbot" || member.peerId.hasPrefix("app:") || ChatVisitorPresence.isTransientVisitor(name: member.name) {
                return
            }
            let key = member.key
            if var known = byKey[key] {
                known.online = known.online || member.online
                known.away = known.online && (known.away || member.away)
                known.isSelf = known.isSelf || member.isSelf
                if known.peerId.isEmpty { known.peerId = member.peerId }
                if known.account.isEmpty { known.account = member.account }
                byKey[key] = known
                return
            }
            byKey[key] = member
            order.append(key)
        }
        func fromGroup(_ group: MemberGroup) -> ChatChannelMember {
            let owner = group.members.first { !$0.owner.isEmpty }?.owner ?? ""
            return ChatChannelMember(
                name: group.name, account: owner, peerId: group.id, online: group.online,
                away: group.online && group.userStatus.away, isSelf: group.isSelf)
        }
        if let room = office?.room(for: conversation), !room.members.isEmpty {
            let own = Set(ownNames.map { $0.lowercased() })
            for name in room.members {
                let group = onlineByName[name.lowercased()]
                add(ChatChannelMember(
                    name: name, account: name, peerId: group?.id ?? "", online: group?.online ?? false,
                    away: (group?.online ?? false) && (group?.userStatus.away ?? false), isSelf: own.contains(name.lowercased())))
            }
        } else if conversation == meshConversation {
            for group in roster { add(fromGroup(group)) }
        } else {
            for group in roster { add(fromGroup(group)) }
            for message in messages(in: conversation) + extras.allReplies.filter({ $0.conversation == conversation }) {
                if message.senderId == "forkbot" || message.senderId.hasPrefix("app:") { continue }
                let group = onlineByName[message.senderName.lowercased()]
                add(ChatChannelMember(
                    name: message.senderName, account: group.flatMap { $0.members.first { !$0.owner.isEmpty }?.owner } ?? "",
                    peerId: group?.id ?? message.senderId, online: group?.online ?? false, isSelf: message.isSelf))
            }
        }
        let list = order.compactMap { byKey[$0] }
        return list.sorted { a, b in
            if a.isSelf != b.isSelf { return a.isSelf }
            if a.online != b.online { return a.online }
            return a.name.lowercased() < b.name.lowercased()
        }
    }

    public func memberCount(_ conversation: String) -> Int { channelMembers(conversation).count }

    public func onlineMemberCount(_ conversation: String) -> Int { channelMembers(conversation).filter(\.online).count }

    public nonisolated static func membersLabel(_ count: Int) -> String { "\(count) \(count == 1 ? "member" : "members")" }

    public func channelFiles(_ conversation: String) -> [ChatChannelFile] {
        var out: [ChatChannelFile] = []
        for message in messages(in: conversation).reversed() where !extras.isHidden(message.id) && !extras.isRemoved(message.id) {
            if !message.fileName.isEmpty {
                out.append(ChatChannelFile(
                    kind: .file, messageId: message.id, senderName: message.senderName, sentAt: message.timestamp,
                    name: message.fileName, url: "", attachment: extras.attachment(for: message.id)))
            }
            for link in ChatLinkPreviewParsing.links(in: extras.text(for: message)) {
                out.append(ChatChannelFile(
                    kind: .link, messageId: message.id, senderName: message.senderName, sentAt: message.timestamp,
                    name: URL(string: link).flatMap { $0.host }.map(ChatLinkSites.bareHost) ?? link, url: link, attachment: nil))
            }
        }
        return out
    }

    public func savedItems(in conversation: String) -> [ChatSavedItem] {
        workspace.saved.filter { $0.conversation == conversation }
    }

    public func bookmarkCount(_ conversation: String) -> Int {
        workspace.bookmarks(for: conversation).count + savedItems(in: conversation).count
    }

    public func channelTabCount(_ conversation: String) -> Int {
        var tabs = 2
        if bookmarkCount(conversation) > 0 { tabs += 1 }
        if workspace.canvas(for: conversation) != nil { tabs += 1 }
        return tabs
    }

    public func channelDetail(_ conversation: String) -> String {
        "\(ChatHub.membersLabel(memberCount(conversation))) \u{2022} \(channelTabCount(conversation)) tabs"
    }

    @discardableResult
    public func addBookmark(_ title: String, url: String, to conversation: String) -> Bool {
        if workspace.addBookmark(conversation, title: title, url: url, now: clock.now) != nil {
            post("Bookmark added to \(conversationLabel(conversation)).")
            return true
        }
        post("That bookmark could not be added. Use an http or https address.")
        return false
    }

    public func saveCanvas(_ conversation: String, title: String, text: String) {
        workspace.setCanvas(conversation, title: title, text: text, now: clock.now)
    }

    public func broadcastWarning(_ text: String, in conversation: String) -> ChatBroadcastWarning? {
        guard isChannel(conversation) || conversation == meshConversation, let broadcast = ChatSmartMentions.strongest(in: text)
        else { return nil }
        let members = channelMembers(conversation).filter { !$0.isSelf }
        let people = broadcast.reachesOfflinePeople ? members.count : members.filter(\.online).count
        guard people >= ChatSmartMentions.warnAt else { return nil }
        return ChatBroadcastWarning(broadcast: broadcast, people: people, place: conversationLabel(conversation))
    }

    public func broadcastSuggestions(_ query: String, in conversation: String) -> [ChatBroadcast] {
        guard isChannel(conversation) || conversation == meshConversation else { return [] }
        return ChatSmartMentions.suggestions(query)
    }

    public func broadcastAudience(_ broadcast: ChatBroadcast, in conversation: String) -> Int {
        let members = channelMembers(conversation).filter { !$0.isSelf }
        return broadcast.reachesOfflinePeople ? members.count : members.filter(\.online).count
    }
}
