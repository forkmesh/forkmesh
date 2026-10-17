import FMCrypto
import Foundation

public enum DirectTrustNoticeKind: Equatable, Sendable {
    case pendingDevice(kid: String)
    case ownDevice(kid: String)
    case safetyNumberChanged
}

public struct DirectTrustNotice: Equatable, Sendable, Identifiable {
    public let id: String
    public let conversation: String
    public let kind: DirectTrustNoticeKind
    public let text: String
    public let timestamp: Date

    public var kid: String? {
        switch kind {
        case .pendingDevice(let kid), .ownDevice(let kid): return kid
        case .safetyNumberChanged: return nil
        }
    }

    public var asksApproval: Bool { kid != nil }
}

public enum DirectSafetyScan: Equatable, Sendable {
    case verified
    case notSafetyCode
    case otherConversation
    case mismatch

    public static let prefix = "forkmesh-safety:3:"

    public static func result(scanned: String, expected: String, matched: Bool) -> DirectSafetyScan {
        if matched { return .verified }
        let code = scanned.trimmingCharacters(in: .whitespacesAndNewlines)
        guard code.hasPrefix(prefix) else { return .notSafetyCode }
        if scope(code) != scope(expected) { return .otherConversation }
        return .mismatch
    }

    static func scope(_ code: String) -> String {
        guard code.hasPrefix(prefix) else { return "" }
        let rest = code.dropFirst(prefix.count)
        guard let end = rest.lastIndex(of: ":") else { return "" }
        return String(rest[..<end])
    }

    public var message: String {
        switch self {
        case .verified: return DirectTrustText.verifiedNotice
        case .notSafetyCode: return "That QR code is not a ForkMesh safety number."
        case .otherConversation: return "That safety number is for a different conversation."
        case .mismatch:
            return "The safety numbers do not match. One of you may have a device that is not approved, or someone is in the middle."
        }
    }
}

extension DirectTrustText {
    public static let ownDevice = "A new device was added to your account. Approve it if it is yours, otherwise block it."
    public static let safetyNumber = "Safety number"
    public static let verified = "Verified"
    public static let notVerified = "Not verified"
    public static let markVerified = "Mark as verified"
    public static let scanCode = "Scan their code"
    public static let verifiedNotice = "Safety number verified."

    public static func safetyExplainer(_ name: String) -> String {
        "Compare these numbers with \(name), or scan the code on their phone. If they match, nobody is in the middle of this chat."
    }
}

extension ChatSafetyNumber {
    public var rows: [[String]] {
        let groups = self.groups
        return stride(from: 0, to: groups.count, by: 4).map { Array(groups[$0..<min($0 + 4, groups.count)]) }
    }
}

extension ChatDeviceInfo {
    init(
        kid: String, acct: String, node: String, state: ChatTrustState, firstSeen: Date, verified: Bool = false,
        isSelf: Bool = false, isOwnAccount: Bool = false
    ) {
        self.kid = kid
        self.acct = acct
        self.node = node
        self.state = state
        self.firstSeen = firstSeen
        self.verified = verified
        self.isSelf = isSelf
        self.isOwnAccount = isOwnAccount
    }
}

public enum DirectTrustNotices {
    public static func pending(conversation: String, devices: [ChatDeviceInfo], name: String) -> [DirectTrustNotice] {
        devices.filter { $0.state == .pending && !$0.isSelf }.sorted { $0.firstSeen < $1.firstSeen }.map { device in
            let own = device.isOwnAccount
            return DirectTrustNotice(
                id: "trust-\(conversation)-\(device.kid)", conversation: conversation,
                kind: own ? .ownDevice(kid: device.kid) : .pendingDevice(kid: device.kid),
                text: own ? DirectTrustText.ownDevice : DirectTrustText.pendingDevice(name), timestamp: device.firstSeen)
        }
    }

    public static func notices(
        conversation: String, devices: [ChatDeviceInfo], name: String, safetyChanged: Bool, changedAt: Date?
    ) -> [DirectTrustNotice] {
        var out = pending(conversation: conversation, devices: devices, name: name)
        if safetyChanged, let changedAt {
            out.append(
                DirectTrustNotice(
                    id: "trust-\(conversation)-safety", conversation: conversation, kind: .safetyNumberChanged,
                    text: DirectTrustText.safetyNumberChanged(name), timestamp: changedAt))
        }
        return out.sorted { $0.timestamp < $1.timestamp }
    }
}
