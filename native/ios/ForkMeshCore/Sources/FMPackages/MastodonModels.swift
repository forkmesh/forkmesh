import FMBase
import Foundation

public struct MastodonAccount: Equatable, Identifiable, Sendable {
    public let id: String
    public let username: String
    public let acct: String
    public let displayName: String
    public let avatar: String
    public let header: String
    public let url: String
    public let note: String
    public let locked: Bool
    public let bot: Bool
    public let followers: Int
    public let following: Int
    public let statuses: Int

    public init?(_ json: JSONObject?) {
        guard let json else { return nil }
        let id = json.str("id")
        guard !id.isEmpty else { return nil }
        self.id = id
        username = json.str("username")
        acct = json.str("acct", username)
        let display = json.str("display_name").trimmingCharacters(in: .whitespaces)
        displayName = display.isEmpty ? username : display
        avatar = MastodonServer.webLink(json.firstStr("avatar_static", "avatar"))
        header = MastodonServer.webLink(json.firstStr("header_static", "header"))
        url = MastodonServer.webLink(json.str("url"))
        note = json.str("note")
        locked = json.bool("locked")
        bot = json.bool("bot")
        followers = json.int("followers_count")
        following = json.int("following_count")
        statuses = json.int("statuses_count")
    }

    public func handle(server: String) -> String {
        acct.contains("@") ? "@" + acct : "@\(acct)@\(MastodonServer.host(server))"
    }
}

public struct MastodonAttachment: Equatable, Identifiable, Sendable {
    public let id: String
    public let type: String
    public let url: String
    public let previewUrl: String
    public let description: String

    public init?(_ json: JSONObject) {
        let id = json.str("id")
        guard !id.isEmpty else { return nil }
        self.id = id
        type = json.str("type", "unknown")
        url = MastodonServer.webLink(json.firstStr("url", "remote_url"))
        previewUrl = MastodonServer.webLink(json.firstStr("preview_url", "url"))
        description = json.str("description")
    }

    public var isVisual: Bool { type == "image" || type == "gifv" || type == "video" }
}

public struct MastodonMention: Equatable, Sendable {
    public let id: String
    public let acct: String
    public let url: String

    public init(id: String, acct: String, url: String) {
        self.id = id
        self.acct = acct
        self.url = url
    }

    public init?(_ json: JSONObject) {
        let id = json.str("id")
        guard !id.isEmpty else { return nil }
        self.init(id: id, acct: json.str("acct"), url: json.str("url"))
    }
}

public struct MastodonTag: Equatable, Sendable {
    public let name: String
    public let url: String

    public init(name: String, url: String) {
        self.name = name
        self.url = url
    }

    public init?(_ json: JSONObject) {
        let name = json.str("name")
        guard !name.isEmpty else { return nil }
        self.init(name: name, url: json.str("url"))
    }
}

public struct MastodonPoll: Equatable, Sendable {
    public struct Option: Equatable, Sendable {
        public let title: String
        public let votes: Int
    }

    public let options: [Option]
    public let votes: Int
    public let expired: Bool

    public init?(_ json: JSONObject?) {
        guard let json, json.contains("options") else { return nil }
        options = json.maps("options").map { Option(title: $0.str("title"), votes: $0.int("votes_count")) }
        votes = json.int("votes_count")
        expired = json.bool("expired")
    }

    public func percent(_ option: Option) -> Int {
        guard votes > 0 else { return 0 }
        return Int((Double(option.votes) / Double(votes) * 100).rounded())
    }
}

public struct MastodonStatus: Equatable, Identifiable, Sendable {
    public let id: String
    public let url: String
    public let account: MastodonAccount
    public let content: String
    public let spoiler: String
    public let visibility: String
    public let sensitive: Bool
    public let createdAt: Int64
    public var replies: Int
    public var boosts: Int
    public var favourites: Int
    public var favourited: Bool
    public var boosted: Bool
    public var bookmarked: Bool
    public let inReplyToId: String
    public let media: [MastodonAttachment]
    public let mentions: [MastodonMention]
    public let tags: [MastodonTag]
    public let poll: MastodonPoll?
    public var reblog: Box?

    public final class Box: Equatable, @unchecked Sendable {
        public let status: MastodonStatus
        init(_ status: MastodonStatus) { self.status = status }
        public static func == (lhs: Box, rhs: Box) -> Bool { lhs.status == rhs.status }
    }

    public init?(_ json: JSONObject?) {
        guard let json, let account = MastodonAccount(json["account"]?.objectValue) else { return nil }
        let id = json.str("id")
        guard !id.isEmpty else { return nil }
        self.id = id
        url = MastodonServer.webLink(json.firstStr("url", "uri"))
        self.account = account
        content = json.str("content")
        spoiler = json.str("spoiler_text")
        visibility = json.str("visibility", "public")
        sensitive = json.bool("sensitive")
        createdAt = MastodonDate.millis(json.str("created_at"))
        replies = json.int("replies_count")
        boosts = json.int("reblogs_count")
        favourites = json.int("favourites_count")
        favourited = json.bool("favourited")
        boosted = json.bool("reblogged")
        bookmarked = json.bool("bookmarked")
        inReplyToId = json.str("in_reply_to_id")
        media = json.maps("media_attachments").compactMap(MastodonAttachment.init)
        mentions = json.maps("mentions").compactMap(MastodonMention.init)
        tags = json.maps("tags").compactMap(MastodonTag.init)
        poll = MastodonPoll(json["poll"]?.objectValue)
        reblog = MastodonStatus(json["reblog"]?.objectValue).map(Box.init)
    }

    public var shown: MastodonStatus { reblog?.status ?? self }

    public var segments: [MastodonSegment] { MastodonHTML.segments(content, mentions: mentions, tags: tags) }

    public var plainText: String { MastodonHTML.text(content) }

    public func replacing(_ updated: MastodonStatus) -> MastodonStatus {
        if let inner = reblog?.status { return inner.id == updated.id ? with(reblog: updated) : self }
        return id == updated.id ? updated : self
    }

    public func with(reblog status: MastodonStatus) -> MastodonStatus {
        var copy = self
        copy.reblog = Box(status)
        return copy
    }
}

public struct MastodonNotification: Equatable, Identifiable, Sendable {
    public let id: String
    public let type: String
    public let createdAt: Int64
    public let account: MastodonAccount
    public var status: MastodonStatus?

    public init?(_ json: JSONObject) {
        let id = json.str("id")
        guard !id.isEmpty, let account = MastodonAccount(json["account"]?.objectValue) else { return nil }
        self.id = id
        type = json.str("type")
        createdAt = MastodonDate.millis(json.str("created_at"))
        self.account = account
        status = MastodonStatus(json["status"]?.objectValue)
    }

    public var summary: String {
        let name = account.displayName
        switch type {
        case "favourite": return "\(name) favourited your post"
        case "reblog": return "\(name) boosted your post"
        case "follow": return "\(name) followed you"
        case "follow_request": return "\(name) requested to follow you"
        case "mention": return "\(name) mentioned you"
        case "status": return "\(name) posted"
        case "update": return "\(name) edited a post"
        case "poll": return "A poll you voted in has ended"
        default: return "\(name): \(type)"
        }
    }

    public var symbol: String {
        switch type {
        case "favourite": return "heart.fill"
        case "reblog": return "arrow.2.squarepath"
        case "follow", "follow_request": return "person.badge.plus"
        case "mention": return "at"
        case "poll": return "chart.bar"
        case "update": return "pencil"
        default: return "bell"
        }
    }
}

public struct MastodonRelationship: Equatable, Sendable {
    public let id: String
    public let following: Bool
    public let followedBy: Bool
    public let requested: Bool
    public let blocking: Bool
    public let muting: Bool

    public init?(_ json: JSONObject?) {
        guard let json else { return nil }
        let id = json.str("id")
        guard !id.isEmpty else { return nil }
        self.id = id
        following = json.bool("following")
        followedBy = json.bool("followed_by")
        requested = json.bool("requested")
        blocking = json.bool("blocking")
        muting = json.bool("muting")
    }
}

public struct MastodonLimits: Equatable, Sendable {
    public let maxCharacters: Int
    public let maxMedia: Int

    public static let standard = MastodonLimits(maxCharacters: 500, maxMedia: 4)

    public init(maxCharacters: Int, maxMedia: Int) {
        self.maxCharacters = maxCharacters
        self.maxMedia = maxMedia
    }

    public init(_ json: JSONObject) {
        let statuses = json.obj("configuration").obj("statuses")
        let characters = statuses.int("max_characters", MastodonLimits.standard.maxCharacters)
        let media = statuses.int("max_media_attachments", MastodonLimits.standard.maxMedia)
        self.init(maxCharacters: characters > 0 ? characters : 500, maxMedia: media > 0 ? media : 4)
    }
}

public struct MastodonSearchResults: Equatable, Sendable {
    public var accounts: [MastodonAccount]
    public var hashtags: [String]
    public var statuses: [MastodonStatus]

    public init(accounts: [MastodonAccount] = [], hashtags: [String] = [], statuses: [MastodonStatus] = []) {
        self.accounts = accounts
        self.hashtags = hashtags
        self.statuses = statuses
    }

    public init(_ json: JSONObject) {
        accounts = json.maps("accounts").compactMap { MastodonAccount($0) }
        hashtags = json.maps("hashtags").map { $0.str("name") }.filter { !$0.isEmpty }
        statuses = json.maps("statuses").compactMap { MastodonStatus($0) }
    }

    public var isEmpty: Bool { accounts.isEmpty && hashtags.isEmpty && statuses.isEmpty }
}

public struct MastodonPage<Item: Sendable>: Sendable {
    public let items: [Item]
    public let next: String?
}

public struct MastodonApp: Equatable, Sendable {
    public let clientId: String
    public let clientSecret: String
}

public struct MastodonSession: Equatable, Sendable {
    public var server: String
    public var token: String
    public var clientId: String
    public var clientSecret: String
    public var accountId: String
    public var acct: String
    public var displayName: String
    public var avatar: String

    public init(server: String, token: String, clientId: String = "", clientSecret: String = "",
                accountId: String = "", acct: String = "", displayName: String = "", avatar: String = "") {
        self.server = server
        self.token = token
        self.clientId = clientId
        self.clientSecret = clientSecret
        self.accountId = accountId
        self.acct = acct
        self.displayName = displayName
        self.avatar = avatar
    }

    public init?(_ json: JSONObject) {
        guard let server = MastodonServer.normalize(json.str("server")), !json.str("token").isEmpty else { return nil }
        self.init(server: server, token: json.str("token"), clientId: json.str("clientId"), clientSecret: json.str("clientSecret"),
                  accountId: json.str("accountId"), acct: json.str("acct"), displayName: json.str("displayName"), avatar: json.str("avatar"))
    }

    public var json: JSONObject {
        ["server": .string(server), "token": .string(token), "clientId": .string(clientId), "clientSecret": .string(clientSecret),
         "accountId": .string(accountId), "acct": .string(acct), "displayName": .string(displayName), "avatar": .string(avatar)]
    }

    public var handle: String { acct.isEmpty ? MastodonServer.host(server) : "@\(acct)@\(MastodonServer.host(server))" }
}

public struct MastodonPending: Equatable, Sendable {
    public var server: String
    public var clientId: String
    public var clientSecret: String
    public var state: String
    public var verifier: String
    public var redirect: String

    public init(server: String, clientId: String, clientSecret: String, state: String, verifier: String, redirect: String) {
        self.server = server
        self.clientId = clientId
        self.clientSecret = clientSecret
        self.state = state
        self.verifier = verifier
        self.redirect = redirect
    }

    public init?(_ json: JSONObject) {
        guard let server = MastodonServer.normalize(json.str("server")), !json.str("clientId").isEmpty else { return nil }
        self.init(server: server, clientId: json.str("clientId"), clientSecret: json.str("clientSecret"),
                  state: json.str("state"), verifier: json.str("verifier"), redirect: json.str("redirect", MastodonOAuth.redirect))
    }

    public var json: JSONObject {
        ["server": .string(server), "clientId": .string(clientId), "clientSecret": .string(clientSecret),
         "state": .string(state), "verifier": .string(verifier), "redirect": .string(redirect)]
    }

    public var authorizeURL: URL? {
        MastodonOAuth.authorizeURL(server: server, clientId: clientId, redirect: redirect, state: state,
                                   challenge: MastodonPKCE.challenge(for: verifier))
    }
}
