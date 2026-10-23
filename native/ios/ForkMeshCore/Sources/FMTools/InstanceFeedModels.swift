import FMApi
import FMBase
import Foundation
import Observation

public struct InstanceFeedAPI {
    public var call: ToolsApiCall
    public var origin: String

    public init(call: @escaping ToolsApiCall, origin: String = "") {
        self.call = call
        self.origin = origin
    }

    @MainActor
    public init(api: ApiService) {
        call = { method, suffix, body in try await api.instanceFeedApi(method, suffix, body) }
        origin = api.gitOrigin
    }
}

public struct InstanceFeedPost: Equatable, Identifiable, Sendable {
    public let id: String
    public let author: String
    public let authorName: String
    public let text: String
    public let spoiler: String
    public let createdAt: Int64
    public let replyTo: String
    public let replyToAuthor: String
    public let replies: Int
    public let boosts: Int
    public let favourites: Int
    public let boosted: Bool
    public let favourited: Bool
    public let mine: Bool
    public let canDelete: Bool

    public init?(_ json: JSONObject?) {
        guard let json, let id = json["id"]?.stringValue, InstanceFeed.validPostId(id) else { return nil }
        self.id = id
        author = json["author"]?.stringValue ?? ""
        let name = json["authorName"]?.stringValue ?? ""
        authorName = name.isEmpty ? author : name
        text = json["text"]?.stringValue ?? ""
        spoiler = json["spoiler"]?.stringValue ?? ""
        createdAt = json["createdAt"]?.int64Value ?? 0
        replyTo = json["replyTo"]?.stringValue ?? ""
        replyToAuthor = json["replyToAuthor"]?.stringValue ?? ""
        replies = json["replies"]?.intValue ?? 0
        boosts = json["boosts"]?.intValue ?? 0
        favourites = json["favourites"]?.intValue ?? 0
        boosted = json["boosted"]?.boolValue == true
        favourited = json["favourited"]?.boolValue == true
        mine = json["mine"]?.boolValue == true
        canDelete = json["canDelete"]?.boolValue == true
    }
}

public struct InstanceFeedEntry: Equatable, Identifiable, Sendable {
    public let id: String
    public var post: InstanceFeedPost
    public let booster: String
    public let boosterName: String

    public var isBoost: Bool { !booster.isEmpty }

    public init?(_ json: JSONObject?) {
        guard let json else { return nil }
        if json["type"]?.stringValue == "boost" {
            guard let post = InstanceFeedPost(json["post"]?.objectValue),
                  let id = json["id"]?.stringValue, InstanceFeed.validPostId(id) else { return nil }
            self.id = id
            self.post = post
            booster = json["booster"]?.stringValue ?? ""
            let name = json["boosterName"]?.stringValue ?? ""
            boosterName = name.isEmpty ? booster : name
        } else {
            guard let post = InstanceFeedPost(json) else { return nil }
            id = post.id
            self.post = post
            booster = ""
            boosterName = ""
        }
    }
}

public struct InstanceFeedProfile: Equatable, Sendable {
    public let name: String
    public let displayName: String
    public let posts: Int
    public let since: Int64

    public init?(_ json: JSONObject?) {
        guard let json, let name = json["name"]?.stringValue, !name.isEmpty else { return nil }
        self.name = name
        let display = json["displayName"]?.stringValue ?? ""
        displayName = display.isEmpty ? name : display
        posts = json["posts"]?.intValue ?? 0
        since = json["since"]?.int64Value ?? 0
    }
}

public struct InstanceFeedThread: Equatable, Sendable {
    public var ancestors: [InstanceFeedPost]
    public var post: InstanceFeedPost
    public var replies: [InstanceFeedPost]

    public init?(_ json: JSONObject) {
        guard let post = InstanceFeedPost(json["post"]?.objectValue) else { return nil }
        self.post = post
        ancestors = ToolsJSON.maps(json["ancestors"]).compactMap { InstanceFeedPost($0) }
        replies = ToolsJSON.maps(json["replies"]).compactMap { InstanceFeedPost($0) }
    }

    public var posts: [InstanceFeedPost] { ancestors + [post] + replies }
}

public enum InstanceFeedSegment: Equatable, Sendable {
    case text(String)
    case mention(String)
    case link(String)
}

public enum InstanceFeedView: Equatable, Sendable {
    case timeline
    case thread(String)
    case profile(String)
}

public enum InstanceFeed {
    public static let packageId = "instance-feed"
    public static let defaultMaxLength = 500
    public static let maxSpoiler = 100
    public static let publicSite = "https://forkmesh.com"
    public static let officialHosts: Set<String> = [AppEnv.prodHost, "world.forkmesh.com"]

    public static func validPostId(_ value: String) -> Bool {
        value.count == 32 && value.allSatisfy { $0.isHexDigit && !$0.isUppercase }
    }

    public static func validAuthor(_ value: String) -> Bool {
        value.range(of: "^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$", options: .regularExpression) != nil
    }

    public static func siteOrigin(_ origin: String) -> String {
        let trimmed = origin.trimmingCharacters(in: CharacterSet(charactersIn: "/ "))
        guard let host = URL(string: trimmed)?.host?.lowercased(), !host.isEmpty else { return publicSite }
        return officialHosts.contains(host) || AppEnv.isProdAlias(host) ? publicSite : trimmed
    }

    public static func publicURL(postId: String, origin: String) -> URL? {
        guard validPostId(postId) else { return nil }
        return URL(string: siteOrigin(origin) + "/feed?post=" + postId)
    }

    public static func timelineSuffix(author: String?, before: String?) -> String {
        var items: [String] = []
        if let author, validAuthor(author) { items.append("author=" + author) }
        if let before, !before.isEmpty,
           let encoded = before.addingPercentEncoding(withAllowedCharacters: .alphanumerics.union(CharacterSet(charactersIn: "-"))) {
            items.append("before=" + encoded)
        }
        return items.isEmpty ? "" : "?" + items.joined(separator: "&")
    }

    private static let segmentPattern = try! NSRegularExpression(
        pattern: #"(https://[^\s<>"']+[^\s<>"'.,;:!?)\]])|(^|[^A-Za-z0-9_@/])@([A-Za-z](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)\b"#)

    public static func segments(_ text: String) -> [InstanceFeedSegment] {
        let source = text as NSString
        var result: [InstanceFeedSegment] = []
        var plain = ""
        var last = 0
        func flush() {
            if !plain.isEmpty { result.append(.text(plain)) }
            plain = ""
        }
        for match in segmentPattern.matches(in: text, range: NSRange(location: 0, length: source.length)) {
            plain += source.substring(with: NSRange(location: last, length: match.range.location - last))
            if match.range(at: 1).location != NSNotFound {
                flush()
                result.append(.link(source.substring(with: match.range(at: 1))))
            } else {
                if match.range(at: 2).location != NSNotFound { plain += source.substring(with: match.range(at: 2)) }
                flush()
                result.append(.mention(source.substring(with: match.range(at: 3)).lowercased()))
            }
            last = match.range.location + match.range.length
        }
        plain += source.substring(from: last)
        flush()
        return result
    }
}

@MainActor
@Observable
public final class InstanceFeedModel {
    public static let title = "Feed"
    public static let notInstalledTitle = "Install Feed"
    public static let notInstalledMessage =
        "Read and write short posts with the people on this instance: reply, favourite, boost, and add a content warning when a post needs one. What you post is visible to everyone here who uses the feed and to anyone you share a post's public link with."
    public static let unavailableTitle = "The feed is unavailable"

    @ObservationIgnored public let api: InstanceFeedAPI

    public private(set) var view: InstanceFeedView = .timeline
    public private(set) var entries: [InstanceFeedEntry] = []
    public private(set) var thread: InstanceFeedThread?
    public private(set) var profile: InstanceFeedProfile?
    public private(set) var next = ""
    public private(set) var installed = true
    public private(set) var checked = false
    public private(set) var loading = false
    public private(set) var posting = false
    public private(set) var error = ""
    public private(set) var maxLength = InstanceFeed.defaultMaxLength
    public private(set) var replyTo: InstanceFeedPost?
    public var text = ""
    public var spoiler = ""
    public var showSpoiler = false
    public var notice: ToolsNotice?
    @ObservationIgnored private var generation = 0

    public init(api: InstanceFeedAPI, view: InstanceFeedView = .timeline) {
        self.api = api
        self.view = view
    }

    public var remaining: Int { maxLength - text.count }
    public func publicURL(_ post: InstanceFeedPost) -> URL? {
        InstanceFeed.publicURL(postId: post.id, origin: api.origin)
    }

    public func linkCopied() { show("Link copied") }

    public var canPost: Bool {
        !posting && !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && text.count <= maxLength
    }
    public var showsSpinner: Bool { loading && entries.isEmpty && thread == nil && error.isEmpty }
    public var showsError: Bool { !error.isEmpty && entries.isEmpty && thread == nil }
    public var showsInstall: Bool { checked && !installed }

    public func showTimeline() async {
        view = .timeline
        await load()
    }

    public func showThread(_ id: String) async {
        guard InstanceFeed.validPostId(id) else { return }
        view = .thread(id)
        await load()
    }

    public func showProfile(_ name: String) async {
        let name = name.lowercased()
        guard InstanceFeed.validAuthor(name) else { return }
        view = .profile(name)
        await load()
    }

    public func load(append: Bool = false) async {
        if append && (loading || next.isEmpty) { return }
        generation += 1
        let current = generation
        let view = view
        loading = true
        if !append { error = "" }
        do {
            switch view {
            case .thread(let id):
                let payload = try await api.call("GET", "/posts/" + id, nil)
                guard current == generation else { return }
                thread = InstanceFeedThread(payload)
                entries = []
                profile = nil
                installed = true
            case .timeline, .profile:
                var author: String?
                if case .profile(let name) = view { author = name }
                let payload = try await api.call(
                    "GET", InstanceFeed.timelineSuffix(author: author, before: append ? next : nil), nil)
                guard current == generation else { return }
                installed = payload["installed"]?.boolValue != false
                let page = ToolsJSON.maps(payload["posts"]).compactMap { InstanceFeedEntry($0) }
                entries = append ? entries + page : page
                next = payload["next"]?.stringValue ?? ""
                profile = InstanceFeedProfile(payload["profile"]?.objectValue)
                maxLength = max(1, payload["maxLength"]?.intValue ?? maxLength)
                thread = nil
            }
            checked = true
            loading = false
        } catch let failure as OrgApiException where failure.code == "plugin_not_installed" {
            guard current == generation else { return }
            installed = false
            checked = true
            loading = false
        } catch let failure as OrgApiException where failure.code == "post_not_found" {
            guard current == generation else { return }
            thread = nil
            checked = true
            loading = false
            error = "That post was deleted."
        } catch {
            guard current == generation else { return }
            loading = false
            self.error = message(error)
        }
    }

    public func startReply(_ post: InstanceFeedPost) {
        replyTo = post
        let mention = "@" + post.author
        if !text.hasPrefix(mention) {
            text = (mention + " " + text).trimmingCharacters(in: .whitespaces) + " "
        }
    }

    public func cancelReply() { replyTo = nil }

    public func submit() async {
        let body = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard canPost else { return }
        posting = true
        defer { posting = false }
        let parent = replyTo
        do {
            let payload = try await api.call("POST", "/posts", [
                "text": .string(body),
                "spoiler": .string(showSpoiler ? spoiler.trimmingCharacters(in: .whitespacesAndNewlines) : ""),
                "replyTo": .string(parent?.id ?? ""),
            ])
            text = ""
            spoiler = ""
            showSpoiler = false
            replyTo = nil
            if let parent, view != .thread(parent.id) {
                await showThread(parent.id)
            } else if view == .timeline, parent == nil, let entry = InstanceFeedEntry(payload["post"]?.objectValue) {
                entries.insert(entry, at: 0)
            } else {
                await load()
            }
        } catch {
            show(message(error), error: true)
        }
    }

    public func toggleFavourite(_ post: InstanceFeedPost) async {
        await toggle(post, kind: "favourite", active: post.favourited)
    }

    public func toggleBoost(_ post: InstanceFeedPost) async {
        await toggle(post, kind: "boost", active: post.boosted)
    }

    private func toggle(_ post: InstanceFeedPost, kind: String, active: Bool) async {
        guard InstanceFeed.validPostId(post.id) else { return }
        do {
            let payload = try await api.call(active ? "DELETE" : "POST", "/posts/\(post.id)/\(kind)", nil)
            if kind == "boost" && view == .timeline {
                await load()
            } else if let updated = InstanceFeedPost(payload["post"]?.objectValue) {
                replace(updated)
            }
        } catch {
            show(message(error), error: true)
        }
    }

    public func delete(_ post: InstanceFeedPost) async {
        guard InstanceFeed.validPostId(post.id) else { return }
        do {
            _ = try await api.call("DELETE", "/posts/" + post.id, nil)
            show("Post deleted.")
            if view == .thread(post.id) {
                await showTimeline()
            } else {
                await load()
            }
        } catch {
            show(message(error), error: true)
        }
    }

    func replace(_ post: InstanceFeedPost) {
        for index in entries.indices where entries[index].post.id == post.id {
            entries[index].post = post
        }
        guard var thread else { return }
        if thread.post.id == post.id { thread.post = post }
        thread.ancestors = thread.ancestors.map { $0.id == post.id ? post : $0 }
        thread.replies = thread.replies.map { $0.id == post.id ? post : $0 }
        self.thread = thread
    }

    func message(_ error: Error) -> String {
        (error as? OrgApiException)?.message ?? "The feed is unavailable right now."
    }

    func show(_ text: String, error: Bool = false) { notice = ToolsNotice(text, isError: error) }
}
