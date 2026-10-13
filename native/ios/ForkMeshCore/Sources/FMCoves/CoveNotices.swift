import Foundation

public struct CoveInviteNotice: Equatable, Sendable {
    public let inviteeAccount: String
    public let coveId: String
    public let coveName: String
    public let inviterName: String
    public let shareUrl: String
    public let ts: Int64

    public init(inviteeAccount: String, coveId: String, coveName: String, inviterName: String, shareUrl: String, ts: Int64) {
        self.inviteeAccount = inviteeAccount
        self.coveId = coveId
        self.coveName = coveName
        self.inviterName = inviterName
        self.shareUrl = shareUrl
        self.ts = ts
    }
}

public struct CoveOpenNotice: Equatable, Sendable {
    public let creatorKey: String
    public let coveId: String
    public let coveName: String
    public let openerKey: String
    public let openerName: String
    public let ts: Int64

    public init(creatorKey: String, coveId: String, coveName: String, openerKey: String, openerName: String, ts: Int64) {
        self.creatorKey = creatorKey
        self.coveId = coveId
        self.coveName = coveName
        self.openerKey = openerKey
        self.openerName = openerName
        self.ts = ts
    }

    public var canonical: [UInt8] {
        CoveNotices.openCanonical(coveId: coveId, creatorKey: creatorKey, openerKey: openerKey, ts: ts)
    }
}

public enum CoveInviteDelivery: Equatable, Sendable {
    case relayed
    case shareLink(String)
}

public struct CoveNotifier {
    public var invite: @MainActor (CoveInviteNotice) -> Bool
    public var opened: @MainActor (CoveOpenNotice) -> Bool

    public init(invite: @escaping @MainActor (CoveInviteNotice) -> Bool, opened: @escaping @MainActor (CoveOpenNotice) -> Bool) {
        self.invite = invite
        self.opened = opened
    }
}

public enum CoveNotices {
    public static func openCanonical(coveId: String, creatorKey: String, openerKey: String, ts: Int64) -> [UInt8] {
        Array("forkmesh-cove-open-v1\n\(coveId)\n\(creatorKey)\n\(openerKey)\n\(ts)".utf8)
    }

    public static func shouldAnnounceOpen(_ cove: Cove, openerKey: String) -> Bool {
        cove.unlocked && cove.notifyOnOpen && !cove.creator.isEmpty && !openerKey.isEmpty && cove.creator != openerKey
    }
}
