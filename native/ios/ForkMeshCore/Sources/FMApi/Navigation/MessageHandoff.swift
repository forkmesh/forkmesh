import Foundation

public enum MessageHandoff {
    public static let maxMessageLength = 4000

    public static func cleaned(_ message: String) -> String {
        let trimmed: String = message.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.count <= maxMessageLength {
            return trimmed
        }
        return String(trimmed.prefix(maxMessageLength)) + "…"
    }

    public static func canHandOff(_ message: String) -> Bool {
        return !cleaned(message).isEmpty
    }

    public static func agentPrompt(_ message: String) -> String {
        let quoted: String = cleaned(message).split(separator: "\n", omittingEmptySubsequences: false)
            .map { "> \($0)" }.joined(separator: "\n")
        return "The ForkMesh iPhone app showed this message:\n\n\(quoted)\n\nFind out what caused it and fix it."
    }

    public static func composeTarget(_ message: String) -> ComposeTarget {
        return ComposeTarget(text: agentPrompt(message))
    }
}
