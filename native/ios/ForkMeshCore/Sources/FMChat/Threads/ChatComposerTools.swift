import FMApi
import FMBase
import Foundation

public struct ChatMentionToken: Equatable, Sendable {
    public let query: String
    public let start: Int
    public let end: Int
}

public struct ChatSlashCommand: Equatable, Sendable, Identifiable {
    public let command: String
    public let usage: String
    public let summary: String

    public var id: String { command }
}

public enum ChatSlash: Equatable, Sendable {
    case issue(String)
    case topic(String)
    case message(to: String, text: String)
    case shrug(String)
    case me(String)
    case mute
    case search(String)
    case saved
}

public struct ChatLinkInsertion: Equatable, Sendable {
    public let text: String
    public let selectionStart: Int
    public let selectionLength: Int
}

public enum ChatComposerTools {
    public static let emojiPicker: [String] = [
        "\u{1F600}", "\u{1F601}", "\u{1F602}", "\u{1F923}", "\u{1F60A}", "\u{1F60D}", "\u{1F60E}",
        "\u{1F609}", "\u{1F642}", "\u{1F622}", "\u{1F62D}", "\u{1F621}", "\u{1F631}", "\u{1F914}",
        "\u{1F634}", "\u{1F92F}", "\u{1F44D}", "\u{1F44E}", "\u{1F44F}", "\u{1F64F}", "\u{1F4AA}",
        "\u{1F64C}", "\u{1F44B}", "\u{1F91D}", "\u{1F525}", "\u{2728}", "\u{1F389}", "\u{1F4AF}",
        "\u{2764}\u{FE0F}", "\u{1F494}", "\u{2B50}", "\u{2705}", "\u{274C}", "\u{1F440}", "\u{1F680}",
        "\u{1F41B}", "\u{1F4A1}", "\u{1F4CC}", "\u{2615}", "\u{1F355}", "\u{1F382}", "\u{1F973}",
    ]

    public static func mentionToken(_ text: String, cursor: Int? = nil) -> ChatMentionToken? {
        token(text, trigger: "@", cursor: cursor)
    }

    public static func channelToken(_ text: String, cursor: Int? = nil) -> ChatMentionToken? {
        token(text, trigger: "#", cursor: cursor)
    }

    public static func emojiToken(_ text: String, cursor: Int? = nil) -> ChatMentionToken? {
        guard let found = token(text, trigger: ":", cursor: cursor), found.query.count >= 2,
            !found.query.contains(":"), found.query.allSatisfy({ $0.isLetter || $0.isNumber || $0 == "_" || $0 == "+" || $0 == "-" })
        else { return nil }
        return found
    }

    static func token(_ text: String, trigger: Character, cursor: Int?) -> ChatMentionToken? {
        let chars = Array(text)
        let end = min(max(cursor ?? chars.count, 0), chars.count)
        var start = end - 1
        while start >= 0 && !chars[start].isWhitespace && chars[start] != trigger { start -= 1 }
        if start < 0 || chars[start] != trigger { return nil }
        if start > 0 && !chars[start - 1].isWhitespace { return nil }
        return ChatMentionToken(query: String(chars[(start + 1)..<end]), start: start, end: end)
    }

    public static func channelCandidates(_ labels: [String], query: String, limit: Int = 6) -> [String] {
        let folded = query.lowercased()
        var seen = Set<String>()
        var out: [String] = []
        for label in labels {
            let name = label.hasPrefix("#") ? String(label.dropFirst()) : label
            let key = name.lowercased()
            if name.isEmpty || seen.contains(key) { continue }
            seen.insert(key)
            if folded.isEmpty || key.hasPrefix(folded) || key.contains("/" + folded) { out.append(name) }
        }
        return Array(out.prefix(limit))
    }

    public static func insertChannel(_ text: String, name: String, cursor: Int? = nil) -> String {
        replaceToken(text, token: channelToken(text, cursor: cursor), with: "#\(name) ")
    }

    public static func insertEmoji(_ text: String, emoji: String, cursor: Int? = nil) -> String {
        replaceToken(text, token: emojiToken(text, cursor: cursor), with: emoji + " ")
    }

    static func replaceToken(_ text: String, token: ChatMentionToken?, with replacement: String) -> String {
        guard let token else { return text }
        var chars = Array(text)
        chars.replaceSubrange(token.start..<token.end, with: Array(replacement))
        return String(chars)
    }

    public static func mentionCandidates(_ names: [String], excluding own: String, query: String, limit: Int = 8) -> [String] {
        var seen = Set<String>()
        var out: [String] = []
        let ownFolded = own.trimmingCharacters(in: .whitespaces).lowercased()
        let folded = query.lowercased()
        for raw in names {
            let name = raw.trimmingCharacters(in: .whitespaces)
            let key = name.lowercased()
            if name.isEmpty || key == ownFolded || seen.contains(key) { continue }
            seen.insert(key)
            if folded.isEmpty || key.hasPrefix(folded) { out.append(name) }
        }
        out.sort { $0.lowercased() < $1.lowercased() }
        return Array(out.prefix(limit))
    }

    public static func insertMention(_ text: String, name: String, cursor: Int? = nil) -> String {
        guard !name.isEmpty, let token = mentionToken(text, cursor: cursor) else { return text }
        var chars = Array(text)
        chars.replaceSubrange(token.start..<token.end, with: Array("@\(name) "))
        return String(chars)
    }

    public static let shrug = "\u{00AF}\\_(\u{30C4})_/\u{00AF}"

    public static func slashCommands(_ text: String, canSetTopic: Bool) -> [ChatSlashCommand] {
        guard text.hasPrefix("/"), text.dropFirst().allSatisfy({ $0.isLetter }) else { return [] }
        var all = [
            ChatSlashCommand(command: "/msg", usage: "/msg @person <message>", summary: "Send someone a direct message"),
            ChatSlashCommand(command: "/dm", usage: "/dm @person <message>", summary: "Send someone a direct message"),
            ChatSlashCommand(command: "/search", usage: "/search <words>", summary: "Search messages"),
            ChatSlashCommand(command: "/saved", usage: "/saved", summary: "Open your saved messages"),
            ChatSlashCommand(command: "/mute", usage: "/mute", summary: "Mute or unmute this conversation"),
            ChatSlashCommand(command: "/shrug", usage: "/shrug [message]", summary: "Append \(shrug)"),
            ChatSlashCommand(command: "/me", usage: "/me <action>", summary: "Say it in italics"),
            ChatSlashCommand(command: "/issue", usage: "/issue [owner/repo] <title>", summary: "Create a repository issue"),
        ]
        if canSetTopic {
            all.append(ChatSlashCommand(command: "/topic", usage: "/topic <text>", summary: "Change this managed channel topic"))
        }
        let prefix = text.lowercased()
        return all.filter { $0.command.hasPrefix(prefix) }
    }

    public static func parseSlash(_ text: String) -> ChatSlash? {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmed.hasPrefix("/") else { return nil }
        let body = trimmed.dropFirst()
        let command = body.prefix { !$0.isWhitespace }.lowercased()
        let argument = String(body.dropFirst(command.count)).trimmingCharacters(in: .whitespacesAndNewlines)
        switch command {
        case "issue": return .issue(argument)
        case "topic": return .topic(argument)
        case "msg", "dm":
            let target = argument.prefix { !$0.isWhitespace }
            let name = target.hasPrefix("@") ? String(target.dropFirst()) : String(target)
            let message = String(argument.dropFirst(target.count)).trimmingCharacters(in: .whitespacesAndNewlines)
            return .message(to: name, text: message)
        case "shrug": return .shrug(argument.isEmpty ? shrug : "\(argument) \(shrug)")
        case "me": return argument.isEmpty ? nil : .me("_\(argument)_")
        case "mute": return .mute
        case "search": return .search(argument)
        case "saved": return .saved
        default: return nil
        }
    }

    public static func sanitizedHref(_ raw: String) -> String {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty || trimmed.count > 2048 || trimmed.contains(where: { $0.isWhitespace }) { return "" }
        guard let url = URL(string: trimmed), let scheme = url.scheme?.lowercased(), scheme == "https" || scheme == "http",
            let host = url.host, !host.isEmpty
        else { return "" }
        return trimmed
    }

    public static func linkInsertion(_ value: String, selectionStart: Int, selectionEnd: Int, clipboard: String = "")
        -> ChatLinkInsertion
    {
        let chars = Array(value)
        let start = min(max(selectionStart, 0), chars.count)
        let end = min(max(selectionEnd, start), chars.count)
        let selected = String(chars[start..<end])
        let haveTarget = !sanitizedHref(clipboard).isEmpty
        let label = selected.isEmpty ? "link text" : selected
        let target = haveTarget ? clipboard.trimmingCharacters(in: .whitespacesAndNewlines) : "https://"
        let text = String(chars[0..<start]) + "[" + label + "](" + target + ")" + String(chars[end...])
        if haveTarget && selected.isEmpty {
            return ChatLinkInsertion(text: text, selectionStart: start + 1, selectionLength: label.count)
        }
        if haveTarget {
            return ChatLinkInsertion(text: text, selectionStart: start + 1 + label.count + 2 + target.count + 1, selectionLength: 0)
        }
        return ChatLinkInsertion(text: text, selectionStart: start + 1 + label.count + 2, selectionLength: target.count)
    }

    public static func mentionsForkbot(_ text: String) -> Bool {
        let lower = Array(text.lowercased())
        let needle = Array("forkbot")
        var index = 0
        while index + needle.count <= lower.count {
            if Array(lower[index..<(index + needle.count)]) == needle {
                var before = index - 1
                if before >= 0 && lower[before] == "@" { before -= 1 }
                let leftOk = before < 0 || !(lower[before].isLetter || lower[before].isNumber || lower[before] == "_" || lower[before] == "-")
                let after = index + needle.count
                let rightOk = after >= lower.count || !(lower[after].isLetter || lower[after].isNumber || lower[after] == "_")
                if leftOk && rightOk { return true }
            }
            index += 1
        }
        return false
    }

    public static func forkbotContext(_ history: [ChatMessage], threadRootId: String = "", replies: [ChatMessage] = []) -> [JSONValue] {
        let source = threadRootId.isEmpty ? history : history.filter { $0.id == threadRootId } + replies
        var context: [JSONValue] = []
        let candidates = source.dropLast()
        for entry in candidates.reversed() {
            if context.count >= 12 { break }
            if entry.senderId == "forkbot" || entry.text.isEmpty { continue }
            context.insert(
                .object(["sender": .string(String(entry.senderName.prefix(32))), "text": .string(String(entry.text.prefix(600)))]),
                at: 0)
        }
        return context
    }

    public static func forkbotPayload(message: String, sender: String, context: [JSONValue]) -> JSONObject {
        ["message": .string(message), "sender": .string(sender), "room": "general", "context": .array(context)]
    }
}

public struct ChatIssueDraft: Equatable, Sendable {
    public let repo: RepoRef
    public let title: String
    public let body: String

    public enum Failure: Error, Equatable, Sendable {
        case usage
        case noRepositories
        case unknownRepository(String)
    }

    public static func parse(
        _ argument: String, repos: [RepoRef], current: RepoRef?, channel: String, author: String, now: Date
    ) -> Result<ChatIssueDraft, Failure> {
        var text = argument.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.isEmpty { return .failure(.usage) }
        if repos.isEmpty { return .failure(.noRepositories) }
        var repo = current.flatMap { current in repos.first { $0.slug.lowercased() == current.slug.lowercased() } }
        let first = String(text.prefix { !$0.isWhitespace })
        if isRepoToken(first) {
            guard let named = repos.first(where: { $0.slug.lowercased() == first.lowercased() }) else {
                return .failure(.unknownRepository(first))
            }
            repo = named
            text = String(text.dropFirst(first.count)).trimmingCharacters(in: .whitespacesAndNewlines)
        }
        if repo == nil { repo = repos.first { $0.owner.lowercased() == "forkmesh" && $0.name.lowercased() == "forkmesh" } }
        guard let chosen = repo ?? repos.first else { return .failure(.noRepositories) }
        if text.isEmpty { return .failure(.usage) }
        let firstLine = text.split(separator: "\n", omittingEmptySubsequences: false).first.map(String.init) ?? text
        var title = firstLine.split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
        if title.count > 200 { title = String(title.prefix(199)) + "\u{2026}" }
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime]
        let who = author.trimmingCharacters(in: .whitespaces).isEmpty ? "someone" : author.trimmingCharacters(in: .whitespaces)
        let place = channel.trimmingCharacters(in: .whitespaces).isEmpty ? "" : channel.trimmingCharacters(in: .whitespaces) + " "
        let body = text + "\n\n---\nFiled from a " + place + "chat message by " + who + " at " + formatter.string(from: now) + "."
        return .success(ChatIssueDraft(repo: chosen, title: title, body: body))
    }

    public static func fromMessage(_ message: ChatMessage, text: String, repo: RepoRef, channel: String, now: Date) -> ChatIssueDraft {
        let flattened = text.split(separator: "\n", omittingEmptySubsequences: false).first.map(String.init) ?? text
        var title = flattened.split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
        if title.isEmpty { title = "Chat message from \(message.senderName)" }
        if title.count > 200 { title = String(title.prefix(199)) + "\u{2026}" }
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime]
        let body = "> " + text.replacingOccurrences(of: "\n", with: "\n> ") + "\n\n---\nFrom a " + channel + " chat message by "
            + message.senderName + " at " + formatter.string(from: message.timestamp) + "."
        return ChatIssueDraft(repo: repo, title: title, body: body)
    }

    static func isRepoToken(_ token: String) -> Bool {
        let parts = token.split(separator: "/", omittingEmptySubsequences: false)
        guard parts.count == 2 else { return false }
        let owner = Array(parts[0])
        let name = parts[1]
        guard let head = owner.first, head.isLowercase, head.isLetter, owner.count <= 63 else { return false }
        if !owner.allSatisfy({ ($0.isLowercase && $0.isLetter) || $0.isNumber || $0 == "-" }) { return false }
        if name.isEmpty || name.count > 100 { return false }
        return name.allSatisfy { $0.isLetter || $0.isNumber || $0 == "." || $0 == "_" || $0 == "-" }
    }
}

public enum ChatVisitorPresence {
    public static let idleWindow: TimeInterval = 600

    public static func isTransientVisitor(accountKind: String = "", name: String) -> Bool {
        if accountKind.trimmingCharacters(in: .whitespaces).lowercased() == "guest" { return true }
        let lower = name.trimmingCharacters(in: .whitespaces).lowercased()
        for prefix in ["guest", "world guest", "world visitor"] where lower.hasPrefix(prefix) {
            let rest = lower.dropFirst(prefix.count)
            if rest.isEmpty || !(rest.first!.isLetter || rest.first!.isNumber || rest.first! == "_") { return true }
        }
        return false
    }

    public static func isIdle(lastSeen: Date?, now: Date) -> Bool {
        guard let lastSeen else { return false }
        return now.timeIntervalSince(lastSeen) > idleWindow
    }
}

@MainActor
public final class ChatWelcomeAnnouncer {
    public static let channel = "#welcome"
    public static let text = "\u{1F44B} Just joined ForkMesh \u{2014} hello!"
    public static let freshAccountWindowMs: Int64 = 24 * 60 * 60 * 1_000
    private let prefs: KeyValueStore

    public init(prefs: KeyValueStore) {
        self.prefs = prefs
    }

    private func key(_ account: String) -> String { "chat/welcomeAnnounced/\(account.lowercased())" }

    public func shouldAnnounce(account: String, isGuest: Bool, emailVerified: Bool, createdAtMs: Int64, nowMs: Int64, alreadyInRoom: Bool) -> Bool {
        let name = account.trimmingCharacters(in: .whitespaces)
        if name.isEmpty || isGuest || !emailVerified { return false }
        if prefs.bool(forKey: key(name)) == true { return false }
        let created = createdAtMs > 0 && createdAtMs < 100_000_000_000 ? createdAtMs * 1_000 : createdAtMs
        if alreadyInRoom || created <= 0 || nowMs - created > ChatWelcomeAnnouncer.freshAccountWindowMs {
            prefs.set(true, forKey: key(name))
            return false
        }
        return true
    }

    public func markAnnounced(account: String) {
        prefs.set(true, forKey: key(account.trimmingCharacters(in: .whitespaces)))
    }
}
