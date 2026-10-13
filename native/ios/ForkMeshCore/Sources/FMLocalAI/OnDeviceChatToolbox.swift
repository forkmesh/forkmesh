import FMBase
import Foundation

public struct OnDeviceEmailDraft: Sendable, Equatable {
    public var to: String
    public var recipientName: String
    public var subject: String
    public var body: String

    public init(to: String, recipientName: String = "", subject: String, body: String) {
        self.to = to
        self.recipientName = recipientName
        self.subject = subject
        self.body = body
    }

    public var mailtoURL: URL? {
        var components = URLComponents()
        components.scheme = "mailto"
        components.path = to
        components.queryItems = [URLQueryItem(name: "subject", value: subject), URLQueryItem(name: "body", value: body)]
        return components.url
    }

    public var plainText: String {
        "To: \(to)\nSubject: \(subject)\n\n\(body)"
    }
}

public struct OnDeviceToolResult: Sendable, Equatable {
    public var text: String
    public var email: OnDeviceEmailDraft?

    public init(_ text: String, email: OnDeviceEmailDraft? = nil) {
        self.text = text
        self.email = email
    }
}

public struct OnDeviceToolApproval: Sendable, Equatable, Identifiable {
    public var id: String
    public var toolName: String
    public var summary: String

    public init(id: String, toolName: String, summary: String) {
        self.id = id
        self.toolName = toolName
        self.summary = summary
    }
}

@MainActor
public protocol OnDeviceChatToolbox: AnyObject {
    var tools: [LocalToolSpec] { get }
    func briefing() async -> String
    func approval(for call: LocalToolCall) async -> String?
    func invoke(_ call: LocalToolCall) async -> OnDeviceToolResult
}

extension OnDeviceChatToolbox {
    public func approval(for call: LocalToolCall) async -> String? { nil }
}

public enum OnDeviceChatTools {
    public static func clip(_ text: String, limit: Int) -> String {
        guard text.count > limit, limit > 0 else { return text }
        return String(text.prefix(limit)) + "\n… (\(text.count - limit) more characters cut)"
    }

    public static func label(for name: String) -> String {
        let words = name.split(separator: "_").map(String.init)
        guard let first = words.first else { return name }
        return ([first.prefix(1).uppercased() + first.dropFirst()] + words.dropFirst()).joined(separator: " ")
    }

    public static func declined(_ summary: String) -> String {
        "The user declined this, so nothing was done: \(summary). Do not retry it unless they ask again."
    }
}
