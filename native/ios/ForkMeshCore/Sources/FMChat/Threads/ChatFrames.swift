import FMApi
import FMBase
import Foundation

public enum ChatLimits {
    public static let maxTextChars = 16_000
    public static let maxFileBytes = 16 * 1024 * 1024
    public static let maxFileNameChars = 180
    public static let maxMimeChars = 100
    public static let maxRootIdChars = 96
    public static let maxDisplayNameChars = 32
    public static let historyEntriesPerChannel = 200
    public static let historyCharsPerChannel = 2 * 1024 * 1024
    public static let attachmentBudgetBytes = 48 * 1024 * 1024
    public static let persistedFileBytes = 512 * 1024
    public static let typingTimeout: TimeInterval = 6
    public static let typingResend: TimeInterval = 2.5
    public static let hiddenMessages = 2_000
}

public struct ReactionEntry: Equatable, Sendable {
    public let value: String
    public let label: String
    public let emoji: String
}

public enum ReactionCatalog {
    public static let entries: [ReactionEntry] = [
        ReactionEntry(value: "like", label: "Like", emoji: "\u{1F44D}"),
        ReactionEntry(value: "love", label: "Love", emoji: "\u{2764}\u{FE0F}"),
        ReactionEntry(value: "laugh", label: "Laugh", emoji: "\u{1F602}"),
        ReactionEntry(value: "celebrate", label: "Celebrate", emoji: "\u{1F389}"),
        ReactionEntry(value: "hot", label: "Hot", emoji: "\u{1F525}"),
        ReactionEntry(value: "thanks", label: "Thanks", emoji: "\u{1F64F}"),
        ReactionEntry(value: "smile", label: "Smile", emoji: "\u{1F642}"),
        ReactionEntry(value: "wink", label: "Wink", emoji: "\u{1F609}"),
        ReactionEntry(value: "cool", label: "Cool", emoji: "\u{1F60E}"),
        ReactionEntry(value: "thinking", label: "Thinking", emoji: "\u{1F914}"),
        ReactionEntry(value: "surprised", label: "Surprised", emoji: "\u{1F62E}"),
        ReactionEntry(value: "sad", label: "Sad", emoji: "\u{1F622}"),
        ReactionEntry(value: "angry", label: "Angry", emoji: "\u{1F621}"),
        ReactionEntry(value: "dislike", label: "Dislike", emoji: "\u{1F44E}"),
        ReactionEntry(value: "eyes", label: "Eyes", emoji: "\u{1F440}"),
        ReactionEntry(value: "star", label: "Star", emoji: "\u{2B50}"),
        ReactionEntry(value: "rocket", label: "Rocket", emoji: "\u{1F680}"),
        ReactionEntry(value: "check", label: "Done", emoji: "\u{2705}"),
    ]

    public static let quick: [String] = ["like", "love", "laugh", "celebrate", "eyes", "check"]

    public static func withoutVariationSelectors(_ emoji: String) -> String {
        String(String.UnicodeScalarView(emoji.unicodeScalars.filter { $0.value != 0xFE0F && $0.value != 0xFE0E }))
    }

    public static func hasEmoji(_ value: String) -> Bool { entries.contains { $0.value == value } }

    public static func emoji(for value: String) -> String {
        entries.first { $0.value == value }?.emoji ?? value
    }

    public static func label(for value: String) -> String {
        entries.first { $0.value == value }?.label ?? value
    }

    public static func value(forEmoji emoji: String) -> String {
        let bare = withoutVariationSelectors(emoji)
        return entries.first { withoutVariationSelectors($0.emoji) == bare }?.value ?? emoji
    }
}

@MainActor
public protocol ChatFrameSink: AnyObject {
    var selfId: String { get }
    func makeFrame(_ type: String) -> JSONObject
    @discardableResult func sendFrame(_ frame: JSONObject) -> Bool
    func applyFrame(_ frame: JSONObject)
    @discardableResult func sendMessage(_ text: String, to conversation: String) async -> Bool
}

extension RelayService: ChatFrameSink {}

public enum ChatFrameText {
    public static let botField = "bot"

    public static func shownSender(_ frame: JSONObject) -> String {
        let bot = (frame[botField]?.stringValue ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        if !bot.isEmpty { return String(bot.prefix(64)) }
        return frame["senderId"]?.text ?? ""
    }
    public static func bounded(_ value: JSONValue?, _ max: Int) -> String {
        String((value?.text ?? "").prefix(max))
    }

    public static func conversation(of frame: JSONObject) -> String {
        let conversation = frame["conversation"]?.text ?? ""
        if !conversation.isEmpty { return conversation }
        return frame["channel"]?.text ?? ""
    }

    public static func isChannel(_ conversation: String) -> Bool { conversation.hasPrefix("#") }
}
