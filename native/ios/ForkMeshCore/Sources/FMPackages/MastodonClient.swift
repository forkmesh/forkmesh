import FMBase
import FMNet
import Foundation

public enum MastodonError: Error, Equatable, Sendable {
    case unauthorized
    case failed(String)

    public var message: String {
        switch self {
        case .unauthorized: return "Your Mastodon sign-in is no longer valid."
        case .failed(let message): return message
        }
    }
}

public enum MastodonTimeline: Hashable, Sendable {
    case home
    case local
    case federated
    case bookmarks
    case favourites
    case tag(String)
    case account(String)

    public var path: String {
        switch self {
        case .home: return "/api/v1/timelines/home?limit=30"
        case .local: return "/api/v1/timelines/public?local=true&limit=30"
        case .federated: return "/api/v1/timelines/public?limit=30"
        case .bookmarks: return "/api/v1/bookmarks?limit=30"
        case .favourites: return "/api/v1/favourites?limit=30"
        case .tag(let name): return "/api/v1/timelines/tag/\(MastodonForm.escape(name))?limit=30"
        case .account(let id): return "/api/v1/accounts/\(MastodonForm.escape(id))/statuses?limit=30"
        }
    }
}

public enum MastodonToggle: String, Sendable {
    case favourite, unfavourite, reblog, unreblog, bookmark, unbookmark
}

public struct MastodonUpload: Sendable {
    public var data: Data
    public var filename: String
    public var mimeType: String
    public var description: String

    public init(data: Data, filename: String, mimeType: String, description: String = "") {
        self.data = data
        self.filename = filename
        self.mimeType = mimeType
        self.description = description
    }
}

public struct MastodonDraft: Equatable, Sendable {
    public var text: String
    public var visibility: String
    public var spoiler: String
    public var replyTo: String
    public var mediaIds: [String]

    public init(text: String, visibility: String = "public", spoiler: String = "", replyTo: String = "", mediaIds: [String] = []) {
        self.text = text
        self.visibility = visibility
        self.spoiler = spoiler
        self.replyTo = replyTo
        self.mediaIds = mediaIds
    }

    public var json: JSONObject {
        var body: JSONObject = ["status": .string(text), "visibility": .string(visibility)]
        if !spoiler.isEmpty {
            body["spoiler_text"] = .string(spoiler)
            body["sensitive"] = .bool(true)
        }
        if !replyTo.isEmpty { body["in_reply_to_id"] = .string(replyTo) }
        if !mediaIds.isEmpty { body["media_ids"] = .array(mediaIds.map { .string($0) }) }
        return body
    }
}

public struct MastodonClient: Sendable {
    public let server: String
    public let token: String
    public let http: HTTPClient
    public var pause: @Sendable (UInt64) async -> Void

    public init(server: String, token: String = "", http: HTTPClient,
                pause: @escaping @Sendable (UInt64) async -> Void = { try? await Task.sleep(nanoseconds: $0) }) {
        self.server = server
        self.token = token
        self.http = http
        self.pause = pause
    }

    public func url(_ path: String) -> URL? {
        if path.hasPrefix(server + "/") { return URL(string: path) }
        return URL(string: server + path)
    }

    func send(_ method: String, _ path: String, form: [(String, String)]? = nil, json: JSONObject? = nil,
              body: Data? = nil, contentType: String? = nil, headers extra: [String: String] = [:]) async throws -> HTTPResponse {
        guard let url = url(path) else { throw MastodonError.failed("That Mastodon address is not valid.") }
        var request = HTTPRequest(method: method, url: url, headers: ["Accept": "application/json"])
        if !token.isEmpty { request = request.bearer(token) }
        if let form {
            request.body = Data(MastodonForm.encode(form).utf8)
            request.headers["Content-Type"] = "application/x-www-form-urlencoded"
        } else if let json {
            request = request.json(.object(json))
        } else if let body {
            request.body = body
            request.headers["Content-Type"] = contentType
        }
        for (name, value) in extra { request.headers[name] = value }
        let response: HTTPResponse
        do {
            response = try await http.send(request)
        } catch {
            throw MastodonError.failed("Could not reach \(MastodonServer.host(server)).")
        }
        if response.status == 401 { throw MastodonError.unauthorized }
        guard response.ok else {
            let object = response.jsonObject
            let message = object.firstStr("error_description", "error")
            throw MastodonError.failed(message.isEmpty ? "Mastodon returned \(response.status)." : message)
        }
        return response
    }

    func object(_ method: String, _ path: String, form: [(String, String)]? = nil, json: JSONObject? = nil) async throws -> JSONObject {
        try await send(method, path, form: form, json: json).jsonObject
    }

    func array(_ path: String) async throws -> (items: [JSONObject], next: String?) {
        let response = try await send("GET", path)
        let items = ((try? response.json())?.arrayValue ?? []).compactMap(\.objectValue)
        return (items, MastodonPaging.next(response.header("Link"), server: server))
    }

    public func registerApp() async throws -> MastodonApp {
        let json = try await object("POST", "/api/v1/apps", form: [
            ("client_name", MastodonOAuth.clientName),
            ("redirect_uris", MastodonOAuth.redirect + "\n" + MastodonOAuth.outOfBand),
            ("scopes", MastodonOAuth.scopes), ("website", MastodonOAuth.website),
        ])
        let id = json.str("client_id")
        guard !id.isEmpty else { throw MastodonError.failed("This server could not register ForkMesh.") }
        return MastodonApp(clientId: id, clientSecret: json.str("client_secret"))
    }

    public func token(code: String, pending: MastodonPending) async throws -> String {
        let json = try await object("POST", "/oauth/token", form: [
            ("grant_type", "authorization_code"), ("code", code), ("client_id", pending.clientId),
            ("client_secret", pending.clientSecret), ("redirect_uri", pending.redirect),
            ("code_verifier", pending.verifier), ("scope", MastodonOAuth.scopes),
        ])
        let token = json.str("access_token")
        guard !token.isEmpty else { throw MastodonError.failed("Mastodon did not return an access token.") }
        return token
    }

    public func revoke(clientId: String, clientSecret: String) async {
        guard !clientId.isEmpty else { return }
        _ = try? await send("POST", "/oauth/revoke", form: [("client_id", clientId), ("client_secret", clientSecret), ("token", token)])
    }

    public func me() async throws -> MastodonAccount {
        guard let account = MastodonAccount(try await object("GET", "/api/v1/accounts/verify_credentials")) else {
            throw MastodonError.failed("Mastodon did not return your profile.")
        }
        return account
    }

    public func limits() async -> MastodonLimits {
        guard let json = try? await object("GET", "/api/v2/instance") else { return .standard }
        return MastodonLimits(json)
    }

    public func timeline(_ timeline: MastodonTimeline, next: String? = nil) async throws -> MastodonPage<MastodonStatus> {
        let page = try await array(next ?? timeline.path)
        return MastodonPage(items: page.items.compactMap { MastodonStatus($0) }, next: page.next)
    }

    public func notifications(next: String? = nil) async throws -> MastodonPage<MastodonNotification> {
        let page = try await array(next ?? "/api/v1/notifications?limit=30")
        return MastodonPage(items: page.items.compactMap(MastodonNotification.init), next: page.next)
    }

    public func status(_ id: String) async throws -> MastodonStatus {
        guard let status = MastodonStatus(try await object("GET", "/api/v1/statuses/\(MastodonForm.escape(id))")) else {
            throw MastodonError.failed("That post could not be read.")
        }
        return status
    }

    public func context(_ id: String) async throws -> (ancestors: [MastodonStatus], descendants: [MastodonStatus]) {
        let json = try await object("GET", "/api/v1/statuses/\(MastodonForm.escape(id))/context")
        return (json.maps("ancestors").compactMap { MastodonStatus($0) }, json.maps("descendants").compactMap { MastodonStatus($0) })
    }

    public func toggle(_ id: String, _ action: MastodonToggle) async throws -> MastodonStatus {
        guard let status = MastodonStatus(try await object("POST", "/api/v1/statuses/\(MastodonForm.escape(id))/\(action.rawValue)")) else {
            throw MastodonError.failed("Mastodon did not return the post.")
        }
        return status.shown
    }

    public func delete(_ id: String) async throws {
        _ = try await send("DELETE", "/api/v1/statuses/\(MastodonForm.escape(id))")
    }

    public func post(_ draft: MastodonDraft, idempotency: String) async throws -> MastodonStatus {
        let response = try await send("POST", "/api/v1/statuses", json: draft.json, headers: ["Idempotency-Key": idempotency])
        guard let status = MastodonStatus(response.jsonObject) else { throw MastodonError.failed("Mastodon did not return the post.") }
        return status
    }

    public func upload(_ file: MastodonUpload) async throws -> String {
        let boundary = "ForkMesh-" + UUID().uuidString
        var body = Data()
        func field(_ name: String, _ value: String) {
            body.append(Data("--\(boundary)\r\nContent-Disposition: form-data; name=\"\(name)\"\r\n\r\n\(value)\r\n".utf8))
        }
        if !file.description.isEmpty { field("description", file.description) }
        let filename = file.filename.replacingOccurrences(of: "\"", with: "")
        body.append(Data("--\(boundary)\r\nContent-Disposition: form-data; name=\"file\"; filename=\"\(filename)\"\r\nContent-Type: \(file.mimeType)\r\n\r\n".utf8))
        body.append(file.data)
        body.append(Data("\r\n--\(boundary)--\r\n".utf8))
        let response = try await send("POST", "/api/v2/media", body: body, contentType: "multipart/form-data; boundary=\(boundary)")
        let json = response.jsonObject
        let id = json.str("id")
        guard !id.isEmpty else { throw MastodonError.failed("Mastodon did not accept the picture.") }
        if response.status == 202 || json.str("url").isEmpty {
            for _ in 0..<10 {
                await pause(1_000_000_000)
                let media = try await send("GET", "/api/v1/media/\(MastodonForm.escape(id))")
                if media.status == 200 && !media.jsonObject.str("url").isEmpty { return id }
            }
            throw MastodonError.failed("Mastodon is still processing the picture. Try again in a moment.")
        }
        return id
    }

    public func account(_ id: String) async throws -> MastodonAccount {
        guard let account = MastodonAccount(try await object("GET", "/api/v1/accounts/\(MastodonForm.escape(id))")) else {
            throw MastodonError.failed("That profile could not be read.")
        }
        return account
    }

    public func relationship(_ id: String) async throws -> MastodonRelationship? {
        let response = try await send("GET", "/api/v1/accounts/relationships?id%5B%5D=\(MastodonForm.escape(id))")
        return ((try? response.json())?.arrayValue ?? []).compactMap { MastodonRelationship($0.objectValue) }.first
    }

    public func follow(_ id: String, _ follow: Bool) async throws -> MastodonRelationship? {
        MastodonRelationship(try await object("POST", "/api/v1/accounts/\(MastodonForm.escape(id))/\(follow ? "follow" : "unfollow")"))
    }

    public func search(_ query: String) async throws -> MastodonSearchResults {
        MastodonSearchResults(try await object("GET", "/api/v2/search?q=\(MastodonForm.escape(query))&resolve=true&limit=20"))
    }
}
