// ApiService, part 1: the client itself (construction, session callbacks, relay epoch,
// caches, URL building and the request helpers). The endpoints are in the ApiService+*.swift
// files.
// After native/android api/ApiService.kt.

import FMAccount
import FMBase
import FMCrypto
import FMNet
import Foundation
import Observation

/// What ApiService reads from settings. `SettingsService` conforms; tests may use
/// `StaticApiSettings`.
@MainActor
public protocol ApiSettings: AnyObject {
    /// The active relay's WebSocket URL; every HTTP endpoint derives from its host.
    var serverUrl: String { get }
    /// The shared chat room (`server/room`).
    var room: String { get }
}

extension SettingsService: ApiSettings {}

/// Fixed settings, for tests and previews.
@MainActor
public final class StaticApiSettings: ApiSettings {
    public var serverUrl: String
    public var room: String

    public init(serverUrl: String = "wss://forkmesh.test/api/relay", room: String = "world-general") {
        self.serverUrl = serverUrl
        self.room = room
    }
}

/// The Worker REST client.
///
/// Errors: HTTP failures throw `ApiException` (FMNet's `HTTPError` for a request that never
/// got an answer, e.g. `.timedOut`), org and account-package endpoints throw `OrgApiException`,
/// agent endpoints `AgentLaunchException`,
/// and the few contract-bearing calls return `ApiJsonResult` / `EventTicket` /
/// `PushRegistration` / `PushTestResult` / `BugReportResult` instead of throwing.
@MainActor
@Observable
public final class ApiService {
    nonisolated public static let cacheTtl: TimeInterval = 60
    nonisolated public static let offlineRepoTtl: TimeInterval = 60
    nonisolated public static let catalogTtl: TimeInterval = 30
    /// How long a ForkMesh Ads serve result (an ad, or "no ad") is reused. The network asks
    /// long-lived clients to fetch at most every 30 minutes.
    nonisolated public static let sponsoredAdTtl: TimeInterval = 30 * 60
    /// How long a failed serve call is remembered, so a broken network never turns a screen that
    /// is opened repeatedly into a retry loop.
    nonisolated public static let sponsoredAdFailureTtl: TimeInterval = 5 * 60
    nonisolated static let sponsoredAdTimeout: TimeInterval = 6
    /// How long a profile lookup's `userStatus` is reused before the next read asks again. The
    /// avatar from the same lookup is kept for the session.
    nonisolated public static let accountStatusTtl: TimeInterval = 2 * 60

    @ObservationIgnored public let settings: ApiSettings
    @ObservationIgnored public let http: HTTPClient
    @ObservationIgnored let performanceMonitor: PerformanceMonitorService?
    @ObservationIgnored let clock: Clock
    @ObservationIgnored let sleeper: @Sendable (TimeInterval) async throws -> Void
    @ObservationIgnored let issueRecord: @Sendable (String, String) -> Issue?

    /// Supplies the logged-in account's signed session token, or "" when logged out. Wire it to
    /// `AuthService.session?.sessionToken`. When present it is sent as `Authorization: Bearer`.
    @ObservationIgnored public var sessionTokenProvider: (@MainActor () -> String)?

    /// Fired when a Bearer-authenticated request returns HTTP 401 so the app can clear the local
    /// session and return to the login screen.
    @ObservationIgnored public var onUnauthorized: (@MainActor () -> Void)?

    /// Fired when a relay-wide route answers 404/405, naming the feature that proves absent. Wire
    /// it to `RelayDirectory.markFeatureMissing(activeId, feature)`. Only relay-wide routes report
    /// here (see `RelayFeature.learnableFromPath`).
    @ObservationIgnored public var onFeatureMissing: (@MainActor (RelayFeature) -> Void)?

    /// Fired alongside `onUnauthorized` with the Worker's `error` code from the 401 body ("" when
    /// absent). Wire it to `AuthService.handleUnauthorized(code:)`.
    @ObservationIgnored public var onSessionInvalid: (@MainActor (String) -> Void)?

    /// This device's owner-encryption identity, once enrolled. When absent, owner-sealed agent
    /// data stays unreadable here and the UI says so rather than showing an empty list.
    @ObservationIgnored public var ownerCrypto: OwnerKeys?

    @ObservationIgnored public var deployApprovalIdentity: DeviceIdentity?

    /// Bumped every time the app moves to a different relay. Everything cached here is an answer
    /// from one relay; a reply still in flight when the switch happened is recognised by it.
    public private(set) var relayEpoch = 0

    // Caches. All main-actor state.
    @ObservationIgnored let repositoriesCache = TaskCache<[Repository]>()
    @ObservationIgnored let networkStatsCache = TaskCache<NetworkStats>()
    @ObservationIgnored let networkLeaderboardsCache = TaskCache<NetworkLeaderboards>()
    @ObservationIgnored let sponsoredAdCache = TaskCache<SponsoredAd?>()
    @ObservationIgnored var sponsoredAdCacheTtl: TimeInterval = ApiService.sponsoredAdTtl
    @ObservationIgnored var offlineRepoUntil: [String: Date] = [:]
    @ObservationIgnored let treeCache = TaskCache<RepoTree>()
    @ObservationIgnored let blobCache = TaskCache<RepoBlob>()
    @ObservationIgnored let issuesCache = TaskCache<[Issue]>()
    @ObservationIgnored let pullsCache = TaskCache<[PublishedPull]>()
    @ObservationIgnored let pullHeadsCache = TaskCache<[PublishedPull]>()
    @ObservationIgnored let discussionsCache = TaskCache<[RepoDiscussion]>()
    @ObservationIgnored let commitsCache = TaskCache<[JSONObject]>()
    @ObservationIgnored let releasesCache = TaskCache<[RepoRelease]>()
    @ObservationIgnored let mirrorSetCache = TaskCache<RepoMirrorSet>()
    @ObservationIgnored let fediRepliesCache = TaskCache<[FederatedReply]>()
    @ObservationIgnored let commitDetailCache = TaskCache<RepoCommitDetail>()
    /// Serving-node `owner/name` (lower-cased) -> the public owner the catalog folded that row
    /// under. Filled by `repositories()`.
    @ObservationIgnored var publicRouteOwners: [String: String] = [:]
    /// Repositories (`owner/name`, lower-cased) whose public route has answered.
    @ObservationIgnored var publicRouteProven: Set<String> = []
    @ObservationIgnored var accountAvatarCache: [String: Task<String?, Never>] = [:]
    @ObservationIgnored var accountLookupCache: [String: AccountLookupEntry] = [:]
    @ObservationIgnored var ownerEnrolledFor = ""
    @ObservationIgnored var ownerEnrollmentToken: ListenerToken?

    /// `sleep` does the waits (the 250 ms 5xx retry and the merge poll), so tests can skip them.
    public init(
        settings: ApiSettings,
        http: HTTPClient,
        performanceMonitor: PerformanceMonitorService? = nil,
        clock: Clock = SystemClock(),
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        },
        issueRecord: @escaping @Sendable (String, String) -> Issue? = PublishedRecords.issueFromRecordJson
    ) {
        self.settings = settings
        self.http = http
        self.performanceMonitor = performanceMonitor
        self.clock = clock
        self.sleeper = sleep
        self.issueRecord = issueRecord
    }

    // MARK: Session

    /// The session token, or "" when signed out.
    var sessionToken: String { sessionTokenProvider?() ?? "" }

    /// Whether this client holds a session token (`_hasSession`).
    public var hasSession: Bool { !sessionToken.isEmpty }

    func withAuth(_ headers: HTTPHeaders) -> HTTPHeaders {
        let token = sessionToken
        if token.isEmpty { return headers }
        var out = headers
        out["Authorization"] = "Bearer \(token)"
        return out
    }

    // MARK: Relay switch and caches

    /// Drop everything learned from the relay being left. Called by the relay switch controller
    /// between taking the old relay's sockets down and bringing the new relay's up.
    public func resetForRelaySwitch() {
        relayEpoch += 1
        repositoriesCache.removeAll()
        networkStatsCache.removeAll()
        networkLeaderboardsCache.removeAll()
        sponsoredAdCache.removeAll()
        sponsoredAdCacheTtl = ApiService.sponsoredAdTtl
        treeCache.removeAll()
        blobCache.removeAll()
        issuesCache.removeAll()
        pullsCache.removeAll()
        pullHeadsCache.removeAll()
        discussionsCache.removeAll()
        commitsCache.removeAll()
        releasesCache.removeAll()
        // These are answers from one relay too, so they go with the rest.
        mirrorSetCache.removeAll()
        fediRepliesCache.removeAll()
        commitDetailCache.removeAll()
        offlineRepoUntil.removeAll()
        // Mirror routing is a property of one relay's catalog.
        publicRouteOwners.removeAll()
        publicRouteProven.removeAll()
        // Accounts are per relay too.
        accountLookupCache.removeAll()
        accountAvatarCache.removeAll()
    }

    public func clearRepoCache(_ owner: String, _ name: String) {
        let prefix = "\(owner)/\(name):"
        let key = "\(owner)/\(name)"
        treeCache.removeAll { $0.hasPrefix(prefix) }
        blobCache.removeAll { $0.hasPrefix(prefix) }
        issuesCache.remove(key)
        pullsCache.remove(key)
        pullHeadsCache.remove(key)
        discussionsCache.remove(key)
        commitsCache.remove(key)
        releasesCache.remove(key)
        mirrorSetCache.remove(key)
        fediRepliesCache.removeAll { $0.hasPrefix(prefix) }
        commitDetailCache.removeAll { $0.hasPrefix(prefix) }
        offlineRepoUntil.removeValue(forKey: key)
    }

    /// A fresh entry (in flight or finished) is shared; a failure evicts itself so the next call
    /// retries.
    func cached<T: Sendable>(
        _ cache: TaskCache<T>, _ key: String, ttl: TimeInterval = ApiService.cacheTtl,
        _ load: @escaping @MainActor @Sendable () async throws -> T
    ) async throws -> T {
        let now = clock.now
        if let task = cache.fresh(key, ttl: ttl, now: now, epoch: relayEpoch) { return try await task.value }
        return try await cache.load(key, now: now, epoch: relayEpoch, load).value
    }

    func repoOfflineError(_ repoKey: String) -> ApiException? {
        guard let until = offlineRepoUntil[repoKey] else { return nil }
        if clock.now < until {
            return ApiException(
                "Repo host is offline. Skipping refetch for a minute; tap Retry after reconnecting the desktop host.")
        }
        offlineRepoUntil.removeValue(forKey: repoKey)
        return nil
    }

    // MARK: URLs

    /// The relay's HTTP origin plus `path` (percent-encoded as a URL path) and `query` in the
    /// order given.
    public func url(_ path: String, _ query: [(String, String)] = []) -> URL {
        let server = ServerURL.parse(settings.serverUrl)
        if let url = server.httpURL(path, query: query) { return url }
        return URL(string: "https://invalid.invalid" + DartURI.encodePath(path)) ?? URL(fileURLWithPath: "/")
    }

    /// `url(path)` for a path that may carry its own `?query` (`caloriesOverview`, `tasksApi("GET",
    /// "?offset=0")`). Encoding the whole string as a path would escape the `?` to `%3F`, so the
    /// Worker would never see the query (and `/api/tasks%3F...` matches no route); here the
    /// query is kept a query, as written.
    func pathWithQueryURL(_ path: String) -> URL {
        guard let mark = path.firstIndex(of: "?") else { return url(path) }
        let base = url(String(path[..<mark]))
        let query = String(path[path.index(after: mark)...])
        if query.isEmpty { return base }
        let encoded = query.addingPercentEncoding(withAllowedCharacters: ApiService.queryAllowed) ?? query
        return URL(string: base.absoluteString + "?" + encoded) ?? base
    }

    nonisolated static let queryAllowed: CharacterSet = {
        var set = CharacterSet.urlQueryAllowed
        set.insert(charactersIn: "%")
        return set
    }()

    /// `scheme://host[:port]` of the app origin the Git engine talks to (`_base('/').origin`).
    public var gitOrigin: String { ServerURL.parse(settings.serverUrl).httpOrigin }

    /// The percent-encoded path.
    nonisolated static func encodedPath(_ url: URL) -> String {
        URLComponents(url: url, resolvingAgainstBaseURL: false)?.percentEncodedPath ?? url.path
    }

    nonisolated static func encodedQuery(_ url: URL) -> String {
        URLComponents(url: url, resolvingAgainstBaseURL: false)?.percentEncodedQuery ?? ""
    }

    // MARK: Request helpers

    /// Wraps `load` in the performance monitor, when there is one.
    func tracked<T>(_ name: String, _ details: JSONObject, _ load: @escaping @MainActor () async throws -> T) async throws -> T {
        guard let monitor = performanceMonitor else { return try await load() }
        return try await monitor.track(name, details: details, load)
    }

    func hostPathDetails(_ url: URL) -> JSONObject {
        ["host": .string(url.host ?? ""), "path": .string(ApiService.encodedPath(url))]
    }

    /// `_getJson`: GET with the session, 8 s under /api/repo/ and 12 s elsewhere, one retry after
    /// 250 ms on a 5xx other than 503.
    func getJson(_ url: URL) async throws -> JSONValue {
        let issuedEpoch = relayEpoch
        let path = ApiService.encodedPath(url)
        var details = hostPathDetails(url)
        let query = ApiService.encodedQuery(url)
        if !query.isEmpty { details["query"] = .string(query) }
        return try await tracked("api.GET \(path)", details) {
            var response: HTTPResponse?
            for attempt in 0..<2 {
                let request = HTTPRequest(
                    method: "GET", url: url, headers: self.withAuth(["Accept": "application/json"]),
                    timeout: path.contains("/api/repo/") ? 8 : 12)
                response = try await self.http.send(request)
                if let status = response?.status, status < 500 || status == 503 || attempt == 1 { break }
                try await self.sleeper(0.25)
            }
            return try await self.decodeResponseAsync(url, response!, issuedEpoch: issuedEpoch)
        }
    }

    /// `_postJson`: POST a JSON object with the session, 12 s.
    func postJson(_ url: URL, _ payload: JSONObject) async throws -> JSONValue {
        try await sendJson("POST", url, payload)
    }

    /// `_deleteJson`: DELETE with a JSON body and the session, 12 s.
    func deleteJson(_ url: URL, _ payload: JSONObject) async throws -> JSONValue {
        try await sendJson("DELETE", url, payload)
    }

    private func sendJson(_ method: String, _ url: URL, _ payload: JSONObject) async throws -> JSONValue {
        let issuedEpoch = relayEpoch
        return try await tracked("api.\(method) \(ApiService.encodedPath(url))", hostPathDetails(url)) {
            let request = HTTPRequest(
                method: method, url: url,
                headers: self.withAuth(["Accept": "application/json", "Content-Type": "application/json"]),
                body: JSON.encodeData(.object(payload)), timeout: 12)
            let response = try await self.http.send(request)
            return try await self.decodeResponseAsync(url, response, issuedEpoch: issuedEpoch)
        }
    }

    nonisolated static let detachedParseBytes = 16 * 1024

    func decodeResponseAsync(_ url: URL, _ response: HTTPResponse, issuedEpoch: Int = -1) async throws -> JSONValue {
        guard response.status >= 200 && response.status < 300, response.body.count >= ApiService.detachedParseBytes else {
            return try decodeResponse(url, response, issuedEpoch: issuedEpoch)
        }
        let body = response.body
        return try await Task.detached(priority: .userInitiated) { try JSON.parse(body) }.value
    }

    /// `_decodeResponse`. `issuedEpoch` is `relayEpoch` as it stood when the request left: a reply
    /// that arrives after a relay switch has its lasting side effects (sign-out, repo offline)
    /// dropped; the error still reaches the caller.
    func decodeResponse(_ url: URL, _ response: HTTPResponse, issuedEpoch: Int = -1) throws -> JSONValue {
        let current = issuedEpoch < 0 || issuedEpoch == relayEpoch
        let status = response.status
        let path = ApiService.encodedPath(url)
        if status >= 200 && status < 300 {
            if response.body.isEmpty { return .array([]) }
            return try JSON.parse(response.body)
        }
        if status == 401 {
            let hadToken = hasSession
            let code = ApiParse.errorCode(response.body)
            // Signing out on a 401 from the relay we just left would clear the session of the
            // relay we just arrived at, which never 401'd at all.
            if hadToken && current && ApiService.endsSession(path, code) {
                onSessionInvalid?(code)
                onUnauthorized?()
            }
            if code == "device_revoked" {
                throw ApiException("This device was signed out from the web. Please sign in again.")
            }
            if path.contains("/api/repo/") {
                throw ApiException(
                    "This repo needs an authenticated/private repo view token. Mobile private-repo browsing is not wired yet.")
            }
            throw ApiException("Your session expired. Please sign in again.")
        }
        if (status == 404 || status == 405) && current {
            if let feature = RelayFeature.learnableFromPath(path) { onFeatureMissing?(feature) }
        }
        if status == 503 && path.contains("/api/repo/") {
            if let repoKey = ApiService.repoKeyFromPath(path), current {
                offlineRepoUntil[repoKey] = clock.now.addingTimeInterval(ApiService.offlineRepoTtl)
            }
            throw ApiException(
                "Repo host is offline. Open the Qt desktop node for this owner/repo and make sure it is connected to this Worker, then refresh.")
        }
        if status == 500 && path.contains("/api/repo/") {
            throw ApiException(
                "Repo host returned an internal error while serving this view. Refresh or retry in a moment; if it keeps happening, reconnect the desktop host for this repo.")
        }
        throw ApiException("HTTP \(status) for \(path)")
    }

    nonisolated static func repoKeyFromPath(_ path: String) -> String? {
        let parts = path.split(separator: "/").map(String.init).filter { !$0.isEmpty }
        if parts.count >= 4 && parts[0] == "api" && parts[1] == "repo" { return "\(parts[2])/\(parts[3])" }
        return nil
    }

    nonisolated static func endsSession(_ path: String, _ code: String) -> Bool {
        !path.contains("/api/repo/") || code == "invalid_session" || code == "device_revoked"
    }

    /// `_postJsonResult`: POST with the session (20 s) keeping the status. A session-shaped 401
    /// (anything but `bad_signature`) signs out as every other endpoint does.
    func postJsonResult(_ url: URL, _ payload: JSONObject) async throws -> ApiJsonResult {
        let request = HTTPRequest(
            method: "POST", url: url,
            headers: withAuth(["Accept": "application/json", "Content-Type": "application/json"]),
            body: JSON.encodeData(.object(payload)), timeout: 20)
        let response = try await http.send(request)
        let body = ApiParse.jsonMap(response.body)
        if response.status == 401 && hasSession {
            let code = body.str("error")
            // `bad_signature` is a payload problem, not a dead session.
            if code != "bad_signature" && ApiService.endsSession(ApiService.encodedPath(url), code) {
                onSessionInvalid?(code)
                onUnauthorized?()
            }
        }
        return ApiJsonResult(response.status, body)
    }

    /// One authenticated request whose raw status the caller needs: 20 s, the session attached,
    /// and a 401 still signs the session out.
    public func rawRequest(
        _ method: String, _ url: URL, headers: HTTPHeaders = [:], body: Data? = nil
    ) async throws -> HTTPResponse {
        let request = HTTPRequest(method: method, url: url, headers: withAuth(headers), body: body, timeout: 20)
        let response = try await http.send(request)
        // Going around decodeResponse must not also go around the sign-out it performs.
        if response.status == 401 && hasSession {
            let code = ApiParse.errorCode(response.body)
            if ApiService.endsSession(ApiService.encodedPath(url), code) {
                onSessionInvalid?(code)
                onUnauthorized?()
            }
        }
        return response
    }

    /// `_orgRequest`: GET, POST or DELETE with the session (12 s). 2xx answers the decoded body
    /// (`{}` when empty); anything else throws `OrgApiException(code, orgErrorMessage(code))`.
    func orgRequest(_ method: String, _ url: URL, _ body: JSONObject? = nil) async throws -> JSONValue {
        var headers: HTTPHeaders = ["Accept": "application/json"]
        if body != nil { headers["Content-Type"] = "application/json" }
        let request = HTTPRequest(
            method: method, url: url, headers: withAuth(headers), body: body.map { JSON.encodeData(.object($0)) }, timeout: 12)
        let http = self.http
        return try await tracked("api.\(method) \(ApiService.encodedPath(url))", hostPathDetails(url)) {
            guard ["GET", "POST", "DELETE"].contains(method) else {
                throw ApiArgumentError(value: method, name: "method", message: "unsupported method")
            }
            let response = try await http.send(request)
            let decoded: JSONValue? = await ApiParse.parseAsync(response.body)
            if response.ok { return decoded ?? .object(JSONObject()) }
            let code = decoded?["error"]?.stringValue ?? ""
            throw OrgApiException(code.isEmpty ? "http_\(response.status)" : code, orgErrorMessage(code))
        }
    }

    /// One Bearer-authenticated JSON request against an account-scoped Worker API (calories,
    /// notes, tasks, the vault). GET, POST, PUT, PATCH or DELETE, 20 s; a body-less write sends
    /// `{}`. A 2xx without `"ok": false` answers the object; anything else throws
    /// `OrgApiException` with the Worker's error code (message: the code with spaces, or
    /// "The server answered N.").
    public func accountApiCall(_ method: String, _ path: String, _ body: JSONObject? = nil) async throws -> JSONObject {
        let url = pathWithQueryURL(path)
        var headers: HTTPHeaders = ["Accept": "application/json"]
        if body != nil { headers["Content-Type"] = "application/json" }
        let encoded = body.map { JSON.encodeData(.object($0)) } ?? Data("{}".utf8)
        let request = HTTPRequest(
            method: method, url: url, headers: withAuth(headers), body: method == "GET" ? nil : encoded, timeout: 20)
        let http = self.http
        return try await tracked("api.\(method) \(ApiService.encodedPath(url))", hostPathDetails(url)) {
            guard ["GET", "POST", "PUT", "PATCH", "DELETE"].contains(method) else {
                throw ApiArgumentError(value: method, name: "method", message: "unsupported method")
            }
            let response = try await http.send(request)
            let map: JSONObject = await ApiParse.jsonMapAsync(response.body)
            if response.ok && map["ok"] != .bool(false) { return map }
            let code = map["error"]?.stringValue ?? ""
            throw OrgApiException(
                code.isEmpty ? "http_\(response.status)" : code,
                code.isEmpty ? "The server answered \(response.status)." : code.replacingOccurrences(of: "_", with: " "))
        }
    }

    /// `_asList`: a list as-is, or the first list under one of the collection keys.
    nonisolated static func asList(_ data: JSONValue) -> [JSONValue] {
        if let list = data.arrayValue { return list }
        if let map = data.objectValue {
            for key in ["repositories", "issues", "pulls", "commits", "discussions", "mirrors", "branches",
                        "entries", "tree", "data", "items", "agents"] {
                if let list = map[key]?.arrayValue { return list }
            }
        }
        return []
    }
}

@MainActor
final class AccountLookupEntry {
    let createdAt: Date
    var task: Task<JSONObject?, Never>?
    /// Set when the name turned out not to be a user account: kept for the session instead of
    /// expiring with `accountStatusTtl`.
    var permanent = false
    var unreachable = false

    init(createdAt: Date) { self.createdAt = createdAt }
}

/// A keyed cache of in-flight or finished loads.
@MainActor
final class TaskCache<T: Sendable> {
    struct Entry {
        let id: Int
        let task: Task<T, Error>
        let createdAt: Date
        /// The relay generation the entry was created under; an older one is never fresh.
        let epoch: Int
    }

    private(set) var entries: [String: Entry] = [:]
    private var nextId = 0

    /// The entry for `key` when it is younger than `ttl` and from relay generation `epoch`.
    func fresh(_ key: String, ttl: TimeInterval, now: Date, epoch: Int) -> Task<T, Error>? {
        guard let entry = entries[key], entry.epoch == epoch, now.timeIntervalSince(entry.createdAt) < ttl else { return nil }
        return entry.task
    }

    /// Starts `load` and stores it under `key`; a failure removes the entry again (only if it is
    /// still this one).
    func load(_ key: String, now: Date, epoch: Int, _ load: @escaping @MainActor @Sendable () async throws -> T) -> Task<T, Error> {
        nextId += 1
        let id = nextId
        let task = Task<T, Error> { @MainActor [weak self] in
            do {
                return try await load()
            } catch {
                if let self, self.entries[key]?.id == id { self.entries.removeValue(forKey: key) }
                throw error
            }
        }
        entries[key] = Entry(id: id, task: task, createdAt: now, epoch: epoch)
        return task
    }

    func remove(_ key: String) { entries.removeValue(forKey: key) }

    func removeAll() { entries.removeAll() }

    func removeAll(where predicate: (String) -> Bool) {
        for key in entries.keys where predicate(key) { entries.removeValue(forKey: key) }
    }

    var isEmpty: Bool { entries.isEmpty }
}
