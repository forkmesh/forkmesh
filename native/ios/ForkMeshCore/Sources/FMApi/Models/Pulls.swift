import Foundation
import FMBase

public struct PullRequest: Equatable, Hashable, Sendable, Identifiable {
    public var number: Int
    public var title: String
    /// "open", "merged" or "closed".
    public var status: String
    public var body: String
    public var author: String
    public var baseBranch: String
    public var headBranch: String
    /// Epoch ms the PR was opened; 0 when unknown.
    public var createdMs: Int

    public var id: Int { number }

    public init(
        number: Int, title: String, status: String = "open", body: String = "", author: String = "",
        baseBranch: String = "", headBranch: String = "", createdMs: Int = 0
    ) {
        self.number = number
        self.title = title
        self.status = status
        self.body = body
        self.author = author
        self.baseBranch = baseBranch
        self.headBranch = headBranch
        self.createdMs = createdMs
    }

    public init(json j: JSONObject) {
        self.init(
            number: ModelJSON.int(j["number"]),
            title: j.str("title"),
            status: j.str("status", "open"),
            body: j.str("body"),
            author: j.str("author"),
            baseBranch: j.firstStr("base", "baseBranch"),
            headBranch: j.firstStr("head", "headBranch"),
            createdMs: ModelJSON.int(ModelJSON.coalesce(j["createdAt"], j["ts"], j["submittedAt"]) ?? .int(0))
        )
    }

    /// The local yyyy-MM-dd the PR was opened, or "".
    public func createdDate(timeZone: TimeZone = .current) -> String {
        createdMs <= 0 ? "" : ModelJSON.localDate(createdMs, timeZone: timeZone)
    }

    public var createdDate: String { createdDate(timeZone: .current) }
}

public struct PullEvent: Equatable, Hashable, Sendable {
    public var type: String
    public var body: String
    public var author: String
    public var authorName: String
    public var state: String
    public var ts: Int

    public init(
        type: String, body: String = "", author: String = "", authorName: String = "", state: String = "",
        ts: Int = 0
    ) {
        self.type = type
        self.body = body
        self.author = author
        self.authorName = authorName
        self.state = state
        self.ts = ts
    }

    public var displayAuthor: String { authorName.isEmpty ? author : authorName }

    public var displayTitle: String {
        if type == "review" {
            switch state {
            case "approve", "approved": return "Approved"
            case "request-changes", "changes-requested", "changes_requested", "requested_changes":
                return "Requested changes"
            case "comment": return "Review comment"
            default: return "Review"
            }
        }
        switch type {
        case "line-comment": return "Line comment"
        case "thread-comment": return "Thread comment"
        case "thread-reply": return "Thread reply"
        case "thread-state": return "Thread state"
        case "suggestion-state": return "Suggestion state"
        default: return "Comment"
        }
    }
}

public struct PublishedPull: Equatable, Hashable, Sendable, Identifiable {
    public var number: Int
    public var title: String
    public var status: String
    public var body: String
    public var base: String
    public var head: String
    public var branchBacked: Bool
    public var patch: String
    public var commits: String
    public var signed: Bool
    public var events: [PullEvent]
    public var author: String
    public var authorName: String
    public var createdMs: Int

    public var id: Int { number }

    public init(
        number: Int, title: String, status: String = "open", body: String = "", base: String = "",
        head: String = "", branchBacked: Bool = false, patch: String = "", commits: String = "", signed: Bool = false,
        events: [PullEvent] = [], author: String = "", authorName: String = "", createdMs: Int = 0
    ) {
        self.number = number
        self.title = title
        self.status = status
        self.body = body
        self.base = base
        self.head = head
        self.branchBacked = branchBacked
        self.patch = patch
        self.commits = commits
        self.signed = signed
        self.events = events
        self.author = author
        self.authorName = authorName
        self.createdMs = createdMs
    }

    public var displayAuthor: String { authorName.isEmpty ? author : authorName }
}

public struct RepoDiscussion: Equatable, Hashable, Sendable, Identifiable {
    public var number: Int
    public var title: String
    public var body: String
    public var author: String
    public var category: String
    public var updatedMs: Int
    public var events: [DiscussionEvent]

    public var id: Int { number }

    public init(
        number: Int, title: String, body: String = "", author: String = "", category: String = "general",
        updatedMs: Int = 0, events: [DiscussionEvent] = []
    ) {
        self.number = number
        self.title = title
        self.body = body
        self.author = author
        self.category = category
        self.updatedMs = updatedMs
        self.events = events
    }
}

public struct DiscussionEvent: Equatable, Hashable, Sendable {
    public var type: String
    public var body: String
    public var author: String
    public var authorName: String
    public var ts: Int

    public init(type: String, body: String = "", author: String = "", authorName: String = "", ts: Int = 0) {
        self.type = type
        self.body = body
        self.author = author
        self.authorName = authorName
        self.ts = ts
    }

    public var displayAuthor: String { authorName.isEmpty ? author : authorName }
}

/// A remote ActivityPub reply projected by the Worker.
///
/// This is intentionally not an IssueEvent, PullEvent, or DiscussionEvent:
/// HTTP-signature provenance does not make it a ForkMesh-signed native event.
public struct FederatedReply: Equatable, Hashable, Sendable {
    public var remoteId: String
    public var parentRemoteId: String
    public var author: String
    public var authorName: String
    public var authorUrl: String
    public var body: String
    public var backlink: String
    public var sourceInstance: String
    public var sourceSoftware: String
    public var lifecycle: String
    public var depth: Int
    public var timestamp: Int
    public var nativeEvent: Bool

    public init(
        remoteId: String, author: String, body: String, authorName: String = "", authorUrl: String = "",
        backlink: String = "", parentRemoteId: String = "", sourceInstance: String = "",
        sourceSoftware: String = "activitypub", lifecycle: String = "active", depth: Int = 0,
        timestamp: Int = 0, nativeEvent: Bool = false
    ) {
        self.remoteId = remoteId
        self.parentRemoteId = parentRemoteId
        self.author = author
        self.authorName = authorName
        self.authorUrl = authorUrl
        self.body = body
        self.backlink = backlink
        self.sourceInstance = sourceInstance
        self.sourceSoftware = sourceSoftware
        self.lifecycle = lifecycle
        self.depth = depth
        self.timestamp = timestamp
        self.nativeEvent = nativeEvent
    }

    public init(json: JSONObject) {
        let provenance = json.obj("provenance")
        let lifecycle: String
        if let value = ModelJSON.coalesce(json["lifecycle"]) {
            lifecycle = value.text
        } else if json.bool("tombstone") {
            lifecycle = "tombstoned"
        } else if json.bool("moderated") {
            lifecycle = "moderated"
        } else if json.bool("edited") {
            lifecycle = "edited"
        } else {
            lifecycle = "active"
        }
        func first(_ values: JSONValue?...) -> String? {
            for value in values { if let value, !value.isNull { return value.text } }
            return nil
        }
        self.init(
            remoteId: json.firstStr("remoteId", "id"),
            author: json.str("author"),
            body: json.str("body"),
            authorName: json.str("authorName"),
            authorUrl: json.str("authorUrl"),
            backlink: first(json["url"], json["backlink"], provenance["backlink"], json["remoteId"]) ?? "",
            parentRemoteId: json.str("parentRemoteId"),
            sourceInstance: first(json["sourceInstance"], provenance["instance"]) ?? "",
            sourceSoftware: first(json["sourceSoftware"], provenance["software"]) ?? "activitypub",
            lifecycle: lifecycle,
            depth: min(max(ModelJSON.int(json["depth"]), 0), 8),
            timestamp: ModelJSON.int(json["ts"]),
            // Fail closed if a server ever labels a remote projection as native.
            nativeEvent: false
        )
    }

    public var edited: Bool { lifecycle == "edited" }
    public var tombstone: Bool { lifecycle == "tombstoned" }
    public var moderated: Bool { lifecycle == "moderated" || lifecycle == "awaiting-redelivery" }

    public var displayBody: String {
        if tombstone { return "Deleted on the remote instance" }
        if moderated { return "Hidden by remote moderation" }
        return body
    }
}
