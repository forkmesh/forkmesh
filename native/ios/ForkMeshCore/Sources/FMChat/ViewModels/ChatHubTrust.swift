import FMCrypto
import Foundation

extension ChatHub {
    func trustEvent(_ event: DirectTrustEvent) {
        if case .safetyNumberChanged(let conversation) = event, trustChangedAt[conversation] == nil {
            trustChangedAt[conversation] = clock.now
        }
    }

    public func trustName(_ conversation: String) -> String {
        if conversation == relay.currentConversation { return conversationName }
        return direct?.record(for: conversation)?.otherUser ?? ""
    }

    public func trustBanners(_ conversation: String) -> [DirectTrustNotice] {
        guard let direct, DirectMessageFrames.isDirect(conversation) else { return [] }
        return DirectTrustNotices.pending(
            conversation: conversation, devices: direct.devices(conversation), name: trustName(conversation))
    }

    public func trustNotices(_ conversation: String) -> [DirectTrustNotice] {
        guard let direct, DirectMessageFrames.isDirect(conversation) else { return [] }
        let changed = direct.safetyNumberChanged(conversation)
        if !changed {
            trustChangedAt[conversation] = nil
        } else if trustChangedAt[conversation] == nil {
            trustChangedAt[conversation] = clock.now
        }
        return DirectTrustNotices.notices(
            conversation: conversation, devices: direct.devices(conversation), name: trustName(conversation),
            safetyChanged: changed, changedAt: trustChangedAt[conversation])
    }

    public var conversationRouteLine: String { directRoute(relay.currentConversation).label }

    public var conversationDetailLine: String {
        [conversationSubtitle, conversationRouteLine, serverCopiesLine].filter { !$0.isEmpty }.joined(separator: " \u{00B7} ")
    }

    public func approveDevice(_ conversation: String, kid: String) async {
        await direct?.approveDevice(conversation, kid: kid)
    }

    public func blockDevice(_ conversation: String, kid: String) async {
        await direct?.blockDevice(conversation, kid: kid)
    }

    public func safetyNumber(_ conversation: String) -> ChatSafetyNumber? {
        guard DirectMessageFrames.isDirect(conversation) else { return nil }
        return direct?.safetyNumber(conversation)
    }

    public func safetyVerified(_ conversation: String) -> Bool {
        guard let direct, DirectMessageFrames.isDirect(conversation) else { return false }
        let approved = direct.devices(conversation).filter { $0.state == .approved }
        return !approved.isEmpty && approved.allSatisfy(\.verified)
    }

    public func markSafetyVerified(_ conversation: String) {
        direct?.markVerified(conversation)
    }

    public func verifySafetyCode(_ conversation: String, scanned: String) -> DirectSafetyScan {
        guard let direct, let number = direct.safetyNumber(conversation) else { return .notSafetyCode }
        let matched = direct.verifySafetyCode(conversation, scanned: scanned)
        return DirectSafetyScan.result(scanned: scanned, expected: number.qr, matched: matched)
    }

    public func acknowledgeSafetyChange(_ conversation: String) {
        guard let direct, direct.safetyNumberChanged(conversation) else { return }
        direct.acknowledgeSafetyChange(conversation)
    }

    public var trustRevision: Int { direct?.trustRevision ?? 0 }

    public func officeConversation(channelId: String) -> String? {
        guard !channelId.isEmpty else { return nil }
        return office?.rooms.first { $0.id == channelId }?.conversation
    }
}
