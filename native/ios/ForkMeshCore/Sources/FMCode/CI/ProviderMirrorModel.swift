import FMApi
import FMBase
import FMNet
import Foundation
import Observation

public struct ProviderMirrorError: Error, Equatable, Sendable, CustomStringConvertible {
    public let status: Int
    public let code: String
    public let message: String

    public init(status: Int, code: String, message: String = "") {
        self.status = status
        self.code = code
        self.message = message
    }

    public var description: String {
        if !message.isEmpty { return message }
        switch code {
        case "invalid_provider": return "Choose GitHub or GitLab."
        case "invalid_remote": return "Enter the repository as owner/name, or paste its https address."
        case "token_required": return "Paste an access token so ForkMesh can reach the repository."
        case "not_configured": return "This repository is not mirrored yet."
        case "nothing_selected": return "Tick at least one item to import."
        case "not_authorized": return "Only the repository owner can manage its mirror."
        default: return CIError(status: status, code: code).description
        }
    }
}

@MainActor
public protocol ProviderMirrorClient: AnyObject {
    func providerMirror(_ repo: RepoRef, action: String, fields: JSONObject) async throws -> JSONObject
}

public enum ProviderMirrorRequests {
    public static func decode(_ response: HTTPResponse) throws -> JSONObject {
        var map = JSONObject()
        if !response.body.isEmpty, let parsed = try? JSON.parse(response.body), let object = parsed.objectValue {
            map = object
        }
        if response.ok && map["error"] == nil { return map }
        throw ProviderMirrorError(status: response.status, code: map.str("error"), message: map.str("message"))
    }
}

extension ApiService: ProviderMirrorClient {
    public func providerMirror(_ repo: RepoRef, action: String, fields: JSONObject) async throws -> JSONObject {
        var body = fields
        body["action"] = .string(action)
        let headers: HTTPHeaders = ["Accept": "application/json", "Content-Type": "application/json"]
        let response = try await rawRequest(
            "POST", url(CIRequests.repoPath(repo, "provider-mirror")), headers: headers,
            body: JSON.encodeData(.object(body)))
        return try ProviderMirrorRequests.decode(response)
    }
}

public struct ProviderMirrorProvider: Equatable, Hashable, Sendable, Identifiable {
    public var key: String
    public var label: String
    public var web: String
    public var tokenUrl: String
    public var tokenHelp: String
    public var id: String { key }

    public var host: String { web.replacingOccurrences(of: "https://", with: "") }

    public static let defaults: [ProviderMirrorProvider] = [
        ProviderMirrorProvider(
            key: "github", label: "GitHub", web: "https://github.com",
            tokenUrl: "https://github.com/settings/personal-access-tokens/new", tokenHelp: ""),
        ProviderMirrorProvider(
            key: "gitlab", label: "GitLab", web: "https://gitlab.com",
            tokenUrl: "https://gitlab.com/-/user_settings/personal_access_tokens", tokenHelp: ""),
    ]
}

public struct ProviderMirrorResource: Equatable, Hashable, Sendable, Identifiable {
    public var key: String
    public var label: String
    public var labels: [String: String]
    public var icon: String
    public var hint: String
    public var directions: [String]
    public var providers: [String]
    public var defaultDirection: String
    public var id: String { key }

    public init?(json: JSONObject) {
        key = json.str("key")
        guard !key.isEmpty else { return nil }
        label = json.str("label", key)
        let names = json.obj("labels")
        labels = Dictionary(uniqueKeysWithValues: names.keys.map { ($0, names.str($0)) })
        icon = json.str("icon")
        hint = json.str("hint")
        directions = json.strings("directions")
        providers = json.strings("providers")
        defaultDirection = json.str("default", "off")
    }

    public func title(for provider: String) -> String { labels[provider] ?? label }
    public func supported(by provider: String) -> Bool { providers.contains(provider) }
    public var canImport: Bool { directions.contains("pull") }
}

public struct ProviderMirrorSetting: Equatable, Hashable, Sendable {
    public var direction: String
    public var mode: String
    public var comments: Bool

    public init(direction: String = "off", mode: String = "auto", comments: Bool = true) {
        self.direction = direction
        self.mode = mode
        self.comments = comments
    }

    public init(json: JSONObject) {
        direction = json.str("direction", "off")
        mode = json.str("mode", "auto")
        comments = json["comments"] != .bool(false)
    }

    public var imports: Bool { direction == "pull" || direction == "both" }
    public var manual: Bool { imports && mode == "manual" }

    public var json: JSONValue {
        .object(["direction": .string(direction), "mode": .string(mode), "comments": .bool(comments)])
    }
}

public struct ProviderMirrorActivity: Equatable, Hashable, Sendable {
    public var atMs: Int
    public var kind: String
    public var ok: Bool
    public var message: String
}

public struct ProviderMirrorState: Equatable, Sendable {
    public var provider: String
    public var providerLabel: String
    public var remote: String
    public var remoteUrl: String
    public var tokenHint: String
    public var active: Bool
    public var prune: Bool
    public var resources: [String: ProviderMirrorSetting]
    public var login: String
    public var remotePrivate: Bool
    public var canPush: Bool
    public var connectionError: String
    public var codePushAtMs: Int
    public var codePushOk: Bool
    public var codePushRefs: Int
    public var codePushError: String
    public var syncAtMs: Int
    public var syncOk: Bool
    public var syncImported: Int
    public var syncComments: Int
    public var syncError: String
    public var history: [ProviderMirrorActivity]

    public init?(json: JSONObject) {
        provider = json.str("provider")
        guard !provider.isEmpty else { return nil }
        providerLabel = json.str("providerLabel", provider == "gitlab" ? "GitLab" : "GitHub")
        remote = json.str("remote")
        remoteUrl = json.str("remoteUrl")
        tokenHint = json.str("tokenHint")
        active = json["active"] != .bool(false)
        prune = json.bool("prune")
        let map = json.obj("resources")
        resources = Dictionary(uniqueKeysWithValues: map.keys.map { ($0, ProviderMirrorSetting(json: map.obj($0))) })
        let status = json.obj("status")
        login = status.str("login")
        remotePrivate = status.bool("remotePrivate")
        canPush = status.bool("canPush")
        connectionError = status.str("error")
        let push = status.obj("codePush")
        codePushAtMs = ModelJSON.int(push["at"])
        codePushOk = push.bool("ok")
        codePushRefs = ModelJSON.int(push["refs"])
        codePushError = push.str("error")
        let sync = status.obj("lastSync")
        syncAtMs = ModelJSON.int(sync["at"])
        syncOk = sync.bool("ok")
        syncImported = ModelJSON.int(sync["imported"])
        syncComments = ModelJSON.int(sync["comments"])
        syncError = sync.str("error")
        history = json.maps("history").prefix(20).map {
            ProviderMirrorActivity(
                atMs: ModelJSON.int($0["at"]), kind: $0.str("kind"), ok: $0.bool("ok"), message: $0.str("message"))
        }
    }

    public func setting(_ key: String) -> ProviderMirrorSetting { resources[key] ?? ProviderMirrorSetting() }

    public var importing: Bool { resources.values.contains { $0.imports } }
}

public struct ProviderMirrorCandidate: Equatable, Hashable, Sendable, Identifiable {
    public var key: String
    public var number: Int
    public var title: String
    public var author: String
    public var state: String
    public var url: String
    public var updatedAtMs: Int
    public var commentCount: Int
    public var excerpt: String
    public var imported: Bool
    public var mirrored: Bool
    public var merged: Bool
    public var id: String { key }

    public init?(json: JSONObject) {
        key = json.str("key")
        guard !key.isEmpty else { return nil }
        number = ModelJSON.int(json["number"])
        title = json.str("title")
        author = json.str("author")
        state = json.str("state")
        url = json.str("url")
        updatedAtMs = ModelJSON.int(json["updatedAt"])
        commentCount = ModelJSON.int(json["commentCount"])
        excerpt = json.str("excerpt")
        imported = json.bool("imported")
        mirrored = json.bool("mirrored")
        merged = json.bool("merged")
    }

    public var selectable: Bool { !imported && !mirrored }
    public var stateLabel: String { merged ? "merged" : state }
}

public struct ProviderMirrorDraft: Equatable, Sendable {
    public var provider: String = "github"
    public var remote: String = ""
    public var token: String = ""
    public var createRemote: Bool = true
    public var prune: Bool = false
    public var resources: [String: ProviderMirrorSetting] = [:]

    public init(provider: String = "github") {
        self.provider = provider
    }

    public init(state: ProviderMirrorState) {
        provider = state.provider
        remote = state.remote
        prune = state.prune
        resources = state.resources
    }

    public static func cleanRemote(_ text: String) -> String? {
        var value = text.trimmingCharacters(in: .whitespacesAndNewlines)
        for prefix in ["https://github.com/", "https://gitlab.com/", "github.com/", "gitlab.com/"] where value.lowercased().hasPrefix(prefix) {
            value = String(value.dropFirst(prefix.count))
        }
        if value.hasSuffix(".git") { value = String(value.dropLast(4)) }
        let parts = value.split(separator: "/", omittingEmptySubsequences: true).map(String.init)
        guard parts.count == 2 else { return nil }
        let allowed = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")
        for part in parts where part.isEmpty || part.count > 100 || part.hasPrefix(".")
            || part.unicodeScalars.contains(where: { !allowed.contains($0) }) {
            return nil
        }
        return parts.joined(separator: "/")
    }

    public func validationError(hasStoredToken: Bool) -> String? {
        if ProviderMirrorDraft.cleanRemote(remote) == nil { return "Enter the repository as owner/name." }
        let token = token.trimmingCharacters(in: .whitespacesAndNewlines)
        if token.isEmpty && !hasStoredToken { return "Paste an access token." }
        if token.count > 512 || token.contains(where: { $0.isWhitespace }) { return "That token does not look right." }
        return nil
    }

    public func setting(_ resource: ProviderMirrorResource) -> ProviderMirrorSetting {
        resources[resource.key] ?? ProviderMirrorSetting(direction: resource.defaultDirection)
    }

    public func fields(resources catalog: [ProviderMirrorResource], description: String) -> JSONObject {
        var settings = JSONObject()
        for resource in catalog {
            settings[resource.key] = setting(resource).json
        }
        return [
            "provider": .string(provider),
            "remote": .string(ProviderMirrorDraft.cleanRemote(remote) ?? remote),
            "token": .string(token.trimmingCharacters(in: .whitespacesAndNewlines)),
            "createRemote": .bool(createRemote),
            "prune": .bool(prune),
            "description": .string(String(description.prefix(300))),
            "resources": .object(settings),
        ]
    }
}

public enum ProviderMirrorText {
    public static func direction(_ direction: String, provider: String) -> String {
        switch direction {
        case "push": return "ForkMesh → \(provider)"
        case "pull": return "\(provider) → ForkMesh"
        case "both": return "Both ways"
        default: return "Off"
        }
    }

    public static func codeLine(_ state: ProviderMirrorState, hosted: Bool, now: Int) -> (ok: Bool?, text: String) {
        if state.setting("code").direction != "push" { return (nil, "Code is not mirrored.") }
        if !hosted { return (nil, "Waiting for this repository to be hosted on ForkMesh.") }
        if state.codePushAtMs == 0 { return (nil, "First push is on its way.") }
        if !state.codePushOk { return (false, "Last push failed: \(state.codePushError.isEmpty ? "unknown error" : state.codePushError)") }
        return (true, "Pushed \(state.codePushRefs) branch\(state.codePushRefs == 1 ? "" : "es") and tags \(ago(state.codePushAtMs, now: now)).")
    }

    public static func syncLine(_ state: ProviderMirrorState, now: Int) -> (ok: Bool?, text: String) {
        if !state.importing { return (nil, "Nothing is imported into ForkMesh.") }
        if state.syncAtMs == 0 { return (nil, "First import runs within ten minutes.") }
        if !state.syncOk { return (false, "Last import hit a problem: \(state.syncError)") }
        return (true, "Checked \(ago(state.syncAtMs, now: now)): \(state.syncImported) new, \(state.syncComments) comment\(state.syncComments == 1 ? "" : "s").")
    }

    public static func ago(_ atMs: Int, now: Int) -> String {
        let minutes = max(0, now - atMs) / 60000
        if minutes < 1 { return "just now" }
        if minutes < 60 { return "\(minutes) min ago" }
        let hours = minutes / 60
        if hours < 48 { return "\(hours) h ago" }
        return "\(hours / 24) days ago"
    }
}

@MainActor
@Observable
public final class ProviderMirrorModel {
    public let repo: RepoRef
    public private(set) var mirror: ProviderMirrorState?
    public private(set) var providers: [ProviderMirrorProvider] = ProviderMirrorProvider.defaults
    public private(set) var resources: [ProviderMirrorResource] = []
    public private(set) var codeHosted = false
    public private(set) var isPrivate = false
    public private(set) var pending = 0
    public private(set) var loaded = false
    public private(set) var loading = false
    public private(set) var busy = false
    public private(set) var error: String?
    public private(set) var notice: String?
    public private(set) var candidateKind: String?
    public private(set) var candidates: [ProviderMirrorCandidate] = []
    public private(set) var candidatePage = 1
    public private(set) var candidatesMore = false
    public private(set) var candidatesLoading = false
    public var selected: Set<String> = []

    @ObservationIgnored private let client: ProviderMirrorClient

    public init(client: ProviderMirrorClient, repo: RepoRef) {
        self.client = client
        self.repo = repo
    }

    public func provider(_ key: String) -> ProviderMirrorProvider? { providers.first { $0.key == key } }

    public var importKinds: [ProviderMirrorResource] {
        guard let mirror else { return [] }
        return resources.filter { $0.supported(by: mirror.provider) && mirror.setting($0.key).imports }
    }

    func apply(_ data: JSONObject) {
        let catalog = data.obj("catalog")
        let parsedProviders = catalog.maps("providers").compactMap { map -> ProviderMirrorProvider? in
            let key = map.str("key")
            return key.isEmpty ? nil : ProviderMirrorProvider(
                key: key, label: map.str("label", key), web: map.str("web"), tokenUrl: map.str("tokenUrl"),
                tokenHelp: map.str("tokenHelp"))
        }
        if !parsedProviders.isEmpty { providers = parsedProviders }
        let parsedResources = catalog.maps("resources").compactMap(ProviderMirrorResource.init(json:))
        if !parsedResources.isEmpty { resources = parsedResources }
        mirror = ProviderMirrorState(json: data.obj("mirror"))
        codeHosted = data.bool("codeHosted")
        isPrivate = data.bool("private")
        pending = ModelJSON.int(data["pending"])
        loaded = true
    }

    public func load() async {
        loading = true
        defer { loading = false }
        do {
            apply(try await client.providerMirror(repo, action: "get", fields: [:]))
            error = nil
        } catch {
            self.error = "\(error)"
        }
    }

    private func mutate(_ action: String, _ fields: JSONObject) async -> JSONObject? {
        if busy { return nil }
        busy = true
        defer { busy = false }
        do {
            let data = try await client.providerMirror(repo, action: action, fields: fields)
            if data["catalog"] != nil { apply(data) }
            error = nil
            return data
        } catch {
            self.error = "\(error)"
            notice = nil
            return nil
        }
    }

    @discardableResult
    public func save(_ draft: ProviderMirrorDraft, description: String = "") async -> Bool {
        let storedToken = mirror?.provider == draft.provider && !(mirror?.tokenHint.isEmpty ?? true)
        if let problem = draft.validationError(hasStoredToken: storedToken) {
            error = problem
            return false
        }
        guard let data = await mutate("save", draft.fields(resources: resources, description: description)) else { return false }
        notice = data.bool("created") ? "Created \(mirror?.remote ?? "the repository") and connected." : "Mirror saved."
        return true
    }

    public func sync() async {
        guard let data = await mutate("sync", [:]) else { return }
        let sync = data.obj("sync")
        notice = "Synced: \(ModelJSON.int(sync["imported"])) new, \(ModelJSON.int(sync["comments"])) comments."
    }

    public func verify() async {
        guard await mutate("verify", [:]) != nil else { return }
        notice = mirror?.connectionError.isEmpty == false ? mirror?.connectionError : "Connection works."
    }

    public func setActive(_ active: Bool) async {
        guard await mutate("toggle", ["active": .bool(active)]) != nil else { return }
        notice = active ? "Mirroring resumed." : "Mirroring paused."
    }

    public func disconnect() async {
        guard await mutate("disconnect", [:]) != nil else { return }
        closeCandidates()
        notice = "Disconnected."
    }

    public func openCandidates(_ kind: String, page: Int = 1) async {
        candidateKind = kind
        candidatePage = max(1, page)
        candidates = []
        selected = []
        candidatesLoading = true
        defer { candidatesLoading = false }
        do {
            let data = try await client.providerMirror(
                repo, action: "candidates", fields: ["kind": .string(kind), "page": .int(Int64(candidatePage))])
            guard candidateKind == kind else { return }
            candidates = data.maps("items").compactMap(ProviderMirrorCandidate.init(json:))
            candidatesMore = data.bool("more")
            error = nil
        } catch {
            self.error = "\(error)"
        }
    }

    public func closeCandidates() {
        candidateKind = nil
        candidates = []
        selected = []
    }

    public func toggle(_ candidate: ProviderMirrorCandidate) {
        guard candidate.selectable else { return }
        if selected.contains(candidate.key) { selected.remove(candidate.key) } else { selected.insert(candidate.key) }
    }

    @discardableResult
    public func importSelected() async -> Int {
        guard let kind = candidateKind, !selected.isEmpty else {
            error = ProviderMirrorError(status: 400, code: "nothing_selected").description
            return 0
        }
        let keys = selected.sorted()
        guard let data = await mutate("import", [
            "kind": .string(kind), "page": .int(Int64(candidatePage)), "keys": .array(keys.map { .string($0) }),
        ]) else { return 0 }
        let count = ModelJSON.int(data["imported"])
        notice = "Imported \(count) item\(count == 1 ? "" : "s"). They appear once the repository host picks them up."
        await openCandidates(kind, page: candidatePage)
        return count
    }
}
