import FMAccount
import FMApi
import FMBase
import Foundation

public enum ChatEveryone {
    public static let channels = ["#general", "#random", "#welcome"]
    public static let welcomeChannel = "#welcome"
    public static let welcomeWindowMs: Int64 = 7 * 24 * 60 * 60 * 1_000
    public static let refreshInterval: TimeInterval = 5 * 60
    static let welcomePrefix = "welcome-member-"

    public static func contains(_ conversation: String) -> Bool { channels.contains(conversation) }

    public static func handle(_ raw: String) -> String? {
        let name = raw.trimmingCharacters(in: .whitespaces).lowercased()
        guard (1...40).contains(name.count), name.first != "-", name.last != "-",
              name.allSatisfy({ ($0.isASCII && ($0.isLetter || $0.isNumber)) || $0 == "-" }) else { return nil }
        return name
    }

    public static func welcomeText(_ name: String) -> String {
        "\u{1F389} Welcome to ForkMesh, @\(name)! Everyone, come say hi \u{1F44B}"
    }

    public static func isWelcome(_ message: ChatMessage) -> Bool {
        message.senderId == "forkbot" && message.id.hasPrefix(welcomePrefix)
    }

    public static func welcomeMessage(_ raw: String, at: Date) -> ChatMessage? {
        guard let name = handle(raw) else { return nil }
        return ChatMessage(
            id: welcomePrefix + name, conversation: welcomeChannel, senderId: "forkbot", senderName: "forkbot",
            text: welcomeText(name), timestamp: at)
    }
}

extension ChatHub {
    public var everyoneApplies: Bool { sharedRoom() }

    public func refreshMemberDirectory(force: Bool = false) async {
        guard let accounts, sharedRoom() else { return }
        if !force, let at = directoryLoadedAt, clock.now.timeIntervalSince(at) < ChatEveryone.refreshInterval { return }
        if directoryLoading { return }
        directoryLoading = true
        defer { directoryLoading = false }
        do {
            let map = try await accounts.accountApiCall("GET", PeopleModel.directoryPath, nil)
            var seen = Set<String>()
            let members = map["users"]?.objects.compactMap(PeopleMember.parse).filter { seen.insert($0.id).inserted } ?? []
            directoryLoadedAt = clock.now
            applyMemberDirectory(members)
        } catch {}
    }

    public func applyMemberDirectory(_ members: [PeopleMember]) {
        directoryMembers = members
        let now = clock.nowMillis
        for member in members where member.createdAt > 0 && now - member.createdAt < ChatEveryone.welcomeWindowMs {
            if let message = ChatEveryone.welcomeMessage(member.name, at: Date(epochMillis: member.createdAt)) {
                relay.insertLocal(message, countUnread: false)
            }
        }
    }

    public func welcomeNewMember(_ name: String) {
        guard sharedRoom(), let message = ChatEveryone.welcomeMessage(name, at: clock.now) else { return }
        guard relay.insertLocal(message, countUnread: true) else { return }
        filePing(message)
        Task { await refreshMemberDirectory(force: true) }
    }
}
