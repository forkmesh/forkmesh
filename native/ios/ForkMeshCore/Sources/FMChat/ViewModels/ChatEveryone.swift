import FMApi
import FMBase
import Foundation

public enum ChatEveryone {
    public static let names = ["general", "random", "welcome"]
    public static let worldConversation = "#general"
    public static let worldLabel = "World"
    public static let retiredRelayLabels: Set<String> = ["#random", "#welcome"]
    public static let welcomeSenderId = "system:welcome"
    public static let refreshInterval: TimeInterval = 5 * 60

    public static var generalConversation: String { OfficeFrames.conversation(for: "general") }
    public static var welcomeConversation: String { OfficeFrames.conversation(for: "welcome") }

    public static func isWelcome(_ message: ChatMessage) -> Bool { message.senderId == welcomeSenderId }

    @MainActor public static func order(_ rooms: [String], office: OfficeChannelService?) -> [String] {
        let everyone = rooms.filter { office?.room(for: $0)?.everyone == true }
        let ranked = everyone.sorted { a, b in
            let left = names.firstIndex(of: office?.room(for: a)?.name ?? "") ?? names.count
            let right = names.firstIndex(of: office?.room(for: b)?.name ?? "") ?? names.count
            return left < right
        }
        return ranked + rooms.filter { office?.room(for: $0)?.everyone != true }
    }
}

extension ChatHub {
    public func isEveryoneChannel(_ conversation: String) -> Bool {
        office?.room(for: conversation)?.everyone == true
    }

    public func canLeaveChannel(_ conversation: String) -> Bool {
        conversation != ChatEveryone.worldConversation &&
            !isEveryoneChannel(conversation) &&
            office?.room(for: conversation) == nil &&
            conversation.hasPrefix("#")
    }

    public var everyoneRooms: [OfficeRoom] { office?.rooms.filter(\.everyone) ?? [] }

    public var listedChannels: [String] {
        let everyone = officeChannels.filter { isEveryoneChannel($0) }
        return everyone + channels + officeChannels.filter { !isEveryoneChannel($0) }
    }

    public var generalConversation: String {
        everyoneRooms.first { $0.name == "general" }?.conversation ?? ChatEveryone.worldConversation
    }

    public func refreshMemberDirectory(force: Bool = false) async {
        guard let office, let room = everyoneRooms.first(where: { $0.name == "general" }) ?? everyoneRooms.first else { return }
        if !force, let at = directoryLoadedAt, clock.now.timeIntervalSince(at) < ChatEveryone.refreshInterval { return }
        if directoryLoading { return }
        directoryLoading = true
        defer { directoryLoading = false }
        do {
            let names = try await office.members(room)
            var seen = Set<String>()
            directoryMembers = names.filter { seen.insert($0.lowercased()).inserted }
            directoryLoadedAt = clock.now
        } catch {}
    }

    public func adoptEveryoneGeneral() {
        let general = generalConversation
        guard general != ChatEveryone.worldConversation, !workspace.movedToEveryone else { return }
        workspace.markMovedToEveryone()
        if relay.currentConversation == ChatEveryone.worldConversation { select(general) }
    }
}
