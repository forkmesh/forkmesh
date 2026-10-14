import FMApi
import FMBase
import Foundation

public struct ChatListPreview: Equatable, Sendable {
    public let text: String
    public let at: Date

    public init(text: String, at: Date) {
        self.text = text
        self.at = at
    }
}

public enum ChatListPreviews {
    public static let maxChars = 160
    public static let filePrefix = "\u{1F4CE} "

    public static func plain(_ raw: String) -> String {
        var lines: [String] = []
        for line in raw.replacingOccurrences(of: "```", with: "\n").components(separatedBy: .newlines) {
            var body = Substring(line)
            while body.hasPrefix(">") { body = body.dropFirst().drop(while: { $0 == " " }) }
            lines.append(ChatRichText.spans(String(body)).map(\.text).joined())
        }
        let words = lines.joined(separator: " ").split(whereSeparator: { $0.isWhitespace })
        let joined = words.joined(separator: " ")
        if joined.count <= maxChars { return joined }
        return String(joined.prefix(maxChars - 1)).trimmingCharacters(in: .whitespaces) + "\u{2026}"
    }

    public static func body(_ message: ChatMessage, text: String) -> String {
        let shown = plain(text)
        if !shown.isEmpty { return shown }
        let file = plain(message.fileName)
        return file.isEmpty ? "" : filePrefix + file
    }

    public static func line(_ message: ChatMessage, text: String, direct: Bool) -> String {
        let shown = body(message, text: text)
        if shown.isEmpty { return "" }
        if message.isSelf { return "You: " + shown }
        let sender = plain(message.senderName)
        if direct || sender.isEmpty { return shown }
        return "\(sender): \(shown)"
    }

    public static func preview(
        _ messages: [ChatMessage], direct: Bool, text: (ChatMessage) -> String, skip: (ChatMessage) -> Bool = { _ in false }
    ) -> ChatListPreview? {
        for message in messages.reversed() where !skip(message) {
            let shown = line(message, text: text(message), direct: direct)
            if !shown.isEmpty { return ChatListPreview(text: shown, at: message.timestamp) }
        }
        return nil
    }

    public static func timeLabel(_ date: Date, now: Date, timeZone: TimeZone = .current) -> String {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        let days = calendar.dateComponents([.day], from: calendar.startOfDay(for: date), to: calendar.startOfDay(for: now)).day ?? 0
        if days <= 0 { return ChatHub.timeLabel(date, timeZone: timeZone) }
        let parts = calendar.dateComponents([.year, .month, .day, .weekday], from: date)
        if days < 7 {
            let weekdays = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]
            return weekdays[((parts.weekday ?? 1) - 1 + 7) % 7]
        }
        let months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
        let month = months[((parts.month ?? 1) - 1 + 12) % 12]
        if parts.year == calendar.component(.year, from: now) { return "\(month) \(parts.day ?? 1)" }
        return "\(month) \(parts.day ?? 1), \(parts.year ?? 0)"
    }
}

extension ChatHub {
    public func listPreview(_ conversation: String) -> ChatListPreview? {
        _ = extras.revision
        let direct = conversation.hasPrefix("@") || DirectMessageFrames.isDirect(conversation)
        let mesh = conversation == meshConversation
        return ChatListPreviews.preview(
            messages(in: conversation), direct: direct,
            text: { mesh ? $0.text : self.extras.text(for: $0) },
            skip: { message in
                self.extras.isHidden(message.id) || self.extras.isRemoved(message.id) || self.extras.isThreadReply(message.id)
                    || message.app?.ephemeral == true
            })
    }

    public func listTimeLabel(_ preview: ChatListPreview) -> String {
        ChatListPreviews.timeLabel(preview.at, now: clock.now)
    }
}
