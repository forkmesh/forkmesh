// After native/android api/InboxService.kt.
//
// Signed write paths to a repo's relay inbox, mirroring the Qt client's submit*ToInbox
// functions and the worker's verify_issue_event / verify_pull_event /
// verify_pull_comment_event. Every canonical string and content layout below must match
// app/src/entry.py byte for byte or the relay rejects with 401 bad_signature.

import FMAccount
import FMBase
import FMCrypto
import FMNet
import Foundation

@MainActor
public final class InboxService {
    let settings: ApiSettings
    let identity: DeviceIdentity
    let http: HTTPClient
    let performanceMonitor: PerformanceMonitorService?
    let clock: Clock

    public init(
        settings: ApiSettings, identity: DeviceIdentity, http: HTTPClient,
        performanceMonitor: PerformanceMonitorService? = nil, clock: Clock = SystemClock()
    ) {
        self.settings = settings
        self.identity = identity
        self.http = http
        self.performanceMonitor = performanceMonitor
        self.clock = clock
    }

    var author: String { identity.publicKeyB64url }
    var now: Int64 { clock.nowMillis }

    func repoEndpoint(_ owner: String, _ name: String, _ leaf: String) -> URL {
        let server = ServerURL.parse(settings.serverUrl)
        let path = "/api/repo/\(owner)/\(name)/\(leaf)"
        return server.httpURL(path) ?? URL(string: "https://invalid.invalid" + DartURI.encodePath(path))!
    }

    nonisolated static func sha256Hex(_ content: String) -> String { Sha256.hex(Array(content.utf8)) }

    func sign(_ canonical: String) -> String { identity.sign(utf8: canonical) }

    func signingPrefix(_ kind: String, _ owner: String, _ name: String) -> String {
        EventSigning.prefix(kind, owner: owner, name: name)
    }

    /// `'${value ?? ''}'` / `(value ?? '').toString()`.
    nonisolated static func text(_ value: JSONValue?) -> String {
        guard let value, !value.isNull else { return "" }
        return DartCore.toString(value)
    }

    /// `(ev[key] as List?)?.join(',') ?? ''`.
    nonisolated static func joined(_ value: JSONValue?) -> String {
        value?.arrayValue?.map { DartCore.toString($0) }.joined(separator: ",") ?? ""
    }

    /// `'${(ev[key] as num?)?.toInt() ?? 0}'`.
    nonisolated static func intText(_ value: JSONValue?) -> String { "\(ApiParse.numInt(value))" }

    // MARK: Issues

    /// `issue_event_content()`: NUL-joined fields per event type.
    public nonisolated func issueContent(_ type: String, _ ev: JSONObject) -> String {
        let attachments = InboxService.joined(ev["attachments"])
        switch type {
        case "open": return [InboxService.text(ev["title"]), InboxService.text(ev["body"]), attachments].joined(separator: "\0")
        case "comment", "edit": return [InboxService.text(ev["body"]), attachments].joined(separator: "\0")
        case "title": return InboxService.text(ev["title"])
        case "status": return InboxService.text(ev["status"])
        case "labels": return InboxService.joined(ev["labels"])
        case "milestone": return InboxService.text(ev["milestone"])
        case "priority": return InboxService.intText(ev["priority"])
        case "progress": return InboxService.intText(ev["progress"])
        case "assignees": return InboxService.joined(ev["assignees"])
        case "delete": return InboxService.text(ev["target"])
        default: return ""
        }
    }

    func signedIssueEvent(_ owner: String, _ name: String, _ number: Int, _ type: String, _ fields: JSONObject) -> JSONObject {
        let ts = now
        var ev: JSONObject = ["type": .string(type)]
        ev.merge(fields)
        let contentHash = InboxService.sha256Hex(issueContent(type, ev))
        let canonical = signingPrefix("issue-event", owner, name) + "\(type)\n\(number)\n\(author)\n\(ts)\n\(contentHash)"
        let sig = sign(canonical)
        ev["author"] = .string(author)
        ev["ts"] = .int(ts)
        ev["sig"] = .string(sig)
        return ev
    }

    public func submitNewIssue(
        _ owner: String, _ name: String, title: String, body: String, labels: [String] = [], milestone: String = "",
        priority: Int = 0, assignees: [String] = []
    ) async throws {
        let event = signedIssueEvent(owner, name, 0, "open", [
            "title": .string(title), "body": .string(body), "attachments": .array([]),
        ])
        try await post(repoEndpoint(owner, name, "issues"), [
            "number": 0,
            "titleIfNew": .string(title),
            "event": .object(event),
            "meta": .object([
                "labels": JSONValue(labels),
                "milestone": .string(milestone),
                "priority": .int(Int64(priority)),
                "assignees": JSONValue(assignees),
            ]),
        ])
    }

    public func commentOnIssue(_ owner: String, _ name: String, _ number: Int, _ body: String) async throws {
        let event = signedIssueEvent(owner, name, number, "comment", ["body": .string(body), "attachments": .array([])])
        try await post(repoEndpoint(owner, name, "issues"), ["number": .int(Int64(number)), "event": .object(event)])
    }

    public func voteOnIssue(_ owner: String, _ name: String, _ number: Int) async throws {
        let event = signedIssueEvent(owner, name, number, "vote", JSONObject())
        try await post(repoEndpoint(owner, name, "issues"), ["number": .int(Int64(number)), "event": .object(event)])
    }

    public func setIssueStatus(_ owner: String, _ name: String, _ number: Int, _ status: String) async throws {
        let event = signedIssueEvent(owner, name, number, "status", ["status": .string(status)])
        try await post(repoEndpoint(owner, name, "issues"), ["number": .int(Int64(number)), "event": .object(event)])
    }

    // MARK: Pulls

    /// Submit a new pull request. `commits` is a `git format-patch --stdout` mbox over the same
    /// range as `patch` (empty is legitimate); the five-field content is always signed.
    public func submitNewPull(
        _ owner: String, _ name: String, title: String, base: String, head: String, patch: String,
        commits: String = "", description: String = "", authorName: String = ""
    ) async throws {
        _ = try await openNewPull(
            owner, name, title: title, base: base, head: head, patch: patch, commits: commits,
            description: description, authorName: authorName)
    }

    public func openNewPull(
        _ owner: String, _ name: String, title: String, base: String, head: String, patch: String,
        commits: String = "", description: String = "", authorName: String = ""
    ) async throws -> Int {
        let ts = now
        let content = [title, base, head, patch, commits].joined(separator: "\0")
        let contentHash = InboxService.sha256Hex(content)
        let canonical = signingPrefix("pull-event", owner, name) + "\(author)\n\(ts)\n\(contentHash)"
        let sig = sign(canonical)
        let answer = try await postAnswer(repoEndpoint(owner, name, "pulls"), [
            "pull": .object([
                "title": .string(title),
                "base": .string(base),
                "head": .string(head),
                "patch": .string(patch),
                "commits": .string(commits),
                "description": .string(description),
                "author": .string(author),
                "authorName": .string(authorName),
                "ts": .int(ts),
                "sig": .string(sig),
            ]),
        ])
        return ApiParse.numInt(answer["number"])
    }

    /// Signed patchset anchor of a line-anchored pull event, or nothing (the desktop's
    /// `PullEvent::hasAnchor()`).
    nonisolated static func pullCommentAnchorFields(_ ev: JSONObject) -> [String] {
        let patchset = text(ev["patchset"])
        let anchorLines = text(ev["anchorLines"])
        if patchset.isEmpty && anchorLines.isEmpty { return [] }
        return [patchset, anchorLines]
    }

    /// `pull_comment_content()`: NUL-joined per type.
    public nonisolated func pullCommentContent(_ type: String, _ ev: JSONObject) -> String {
        let t = InboxService.text
        switch type {
        case "comment":
            return t(ev["body"])
        case "review":
            return [t(ev["state"]), t(ev["body"])].joined(separator: "\0")
        case "line-comment":
            return ([t(ev["path"]), t(ev["side"]), InboxService.intText(ev["line"]), t(ev["body"])]
                + InboxService.pullCommentAnchorFields(ev)).joined(separator: "\0")
        case "thread-comment":
            return ([
                t(ev["threadId"]), t(ev["path"]), t(ev["side"]), InboxService.intText(ev["lineStart"]),
                InboxService.intText(ev["lineEnd"]), t(ev["body"]), t(ev["suggestionPatch"]),
            ] + InboxService.pullCommentAnchorFields(ev)).joined(separator: "\0")
        case "thread-reply":
            return [t(ev["threadId"]), t(ev["parentId"]), t(ev["body"])].joined(separator: "\0")
        case "thread-state":
            return [t(ev["threadId"]), t(ev["state"]), t(ev["body"])].joined(separator: "\0")
        case "suggestion-state":
            return [t(ev["threadId"]), t(ev["state"]), t(ev["appliedCommit"]), t(ev["body"])].joined(separator: "\0")
        default:
            return ""
        }
    }

    func signedPullComment(_ owner: String, _ name: String, _ number: Int, _ type: String, _ fields: JSONObject) -> JSONObject {
        let ts = now
        var ev: JSONObject = ["type": .string(type)]
        ev.merge(fields)
        let contentHash = InboxService.sha256Hex(pullCommentContent(type, ev))
        let canonical = signingPrefix("pull-comment", owner, name) + "\(type)\n\(number)\n\(author)\n\(ts)\n\(contentHash)"
        let sig = sign(canonical)
        ev["author"] = .string(author)
        ev["ts"] = .int(ts)
        ev["sig"] = .string(sig)
        return ev
    }

    public func commentOnPull(_ owner: String, _ name: String, _ number: Int, _ body: String) async throws {
        let event = signedPullComment(owner, name, number, "comment", ["body": .string(body)])
        try await post(repoEndpoint(owner, name, "pulls"), ["number": .int(Int64(number)), "event": .object(event)])
    }

    /// `state` is "approve" | "request-changes" | "comment".
    public func reviewPull(_ owner: String, _ name: String, _ number: Int, _ state: String, _ body: String) async throws {
        let event = signedPullComment(owner, name, number, "review", ["state": .string(state), "body": .string(body)])
        try await post(repoEndpoint(owner, name, "pulls"), ["number": .int(Int64(number)), "event": .object(event)])
    }

    // MARK: Discussions

    nonisolated static func discussionContent(_ type: String, _ ev: JSONObject) -> String {
        switch type {
        case "open": return [text(ev["title"]), text(ev["body"]), text(ev["category"])].joined(separator: "\0")
        case "comment": return text(ev["body"])
        case "answer": return text(ev["target"])
        default: return ""
        }
    }

    func signedDiscussionEvent(_ owner: String, _ name: String, _ number: Int, _ type: String, _ fields: JSONObject) -> JSONObject {
        let ts = now
        var ev: JSONObject = ["type": .string(type)]
        ev.merge(fields)
        let contentHash = InboxService.sha256Hex(InboxService.discussionContent(type, ev))
        let canonical = signingPrefix("discussion-event", owner, name) + "\(type)\n\(number)\n\(author)\n\(ts)\n\(contentHash)"
        let sig = sign(canonical)
        ev["author"] = .string(author)
        ev["ts"] = .int(ts)
        ev["sig"] = .string(sig)
        return ev
    }

    public func submitNewDiscussion(
        _ owner: String, _ name: String, title: String, body: String, category: String = "general"
    ) async throws {
        let event = signedDiscussionEvent(owner, name, 0, "open", [
            "title": .string(title), "body": .string(body), "category": .string(category),
        ])
        try await post(repoEndpoint(owner, name, "discussions"), [
            "number": 0, "titleIfNew": .string(title), "event": .object(event),
        ])
    }

    public func commentOnDiscussion(_ owner: String, _ name: String, _ number: Int, _ body: String) async throws {
        let event = signedDiscussionEvent(owner, name, number, "comment", ["body": .string(body)])
        try await post(repoEndpoint(owner, name, "discussions"), ["number": .int(Int64(number)), "event": .object(event)])
    }

    public func markDiscussionAnswer(
        _ owner: String, _ name: String, _ number: Int, target: String, sessionToken: String
    ) async throws {
        let event = signedDiscussionEvent(
            owner, name, number, "answer", ["id": .string("answer-ios-\(now)"), "target": .string(target)])
        var body: JSONObject = ["number": .int(Int64(number)), "event": .object(event)]
        if !sessionToken.isEmpty { body["sessionToken"] = .string(sessionToken) }
        try await post(repoEndpoint(owner, name, "discussions"), body)
    }

    // MARK: Subscriptions

    /// `source` is "issue" or "pull". Signs the thread subscription contract so replies fan out
    /// into the notification inbox. Throws `ApiArgumentError` for a bad node, source or number.
    public func setThreadSubscription(
        _ owner: String, _ name: String, node: String, source: String, number: Int, subscribed: Bool
    ) async throws {
        let cleanNode = node.dartTrimmed.lowercased()
        if cleanNode.isEmpty { throw ApiArgumentError(value: node, name: "node", message: "expected account node name") }
        if source != "issue" && source != "pull" {
            throw ApiArgumentError(value: source, name: "source", message: "expected issue or pull")
        }
        if number <= 0 { throw ApiArgumentError(value: "\(number)", name: "number", message: "expected positive thread number") }
        let ts = "\(now)"
        let canonical = "forkmesh-subscribe-v1\n\(owner)/\(name)\n\(source)\n\(number)\n\(subscribed ? "1" : "0")\n\(ts)"
        let sig = sign(canonical)
        try await post(repoEndpoint(owner, name, "subscribe"), [
            "node": .string(cleanNode),
            "source": .string(source),
            "number": .int(Int64(number)),
            "subscribed": .bool(subscribed),
            "ts": .string(ts),
            "sig": .string(sig),
        ])
    }

    // MARK: Transport

    /// POST without a session (20 s). 200 and 201 succeed; anything else throws `ApiException`
    /// with the Worker's `error` or "HTTP n".
    func post(_ url: URL, _ body: JSONObject) async throws {
        _ = try await postAnswer(url, body)
    }

    func postAnswer(_ url: URL, _ body: JSONObject) async throws -> JSONObject {
        let request = HTTPRequest(
            method: "POST", url: url, headers: ["Content-Type": "application/json"],
            body: JSON.encodeData(.object(body)), timeout: 20)
        let http = self.http
        let send: @MainActor () async throws -> JSONObject = {
            let response = try await http.send(request)
            if response.status == 200 || response.status == 201 { return ApiParse.jsonMap(response.body) }
            var detail = "HTTP \(response.status)"
            if let decoded = try? JSON.parse(response.body), let error = decoded["error"], !error.isNull {
                detail = DartCore.toString(error)
            }
            throw ApiException(detail)
        }
        guard let monitor = performanceMonitor else { return try await send() }
        let path = ApiService.encodedPath(url)
        return try await monitor.track("inbox.POST \(path)", details: ["path": .string(path), "host": .string(url.host ?? "")], send)
    }
}
