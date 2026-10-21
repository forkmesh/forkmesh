import Foundation

public struct DirectCallRingPush: Equatable, Sendable {
    public let callId: String
    public let conversation: String
    public let caller: String
    public let origin: String

    public init(callId: String = "", conversation: String, caller: String = "", origin: String = "") {
        self.callId = callId
        self.conversation = conversation
        self.caller = caller
        self.origin = origin
    }

    public init?(fields: [String: String]) {
        guard fields["topic"] == DirectCallEngine.ringTopic else { return nil }
        let conversation = (fields["conversation"] ?? "").trimmingCharacters(in: .whitespaces)
        guard DirectCallRingPush.validConversation(conversation) else { return nil }
        let callId = (fields["callId"] ?? "").lowercased()
        self.init(
            callId: DirectCallRingPush.validCallId(callId) ? callId : "", conversation: conversation,
            caller: String((fields["caller"] ?? "").trimmingCharacters(in: .whitespaces).lowercased().prefix(64)),
            origin: fields["origin"] ?? "")
    }

    public static func validConversation(_ value: String) -> Bool {
        guard value.hasPrefix(DirectMessageFrames.prefix) else { return false }
        let id = value.dropFirst(DirectMessageFrames.prefix.count)
        return id.count == 32 && id.allSatisfy { ("0"..."9").contains($0) || ("a"..."f").contains($0) }
    }

    public static func validCallId(_ value: String) -> Bool {
        UUID(uuidString: value) != nil
    }
}

public enum DirectCallKitEndReason: Equatable, Sendable {
    case remoteEnded
    case unanswered
    case failed
}

public enum DirectCallKitEffect: Equatable, Sendable {
    case report(id: String, caller: String, video: Bool)
    case update(id: String, caller: String, video: Bool)
    case end(id: String, reason: DirectCallKitEndReason)
    case open(conversation: String)
    case wait(id: String)
    case accept(callId: String)
    case decline(callId: String)
    case hangUp
    case answerInApp(id: String)
}

@MainActor
public final class DirectCallKitLedger {
    public static let inviteWait: TimeInterval = 20
    static let endedMemory = 32

    struct Call: Equatable {
        var id: String
        var callId: String
        var conversation: String
        var invited = false
        var answered = false
        var declined = false
        var live = false
    }

    private(set) var calls: [Call] = []
    private var ended: [String] = []
    private let makeId: () -> String

    public init(makeId: @escaping () -> String = { UUID().uuidString.lowercased() }) {
        self.makeId = makeId
    }

    public func ringing(_ callId: String) -> Bool {
        let key = callId.lowercased()
        return calls.contains { $0.callId == key && !$0.declined }
    }

    public func ringing(conversation: String) -> Bool {
        calls.contains { $0.conversation == conversation && !$0.declined }
    }

    public func push(_ push: DirectCallRingPush) -> [DirectCallKitEffect] {
        if !push.callId.isEmpty, let index = calls.firstIndex(where: { $0.callId == push.callId }) {
            return [.report(id: calls[index].id, caller: push.caller, video: false)]
        }
        if push.callId.isEmpty, let index = calls.firstIndex(where: { $0.conversation == push.conversation }) {
            return [.report(id: calls[index].id, caller: push.caller, video: false)]
        }
        let id = push.callId.isEmpty ? makeId() : push.callId
        if ended.contains(id) {
            return [.report(id: id, caller: push.caller, video: false), .end(id: id, reason: .remoteEnded)]
        }
        calls.append(Call(id: id, callId: push.callId, conversation: push.conversation))
        return [
            .report(id: id, caller: push.caller, video: false), .open(conversation: push.conversation),
            .wait(id: id),
        ]
    }

    public func engine(invite: DirectCallInvite?, activeCallId: String) -> [DirectCallKitEffect] {
        var effects: [DirectCallKitEffect] = []
        let active = activeCallId.lowercased()
        if let invite {
            let callId = invite.callId.lowercased()
            let video = invite.media == .video
            if let index = calls.firstIndex(where: { $0.callId == callId })
                ?? calls.firstIndex(where: {
                    $0.callId.isEmpty && !$0.invited && $0.conversation == invite.conversation
                })
            {
                if !calls[index].invited {
                    calls[index].invited = true
                    calls[index].callId = callId
                    if calls[index].declined {
                        effects.append(.decline(callId: callId))
                        retire(index)
                    } else {
                        effects.append(.update(id: calls[index].id, caller: invite.partner, video: video))
                        if calls[index].answered { effects.append(.accept(callId: callId)) }
                    }
                }
            } else if !ended.contains(callId) {
                let id = DirectCallRingPush.validCallId(callId) ? callId : makeId()
                calls.append(Call(id: id, callId: callId, conversation: invite.conversation, invited: true))
                effects.append(.report(id: id, caller: invite.partner, video: video))
            }
        }
        if !active.isEmpty, let index = calls.firstIndex(where: { $0.callId == active && !$0.declined }),
            !calls[index].live
        {
            calls[index].live = true
            if !calls[index].answered {
                calls[index].answered = true
                effects.append(.answerInApp(id: calls[index].id))
            }
        }
        let pending = invite?.callId.lowercased() ?? ""
        for call in calls where call.invited && !call.declined && call.callId != pending {
            if call.live && call.callId == active { continue }
            effects.append(.end(id: call.id, reason: .remoteEnded))
            if let index = calls.firstIndex(of: call) { retire(index) }
        }
        return effects
    }

    public func answered(_ id: String) -> [DirectCallKitEffect] {
        guard let index = calls.firstIndex(where: { $0.id == id.lowercased() }), !calls[index].declined else { return [] }
        if calls[index].answered { return [] }
        calls[index].answered = true
        return calls[index].invited ? [.accept(callId: calls[index].callId)] : []
    }

    public func endRequested(_ id: String) -> [DirectCallKitEffect] {
        guard let index = calls.firstIndex(where: { $0.id == id.lowercased() }) else { return [] }
        let call = calls[index]
        if call.live {
            retire(index)
            return [.hangUp]
        }
        if call.invited {
            retire(index)
            return [.decline(callId: call.callId)]
        }
        calls[index].declined = true
        remember(call.id)
        return []
    }

    public func timedOut(_ id: String) -> [DirectCallKitEffect] {
        guard let index = calls.firstIndex(where: { $0.id == id.lowercased() }), !calls[index].invited else { return [] }
        let declined = calls[index].declined
        retire(index)
        return declined ? [] : [.end(id: id.lowercased(), reason: .unanswered)]
    }

    public func reportFailed(_ id: String) {
        guard let index = calls.firstIndex(where: { $0.id == id.lowercased() }) else { return }
        retire(index)
    }

    public func reset() -> [DirectCallKitEffect] {
        let live = calls.contains { $0.live }
        for call in calls { remember(call.id) }
        calls.removeAll()
        return live ? [.hangUp] : []
    }

    private func retire(_ index: Int) {
        remember(calls[index].id)
        if !calls[index].callId.isEmpty { remember(calls[index].callId) }
        calls.remove(at: index)
    }

    private func remember(_ id: String) {
        if ended.contains(id) { return }
        ended.append(id)
        if ended.count > DirectCallKitLedger.endedMemory { ended.removeFirst(ended.count - DirectCallKitLedger.endedMemory) }
    }
}
