import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation

public struct CIError: Error, Equatable, Sendable, CustomStringConvertible {
    public let status: Int
    public let code: String

    public init(status: Int, code: String) {
        self.status = status
        self.code = code
    }

    public var description: String {
        switch (status, code) {
        case (_, "forbidden"), (_, "not_authorized"), (403, _): return "Only the repository owner and organization writers can do that."
        case (_, "actions_node_unavailable"), (503, _): return "The node that runs this repository's actions is not reachable right now."
        case (_, "invalid_session"), (401, _): return "Sign in again to manage this repository."
        case (404, _), (_, "not_found"): return "This repository was not found, or it is private."
        case (_, "request_too_large"): return "That request is too large."
        case (_, "invalid_url"): return "Use a public https:// address."
        case (_, "no_events"): return "Choose at least one event."
        case (_, "duplicate_url"): return "A webhook for that address already exists."
        case (_, "too_many_webhooks"): return "This repository already has the most webhooks it can have."
        case (_, "invalid_request"): return "The server refused that request as invalid."
        case (_, "not_due"): return "This repository was scanned in the last day."
        case (_, "lease_held"): return "A scan is already running."
        case (429, _), (_, "retry_backoff"): return "Too many attempts. Try again later."
        default:
            if !code.isEmpty { return code.replacingOccurrences(of: "_", with: " ").capitalized + "." }
            return "The server answered \(status)."
        }
    }
}

@MainActor
public protocol CIClient: AnyObject {
    func actionsRuns(_ repo: RepoRef) async throws -> ActionsSummary
    func actionsControl(_ repo: RepoRef, _ request: ActionsControlRequest) async throws
    func webhooks(_ repo: RepoRef, action: String, fields: JSONObject) async throws -> JSONObject
    func securityScans(_ repo: RepoRef, operation: String, query: [(String, String)]) async throws -> JSONObject
    func securityTriage(_ repo: RepoRef, scanId: String, findingId: String, status: String, note: String) async throws -> JSONObject
    func releases(_ owner: String, _ name: String) async throws -> [RepoRelease]
    func mirrorsPayload(_ repo: RepoRef) async throws -> JSONObject
    func clearRepoCache(_ owner: String, _ name: String)
    func announceRelease(_ repo: RepoRef, tag: String, title: String, body: String, account: String, identity: DeviceIdentity, nowMillis: Int64) async throws
}

public enum CIRequests {
    public static func repoPath(_ repo: RepoRef, _ tail: String) -> String {
        "/api/repo/\(repo.owner)/\(repo.name)/\(tail)"
    }

    public static func decode(_ response: HTTPResponse) throws -> JSONObject {
        var map = JSONObject()
        if !response.body.isEmpty, let parsed = try? JSON.parse(response.body), let object = parsed.objectValue {
            map = object
        }
        if response.ok && map["error"] == nil { return map }
        throw CIError(status: response.status, code: map.str("error").isEmpty ? map.str("reason") : map.str("error"))
    }

    public static func announceCanonical(account: String, ts: String) -> String {
        "forkmesh-issues-pull-v1\n\(account)\n\(ts)"
    }
}

extension ApiService: CIClient {
    private func ciSend(_ method: String, _ path: String, query: [(String, String)] = [], body: JSONObject? = nil) async throws -> JSONObject {
        var headers: HTTPHeaders = ["Accept": "application/json"]
        if body != nil { headers["Content-Type"] = "application/json" }
        let response = try await rawRequest(method, url(path, query), headers: headers, body: body.map { JSON.encodeData(.object($0)) })
        return try CIRequests.decode(response)
    }

    public func actionsRuns(_ repo: RepoRef) async throws -> ActionsSummary {
        ActionsSummary(json: try await ciSend("GET", CIRequests.repoPath(repo, "actions/runs")))
    }

    public func actionsControl(_ repo: RepoRef, _ request: ActionsControlRequest) async throws {
        _ = try await ciSend("POST", CIRequests.repoPath(repo, "actions/control"), body: request.body)
    }

    public func webhooks(_ repo: RepoRef, action: String, fields: JSONObject) async throws -> JSONObject {
        var body = fields
        body["action"] = .string(action)
        return try await ciSend("POST", CIRequests.repoPath(repo, "webhooks"), body: body)
    }

    public func securityScans(_ repo: RepoRef, operation: String, query: [(String, String)]) async throws -> JSONObject {
        try await ciSend("GET", CIRequests.repoPath(repo, "security-scans/\(operation)"), query: query)
    }

    public func securityTriage(_ repo: RepoRef, scanId: String, findingId: String, status: String, note: String) async throws -> JSONObject {
        var body: JSONObject = ["findingId": .string(findingId), "status": .string(status)]
        if !scanId.isEmpty { body["scanId"] = .string(scanId) }
        if !note.isEmpty { body["note"] = .string(note) }
        return try await ciSend("PATCH", CIRequests.repoPath(repo, "security-scans/triage"), body: body)
    }

    public func mirrorsPayload(_ repo: RepoRef) async throws -> JSONObject {
        try await ciSend("GET", CIRequests.repoPath(repo, "mirrors"))
    }

    public func announceRelease(
        _ repo: RepoRef, tag: String, title: String, body: String, account: String, identity: DeviceIdentity, nowMillis: Int64
    ) async throws {
        let ts = String(nowMillis)
        let sig = identity.sign(Array(Utf8.encode(CIRequests.announceCanonical(account: account, ts: ts))))
        _ = try await ciSend(
            "POST", CIRequests.repoPath(repo, "ap-publish"),
            query: [("owner", account), ("ts", ts), ("sig", sig)],
            body: [
                "kind": "release", "eventType": "publish", "manual": true, "tag": .string(tag),
                "title": .string(String(title.prefix(240))), "body": .string(String(body.prefix(4000))),
                "author": .string(account),
            ])
    }

    public func announcePost(
        _ repo: RepoRef, title: String, body: String, account: String, identity: DeviceIdentity, nowMillis: Int64
    ) async throws {
        let ts = String(nowMillis)
        let sig = identity.sign(Array(Utf8.encode(CIRequests.announceCanonical(account: account, ts: ts))))
        _ = try await ciSend(
            "POST", CIRequests.repoPath(repo, "ap-publish"),
            query: [("owner", account), ("ts", ts), ("sig", sig)],
            body: [
                "kind": "discussion", "eventType": "open", "manual": true,
                "title": .string(String(title.prefix(240))), "body": .string(String(body.prefix(4000))),
                "author": .string(account),
            ])
    }
}
