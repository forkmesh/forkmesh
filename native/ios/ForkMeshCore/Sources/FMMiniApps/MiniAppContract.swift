import FMBase
import Foundation

public enum MiniAppContract {
    public static let coreProtocol = 2
    public static let pluginAPI = 1
    public static let listMethod = "plugins.list"
    public static let cardMethod = "card"
    public static let changedEvent = "changed"

    public static func method(_ plugin: String, _ name: String) -> String { "plugin.\(plugin).\(name)" }

    public static func event(_ plugin: String, _ name: String) -> String { "plugin.\(plugin).\(name)" }

    public static func split(event: String) -> (plugin: String, name: String)? {
        let parts = event.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 3, parts[0] == "plugin", !parts[1].isEmpty, !parts[2].isEmpty else { return nil }
        return (String(parts[1]), String(parts[2]))
    }

    public static let permanentErrors: Set<String> = [
        "forbidden", "not_owner", "unknown_method", "unknown_plugin", "bad_params",
        "plugin_invalid", "plugin_untrusted", "plugin_disabled", "plugin_needs_consent", "plugin_failed",
    ]

    public static func isPermanent(_ code: String) -> Bool { permanentErrors.contains(code) }
}

public struct MiniAppDescriptor: Equatable, Sendable, Identifiable {
    public let id: String
    public let name: String
    public let version: String
    public let summary: String
    public let methods: [String]
    public let events: [String]
    public let running: Bool

    public init(id: String, name: String, version: String = "", summary: String = "",
                methods: [String], events: [String] = [], running: Bool = false) {
        self.id = id
        self.name = name
        self.version = version
        self.summary = summary
        self.methods = methods
        self.events = events
        self.running = running
    }

    public init?(_ object: JSONObject) {
        let id = object.str("id")
        guard !id.isEmpty else { return nil }
        self.init(id: id, name: object.str("name", id), version: object.str("version"),
                  summary: object.str("summary"), methods: object.strings("methods"),
                  events: object.strings("events"), running: object.bool("running"))
    }

    public var hasCard: Bool { methods.contains(MiniAppContract.cardMethod) }

    public func allows(_ method: String) -> Bool { methods.contains(method) }

    public static func catalog(_ result: JSONObject) -> [MiniAppDescriptor] {
        result.maps("plugins").compactMap(MiniAppDescriptor.init).sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }
}
