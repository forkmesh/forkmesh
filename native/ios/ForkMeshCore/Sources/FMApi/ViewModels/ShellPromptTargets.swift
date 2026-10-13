import Foundation
import Observation

public struct ShellPromptTarget {
    public let id: String
    public let label: String
    public let icon: String
    public let title: String
    public let placeholder: String
    public let submit: @MainActor (String) async throws -> String

    public init(id: String, label: String, icon: String, title: String, placeholder: String,
                submit: @escaping @MainActor (String) async throws -> String) {
        self.id = id
        self.label = label
        self.icon = icon
        self.title = title
        self.placeholder = placeholder
        self.submit = submit
    }
}

public struct ShellPromptFailure: Error, Equatable, Sendable, CustomStringConvertible {
    public let message: String

    public init(_ message: String) {
        self.message = message
    }

    public var description: String { message }
}

@MainActor
@Observable
public final class ShellPromptTargets {
    private struct Entry {
        let token: UUID
        let target: ShellPromptTarget
    }

    private var entries: [Entry] = []
    public var bypassed = false

    public init() {}

    public var active: ShellPromptTarget? { entries.last?.target }

    public var routing: ShellPromptTarget? { bypassed ? nil : active }

    @discardableResult
    public func register(_ target: ShellPromptTarget) -> UUID {
        let token = UUID()
        entries.removeAll { $0.target.id == target.id }
        entries.append(Entry(token: token, target: target))
        bypassed = false
        return token
    }

    public func unregister(_ token: UUID) {
        entries.removeAll { $0.token == token }
        if entries.isEmpty { bypassed = false }
    }

    public static func message(_ error: Error) -> String {
        if let failure = error as? ShellPromptFailure { return failure.message }
        if let failure = error as? OrgApiException { return failure.message }
        return "That did not work."
    }
}
