import Foundation
import FMBase

/// One node on the relay roster.
public struct Member: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var name: String
    /// This device's own node.
    public var isSelf: Bool
    public var online: Bool
    public var platform: String
    public var version: String
    public var mirrors: [String]
    public var owner: String
    public var nodeName: String
    /// The away flag and custom status the member's node last announced on the
    /// relay (hello/presence `userStatus`). Unexpired filtering happens at paint
    /// time: see `UserStatus.visibleAt`.
    public var userStatus: UserStatus
    public var diagnostics: JSONValue?
    public var timeZone: String

    public init(
        id: String, name: String, isSelf: Bool = false, online: Bool = false, platform: String = "",
        version: String = "", mirrors: [String] = [], owner: String = "",
        nodeName: String = "", userStatus: UserStatus = .empty, diagnostics: JSONValue? = nil, timeZone: String = ""
    ) {
        self.id = id
        self.name = name
        self.isSelf = isSelf
        self.online = online
        self.platform = platform
        self.version = version
        self.mirrors = mirrors
        self.owner = owner
        self.nodeName = nodeName
        self.userStatus = userStatus
        self.diagnostics = diagnostics
        self.timeZone = timeZone
    }
}

/// The nodes of one person, shown as one roster row. `members` is never empty.
public struct MemberGroup: Equatable, Hashable, Sendable, Identifiable {
    public var name: String
    public var members: [Member]
    public var isSelf: Bool
    public var disambiguator: String

    public init(name: String, members: [Member], isSelf: Bool = false, disambiguator: String = "") {
        self.name = name
        self.members = members
        self.isSelf = isSelf
        self.disambiguator = disambiguator
    }

    public var online: Bool { members.contains { $0.online } }

    /// One status per person: every node of an account carries the same one, so
    /// the first node that announced anything speaks for the group.
    public var userStatus: UserStatus {
        members.first { !$0.userStatus.isEmpty }?.userStatus ?? .empty
    }

    public var timeZone: String { members.first { !$0.timeZone.isEmpty }?.timeZone ?? "" }

    public var account: String { members.first { !$0.owner.isEmpty }?.owner.lowercased() ?? "" }

    public var primary: Member { members[0] }
    public var id: String { primary.id }
    public var nodeCount: Int { members.count }
}

public enum ChatReceipt: String, Equatable, Hashable, Sendable, CaseIterable {
    case sending, sent, delivered, read

    public var label: String {
        switch self {
        case .sending: return "Sending"
        case .sent: return "Sent"
        case .delivered: return "Delivered"
        case .read: return "Read"
        }
    }

    public var rank: Int {
        switch self {
        case .sending: return 0
        case .sent: return 1
        case .delivered: return 2
        case .read: return 3
        }
    }
}

public struct ChatMessage: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    /// "#channel" or "@peerId".
    public var conversation: String
    public var senderId: String
    public var senderName: String
    public var text: String
    public var timestamp: Date
    public var isSelf: Bool
    public var fileName: String
    public var recipients: Int?
    public var app: AppMessage?

    public init(
        id: String, conversation: String, senderId: String, senderName: String, text: String,
        timestamp: Date, isSelf: Bool = false, fileName: String = "", recipients: Int? = nil, app: AppMessage? = nil
    ) {
        self.id = id
        self.conversation = conversation
        self.senderId = senderId
        self.senderName = senderName
        self.text = text
        self.timestamp = timestamp
        self.isSelf = isSelf
        self.fileName = fileName
        self.recipients = recipients
        self.app = app
    }
}
