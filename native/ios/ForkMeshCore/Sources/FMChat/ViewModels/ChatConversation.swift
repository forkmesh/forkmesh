import FMApi
import FMBase
import Foundation

public enum ChatBubblePosition: Equatable, Sendable {
    case single, first, middle, last

    public var hasTail: Bool { self == .single || self == .last }
    public var startsRun: Bool { self == .single || self == .first }
}

public struct ChatBubble: Equatable, Sendable {
    public let row: ChatRow
    public let position: ChatBubblePosition
    public let showsSender: Bool
    public let receipt: ChatReceipt?
    public let jumbo: Bool
}

public enum ChatConversationItem: Equatable, Sendable, Identifiable {
    case time(id: String, date: Date)
    case event(ChatLookEvent)
    case message(ChatBubble)

    public var id: String {
        switch self {
        case .time(let id, _): return "time-\(id)"
        case .event(let event): return "look-\(event.id)"
        case .message(let bubble): return bubble.row.id
        }
    }
}

public enum ChatConversationLayout {
    public static let timeGap: TimeInterval = 15 * 60
    public static let maxJumboEmoji = 3
    public static let maxMediaBytes = 600 * 1024

    private enum Entry {
        case row(ChatRow)
        case event(ChatLookEvent)

        var timestamp: Date {
            switch self {
            case .row(let row): return row.message.timestamp
            case .event(let event): return event.timestamp
            }
        }
    }

    public static func items(
        rows: [ChatRow], events: [ChatLookEvent], group: Bool, timeZone: TimeZone = .current
    ) -> [ChatConversationItem] {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        var entries: [Entry] = rows.map { .row($0) }
        for event in events {
            var at = entries.count
            while at > 0 && entries[at - 1].timestamp > event.timestamp { at -= 1 }
            entries.insert(.event(event), at: at)
        }
        var headed: [(entry: Entry, header: Bool)] = []
        var last: Date?
        for entry in entries {
            let header =
                last.map {
                    entry.timestamp.timeIntervalSince($0) >= timeGap || !calendar.isDate(entry.timestamp, inSameDayAs: $0)
                } ?? true
            headed.append((entry, header))
            last = entry.timestamp
        }
        var out: [ChatConversationItem] = []
        for (index, item) in headed.enumerated() {
            if item.header {
                let key: String
                switch item.entry {
                case .row(let row): key = row.id
                case .event(let event): key = event.id
                }
                out.append(.time(id: key, date: item.entry.timestamp))
            }
            switch item.entry {
            case .event(let event):
                out.append(.event(event))
            case .row(let row):
                let joinsPrevious = index > 0 && !item.header && sameRun(headed[index - 1].entry, row)
                let joinsNext = index + 1 < headed.count && !headed[index + 1].header && sameRun(headed[index + 1].entry, row)
                let position: ChatBubblePosition
                switch (joinsPrevious, joinsNext) {
                case (false, false): position = .single
                case (false, true): position = .first
                case (true, true): position = .middle
                case (true, false): position = .last
                }
                let receipt = self.receipt(row)
                out.append(
                    .message(
                        ChatBubble(
                            row: row, position: position, showsSender: group && !row.message.isSelf && position.startsRun,
                            receipt: receipt, jumbo: row.message.fileName.isEmpty && isJumboEmoji(row.text))))
            }
        }
        return out
    }

    private static func sameRun(_ entry: Entry, _ row: ChatRow) -> Bool {
        guard case .row(let other) = entry else { return false }
        return other.message.isSelf == row.message.isSelf && other.message.senderId == row.message.senderId
            && other.message.senderName == row.message.senderName
    }

    public static func receipt(_ row: ChatRow) -> ChatReceipt? {
        guard row.message.isSelf else { return nil }
        if row.queued { return .sending }
        return row.receipt ?? .sent
    }

    public static func isJumboEmoji(_ text: String) -> Bool {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { return false }
        let characters = trimmed.filter { !$0.isWhitespace }
        if characters.isEmpty || characters.count > maxJumboEmoji { return false }
        return characters.allSatisfy(isEmojiCharacter)
    }

    static func isEmojiCharacter(_ character: Character) -> Bool {
        let scalars = character.unicodeScalars
        guard let first = scalars.first, !first.isASCII else { return false }
        if scalars.contains(where: { $0.properties.isEmojiPresentation }) { return true }
        return first.properties.isEmoji && scalars.contains { $0.value == 0xFE0F }
    }

    public static func timeLabel(_ date: Date, now: Date, timeZone: TimeZone = .current) -> String {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        let time = ChatHub.timeLabel(date, timeZone: timeZone)
        let day = calendar.startOfDay(for: date)
        let today = calendar.startOfDay(for: now)
        let days = calendar.dateComponents([.day], from: day, to: today).day ?? 0
        if days == 0 { return "Today \(time)" }
        if days == 1 { return "Yesterday \(time)" }
        let parts = calendar.dateComponents([.year, .month, .day, .weekday], from: date)
        if days > 1 && days < 7 {
            let weekdays = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"]
            return "\(weekdays[((parts.weekday ?? 1) - 1 + 7) % 7]) \(time)"
        }
        let months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
        let month = months[((parts.month ?? 1) - 1 + 12) % 12]
        if parts.year == calendar.component(.year, from: now) { return "\(month) \(parts.day ?? 1) at \(time)" }
        return "\(month) \(parts.day ?? 1), \(parts.year ?? 0) at \(time)"
    }
}

extension ChatHub {
    public func isConversation(_ conversation: String) -> Bool {
        if conversation.hasPrefix("@") || DirectMessageFrames.isDirect(conversation) { return true }
        if let room = office?.room(for: conversation) { return room.isPrivate && !room.serverVisible && !room.readOnly }
        return extras.isPrivate(conversation)
    }

    public func isGroupConversation(_ conversation: String) -> Bool {
        isConversation(conversation) && !conversation.hasPrefix("@") && !DirectMessageFrames.isDirect(conversation)
    }

    public var inConversation: Bool { isConversation(relay.currentConversation) }

    public var inGroupConversation: Bool { isGroupConversation(relay.currentConversation) }

    public var conversationLook: ChatLook { extras.look(for: relay.currentConversation) }

    public var conversationItems: [ChatConversationItem] {
        ChatConversationLayout.items(
            rows: rows, events: extras.lookEvents(in: relay.currentConversation), group: inGroupConversation)
    }

    public var conversationPeer: Member? {
        let conversation = relay.currentConversation
        guard conversation.hasPrefix("@") else { return nil }
        let id = String(conversation.dropFirst())
        return relay.roster().first { $0.id == id }
    }

    public var conversationName: String {
        let named = conversationLook.name
        if inGroupConversation && !named.isEmpty { return named }
        if let room = office?.room(for: relay.currentConversation) { return room.name }
        return roomName
    }

    public var conversationMembers: [String] {
        let conversation = relay.currentConversation
        if let room = office?.room(for: conversation) { return room.members }
        if conversation.hasPrefix("@") || DirectMessageFrames.isDirect(conversation) { return [roomName] }
        var names: [String] = []
        let me = relay.roster().first { $0.isSelf }?.name ?? ""
        if !me.isEmpty { names.append(me) }
        for message in relay.messages(conversation) where !message.senderName.isEmpty && !names.contains(message.senderName) {
            names.append(message.senderName)
        }
        return names
    }

    public var conversationSubtitle: String {
        if inGroupConversation {
            let count = conversationMembers.count
            return count == 1 ? "1 person" : "\(count) people"
        }
        guard let peer = conversationPeer else { return "Offline" }
        if peer.online && peer.userStatus.away { return "Away" }
        return peer.online ? "Online" : "Offline"
    }

    public var conversationPhotos: [ChatAttachment] {
        extras.attachments(in: rows.reversed().map(\.id)).filter { $0.kind == .image }
    }

    public var conversationTyping: [String] {
        _ = extras.revision
        return extras.typingNames(in: relay.currentConversation)
    }

    public var canChangeLook: Bool {
        let conversation = relay.currentConversation
        guard isConversation(conversation), guestComposeBlocked(conversation) == nil else { return false }
        if DirectMessageFrames.isDirect(conversation) { return direct?.record(for: conversation) != nil }
        if office?.room(for: conversation) != nil { return true }
        return relay.state == .connected
    }

    public var canSendMedia: Bool {
        let conversation = relay.currentConversation
        if inMesh { return false }
        if DirectMessageFrames.isDirect(conversation) { return direct?.record(for: conversation) != nil }
        if let room = office?.room(for: conversation) { return room.isPrivate && !room.readOnly }
        return !inOffice
    }

    @discardableResult
    public func setLook(_ change: ChatLookChange) async -> Bool {
        let conversation = relay.currentConversation
        guard canChangeLook else {
            post("This conversation cannot be changed right now.")
            return false
        }
        if let image = change.image, image.count > ChatLookFrames.maxImageBytes {
            post("That picture is too large to share.")
            return false
        }
        let sent: Bool
        if let direct, DirectMessageFrames.isDirect(conversation) {
            sent = await direct.sendLook(conversation, change: change)
            if !sent {
                post("The change could not be shared. Check the connection and try again.")
                return false
            }
            return true
        } else if let office, office.room(for: conversation) != nil {
            sent = await office.postFrame(conversation, type: ChatLookFrames.type, fields: ChatLookFrames.fields(change)) != nil
        } else {
            sent = extras.sendLook(conversation: conversation, change: change)
        }
        if !sent {
            post("The change could not be shared. Reconnecting...")
            if office?.room(for: conversation) == nil { Task { await relay.connect() } }
        }
        return sent
    }

    @discardableResult
    public func sendMedia(fileName: String, mime: String, data: Data) async -> Bool {
        let conversation = relay.currentConversation
        guard canSendMedia else {
            post("Photos and voice messages cannot be sent here.")
            return false
        }
        if data.count > ChatConversationLayout.maxMediaBytes {
            post("That is too large to send.")
            return false
        }
        if DirectMessageFrames.isDirect(conversation) {
            let name = fileName.trimmingCharacters(in: .whitespacesAndNewlines)
            return await sendDirectFile(fileName: name.isEmpty ? "photo.jpg" : name, mime: mime, data: data)
        }
        guard let office, office.room(for: conversation) != nil else {
            return sendAttachment(fileName: fileName, mime: mime, data: data)
        }
        let name = String(fileName.trimmingCharacters(in: .whitespacesAndNewlines).prefix(ChatLimits.maxFileNameChars))
        let fields: JSONObject = [
            "fileName": .string(name.isEmpty ? "photo.jpg" : name),
            "fileMime": .string(String(mime.prefix(ChatLimits.maxMimeChars))),
            "file": .string(data.base64EncodedString()),
        ]
        if await office.postFrame(conversation, type: "chat", fields: fields) != nil { return true }
        post("It could not be sent.")
        return false
    }

    @discardableResult
    public func sendDirectFile(fileName: String, mime: String, data: Data) async -> Bool {
        let conversation = relay.currentConversation
        guard let direct, DirectMessageFrames.isDirect(conversation), direct.record(for: conversation) != nil else {
            post("Files cannot be sent here.")
            return false
        }
        if data.count > DirectMessageFrames.maxFileBytes {
            post("Direct messages take files up to 1 MB.")
            return false
        }
        if await direct.sendFile(conversation, fileName: fileName, mime: mime, data: data) { return true }
        post("It could not be sent.")
        return false
    }

    public func clearConversationHistory() {
        for row in rows { extras.hideMessage(row.id) }
    }

    public func leaveGroupConversation() {
        let conversation = relay.currentConversation
        guard extras.isPrivate(conversation) else { return }
        extras.removeChannel(conversation)
        select(generalConversation)
    }
}
