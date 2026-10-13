// The node-sync models: NodeSyncMode, NodeSyncConnection, NodeSyncKind (+ NodeSyncKindLabel),
// NodeSyncItem, NodeSyncRepoState and NodeSyncPayload, the decoded shape of one signed
// `GET /api/sync`.

import FMBase
import Foundation

/// Why the phone is (or is not) acting as this account's node.
public enum NodeSyncMode: String, Sendable, CaseIterable {
    /// Signed out, signed in without a `mobile_node` + `owner_sign` bind, or the user has not
    /// turned on "Serve as a node" in Settings (it is off by default).
    case disabled

    /// Bound as a mobile node, but a desktop node is enrolled on this account.
    ///
    /// `GET /api/sync` is destructive on the relay: it claims every issue and pull inbox row for
    /// the signing key for five minutes, and it deletes (drain-on-read) the About edits and
    /// legacy agent prompts it returns. Two nodes polling the same account therefore starve each
    /// other's drains. When a desktop node exists it is the source of truth and this service
    /// stays quiet.
    case standby

    /// This phone is the only node on the account: it polls and holds the socket.
    case active
}

/// Live-ness of the `/api/nodes/events` advisory socket.
public enum NodeSyncConnection: String, Sendable, CaseIterable {
    case offline, connecting, live
}

/// The kinds `/api/sync` can carry, in the order the status card lists them.
public enum NodeSyncKind: String, Sendable, CaseIterable, Hashable {
    case issues, pulls, discussions, agentPrompts, about, branchOps

    /// The relay's key for the kind (`about` is `aboutUpdate`).
    public var wireName: String {
        switch self {
        case .issues: return "issues"
        case .pulls: return "pulls"
        case .discussions: return "discussions"
        case .agentPrompts: return "agentPrompts"
        case .about: return "aboutUpdate"
        case .branchOps: return "branchOps"
        }
    }

    /// Singular or plural noun for `count` ("1 issue", "2 About edits").
    public func label(_ count: Int) -> String {
        switch self {
        case .issues: return count == 1 ? "issue" : "issues"
        case .pulls: return count == 1 ? "pull" : "pulls"
        case .discussions: return count == 1 ? "discussion" : "discussions"
        case .agentPrompts: return count == 1 ? "agent prompt" : "agent prompts"
        case .about: return count == 1 ? "About edit" : "About edits"
        case .branchOps: return count == 1 ? "branch operation" : "branch operations"
        }
    }

    /// True when the relay drains this kind by exact row id, so an ack can name precisely what
    /// was applied. See `NodeSyncService.ackableKinds`.
    public var isAckable: Bool { self == .issues || self == .pulls || self == .discussions }

    /// The kind whose `wireName` is `wire`, or nil.
    public static func fromWireName(_ wire: String) -> NodeSyncKind? {
        allCases.first { $0.wireName == wire }
    }
}

/// One pending collaboration item drained from a repository's relay inbox.
public struct NodeSyncItem: Equatable, Sendable {
    public let owner: String
    public let repo: String
    public let kind: NodeSyncKind
    /// The inbox row id, used verbatim in the `?ids=` ack. `0` when the relay did not carry one
    /// (agent prompts and About edits, which it drains itself).
    public let id: Int
    public let data: JSONObject

    public init(owner: String, repo: String, kind: NodeSyncKind, id: Int, data: JSONObject) {
        self.owner = owner
        self.repo = repo
        self.kind = kind
        self.id = id
        self.data = data
    }

    public var repoFullName: String { "\(owner)/\(repo)" }

    /// Best-effort human label. The relay's row shapes differ per table, so this reads the
    /// fields that exist rather than assuming one schema.
    public var title: String {
        for key in ["title", "titleIfNew", "name", "about"] {
            let value = (data[key]?.text ?? "").dartTrimmed
            if !value.isEmpty { return value }
        }
        if let event = data["event"]?.objectValue {
            for key in ["title", "body"] {
                let value = (event[key]?.text ?? "").dartTrimmed
                if !value.isEmpty { return value }
            }
        }
        let number = numberOrZero
        if number > 0 { return "#\(number)" }
        return kind.label(1)
    }

    /// `number`, else `pullNumber`, else `issueNumber`, as an int (0 when none parses).
    public var numberOrZero: Int {
        let raw = NodeSyncParse.nonNull(data["number"]) ?? NodeSyncParse.nonNull(data["pullNumber"])
            ?? NodeSyncParse.nonNull(data["issueNumber"])
        return NodeSyncParse.int(raw)
    }

    /// The carried-item cache form.
    public func toJSON() -> JSONObject {
        [
            "owner": .string(owner),
            "repo": .string(repo),
            "kind": .string(kind.wireName),
            "id": .int(Int64(id)),
            "data": .object(data),
        ]
    }

    /// Reads `toJSON`'s shape back; nil for an unknown kind.
    public static func fromJSON(_ json: JSONObject) -> NodeSyncItem? {
        let wire = json["kind"]?.text ?? ""
        guard let kind = NodeSyncKind.fromWireName(wire) else { return nil }
        return NodeSyncItem(
            owner: json["owner"]?.text ?? "",
            repo: json["repo"]?.text ?? "",
            kind: kind,
            id: NodeSyncParse.numInt(json["id"]),
            data: json["data"]?.objectValue ?? JSONObject())
    }
}

/// The relay's pinned view of one repository, plus its pending inbox.
public struct NodeSyncRepoState: Equatable, Sendable {
    public let owner: String
    public let name: String
    public let stateHash: String
    public let updatedAt: String
    public let source: String
    public let items: [NodeSyncItem]
    /// Present when the relay returned owner-sealed agent prompts, which are acknowledged through
    /// this path after a host decrypts and runs them. The phone never posts to it.
    public let agentPromptAckPath: String
    public let mirrorMerges: [JSONObject]

    public init(
        owner: String, name: String, stateHash: String = "", updatedAt: String = "", source: String = "",
        items: [NodeSyncItem] = [], agentPromptAckPath: String = "", mirrorMerges: [JSONObject] = []
    ) {
        self.owner = owner
        self.name = name
        self.stateHash = stateHash
        self.updatedAt = updatedAt
        self.source = source
        self.items = items
        self.agentPromptAckPath = agentPromptAckPath
        self.mirrorMerges = mirrorMerges
    }

    public var fullName: String { "\(owner)/\(name)" }

    public func ofKind(_ kind: NodeSyncKind) -> [NodeSyncItem] { items.filter { $0.kind == kind } }
}

/// One decoded `GET /api/sync` response.
public struct NodeSyncPayload: Equatable, Sendable {
    public let serverTime: Int64
    public let repos: [NodeSyncRepoState]

    public init(serverTime: Int64 = 0, repos: [NodeSyncRepoState] = []) {
        self.serverTime = serverTime
        self.repos = repos
    }

    public var items: [NodeSyncItem] { repos.flatMap(\.items) }

    public func countOf(_ kind: NodeSyncKind) -> Int { items.filter { $0.kind == kind }.count }

    /// The list-valued kinds, in the order they are read. Branch operations can only be applied
    /// by a node with the repository's Git checkout, which is never this phone, so they are
    /// carried locally rather than dropped: the relay hands them over exactly once.
    static let kindKeys: [(String, NodeSyncKind)] = [
        ("issues", .issues),
        ("pulls", .pulls),
        ("discussions", .discussions),
        ("agentPrompts", .agentPrompts),
        ("branchOps", .branchOps),
    ]

    /// Parses the Worker's `{ok, serverTime, repos:[...]}` body. Anything the relay adds later is
    /// ignored rather than fatal.
    public static func parse(_ decoded: JSONValue?) -> NodeSyncPayload {
        guard let body = decoded?.objectValue else { return NodeSyncPayload() }
        var repos: [NodeSyncRepoState] = []
        for row in body["repos"]?.arrayValue ?? [] {
            guard let entry = row.objectValue else { continue }
            let owner = entry["owner"]?.text ?? ""
            let name = entry["name"]?.text ?? ""
            if name.isEmpty { continue }
            var items: [NodeSyncItem] = []
            for (key, kind) in kindKeys {
                for raw in entry[key]?.arrayValue ?? [] {
                    guard let data = raw.objectValue else { continue }
                    let id = NodeSyncParse.nonNull(data["id"]) ?? NodeSyncParse.nonNull(data["queueId"])
                    items.append(NodeSyncItem(
                        owner: owner, repo: name, kind: kind,
                        // Only issues/pulls/discussions carry a drainable inbox row id; an agent
                        // prompt's `queueId` names an already-deleted row.
                        id: kind == .agentPrompts ? 0 : NodeSyncParse.int(id),
                        data: data))
                }
            }
            if let about = entry["aboutUpdate"]?.objectValue {
                items.append(NodeSyncItem(owner: owner, repo: name, kind: .about, id: 0, data: about))
            }
            repos.append(NodeSyncRepoState(
                owner: owner,
                name: name,
                stateHash: entry["stateHash"]?.text ?? "",
                updatedAt: entry["updatedAt"]?.text ?? "",
                source: entry["source"]?.text ?? "",
                items: items,
                agentPromptAckPath: entry["agentPromptAckPath"]?.text ?? "",
                mirrorMerges: (entry["mirrorMerges"]?.arrayValue ?? []).compactMap(\.objectValue)))
        }
        return NodeSyncPayload(serverTime: Int64(NodeSyncParse.numInt(body["serverTime"])), repos: repos)
    }
}

/// The lenient JSON reads the node-sync code shares.
enum NodeSyncParse {
    /// `value` unless it is missing or JSON null.
    static func nonNull(_ value: JSONValue?) -> JSONValue? {
        guard let value, !value.isNull else { return nil }
        return value
    }

    /// A JSON number truncated toward zero; 0 for anything else.
    static func numInt(_ value: JSONValue?) -> Int {
        switch value {
        case .int(let n)?: return Int(truncatingIfNeeded: n)
        case .double(let d)?: return d.isFinite ? Int(exactly: d.rounded(.towardZero)) ?? 0 : 0
        default: return 0
        }
    }

    /// A JSON number truncated toward zero, any other value's text through `dartIntTryParse`;
    /// 0 when missing or unparsable.
    static func int(_ value: JSONValue?) -> Int {
        switch value {
        case .int?, .double?: return numInt(value)
        case nil: return 0
        case let other?: return dartIntTryParse(other.text) ?? 0
        }
    }

    /// Integer parsing without a radix: surrounding whitespace is ignored, an optional sign, then
    /// decimal digits or a `0x` hex literal.
    static func dartIntTryParse(_ source: String) -> Int? {
        var text = Substring(source.dartTrimmed)
        var negative = false
        if let first = text.first, first == "+" || first == "-" {
            negative = first == "-"
            text = text.dropFirst()
        }
        var radix = 10
        if text.hasPrefix("0x") || text.hasPrefix("0X") {
            radix = 16
            text = text.dropFirst(2)
        }
        if text.isEmpty { return nil }
        guard text.allSatisfy({ $0.isASCII && $0.isHexDigit && (radix == 16 || $0.isNumber) }) else { return nil }
        guard let magnitude = Int(text, radix: radix) else { return nil }
        return negative ? -magnitude : magnitude
    }

    /// The value's trimmed text; "" when missing or null.
    static func string(_ value: JSONValue?) -> String {
        guard let value, !value.isNull else { return "" }
        return value.text.dartTrimmed
    }

    /// The first `count` UTF-16 code units of `text`.
    static func utf16Prefix(_ text: String, _ count: Int) -> String {
        let units = Array(text.utf16)
        if units.count <= count { return text }
        return String(decoding: units.prefix(count), as: UTF16.self)
    }
}
