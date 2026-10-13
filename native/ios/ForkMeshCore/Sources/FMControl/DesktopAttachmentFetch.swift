import FMApi
import FMBase
import FMChat
import Foundation

extension DesktopControlHub {
    public var prefersAttachmentPreviews: Bool { currentLink?.transport != .lan }

    public func attachmentPayload(_ ref: AgentAttachmentRef, preview: Bool) async throws -> AgentAttachmentPayload {
        guard case .desktop(let sessionId) = ref.origin else {
            throw AgentAttachmentError("This attachment is not on the desktop.")
        }
        do {
            let link: DesktopControlLink = try await link()
            let wantsPreview: Bool = preview || link.transport == .relay
            let params: JSONObject = LocalAgentTranscript.attachmentParams(
                sessionId: sessionId, attachmentId: ref.meta.id, preview: wantsPreview)
            let result: JSONObject = try await link.request(
                "agents.attachment", params, timeout: wantsPreview ? 60 : 30)
            return try LocalAgentTranscript.attachmentPayload(result)
        } catch let error as LocalControlException {
            throw AgentAttachmentError(LocalAgentTranscript.attachmentErrorText(error.code))
        }
    }

    public nonisolated func attachmentPayloadFetch()
        -> @Sendable (AgentAttachmentRef, Bool) async throws -> AgentAttachmentPayload
    {
        { [weak self] ref, preview in
            guard let self else { throw AgentAttachmentError("The desktop is not connected.") }
            return try await self.attachmentPayload(ref, preview: preview)
        }
    }

    public nonisolated func attachmentPreviewProbe() -> @Sendable () async -> Bool {
        { [weak self] in
            guard let self else { return true }
            return await self.prefersAttachmentPreviews
        }
    }

    public nonisolated func attachmentLoader(
        store: AgentAttachmentStore = .shared,
        relay: @escaping @Sendable (AgentAttachmentRef) async throws -> Data
    ) -> AgentAttachmentLoader {
        AgentAttachmentLoader(
            store: store, relay: relay, desktopPayload: attachmentPayloadFetch(),
            desktopRelayed: attachmentPreviewProbe())
    }
}
