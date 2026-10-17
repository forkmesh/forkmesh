import FMBase
import FMCrypto
import Foundation

public struct ChatDeviceInfo: Equatable, Sendable, Identifiable {
    public let kid: String
    public let acct: String
    public let node: String
    public let state: ChatTrustState
    public let firstSeen: Date
    public let verified: Bool
    public let isSelf: Bool
    public let isOwnAccount: Bool

    public var id: String { kid }

    init(_ record: ChatTrustRecord, ownKid: String, ownAcct: String) {
        kid = record.kid
        acct = record.acct
        node = record.node
        state = record.state
        firstSeen = Date(epochMillis: record.firstSeen)
        verified = record.verified
        isSelf = record.kid == ownKid
        isOwnAccount = !ownAcct.isEmpty && record.acct == ownAcct
    }
}

public enum DirectRoute: String, Equatable, Sendable {
    case none, wifi, bluetooth, directOnly

    public var label: String {
        switch self {
        case .none: return ""
        case .wifi: return "Direct \u{00B7} Wi-Fi"
        case .bluetooth: return "Direct \u{00B7} Bluetooth"
        case .directOnly: return "Direct only"
        }
    }
}

public enum DirectTrustEvent: Equatable, Sendable {
    case pendingDevice(conversation: String, kid: String)
    case safetyNumberChanged(conversation: String)
}

public enum DirectTrustText {
    public static let approve = "Approve"
    public static let block = "Block"

    public static func pendingDevice(_ name: String) -> String {
        "A new device for \(name) wants to join this chat. Check the safety number, then approve or block."
    }

    public static func safetyNumberChanged(_ name: String) -> String {
        "Your safety number with \(name) changed"
    }
}

struct DirectAckWait {
    let conversationId: String
    let plain: JSONObject
    let notify: Bool
    let kids: Set<String>
}
