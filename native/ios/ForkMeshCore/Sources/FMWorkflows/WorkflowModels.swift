import FMBase
import Foundation

public struct WorkflowField: Equatable, Sendable, Identifiable {
    public struct Option: Equatable, Sendable, Hashable {
        public var value: String
        public var label: String
    }

    public var key: String
    public var label: String
    public var type: String
    public var required: Bool
    public var placeholder: String
    public var defaultValue: String
    public var options: [Option]
    public var secret: Bool

    public var id: String { key }

    public init(json: JSONObject) {
        key = json.str("key")
        label = json.str("label", json.str("key"))
        type = json.str("type", "string")
        required = json.bool("required")
        placeholder = json.str("placeholder")
        defaultValue = json["default"]?.text ?? ""
        options = json.maps("options").map { Option(value: $0.str("value"), label: $0.str("label", $0.str("value"))) }
        secret = json.bool("secret")
    }

    public var multiline: Bool { type == "json" || type == "text" || type == "code" }
    public var mono: Bool { type == "json" || type == "code" }
}

public struct WorkflowOperation: Equatable, Sendable, Identifiable {
    public var key: String
    public var name: String
    public var method: String
    public var fields: [WorkflowField]

    public var id: String { key }
}

public struct WorkflowNodeKind: Equatable, Sendable, Identifiable {
    public var type: String
    public var name: String
    public var group: String
    public var description: String
    public var color: String
    public var icon: String
    public var trigger: Bool
    public var outputs: [String]
    public var fields: [WorkflowField]
    public var credentialFields: [WorkflowField]
    public var operations: [WorkflowOperation]
    public var host: String

    public var id: String { type }
    public var isApp: Bool { type.hasPrefix("app.") }
    public var appKey: String { isApp ? String(type.dropFirst(4)) : "" }
    public var needsCredential: Bool { !credentialFields.isEmpty }

    public func fields(operation: String) -> [WorkflowField] {
        guard isApp else { return fields }
        return (operations.first { $0.key == operation } ?? operations.first)?.fields ?? []
    }
}

public struct WorkflowCatalog: Equatable, Sendable {
    public var core: [WorkflowNodeKind]
    public var integrations: [WorkflowNodeKind]
    public var categories: [String]
    public var operators: [WorkflowField.Option]

    public static let empty = WorkflowCatalog(json: [:])

    public init(json: JSONObject) {
        core = json.maps("core").map { (item: JSONObject) in
            WorkflowNodeKind(
                type: item.str("type"), name: item.str("name"), group: item.str("group"), description: item.str("description"),
                color: item.str("color"), icon: item.str("icon"), trigger: item.bool("trigger"),
                outputs: item.list("outputs").compactMap(\.stringValue), fields: item.maps("fields").map(WorkflowField.init(json:)),
                credentialFields: [], operations: [], host: "")
        }
        integrations = json.maps("integrations").map { (item: JSONObject) in
            WorkflowNodeKind(
                type: item.str("type"), name: item.str("name"), group: item.str("category"), description: item.str("description"),
                color: item.str("color"), icon: "", trigger: false, outputs: ["main"], fields: [],
                credentialFields: item.maps("credential").map(WorkflowField.init(json:)),
                operations: item.maps("operations").map {
                    WorkflowOperation(key: $0.str("key"), name: $0.str("name"), method: $0.str("method"),
                                      fields: $0.maps("fields").map(WorkflowField.init(json:)))
                },
                host: item.str("host"))
        }
        categories = json.list("categories").compactMap(\.stringValue)
        operators = json.maps("operators").map { WorkflowField.Option(value: $0.str("value"), label: $0.str("label")) }
    }

    public func kind(_ type: String) -> WorkflowNodeKind? {
        core.first { $0.type == type } ?? integrations.first { $0.type == type }
    }

    public var triggers: [WorkflowNodeKind] { core.filter(\.trigger) }
    public var steps: [WorkflowNodeKind] { core.filter { !$0.trigger } }

    public func integrations(in category: String) -> [WorkflowNodeKind] { integrations.filter { $0.group == category } }

    public func search(_ query: String) -> [WorkflowNodeKind] {
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        let all = steps + integrations
        guard !needle.isEmpty else { return all }
        return all.filter {
            $0.name.lowercased().contains(needle) || $0.group.lowercased().contains(needle)
                || $0.description.lowercased().contains(needle)
        }
    }

    public func title(_ type: String) -> String { kind(type)?.name ?? type }
}

public struct WorkflowSummary: Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var active: Bool
    public var trigger: String
    public var steps: Int
    public var scheduleMinutes: Int
    public var lastRunAt: Int64
    public var lastStatus: String
    public var updatedAt: Int64

    public init(json: JSONObject) {
        id = json.str("id")
        name = json.str("name", "Untitled workflow")
        active = json.bool("active")
        trigger = json.str("trigger")
        steps = json["nodes"]?.arrayValue?.count ?? json.int("nodes")
        scheduleMinutes = json.int("scheduleMinutes")
        lastRunAt = json["lastRunAt"]?.int64Value ?? 0
        lastStatus = json.str("lastStatus")
        updatedAt = json["updatedAt"]?.int64Value ?? 0
    }
}

public struct WorkflowConnection: Equatable, Sendable, Hashable {
    public var from: String
    public var output: String
    public var to: String

    public init(from: String, output: String = "main", to: String) {
        self.from = from
        self.output = output
        self.to = to
    }

    public var json: JSONValue { .object(["from": .string(from), "output": .string(output), "to": .string(to)]) }
}

public struct WorkflowNode: Equatable, Sendable, Identifiable {
    public var id: String
    public var type: String
    public var name: String
    public var x: Int
    public var y: Int
    public var params: JSONObject
    public var credential: String
    public var disabled: Bool
    public var continueOnFail: Bool

    public init(id: String, type: String, name: String, x: Int = 0, y: Int = 0, params: JSONObject = [:],
                credential: String = "", disabled: Bool = false, continueOnFail: Bool = false) {
        self.id = id
        self.type = type
        self.name = name
        self.x = x
        self.y = y
        self.params = params
        self.credential = credential
        self.disabled = disabled
        self.continueOnFail = continueOnFail
    }

    public init(json: JSONObject) {
        self.init(id: json.str("id"), type: json.str("type"), name: json.str("name"), x: json.int("x"), y: json.int("y"),
                  params: json.obj("params"), credential: json.str("credential"), disabled: json.bool("disabled"),
                  continueOnFail: json.bool("continueOnFail"))
    }

    public var json: JSONValue {
        .object([
            "id": .string(id), "type": .string(type), "name": .string(name), "x": .int(Int64(x)), "y": .int(Int64(y)),
            "params": .object(params), "credential": .string(credential), "disabled": .bool(disabled),
            "continueOnFail": .bool(continueOnFail),
        ])
    }

    public var isTrigger: Bool { ["core.manual", "core.webhook", "core.schedule"].contains(type) }

    public func text(_ key: String) -> String {
        guard let value = params[key], !value.isNull else { return "" }
        if case .string(let text) = value { return text }
        return JSON.encode(value)
    }

    public var operation: String { params.str("operation") }
}

public struct WorkflowDraft: Equatable, Sendable {
    public static let column = 288
    public static let row = 120

    public var id: String
    public var name: String
    public var active: Bool
    public var nodes: [WorkflowNode]
    public var connections: [WorkflowConnection]
    public var hookPath: String

    public init(id: String = "", name: String = "Untitled workflow", active: Bool = false, nodes: [WorkflowNode] = [],
                connections: [WorkflowConnection] = [], hookPath: String = "") {
        self.id = id
        self.name = name
        self.active = active
        self.nodes = nodes
        self.connections = connections
        self.hookPath = hookPath
    }

    public init(json: JSONObject) {
        self.init(
            id: json.str("id"), name: json.str("name", "Untitled workflow"), active: json.bool("active"),
            nodes: json.maps("nodes").map(WorkflowNode.init(json:)),
            connections: json.maps("connections").map {
                WorkflowConnection(from: $0.str("from"), output: $0.str("output", "main"), to: $0.str("to"))
            },
            hookPath: json.str("hookPath"))
    }

    public static var blank: WorkflowDraft {
        WorkflowDraft(name: "Untitled workflow", nodes: [WorkflowNode(id: "trigger", type: "core.manual", name: "Run manually")])
    }

    public var graph: JSONObject {
        ["name": .string(name), "nodes": .array(nodes.map(\.json)), "connections": .array(connections.map(\.json))]
    }

    public var body: JSONObject {
        var out = graph
        out["active"] = .bool(active)
        return out
    }

    public func node(_ id: String) -> WorkflowNode? { nodes.first { $0.id == id } }

    public var trigger: WorkflowNode? { nodes.first(where: \.isTrigger) }

    public var ordered: [WorkflowNode] {
        var incoming: [String: Int] = [:]
        var edges: [String: [String]] = [:]
        for node in nodes {
            incoming[node.id] = 0
            edges[node.id] = []
        }
        for connection in connections where incoming[connection.to] != nil && edges[connection.from] != nil {
            edges[connection.from, default: []].append(connection.to)
            incoming[connection.to, default: 0] += 1
        }
        var ready = nodes.filter { incoming[$0.id] == 0 }.map(\.id)
        var order: [String] = []
        while !ready.isEmpty {
            let current = ready.removeFirst()
            order.append(current)
            for target in edges[current] ?? [] {
                incoming[target, default: 0] -= 1
                if incoming[target] == 0 { ready.append(target) }
            }
        }
        let seen = Set(order)
        return order.compactMap(node) + nodes.filter { !seen.contains($0.id) }
    }

    public func inputs(of id: String) -> [WorkflowConnection] { connections.filter { $0.to == id } }
    public func outputs(of id: String) -> [WorkflowConnection] { connections.filter { $0.from == id } }

    public func uniqueName(_ base: String) -> String {
        let names = Set(nodes.map(\.name))
        let root = String(base.prefix(56))
        if !names.contains(root) { return root }
        var index = 2
        while names.contains("\(root) \(index)") { index += 1 }
        return "\(root) \(index)"
    }

    public func newId(_ type: String) -> String {
        let stem = type.split(separator: ".").last.map(String.init)?.filter { $0.isLetter || $0.isNumber } ?? "step"
        let base = String((stem.isEmpty ? "step" : stem).prefix(24))
        let ids = Set(nodes.map(\.id))
        var index = 1
        while ids.contains("\(base)\(index)") { index += 1 }
        return "\(base)\(index)"
    }

    @discardableResult
    public mutating func addStep(_ kind: WorkflowNodeKind, after parent: String?, output: String = "main") -> WorkflowNode {
        let anchor = parent.flatMap(node) ?? ordered.last
        let siblings = anchor.map { outputs(of: $0.id).count } ?? 0
        var params: JSONObject = [:]
        if kind.isApp, let first = kind.operations.first { params["operation"] = .string(first.key) }
        let fields = kind.isApp ? kind.fields(operation: params.str("operation")) : kind.fields
        for field in fields where !field.defaultValue.isEmpty { params[field.key] = .string(field.defaultValue) }
        let step = WorkflowNode(
            id: newId(kind.type), type: kind.type, name: uniqueName(kind.name),
            x: (anchor?.x ?? 0) + WorkflowDraft.column, y: (anchor?.y ?? 0) + siblings * WorkflowDraft.row, params: params)
        nodes.append(step)
        if let anchor, !kind.trigger {
            connections.append(WorkflowConnection(from: anchor.id, output: output, to: step.id))
        }
        return step
    }

    public mutating func setTrigger(_ kind: WorkflowNodeKind) {
        guard kind.trigger else { return }
        if let index = nodes.firstIndex(where: \.isTrigger) {
            var params: JSONObject = [:]
            for field in kind.fields where !field.defaultValue.isEmpty { params[field.key] = .string(field.defaultValue) }
            nodes[index].type = kind.type
            nodes[index].params = params
            let id = nodes[index].id
            nodes[index].name = nameTaken(kind.name, except: id) ? uniqueName(kind.name) : kind.name
        } else {
            nodes.insert(WorkflowNode(id: newId(kind.type), type: kind.type, name: uniqueName(kind.name)), at: 0)
        }
    }

    public mutating func remove(_ id: String) {
        let parents = inputs(of: id)
        let children = outputs(of: id)
        nodes.removeAll { $0.id == id }
        connections.removeAll { $0.from == id || $0.to == id }
        for parent in parents {
            for child in children where !connections.contains(WorkflowConnection(from: parent.from, output: parent.output, to: child.to)) {
                connections.append(WorkflowConnection(from: parent.from, output: parent.output, to: child.to))
            }
        }
    }

    public mutating func update(_ step: WorkflowNode) {
        guard let index = nodes.firstIndex(where: { $0.id == step.id }) else { return }
        nodes[index] = step
    }

    public func nameTaken(_ name: String, except id: String) -> Bool {
        nodes.contains { $0.id != id && $0.name == name }
    }
}

public struct WorkflowStepResult: Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var type: String
    public var status: String
    public var error: String
    public var ms: Int
    public var outputs: [(name: String, count: Int, items: [JSONValue])]

    public static func == (lhs: WorkflowStepResult, rhs: WorkflowStepResult) -> Bool {
        lhs.id == rhs.id && lhs.status == rhs.status && lhs.error == rhs.error && lhs.ms == rhs.ms
            && lhs.outputs.map(\.name) == rhs.outputs.map(\.name) && lhs.outputs.map(\.count) == rhs.outputs.map(\.count)
            && lhs.outputs.map(\.items) == rhs.outputs.map(\.items)
    }

    public var itemCount: Int { outputs.reduce(0) { $0 + $1.count } }
}

public enum WorkflowRunPlace: String, Equatable, Sendable {
    case phone
    case server
    case desktop
}

public struct WorkflowRun: Equatable, Sendable, Identifiable {
    public var id: String
    public var place: WorkflowRunPlace
    public var trigger: String
    public var status: String
    public var error: String
    public var requests: Int
    public var startedAt: Int64
    public var finishedAt: Int64
    public var nodes: JSONObject

    public init(json: JSONObject, place: WorkflowRunPlace, id: String? = nil, startedAt: Int64? = nil, finishedAt: Int64? = nil) {
        self.id = id ?? json.str("id")
        trigger = json.str("trigger", "manual")
        self.place = trigger == "desktop" ? .desktop : place
        status = json.str("status")
        error = json.str("error")
        requests = json.int("requests")
        self.startedAt = startedAt ?? json["startedAt"]?.int64Value ?? 0
        self.finishedAt = finishedAt ?? json["finishedAt"]?.int64Value ?? 0
        nodes = json.obj("nodes")
    }

    public var succeeded: Bool { status == "success" }
    public var hasDetail: Bool { !nodes.isEmpty }

    public func steps(in draft: WorkflowDraft) -> [WorkflowStepResult] {
        let known = draft.ordered.map { (id: $0.id, name: $0.name, type: $0.type) }
        let extra = nodes.keys.filter { key in !known.contains { $0.id == key } }.map { (id: $0, name: $0, type: "") }
        return (known + extra).compactMap { (entry: (id: String, name: String, type: String)) -> WorkflowStepResult? in
            guard let report = nodes[entry.id]?.objectValue else { return nil }
            let outputs = report.obj("outputs").map { (name: String, value: JSONValue) in
                (name: name, count: value.int("count"), items: value.list("items"))
            }
            return WorkflowStepResult(id: entry.id, name: entry.name, type: entry.type, status: report.str("status"),
                                      error: report.str("error"), ms: report.int("ms"), outputs: outputs)
        }
    }
}

public struct WorkflowCredential: Equatable, Sendable, Identifiable {
    public var id: String
    public var integration: String
    public var name: String
    public var hint: JSONObject
    public var createdAt: Int64

    public init(json: JSONObject) {
        id = json.str("id")
        integration = json.str("integration")
        name = json.str("name")
        hint = json.obj("hint")
        createdAt = json["createdAt"]?.int64Value ?? 0
    }

    public var hintText: String { hint.map { "\($0.key) \($0.value.text)" }.joined(separator: " · ") }
}

public struct WorkflowTemplate: Equatable, Sendable, Identifiable {
    public var key: String
    public var name: String
    public var description: String
    public var tags: [String]
    public var steps: Int

    public var id: String { key }

    public init(json: JSONObject) {
        key = json.str("key")
        name = json.str("name")
        description = json.str("description")
        tags = json.list("tags").compactMap(\.stringValue)
        steps = json.int("steps")
    }
}

extension WorkflowCatalog {
    public func summary(_ node: WorkflowNode) -> String {
        let kind = kind(node.type)
        var parts: [String] = []
        switch node.type {
        case "core.http":
            parts.append([node.text("method").isEmpty ? "GET" : node.text("method"), node.text("url")].filter { !$0.isEmpty }.joined(separator: " "))
        case "core.if", "core.filter":
            let op = operators.first { $0.value == node.text("operator") }?.label ?? node.text("operator")
            parts.append([node.text("left"), op, node.text("right")].filter { !$0.isEmpty }.joined(separator: " "))
        case "core.schedule":
            parts.append(WorkflowsText.every(Int(node.text("every")) ?? 0))
        case "core.limit":
            parts.append("Keep \(node.text("max").isEmpty ? "10" : node.text("max"))")
        case "core.split":
            parts.append(node.text("field"))
        default:
            if let kind, kind.isApp {
                parts.append(kind.operations.first { $0.key == node.operation }?.name ?? kind.operations.first?.name ?? "")
            } else {
                parts.append(kind?.description ?? "")
            }
        }
        if node.disabled { parts.append("Off") }
        return parts.filter { !$0.isEmpty }.joined(separator: " · ")
    }

    public func branch(_ connection: WorkflowConnection, in draft: WorkflowDraft) -> String {
        let from = draft.node(connection.from)?.name ?? connection.from
        return connection.output == "main" ? "After \(from)" : "\(from): \(connection.output)"
    }

    public func outputs(_ type: String) -> [String] { kind(type)?.outputs ?? ["main"] }
}
