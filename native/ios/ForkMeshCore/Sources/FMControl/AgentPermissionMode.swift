import FMBase
import Foundation

public enum AgentPermissionMode: String, CaseIterable, Equatable, Hashable, Sendable, Identifiable {
    case auto = "Auto"
    case ask = "Ask"
    case plan = "Plan"
    case edit = "Edit"

    public static let preferenceKey = "agent/desktop/run-mode"

    public var id: String { rawValue }

    public var label: String { rawValue }

    public var note: String {
        switch self {
        case .auto: return "Run without asking"
        case .ask: return "Confirm every change"
        case .plan: return "Read-only, propose a plan"
        case .edit: return "Edit files, ask before commands"
        }
    }

    public var systemImage: String {
        switch self {
        case .auto: return "bolt.fill"
        case .ask: return "hand.raised"
        case .plan: return "list.bullet.clipboard"
        case .edit: return "pencil"
        }
    }

    public init?(label: String) {
        let key = label.dartTrimmed.lowercased()
        if key == "auto mode" {
            self = .auto
            return
        }
        guard let mode = Self.allCases.first(where: { $0.rawValue.lowercased() == key }) else { return nil }
        self = mode
    }

    public static func resolve(_ labels: String...) -> AgentPermissionMode {
        labels.lazy.compactMap { AgentPermissionMode(label: $0) }.first ?? .auto
    }

    public static func providerTakesMode(_ provider: String) -> Bool {
        provider.isEmpty || provider == "claude-code" || provider == "codex"
    }

    public static func read(_ store: KeyValueStore?) -> AgentPermissionMode {
        AgentPermissionMode(label: store?.string(forKey: preferenceKey) ?? "") ?? .auto
    }

    public func write(to store: KeyValueStore?) {
        store?.set(rawValue, forKey: Self.preferenceKey)
    }
}
