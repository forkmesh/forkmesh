import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation

public enum CollabContent {
    static func text(_ value: JSONValue?) -> String {
        guard let value, !value.isNull else { return "" }
        return value.text
    }

    static func intText(_ value: JSONValue?) -> String {
        guard let value, !value.isNull else { return "0" }
        if let number = value.int64Value { return "\(number)" }
        if let number = value.doubleValue, number.isFinite { return "\(Int64(number.rounded(.towardZero)))" }
        if let parsed = Int64(value.text.dartTrimmed) { return "\(parsed)" }
        return "0"
    }

    static func joined(_ value: JSONValue?) -> String {
        value?.arrayValue?.map(\.text).joined(separator: ",") ?? ""
    }

    public static func issue(_ type: String, _ ev: JSONObject) -> String {
        let attachments = joined(ev["attachments"])
        switch type {
        case "open": return [text(ev["title"]), text(ev["body"]), attachments].joined(separator: "\0")
        case "comment", "edit": return [text(ev["body"]), attachments].joined(separator: "\0")
        case "title": return text(ev["title"])
        case "status": return text(ev["status"])
        case "labels": return joined(ev["labels"])
        case "milestone": return text(ev["milestone"])
        case "dates": return [intText(ev["startDate"]), intText(ev["endDate"])].joined(separator: "\0")
        case "priority": return intText(ev["priority"])
        case "progress": return intText(ev["progress"])
        case "assignees": return joined(ev["assignees"])
        case "claim": return [text(ev["assignee"]), text(ev["claimState"])].joined(separator: "\0")
        case "reporter": return text(ev["reporter"])
        case "reaction": return [text(ev["target"]), text(ev["reaction"])].joined(separator: "\0")
        case "delete": return text(ev["target"])
        default: return ""
        }
    }

    public static func pull(_ type: String, _ ev: JSONObject) -> String {
        let anchor: [String] = {
            let patchset = text(ev["patchset"])
            let lines = text(ev["anchorLines"])
            return patchset.isEmpty && lines.isEmpty ? [] : [patchset, lines]
        }()
        switch type {
        case "comment": return text(ev["body"])
        case "review": return [text(ev["state"]), text(ev["body"])].joined(separator: "\0")
        case "line-comment":
            return ([text(ev["path"]), text(ev["side"]), intText(ev["line"]), text(ev["body"])] + anchor)
                .joined(separator: "\0")
        case "thread-comment":
            return ([
                text(ev["threadId"]), text(ev["path"]), text(ev["side"]), intText(ev["lineStart"]),
                intText(ev["lineEnd"]), text(ev["body"]), text(ev["suggestionPatch"]),
            ] + anchor).joined(separator: "\0")
        case "thread-reply": return [text(ev["threadId"]), text(ev["parentId"]), text(ev["body"])].joined(separator: "\0")
        case "thread-state": return [text(ev["threadId"]), text(ev["state"]), text(ev["body"])].joined(separator: "\0")
        case "suggestion-state":
            return [text(ev["threadId"]), text(ev["state"]), text(ev["appliedCommit"]), text(ev["body"])]
                .joined(separator: "\0")
        default: return ""
        }
    }

    public static func discussion(_ type: String, _ ev: JSONObject) -> String {
        switch type {
        case "open": return [text(ev["title"]), text(ev["body"]), text(ev["category"])].joined(separator: "\0")
        case "comment": return text(ev["body"])
        default: return ""
        }
    }

    public static let pullEventTypes: Set<String> = [
        "comment", "review", "line-comment", "thread-comment", "thread-reply", "thread-state", "suggestion-state",
    ]

    public static let issueEventTypes: Set<String> = [
        "open", "comment", "edit", "title", "status", "labels", "milestone", "dates", "priority", "progress",
        "assignees", "claim", "reporter", "reaction", "delete", "vote",
    ]
}

public enum CollabRecordKind: String, Sendable, CaseIterable {
    case issue, pull, discussion

    public var leaf: String {
        switch self {
        case .issue: return "issues"
        case .pull: return "pulls"
        case .discussion: return "discussions"
        }
    }

    public var signingKind: String {
        switch self {
        case .issue: return "issue-event"
        case .pull: return "pull-comment"
        case .discussion: return "discussion-event"
        }
    }

    public func content(_ type: String, _ ev: JSONObject) -> String {
        switch self {
        case .issue: return CollabContent.issue(type, ev)
        case .pull: return CollabContent.pull(type, ev)
        case .discussion: return CollabContent.discussion(type, ev)
        }
    }
}

public struct CollabSignError: Error, Equatable, CustomStringConvertible {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var description: String { message }
}

public struct CollabSigner: Sendable {
    public let identity: DeviceIdentity
    public let clock: Clock
    public let authorName: String

    public init(identity: DeviceIdentity, clock: Clock = SystemClock(), authorName: String = "") {
        self.identity = identity
        self.clock = clock
        self.authorName = authorName
    }

    public var author: String { identity.publicKeyB64url }

    public func randomId() -> String { Hex.encode(Bytes.random(9)) }

    public func canonical(
        _ kind: CollabRecordKind, owner: String, name: String, type: String, number: Int, ts: Int64, event: JSONObject
    ) -> String {
        let hash = Sha256.hex(Array(kind.content(type, event).utf8))
        let prefix = EventSigning.prefix(kind.signingKind, owner: owner, name: name)
        return "\(prefix)\(type)\n\(number)\n\(author)\n\(ts)\n\(hash)"
    }

    public func sign(
        _ kind: CollabRecordKind, owner: String, name: String, number: Int, type: String, fields: JSONObject
    ) throws -> JSONObject {
        let allowed: Set<String>
        switch kind {
        case .issue: allowed = CollabContent.issueEventTypes
        case .pull: allowed = CollabContent.pullEventTypes
        case .discussion: allowed = ["open", "comment"]
        }
        guard allowed.contains(type) else { throw CollabSignError("Unsupported \(kind.rawValue) event: \(type).") }
        return signed(type: type, fields: fields) { ts, ev in
            canonical(kind, owner: owner, name: name, type: type, number: number, ts: ts, event: ev)
        }
    }

    public static let branchSigningKind = "branch-comment"
    public static let branchEventTypes: Set<String> = ["thread-comment", "thread-reply", "thread-state"]

    public static func branchCanonical(
        owner: String, name: String, branch: String, type: String, author: String, ts: Int64, event: JSONObject
    ) -> String {
        let hash = Sha256.hex(Array(CollabContent.pull(type, event).utf8))
        let prefix = EventSigning.prefix(branchSigningKind, owner: owner, name: name)
        return "\(prefix)\(type)\n\(branch)\n\(author)\n\(ts)\n\(hash)"
    }

    public func signBranch(owner: String, name: String, branch: String, type: String, fields: JSONObject) throws -> JSONObject {
        guard CollabSigner.branchEventTypes.contains(type) else { throw CollabSignError("Unsupported branch event: \(type).") }
        guard !EventSigning.repoKey(owner, name).isEmpty else { throw CollabSignError("Branch comments need a repository name.") }
        guard !branch.isEmpty, !branch.contains("\n"), !branch.contains("\0") else {
            throw CollabSignError("Branch comments need a branch name.")
        }
        let author = self.author
        return signed(type: type, fields: fields) { ts, ev in
            CollabSigner.branchCanonical(owner: owner, name: name, branch: branch, type: type, author: author, ts: ts, event: ev)
        }
    }

    func signed(type: String, fields: JSONObject, canonical: (Int64, JSONObject) -> String) -> JSONObject {
        let ts = clock.nowMillis
        var ev: JSONObject = ["type": .string(type)]
        ev.merge(fields)
        if ev.str("id").isEmpty { ev["id"] = .string(ReviewComposer.eventId(type: type, ts: ts, random: randomId())) }
        let sig = identity.sign(utf8: canonical(ts, ev))
        ev["author"] = .string(author)
        if !authorName.isEmpty { ev["authorName"] = .string(authorName) }
        ev["ts"] = .int(ts)
        ev["sig"] = .string(sig)
        return ev
    }
}

@MainActor
public protocol CollabTransport: AnyObject {
    func url(_ path: String, _ query: [(String, String)]) -> URL
    func rawRequest(_ method: String, _ url: URL, headers: HTTPHeaders, body: Data?) async throws -> HTTPResponse
}

extension ApiService: CollabTransport {}

public enum CollabHTTP {
    public static func errorMessage(_ response: HTTPResponse) -> String {
        let decoded = JSON.parseObject(response.body)
        let message = decoded.str("message").dartTrimmed
        if !message.isEmpty { return message }
        let code = decoded.str("error").dartTrimmed
        switch code {
        case "bad_signature": return "The relay could not verify this device's signature."
        case "inbox_full", "author_quota": return "The repository's inbox is full. Try again later."
        case "event_too_large": return "That is longer than the relay accepts."
        case "unauthorized", "invalid_session", "account_required": return "Sign in again and retry."
        case "not_authorized", "forbidden": return "Only the repository owner can do that."
        case "not_found": return "That repository or record was not found."
        case "": return "The relay answered HTTP \(response.status)."
        default: return code.replacingOccurrences(of: "_", with: " ")
        }
    }

    @MainActor
    public static func json(
        _ transport: CollabTransport, _ method: String, _ path: String, query: [(String, String)] = [],
        body: JSONObject? = nil
    ) async throws -> JSONObject {
        var headers: HTTPHeaders = ["Accept": "application/json"]
        if body != nil { headers["Content-Type"] = "application/json" }
        let response = try await transport.rawRequest(
            method, transport.url(path, query), headers: headers, body: body.map { JSON.encodeData(.object($0)) })
        let decoded = JSON.parseObject(response.body)
        if !response.ok || decoded["ok"] == .bool(false) { throw ApiException(errorMessage(response)) }
        return decoded
    }
}

@MainActor
public final class CollabRecordWriter {
    public let transport: CollabTransport
    public let signer: CollabSigner?
    public let owner: String
    public let name: String

    public init(transport: CollabTransport, signer: CollabSigner?, owner: String, name: String) {
        self.transport = transport
        self.signer = signer
        self.owner = owner
        self.name = name
    }

    func requireSigner() throws -> CollabSigner {
        guard let signer else { throw CollabInboxUnavailable() }
        return signer
    }

    @discardableResult
    public func send(_ kind: CollabRecordKind, number: Int, type: String, fields: JSONObject, extra: JSONObject = [:])
        async throws -> JSONObject
    {
        let event = try requireSigner().sign(kind, owner: owner, name: name, number: number, type: type, fields: fields)
        var body: JSONObject = ["number": .int(Int64(number)), "event": .object(event)]
        body.merge(extra)
        _ = try await CollabHTTP.json(transport, "POST", "/api/repo/\(owner)/\(name)/\(kind.leaf)", body: body)
        return event
    }

    @discardableResult
    public func sendPull(_ number: Int, _ fields: ReviewEventFields) async throws -> JSONObject {
        try await send(.pull, number: number, type: fields.type, fields: fields.fields)
    }

    @discardableResult
    public func sendBranch(_ branch: String, _ fields: ReviewEventFields) async throws -> JSONObject {
        let event = try requireSigner().signBranch(owner: owner, name: name, branch: branch, type: fields.type, fields: fields.fields)
        let body: JSONObject = ["branch": .string(branch), "event": .object(event)]
        _ = try await CollabHTTP.json(transport, "POST", "/api/repo/\(owner)/\(name)/\(CollabRecordKind.pull.leaf)", body: body)
        return event
    }

    public func submitReview(_ number: Int, _ events: [ReviewEventFields]) async throws -> Int {
        var sent = 0
        for fields in events {
            try await sendPull(number, fields)
            sent += 1
        }
        return sent
    }

    @discardableResult
    public func sendIssue(_ number: Int, _ edit: IssueEdit) async throws -> JSONObject {
        try await send(.issue, number: number, type: edit.type, fields: edit.fields)
    }

    public func applyIssue(_ number: Int, _ edits: [IssueEdit]) async throws {
        for edit in edits { try await sendIssue(number, edit) }
    }

    public func openDiscussion(title: String, body: String, category: String) async throws {
        try await send(
            .discussion, number: 0, type: "open",
            fields: ["title": .string(title), "body": .string(body), "category": .string(category)],
            extra: ["titleIfNew": .string(title)])
    }

    public func commentOnDiscussion(_ number: Int, body: String) async throws {
        try await send(.discussion, number: number, type: "comment", fields: ["body": .string(body)])
    }
}
