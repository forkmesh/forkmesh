import FMApi
import FMBase
import Foundation

extension ChatHub {
    public var canSendViewOnce: Bool {
        let conversation = relay.currentConversation
        guard DirectMessageFrames.isDirect(conversation), let direct else { return false }
        return direct.record(for: conversation) != nil && !inGroupConversation
    }

    public func viewOnce(for message: ChatMessage) -> ViewOnceEntry? {
        guard DirectMessageFrames.isDirect(message.conversation) else { return nil }
        return direct?.viewOnce.entry(message.id)
    }

    @discardableResult
    public func unsendViewOnce(_ messageId: String) async -> Bool {
        guard let direct else { return false }
        do {
            try await direct.withdrawViewOnce(messageId)
            return true
        } catch let error as ViewOnceError {
            post(error == .unavailable ? "The message could not be unsent. It may already have been opened." : error.description)
        } catch {
            post("The message could not be unsent.")
        }
        return false
    }

    @discardableResult
    public func sendViewOnce(
        kind: ViewOnceKind, mime: String, data: Data, durationMs: Int64, progress: (@MainActor (Double) -> Void)? = nil
    ) async -> Bool {
        let conversation = relay.currentConversation
        guard canSendViewOnce, let direct else {
            post("View-once messages can only be sent in a direct message.")
            return false
        }
        do {
            try await direct.sendViewOnce(
                conversation, kind: kind, mime: mime, data: data, durationMs: durationMs, progress: progress)
            return true
        } catch let error as ViewOnceError {
            post(error.description)
        } catch {
            post(ViewOnceError.unavailable.description)
        }
        return false
    }
}
