import FMBase
import FMNet
import Foundation

extension ApiService {
    func agentSessionAttachmentURL(
        _ owner: String, _ repo: String, sessionId: String, attachmentId: String, orgScope: Bool
    ) -> URL {
        let session = agentSessionsURL(owner, repo, sessionId: sessionId, orgScope: orgScope)
        return session.appendingPathComponent("attachments").appendingPathComponent(attachmentId)
    }

    public func agentSessionAttachment(
        _ owner: String, _ repo: String, sessionId: String, attachmentId: String, orgScope: Bool = false
    ) async throws -> Data {
        guard attachmentId.range(of: "^[0-9a-f]{32}$", options: .regularExpression) != nil else {
            throw AgentAttachmentError("This attachment has no valid id.")
        }
        if !hasSession { throw AgentAttachmentError("Sign in to see this photo.") }
        let url = agentSessionAttachmentURL(
            owner, repo, sessionId: sessionId, attachmentId: attachmentId, orgScope: orgScope)
        let response = try await rawRequest("GET", url, headers: ["Accept": "image/*"])
        switch response.status {
        case 200..<300:
            _ = try AgentAttachmentMedia.checkImage(response.body)
            return response.body
        case 401: throw AgentAttachmentError("Sign in again to see this photo.")
        case 403: throw AgentAttachmentError("Only the person who sent this photo can open it.")
        case 404: throw AgentAttachmentError("This relay does not keep photos for transcripts.")
        case 410: throw AgentAttachmentError("The relay no longer keeps this photo (they are kept for a week).")
        default: throw AgentAttachmentError("The relay could not send this photo (HTTP \(response.status)).")
        }
    }

    public nonisolated func agentAttachmentRelayFetch() -> @Sendable (AgentAttachmentRef) async throws -> Data {
        { [weak self] ref in
            guard let self else { throw AgentAttachmentError("Not signed in.") }
            guard case .relay(let owner, let repo, let sessionId, let orgScope) = ref.origin else {
                throw AgentAttachmentError("This attachment is not on the relay.")
            }
            return try await self.agentSessionAttachment(
                owner, repo, sessionId: sessionId, attachmentId: ref.meta.id, orgScope: orgScope)
        }
    }
}
