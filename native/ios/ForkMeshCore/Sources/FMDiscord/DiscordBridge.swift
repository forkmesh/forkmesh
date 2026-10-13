import FMBase
import FMChat
import FMControl
import Foundation
import Observation

public struct DiscordStatus: Equatable, Sendable {
    public var available = false
    public var configured = false
    public var connected = false
    public var bot = ""

    public init(available: Bool = false, configured: Bool = false, connected: Bool = false, bot: String = "") {
        self.available = available
        self.configured = configured
        self.connected = connected
        self.bot = bot
    }
}

public struct DiscordChannel: Equatable, Sendable, Identifiable {
    public let id: String
    public let name: String
    public let display: String
    public let group: String
    public let topic: String
    public let direct: Bool
    public let guild: Bool
    public let canSend: Bool
}

public struct DiscordGuild: Equatable, Sendable, Identifiable {
    public let name: String
    public let channels: [DiscordChannel]

    public var id: String { name }
}

public struct DiscordReaction: Equatable, Sendable, Identifiable {
    public let value: String
    public let count: Int

    public var id: String { value }
    public var emoji: String { ReactionCatalog.emoji(for: value) }
}

public struct DiscordMessage: Equatable, Sendable, Identifiable {
    public let id: String
    public let sender: String
    public let text: String
    public let timestamp: Date
    public let isSelf: Bool
    public let rootId: String
    public let fileName: String
    public let edited: Bool
    public let truncated: Bool
    public let reactions: [DiscordReaction]
}

public struct DiscordSendConfirmation: Equatable, Sendable {
    public let channel: String
    public let text: String
    public let replyTo: String
    public let token: String
    public let summary: String
    public let expiresAt: Date
}

public enum DiscordBridgeError: Error, Equatable, CustomStringConvertible {
    case desktopTooOld
    case unavailable
    case invalid(String)
    case refused(String)

    public var description: String {
        switch self {
        case .desktopTooOld: return "This desktop does not share its Discord bridge yet. Update ForkMesh on the desktop."
        case .unavailable: return "The Discord package is not running on your desktop."
        case .invalid(let reason): return reason
        case .refused(let code): return DiscordBridge.errorMessage(code)
        }
    }
}

@MainActor
@Observable
public final class DiscordBridge {
    nonisolated public static let maxSendChars = 2_000
    nonisolated public static let maxChannels = 300
    nonisolated public static let maxMessages = 100

    @ObservationIgnored let family: DesktopControlFamily
    @ObservationIgnored let clock: Clock
    @ObservationIgnored private var tap: UUID?
    @ObservationIgnored public var watching: Set<String> = []

    public private(set) var status = DiscordStatus()
    public private(set) var channels: [DiscordChannel] = []
    public private(set) var messages: [String: [DiscordMessage]] = [:]
    public private(set) var loading = false
    public private(set) var lastError: String?

    public init(channel: DesktopControlCalling, clock: Clock = SystemClock()) {
        family = DesktopControlFamily(.discord, channel: channel)
        self.clock = clock
    }

    public var guilds: [DiscordGuild] { DiscordBridge.guilds(channels) }

    public func channel(_ id: String) -> DiscordChannel? { channels.first { $0.id == id } }

    public nonisolated static func guilds(_ channels: [DiscordChannel]) -> [DiscordGuild] {
        var order: [String] = []
        var grouped: [String: [DiscordChannel]] = [:]
        for channel in channels {
            let key = channel.group.isEmpty ? "Discord" : channel.group
            if grouped[key] == nil { order.append(key) }
            grouped[key, default: []].append(channel)
        }
        return order.map { DiscordGuild(name: $0, channels: grouped[$0] ?? []) }
    }

    public nonisolated static func validChannelId(_ id: String) -> Bool {
        !id.isEmpty && id.count <= 24 && id.allSatisfy { $0.isASCII && $0.isNumber }
    }

    public nonisolated static func parseStatus(_ reply: JSONObject) -> DiscordStatus {
        DiscordStatus(
            available: reply["available"]?.isTrue == true, configured: reply["configured"]?.isTrue == true,
            connected: reply["connected"]?.isTrue == true, bot: String((reply["bot"]?.text ?? "").prefix(120)))
    }

    public nonisolated static func parseChannels(_ reply: JSONObject) -> [DiscordChannel] {
        var out: [DiscordChannel] = []
        var seen = Set<String>()
        for row in reply["channels"]?.objects ?? [] {
            if out.count >= maxChannels { break }
            let id = row["id"]?.text ?? ""
            guard validChannelId(id), !seen.contains(id) else { continue }
            seen.insert(id)
            out.append(
                DiscordChannel(
                    id: id, name: String((row["name"]?.text ?? "").prefix(120)),
                    display: String((row["display"]?.text ?? "").prefix(120)),
                    group: String((row["group"]?.text ?? "").prefix(120)), topic: String((row["topic"]?.text ?? "").prefix(240)),
                    direct: row["direct"]?.isTrue == true, guild: row["guild"]?.isTrue == true,
                    canSend: row["canSend"]?.isTrue == true))
        }
        return out
    }

    public nonisolated static func parseMessages(_ reply: JSONObject) -> [DiscordMessage] {
        var out: [DiscordMessage] = []
        for row in (reply["messages"]?.objects ?? []).suffix(maxMessages) {
            let id = String((row["id"]?.text ?? "").prefix(64))
            if id.isEmpty { continue }
            var reactions: [DiscordReaction] = []
            for (value, count) in row["reactions"]?.objectOrEmpty ?? [:] {
                let n = count.intValue ?? 0
                if n > 0 && reactions.count < 24 { reactions.append(DiscordReaction(value: ReactionCatalog.value(forEmoji: value), count: n)) }
            }
            reactions.sort { $0.value < $1.value }
            out.append(
                DiscordMessage(
                    id: id, sender: String((row["sender"]?.text ?? "").prefix(120)),
                    text: String((row["text"]?.text ?? "").prefix(4_000)),
                    timestamp: Date(epochMillis: row["ts"]?.int64Value ?? 0), isSelf: row["self"]?.isTrue == true,
                    rootId: String((row["root"]?.text ?? "").prefix(64)), fileName: String((row["fileName"]?.text ?? "").prefix(180)),
                    edited: row["edited"]?.isTrue == true, truncated: row["truncated"]?.isTrue == true, reactions: reactions))
        }
        return out
    }

    public nonisolated static func errorMessage(_ code: String) -> String {
        switch code {
        case "discord_unavailable": return "The Discord package is not running on your desktop."
        case "unknown_channel": return "That Discord channel is no longer available on the desktop."
        case "not_writable": return "The desktop's Discord bot cannot post in this channel."
        case "bad_text": return "Discord messages must be 1 to 2,000 characters."
        case "confirmation_expired": return "The confirmation expired. Send again."
        case "confirmation_mismatch", "confirmation_required": return "The desktop did not accept this confirmation. Send again."
        case "send_failed": return "The desktop could not send the message to Discord."
        case "not_owner": return "Only the desktop's owner can use its Discord bridge."
        default: return code.isEmpty ? "The desktop refused." : code.replacingOccurrences(of: "_", with: " ")
        }
    }

    private func call(_ method: String, _ params: JSONObject = [:]) async throws -> JSONObject {
        do {
            return try await family.call(method, params)
        } catch {
            if DesktopControlFamily.isUnsupported(error) { throw DiscordBridgeError.desktopTooOld }
            if let failure = error as? LocalControlException {
                throw failure.code == "discord_unavailable" ? DiscordBridgeError.unavailable : DiscordBridgeError.refused(failure.code)
            }
            throw error
        }
    }

    public func refresh() async {
        loading = true
        defer { loading = false }
        do {
            status = DiscordBridge.parseStatus(try await call("status"))
            if !status.available {
                channels = []
                lastError = DiscordBridgeError.unavailable.description
                return
            }
            channels = DiscordBridge.parseChannels(try await call("list"))
            let live = Set(channels.map(\.id))
            messages = messages.filter { live.contains($0.key) }
            lastError = nil
        } catch {
            lastError = "\(error)"
        }
    }

    public func load(_ channelId: String, limit: Int = 50) async {
        guard DiscordBridge.validChannelId(channelId) else { return }
        do {
            let reply = try await call("messages", ["channel": .string(channelId), "limit": .int(Int64(min(max(limit, 1), DiscordBridge.maxMessages)))])
            messages[channelId] = DiscordBridge.parseMessages(reply)
            lastError = nil
        } catch {
            lastError = "\(error)"
        }
    }

    public func prepareSend(channel channelId: String, text: String, replyTo: String = "") async throws -> DiscordSendConfirmation {
        let body = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard DiscordBridge.validChannelId(channelId) else { throw DiscordBridgeError.invalid("Pick a Discord channel.") }
        guard !body.isEmpty, body.count <= DiscordBridge.maxSendChars else {
            throw DiscordBridgeError.invalid(DiscordBridge.errorMessage("bad_text"))
        }
        var params: JSONObject = ["channel": .string(channelId), "text": .string(body)]
        if !replyTo.isEmpty { params["replyTo"] = .string(replyTo) }
        let reply = try await call("prepare", params)
        let token = reply["token"]?.text ?? ""
        if token.isEmpty { throw DiscordBridgeError.refused("confirmation_required") }
        let expires = reply["expiresInMs"]?.doubleValue ?? 60_000
        return DiscordSendConfirmation(
            channel: channelId, text: body, replyTo: replyTo, token: token,
            summary: String((reply["summary"]?.text ?? "Send to Discord").prefix(300)),
            expiresAt: clock.now.addingTimeInterval(expires / 1_000))
    }

    public func send(_ confirmation: DiscordSendConfirmation) async throws {
        if clock.now >= confirmation.expiresAt { throw DiscordBridgeError.refused("confirmation_expired") }
        var params: JSONObject = [
            "channel": .string(confirmation.channel), "text": .string(confirmation.text), "token": .string(confirmation.token),
        ]
        if !confirmation.replyTo.isEmpty { params["replyTo"] = .string(confirmation.replyTo) }
        _ = try await call("send", params)
        await load(confirmation.channel)
    }

    public func startWatching() {
        if tap != nil { return }
        tap = family.addEventTap { [weak self] suffix, _ in
            guard let self, suffix == "changed" else { return }
            Task { @MainActor in
                await self.refresh()
                for id in self.watching { await self.load(id) }
            }
        }
    }

    public func stopWatching() {
        if let tap { family.removeEventTap(tap) }
        tap = nil
    }
}
