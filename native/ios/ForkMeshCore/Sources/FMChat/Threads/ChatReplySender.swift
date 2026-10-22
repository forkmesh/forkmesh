import FMApi
import FMBase
import Foundation

public enum ChatReplyError: Error, Equatable, CustomStringConvertible {
    case unknownConversation(String)
    case offline
    case refused

    public var description: String {
        switch self {
        case .unknownConversation(let name): return "No conversation named \(name) is open on this phone."
        case .offline: return "Chat is offline."
        case .refused: return "The reply could not be sent."
        }
    }
}

public enum ChatReplyTarget {
    public static func resolve(_ raw: String, channels: [String], roster: [Member], offices: [String] = []) throws -> String {
        let name = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if name.isEmpty { throw ChatReplyError.unknownConversation(raw) }
        if name.hasPrefix("#") || DirectMessageFrames.isDirect(name) { return name }
        let others = roster.filter { !$0.isSelf }
        if name.hasPrefix("@") {
            let rest = String(name.dropFirst())
            if others.contains(where: { $0.id == rest }) { return name }
            let folded = rest.lowercased()
            let matches = others.filter {
                $0.owner.lowercased() == folded || $0.name.lowercased() == folded || $0.nodeName.lowercased() == folded
            }
            if let best = matches.first(where: \.online) ?? matches.first { return "@\(best.id)" }
            return name
        }
        if offices.contains(OfficeFrames.conversation(for: name)) { return OfficeFrames.conversation(for: name) }
        if channels.contains("#\(name)") { return "#\(name)" }
        if let exact = others.first(where: { $0.id == name }) { return "@\(exact.id)" }
        let folded = name.lowercased()
        let matches = others.filter {
            $0.owner.lowercased() == folded || $0.name.lowercased() == folded || $0.nodeName.lowercased() == folded
        }
        if let best = matches.first(where: \.online) ?? matches.first { return "@\(best.id)" }
        throw ChatReplyError.unknownConversation(name)
    }
}

@MainActor
public final class ChatReplySender: NotificationReplySending {
    let relay: RelayService
    let extras: ChatExtras
    let office: () -> OfficeChannelService?
    let direct: () -> DirectMessageService?
    let connectWait: TimeInterval

    public init(
        relay: RelayService, extras: ChatExtras, office: @escaping () -> OfficeChannelService? = { nil },
        direct: @escaping () -> DirectMessageService? = { nil }, connectWait: TimeInterval = 10
    ) {
        self.relay = relay
        self.extras = extras
        self.office = office
        self.direct = direct
        self.connectWait = connectWait
    }

    func directConversation(_ conversation: String, direct: DirectMessageService) async -> String? {
        if DirectMessageFrames.isDirect(conversation) { return conversation }
        let id = String(conversation.dropFirst())
        let account = relay.roster().first { !$0.isSelf && $0.id == id }.map { ChatNodeOwners.normalized($0.owner) } ?? ""
        if account.isEmpty { return nil }
        if let record = direct.records.first(where: { $0.otherUser == account }) { return record.conversation }
        return try? await direct.start(username: account).conversation
    }

    public func reply(channel: String, text: String) async throws {
        let body = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if body.isEmpty { throw ChatReplyError.refused }
        let conversation = try ChatReplyTarget.resolve(
            channel, channels: relay.channels, roster: relay.roster(), offices: office()?.conversations ?? [])
        if OfficeFrames.isOffice(conversation) {
            guard let office = office(), await office.send(conversation, text: body) else { throw ChatReplyError.refused }
            return
        }
        if DirectMessageFrames.isDirect(conversation) || conversation.hasPrefix("@") {
            guard let direct = direct(), let target = await directConversation(conversation, direct: direct),
                await direct.send(target, text: body)
            else { throw ChatReplyError.refused }
            return
        }
        if relay.state != .connected {
            await relay.connect()
            let deadline = Date().addingTimeInterval(connectWait)
            while relay.state != .connected && Date() < deadline {
                try? await Task.sleep(nanoseconds: 100_000_000)
            }
            if relay.state != .connected { throw ChatReplyError.offline }
        }
        if !(await extras.sendText(conversation: conversation, text: body)) { throw ChatReplyError.refused }
    }
}
