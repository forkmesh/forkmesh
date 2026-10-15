import FMBase
import Foundation

public struct MiniAppCard: Equatable, Sendable {
    public enum Tone: String, Sendable {
        case neutral, good, attention, warning
    }

    public struct Row: Equatable, Sendable, Identifiable {
        public let id: String
        public let label: String
        public let value: String
        public let tone: Tone
    }

    public struct Action: Equatable, Sendable, Identifiable {
        public let id: String
        public let title: String
        public let method: String
        public let params: JSONObject
        public let destructive: Bool
        public let confirm: String
    }

    public static let maxRows = 50
    public static let maxActions = 6
    public static let minRefreshSeconds = 30

    public let title: String
    public let subtitle: String
    public let rows: [Row]
    public let actions: [Action]
    public let refreshSeconds: Int

    public init(title: String, subtitle: String = "", rows: [Row] = [], actions: [Action] = [], refreshSeconds: Int = 0) {
        self.title = title
        self.subtitle = subtitle
        self.rows = rows
        self.actions = actions
        self.refreshSeconds = refreshSeconds
    }

    public init(_ object: JSONObject, for app: MiniAppDescriptor) {
        let rows = object.maps("rows").prefix(Self.maxRows).enumerated().map { index, row in
            Row(id: row.str("id", "\(index)"), label: String(row.str("label").prefix(80)),
                value: String(row.str("value").prefix(400)), tone: Tone(rawValue: row.str("tone")) ?? .neutral)
        }
        let actions = object.maps("actions").compactMap { action -> Action? in
            let method = action.str("method")
            guard method != MiniAppContract.cardMethod, app.allows(method) else { return nil }
            return Action(id: action.str("id", method), title: String(action.str("title", method).prefix(40)),
                          method: method, params: action.obj("params"), destructive: action.bool("destructive"),
                          confirm: String(action.str("confirm").prefix(200)))
        }
        let refresh = object.int("refreshSeconds")
        self.init(title: String(object.str("title", app.name).prefix(80)),
                  subtitle: String(object.str("subtitle").prefix(200)),
                  rows: Array(rows), actions: Array(actions.prefix(Self.maxActions)),
                  refreshSeconds: refresh <= 0 ? 0 : max(refresh, Self.minRefreshSeconds))
    }

    public var json: JSONObject {
        [
            "title": .string(title),
            "subtitle": .string(subtitle),
            "refreshSeconds": .int(Int64(refreshSeconds)),
            "rows": .array(rows.map { ["id": .string($0.id), "label": .string($0.label), "value": .string($0.value), "tone": .string($0.tone.rawValue)] }),
            "actions": .array(actions.map {
                ["id": .string($0.id), "title": .string($0.title), "method": .string($0.method), "params": .object($0.params),
                 "destructive": .bool($0.destructive), "confirm": .string($0.confirm)]
            }),
        ]
    }
}
