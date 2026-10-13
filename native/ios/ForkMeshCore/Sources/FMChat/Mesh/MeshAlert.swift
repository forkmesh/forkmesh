import FMApi
import Foundation

public let meshAlertFreshSeconds: TimeInterval = 10 * 60
public let meshAlertPreviewChars = 180
public let meshAlertIdentifier = "mesh-room"

public struct MeshAlert: Equatable, Sendable {
    public let title: String
    public let body: String
    public let sender: String
}

public func meshAlert(_ message: ChatMessage, unread: Int, watching: Bool, now: Date) -> MeshAlert? {
    if message.isSelf || watching || message.conversation != meshConversation { return nil }
    if now.timeIntervalSince(message.timestamp) > meshAlertFreshSeconds { return nil }
    let name: String = message.senderName.trimmingCharacters(in: .whitespacesAndNewlines)
    let sender: String = name.isEmpty ? "Someone nearby" : name
    let text: String = message.text.split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
    let preview: String =
        text.isEmpty ? (message.fileName.isEmpty ? "New message" : "File: \(message.fileName)") : text
    let body: String =
        preview.count > meshAlertPreviewChars ? String(preview.prefix(meshAlertPreviewChars - 3)) + "..." : preview
    let title: String = unread > 1 ? "\(sender) in \(meshConversation) · \(unread) new" : "\(sender) in \(meshConversation)"
    return MeshAlert(title: title, body: body, sender: sender)
}
