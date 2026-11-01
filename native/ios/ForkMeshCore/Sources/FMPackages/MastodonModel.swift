import FMBase
import FMNet
import Foundation
import Observation

public struct MastodonNotice: Equatable, Identifiable, Sendable {
    public let id = UUID()
    public let message: String
    public let isError: Bool

    public init(_ message: String, isError: Bool = false) {
        self.message = message
        self.isError = isError
    }

    public static func == (lhs: MastodonNotice, rhs: MastodonNotice) -> Bool {
        lhs.message == rhs.message && lhs.isError == rhs.isError
    }
}

@MainActor
@Observable
public final class MastodonHub {
    public static let packageId = "mastodon"
    public static let title = "Mastodon"
    static let sessionKey = "mastodon.session.v1"
    static let pendingKey = "mastodon.pending.v1"

    private let store: SecureStore
    public let http: HTTPClient
    public private(set) var session: MastodonSession?
    public private(set) var me: MastodonAccount?
    public private(set) var limits: MastodonLimits = .standard
    public private(set) var pending: MastodonPending?
    public private(set) var busy = false
    public private(set) var generation = 0
    public var server = ""
    public var code = ""
    public var token = ""
    public var showCode = false
    public var showToken = false
    public var notice: MastodonNotice?

    public init(store: SecureStore, http: HTTPClient) {
        self.store = store
        self.http = http
        session = store.read(Self.sessionKey).flatMap { MastodonSession(JSON.parseObject($0)) }
        pending = store.read(Self.pendingKey).flatMap { MastodonPending(JSON.parseObject($0)) }
        if let server = session?.server ?? pending?.server { self.server = MastodonServer.host(server) }
    }

    public var signedIn: Bool { session != nil }

    public var client: MastodonClient? {
        session.map { MastodonClient(server: $0.server, token: $0.token, http: http) }
    }

    public var serverValid: Bool { MastodonServer.normalize(server) != nil }

    public func start(outOfBand: Bool = false) async -> URL? {
        guard let origin = MastodonServer.normalize(server) else {
            notice = MastodonNotice("Enter your server, such as mastodon.social.", isError: true)
            return nil
        }
        busy = true
        defer { busy = false }
        notice = nil
        do {
            let app: MastodonApp
            if let pending, pending.server == origin {
                app = MastodonApp(clientId: pending.clientId, clientSecret: pending.clientSecret)
            } else {
                app = try await MastodonClient(server: origin, http: http).registerApp()
            }
            let pkce = MastodonPKCE.make()
            let next = MastodonPending(
                server: origin, clientId: app.clientId, clientSecret: app.clientSecret, state: pkce.state,
                verifier: pkce.verifier, redirect: outOfBand ? MastodonOAuth.outOfBand : MastodonOAuth.redirect)
            pending = next
            store.write(Self.pendingKey, JSON.encode(next.json))
            if outOfBand { showCode = true }
            return next.authorizeURL
        } catch {
            notice = MastodonNotice(Self.message(error), isError: true)
            return nil
        }
    }

    public func finish(_ input: String) async {
        guard let pending else {
            notice = MastodonNotice("Start the sign-in again.", isError: true)
            return
        }
        if let error = MastodonOAuth.callbackError(input) {
            notice = MastodonNotice(error == "access_denied" ? "The sign-in was cancelled." : error, isError: true)
            return
        }
        guard let code = MastodonOAuth.code(from: input, state: pending.state) else {
            notice = MastodonNotice("That code does not match this sign-in. Start again.", isError: true)
            return
        }
        busy = true
        defer { busy = false }
        do {
            let token = try await MastodonClient(server: pending.server, http: http).token(code: code, pending: pending)
            try await complete(MastodonSession(server: pending.server, token: token, clientId: pending.clientId, clientSecret: pending.clientSecret))
        } catch {
            notice = MastodonNotice(Self.message(error), isError: true)
        }
    }

    public func signIn(token raw: String) async {
        let token = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let origin = MastodonServer.normalize(server) else {
            notice = MastodonNotice("Enter your server, such as mastodon.social.", isError: true)
            return
        }
        guard !token.isEmpty else {
            notice = MastodonNotice("Paste an access token from your server's Development settings.", isError: true)
            return
        }
        busy = true
        defer { busy = false }
        do {
            try await complete(MastodonSession(server: origin, token: token))
        } catch MastodonError.unauthorized {
            notice = MastodonNotice("The server did not accept that token.", isError: true)
        } catch {
            notice = MastodonNotice(Self.message(error), isError: true)
        }
    }

    private func complete(_ draft: MastodonSession) async throws {
        let client = MastodonClient(server: draft.server, token: draft.token, http: http)
        let account = try await client.me()
        var session = draft
        session.accountId = account.id
        session.acct = account.acct
        session.displayName = account.displayName
        session.avatar = account.avatar
        store.write(Self.sessionKey, JSON.encode(session.json))
        store.delete(Self.pendingKey)
        pending = nil
        self.session = session
        me = account
        code = ""
        token = ""
        showCode = false
        showToken = false
        notice = nil
        generation += 1
        limits = await client.limits()
    }

    public func refresh() async {
        guard let client else { return }
        do {
            let account = try await client.me()
            me = account
            if var session, session.displayName != account.displayName || session.avatar != account.avatar || session.acct != account.acct {
                session.displayName = account.displayName
                session.avatar = account.avatar
                session.acct = account.acct
                self.session = session
                store.write(Self.sessionKey, JSON.encode(session.json))
            }
            limits = await client.limits()
        } catch {
            handle(error)
        }
    }

    public func signOut() async {
        guard let session else { return }
        let client = MastodonClient(server: session.server, token: session.token, http: http)
        forget()
        notice = MastodonNotice("Signed out of \(MastodonServer.host(session.server)).")
        await client.revoke(clientId: session.clientId, clientSecret: session.clientSecret)
    }

    private func forget() {
        store.delete(Self.sessionKey)
        if let session { server = MastodonServer.host(session.server) }
        session = nil
        me = nil
        limits = .standard
        generation += 1
    }

    @discardableResult
    public func handle(_ error: Error) -> String {
        if case MastodonError.unauthorized = error, let session {
            let host = MastodonServer.host(session.server)
            forget()
            notice = MastodonNotice("Your Mastodon sign-in was revoked on \(host). Sign in again.", isError: true)
            return ""
        }
        return Self.message(error)
    }

    public func isMine(_ account: MastodonAccount) -> Bool {
        account.id == session?.accountId
    }

    public static func message(_ error: Error) -> String {
        (error as? MastodonError)?.message ?? "Mastodon could not finish that."
    }
}

public enum MastodonSource: Hashable, Sendable {
    case timeline(MastodonTimeline)
    case notifications
    case thread(String)
}

public enum MastodonSection: String, CaseIterable, Identifiable, Sendable {
    case home, notifications, local, federated, bookmarks, favourites

    public var id: String { rawValue }

    public var title: String {
        switch self {
        case .home: return "Home"
        case .notifications: return "Notifications"
        case .local: return "Local"
        case .federated: return "Federated"
        case .bookmarks: return "Bookmarks"
        case .favourites: return "Favourites"
        }
    }

    public var source: MastodonSource {
        switch self {
        case .home: return .timeline(.home)
        case .notifications: return .notifications
        case .local: return .timeline(.local)
        case .federated: return .timeline(.federated)
        case .bookmarks: return .timeline(.bookmarks)
        case .favourites: return .timeline(.favourites)
        }
    }

    public var empty: String {
        switch self {
        case .home: return "Your home timeline is quiet. Follow people to fill it."
        case .notifications: return "No notifications yet."
        case .local: return "Nobody on your server has posted yet."
        case .federated: return "Nothing has reached your server yet."
        case .bookmarks: return "Posts you bookmark appear here."
        case .favourites: return "Posts you favourite appear here."
        }
    }
}

@MainActor
@Observable
public final class MastodonFeedModel {
    public let source: MastodonSource
    private let hub: MastodonHub
    public private(set) var statuses: [MastodonStatus] = []
    public private(set) var notifications: [MastodonNotification] = []
    public private(set) var next: String?
    public private(set) var loading = false
    public private(set) var loaded = false
    public var error = ""
    public var notice: MastodonNotice?

    public init(hub: MastodonHub, source: MastodonSource) {
        self.hub = hub
        self.source = source
    }

    public var focusedId: String? {
        if case .thread(let id) = source { return id }
        return nil
    }

    public var isEmpty: Bool { statuses.isEmpty && notifications.isEmpty }

    public func load(append: Bool = false) async {
        guard let client = hub.client, !loading else { return }
        if append && next == nil { return }
        loading = true
        defer {
            loading = false
            loaded = true
        }
        do {
            switch source {
            case .timeline(let timeline):
                let page = try await client.timeline(timeline, next: append ? next : nil)
                statuses = append ? merge(statuses, page.items) : page.items
                next = page.next
            case .notifications:
                let page = try await client.notifications(next: append ? next : nil)
                notifications = append ? notifications + page.items.filter { item in !notifications.contains { $0.id == item.id } } : page.items
                next = page.next
            case .thread(let id):
                async let focused = client.status(id)
                async let context = client.context(id)
                let (status, around) = try await (focused, context)
                statuses = around.ancestors + [status] + around.descendants
                next = nil
            }
            error = ""
        } catch {
            let message = hub.handle(error)
            if !message.isEmpty {
                if append || isEmpty { self.error = message } else { notice = MastodonNotice(message, isError: true) }
            }
        }
    }

    private func merge(_ old: [MastodonStatus], _ new: [MastodonStatus]) -> [MastodonStatus] {
        let seen = Set(old.map(\.id))
        return old + new.filter { !seen.contains($0.id) }
    }

    public func apply(_ updated: MastodonStatus) {
        statuses = statuses.map { $0.replacing(updated) }
        notifications = notifications.map { item in
            var copy = item
            copy.status = item.status?.replacing(updated)
            return copy
        }
    }

    public func toggleFavourite(_ status: MastodonStatus) async {
        var flipped = status
        flipped.favourited.toggle()
        flipped.favourites = max(0, status.favourites + (flipped.favourited ? 1 : -1))
        await toggle(status, flipped, flipped.favourited ? .favourite : .unfavourite)
    }

    public func toggleBoost(_ status: MastodonStatus) async {
        var flipped = status
        flipped.boosted.toggle()
        flipped.boosts = max(0, status.boosts + (flipped.boosted ? 1 : -1))
        await toggle(status, flipped, flipped.boosted ? .reblog : .unreblog)
    }

    public func toggleBookmark(_ status: MastodonStatus) async {
        var flipped = status
        flipped.bookmarked.toggle()
        await toggle(status, flipped, flipped.bookmarked ? .bookmark : .unbookmark)
    }

    private func toggle(_ original: MastodonStatus, _ flipped: MastodonStatus, _ action: MastodonToggle) async {
        guard let client = hub.client else { return }
        apply(flipped)
        do {
            var updated = try await client.toggle(original.id, action)
            if action == .unreblog && updated.boosted { updated.boosted = false }
            apply(updated)
        } catch {
            apply(original)
            let message = hub.handle(error)
            if !message.isEmpty { notice = MastodonNotice(message, isError: true) }
        }
    }

    public func delete(_ status: MastodonStatus) async -> Bool {
        guard let client = hub.client else { return false }
        do {
            try await client.delete(status.id)
            statuses.removeAll { $0.id == status.id || $0.reblog?.status.id == status.id }
            notifications.removeAll { $0.status?.id == status.id }
            notice = MastodonNotice("Post deleted.")
            return true
        } catch {
            let message = hub.handle(error)
            if !message.isEmpty { notice = MastodonNotice(message, isError: true) }
            return false
        }
    }

    public func posted(_ status: MastodonStatus) {
        switch source {
        case .timeline(.home):
            statuses.insert(status, at: 0)
        case .timeline(.account(let id)) where id == status.account.id:
            statuses.insert(status, at: 0)
        case .thread:
            if statuses.contains(where: { $0.id == status.inReplyToId }) { statuses.append(status) }
        default:
            break
        }
    }
}

@MainActor
@Observable
public final class MastodonProfileModel {
    private let hub: MastodonHub
    public let accountId: String
    public let feed: MastodonFeedModel
    public private(set) var account: MastodonAccount?
    public private(set) var relationship: MastodonRelationship?
    public private(set) var following = false
    public var error = ""

    public init(hub: MastodonHub, accountId: String) {
        self.hub = hub
        self.accountId = accountId
        feed = MastodonFeedModel(hub: hub, source: .timeline(.account(accountId)))
    }

    public var isMe: Bool { accountId == hub.session?.accountId }

    public func load() async {
        guard let client = hub.client else { return }
        do {
            account = try await client.account(accountId)
            if !isMe { relationship = try await client.relationship(accountId) }
            error = ""
        } catch {
            self.error = hub.handle(error)
        }
        await feed.load()
    }

    public func toggleFollow() async {
        guard let client = hub.client, !following else { return }
        following = true
        defer { following = false }
        let follow = !(relationship?.following == true || relationship?.requested == true)
        do {
            relationship = try await client.follow(accountId, follow) ?? relationship
        } catch {
            let message = hub.handle(error)
            if !message.isEmpty { feed.notice = MastodonNotice(message, isError: true) }
        }
    }

    public var followLabel: String {
        guard let relationship else { return "Follow" }
        if relationship.following { return "Unfollow" }
        if relationship.requested { return "Requested" }
        return relationship.followedBy ? "Follow back" : "Follow"
    }
}

@MainActor
@Observable
public final class MastodonSearchModel {
    private let hub: MastodonHub
    public var query = ""
    public private(set) var results = MastodonSearchResults()
    public private(set) var searching = false
    public private(set) var searched = ""
    public var error = ""

    public init(hub: MastodonHub) { self.hub = hub }

    public func run() async {
        let text = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let client = hub.client, !text.isEmpty else { return }
        searching = true
        defer { searching = false }
        do {
            results = try await client.search(text)
            searched = text
            self.error = ""
        } catch {
            self.error = hub.handle(error)
        }
    }
}

public struct MastodonVisibility: Identifiable, Equatable, Sendable {
    public let value: String
    public let label: String

    public var id: String { value }
}

public struct MastodonPicked: Identifiable, Sendable {
    public let id = UUID()
    public var upload: MastodonUpload

    public init(_ upload: MastodonUpload) { self.upload = upload }
}

@MainActor
@Observable
public final class MastodonComposer {
    public static let visibilities: [MastodonVisibility] = [
        MastodonVisibility(value: "public", label: "Public"), MastodonVisibility(value: "unlisted", label: "Unlisted"),
        MastodonVisibility(value: "private", label: "Followers only"), MastodonVisibility(value: "direct", label: "Mentioned people only"),
    ]

    private let hub: MastodonHub
    public let replyTo: MastodonStatus?
    public var text: String
    public var spoiler = ""
    public var warning = false
    public var visibility: String
    public var media: [MastodonPicked] = []
    public private(set) var posting = false
    public var error = ""
    private let idempotency = UUID().uuidString

    public init(hub: MastodonHub, replyTo: MastodonStatus? = nil) {
        self.hub = hub
        self.replyTo = replyTo
        text = replyTo.map { MastodonComposer.mentions(for: $0, me: hub.session?.acct ?? "") } ?? ""
        visibility = replyTo?.visibility ?? "public"
        if let replyTo, !replyTo.spoiler.isEmpty {
            spoiler = replyTo.spoiler
            warning = true
        }
    }

    public static func mentions(for status: MastodonStatus, me: String) -> String {
        var names: [String] = []
        for acct in [status.account.acct] + status.mentions.map(\.acct) where !acct.isEmpty && acct != me && !names.contains(acct) {
            names.append(acct)
        }
        return names.isEmpty ? "" : names.map { "@" + $0 }.joined(separator: " ") + " "
    }

    public var limit: Int { hub.limits.maxCharacters }
    public var maxMedia: Int { hub.limits.maxMedia }

    public var remaining: Int {
        MastodonText.remaining(limit: limit, text: text, spoiler: spoiler, warning: warning)
    }

    public var canPost: Bool {
        !posting && remaining >= 0 && (!text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || !media.isEmpty)
    }

    public var visibilityLabel: String {
        MastodonComposer.visibilities.first { $0.value == visibility }?.label ?? "Public"
    }

    public func add(_ upload: MastodonUpload) {
        guard media.count < maxMedia else { return }
        media.append(MastodonPicked(upload))
    }

    public func remove(_ id: UUID) {
        media.removeAll { $0.id == id }
    }

    public func submit() async -> MastodonStatus? {
        guard canPost, let client = hub.client else { return nil }
        posting = true
        defer { posting = false }
        do {
            var ids: [String] = []
            for item in media { ids.append(try await client.upload(item.upload)) }
            let draft = MastodonDraft(
                text: text, visibility: visibility, spoiler: warning ? spoiler.trimmingCharacters(in: .whitespacesAndNewlines) : "",
                replyTo: replyTo?.id ?? "", mediaIds: ids)
            let status = try await client.post(draft, idempotency: idempotency)
            error = ""
            return status
        } catch {
            self.error = hub.handle(error)
            if self.error.isEmpty { self.error = "Your sign-in ended before the post was sent." }
            return nil
        }
    }
}
