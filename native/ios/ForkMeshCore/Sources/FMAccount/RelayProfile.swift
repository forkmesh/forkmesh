import FMBase
import Foundation

/// A project: one ForkMesh instance and everything this phone holds for it.
///
/// "Project" is the user-facing name; in code a project is still a `RelayProfile`, because the
/// relay origin is what identifies it and every relay-scoped store (sessions, carried node-sync
/// rows, learned features) is already keyed by that origin.
public typealias Project = RelayProfile

/// One relay the app knows about, as a workspace the user can switch into.
///
/// A relay is identified by its **origin** (`host` plus an explicit port when there is one) and
/// never by the full relay URL. The URL carries a room path that can change between builds and a
/// scheme that follows loopback-ness, so keying anything durable on it would orphan a relay's
/// saved session the first time either moved. Equality is by `id`.
public struct RelayProfile: Hashable, Sendable, CustomStringConvertible {
    /// Normalized origin: lowercased host, plus `:port` when the URL named one. This is the
    /// storage scope for the relay's session and its cached data.
    public let id: String
    /// What the user sees. Defaults to the host, and is renameable.
    public let label: String
    /// Full `ws(s)://host[:port]/api/repo/.../ws` URL. Kept whole because the relay socket dials it
    /// directly and `RoomSeal` scopes its stored key by it; only `id` is used for identity.
    public let serverUrl: String
    public let addedAt: Int64
    public let lastUsedAt: Int64
    /// When the `/api/version` handshake last succeeded against this relay, and the build rev it
    /// reported. Display-only.
    public let verifiedAt: Int64
    public let verifiedRev: String
    /// True for the relay this build ships with. It can be switched away from and renamed, but not
    /// removed.
    public let builtIn: Bool
    /// What this relay supports: declared in `/api/version`, or learned from 404s.
    public let features: RelayFeatureSet
    /// The public World site for this instance. Empty means derived from the app host (`worldURL`).
    public let worldUrl: String
    /// The instance's public homepage (legal pages, plans). Empty means derived (`homepageURL`).
    public let homepageUrl: String
    /// Storage namespace for everything this project keeps on the phone (see `ProjectScope`).
    /// Empty is the legacy, pre-projects namespace: the project an existing install upgraded into
    /// keeps its data where it was.
    public let namespace: String
    public let logo: String
    public let hasInstance: Bool

    public init(
        id: String,
        label: String,
        serverUrl: String,
        addedAt: Int64 = 0,
        lastUsedAt: Int64 = 0,
        verifiedAt: Int64 = 0,
        verifiedRev: String = "",
        builtIn: Bool = false,
        features: RelayFeatureSet = .undeclared(),
        worldUrl: String = "",
        homepageUrl: String = "",
        namespace: String = "",
        logo: String = "",
        hasInstance: Bool = true
    ) {
        self.id = id
        self.label = label
        self.serverUrl = serverUrl
        self.addedAt = addedAt
        self.lastUsedAt = lastUsedAt
        self.verifiedAt = verifiedAt
        self.verifiedRev = verifiedRev
        self.builtIn = builtIn
        self.features = features
        self.worldUrl = worldUrl
        self.homepageUrl = homepageUrl
        self.namespace = namespace
        self.logo = logo
        self.hasInstance = hasInstance
    }

    /// Where this project's data lives.
    public var scope: ProjectScope { ProjectScope(projectId: id, namespace: namespace) }

    /// The World site: the stored one, else `world.<rest of the app host>`, else the build's.
    public var worldURL: URL? {
        let stored = worldUrl.dartTrimmed
        if !stored.isEmpty, let parsed = URL(string: stored), !(parsed.host ?? "").isEmpty { return parsed }
        let derived = RelayProfile.deriveWorldHost(host)
        if derived.isEmpty { return URL(string: AppEnv.defaultWorldSiteUrl) }
        return URL(string: "https://\(derived)/")
    }

    /// The public homepage: the stored one, else the app host without its `app.` (or `api.`)
    /// label (`app.next.forkmesh.com` -> `next.forkmesh.com`), else the app origin itself.
    public var homepageURL: URL? {
        let stored = homepageUrl.dartTrimmed
        if !stored.isEmpty, let parsed = URL(string: stored), !(parsed.host ?? "").isEmpty { return parsed }
        let labels = host.split(separator: ".", omittingEmptySubsequences: false).map(String.init)
        if labels.count >= 3 && (labels[0] == "app" || labels[0] == "api") {
            return URL(string: "https://\(labels.dropFirst().joined(separator: "."))/")
        }
        return URL(string: "\(httpOrigin)/")
    }

    /// `world.<rest>` for an app host, or empty when there is no sensible derivation (a loopback
    /// or IP host, or a bare two-label domain whose "rest" would be a TLD).
    public static func deriveWorldHost(_ appHost: String) -> String {
        let host = appHost.dartTrimmed.lowercased()
        if host.isEmpty || AppEnv.isLoopbackHost(host) { return "" }
        if host.allSatisfy({ $0 == "." || ($0.isASCII && $0.isNumber) }) || host.hasPrefix("[") { return "" }
        let labels = host.split(separator: ".", omittingEmptySubsequences: false).map(String.init)
        if labels.count < 2 { return "" }
        if labels.count == 2 { return "world.\(host)" }
        return "world.\(labels.dropFirst().joined(separator: "."))"
    }

    public static func == (lhs: RelayProfile, rhs: RelayProfile) -> Bool { lhs.id == rhs.id }
    public func hash(into hasher: inout Hasher) { hasher.combine(id) }
    public var description: String { "RelayProfile(\(id))" }

    /// `https://host[:port]`, the origin every REST call and the web dashboard are addressed by.
    /// `ws` maps to `http` so a loopback relay stays plain.
    public var httpOrigin: String {
        if !hasInstance { return "" }
        let parsed = ServerURL.parse(serverUrl)
        if parsed.host.isEmpty { return "https://\(id)" }
        return parsed.httpOrigin
    }

    /// The host without its port, for a compact subtitle.
    public var host: String {
        let parsed = ServerURL.parse(serverUrl)
        return parsed.host
    }

    public var isLoopback: Bool { AppEnv.isLoopbackHost(host) }

    public var instanceId: String? {
        if !hasInstance { return nil }
        return RelayProfile.normalizeOrigin(serverUrl) ?? id
    }

    public var displayOrigin: String {
        if !hasInstance { return RelayProfile.noInstanceLabel }
        return AppEnv.isProdAlias(id) ? AppEnv.prodHost : id
    }

    /// A short description of where this relay lives, shown under the label.
    public var subtitle: String {
        if !hasInstance { return RelayProfile.noInstanceLabel }
        return isLoopback ? "\(id) · on this network" : id
    }

    public static let noInstanceLabel = "No instance attached"

    public static func named(_ name: String, now: Int64 = Date().epochMillis) -> RelayProfile? {
        let clean = name.dartTrimmed
        if clean.isEmpty { return nil }
        let slug = String(ProjectScope.slugFor(clean.lowercased()).prefix(32))
        let suffix = String(UUID().uuidString.lowercased().filter { $0 != "-" }.prefix(8))
        let id = "local-\(slug)-\(suffix)"
        return RelayProfile(
            id: id,
            label: clean,
            serverUrl: "",
            addedAt: now,
            namespace: ProjectScope.namespaceFor(id),
            hasInstance: false
        )
    }

    /// A copy with the given fields replaced. `addedAt` is never changed by a copy.
    public func copyWith(
        label: String? = nil,
        serverUrl: String? = nil,
        lastUsedAt: Int64? = nil,
        verifiedAt: Int64? = nil,
        verifiedRev: String? = nil,
        features: RelayFeatureSet? = nil,
        worldUrl: String? = nil,
        homepageUrl: String? = nil,
        namespace: String? = nil,
        builtIn: Bool? = nil,
        logo: String? = nil
    ) -> RelayProfile {
        RelayProfile(
            id: id,
            label: label ?? self.label,
            serverUrl: serverUrl ?? self.serverUrl,
            addedAt: addedAt,
            lastUsedAt: lastUsedAt ?? self.lastUsedAt,
            verifiedAt: verifiedAt ?? self.verifiedAt,
            verifiedRev: verifiedRev ?? self.verifiedRev,
            builtIn: builtIn ?? self.builtIn,
            features: features ?? self.features,
            worldUrl: worldUrl ?? self.worldUrl,
            homepageUrl: homepageUrl ?? self.homepageUrl,
            namespace: namespace ?? self.namespace,
            logo: logo ?? self.logo,
            hasInstance: hasInstance
        )
    }

    public func attached(to instance: RelayProfile) -> RelayProfile {
        RelayProfile(
            id: id,
            label: label,
            serverUrl: instance.serverUrl,
            addedAt: addedAt,
            lastUsedAt: lastUsedAt,
            verifiedAt: instance.verifiedAt,
            verifiedRev: instance.verifiedRev,
            builtIn: builtIn,
            features: instance.features,
            worldUrl: instance.worldUrl,
            homepageUrl: instance.homepageUrl,
            namespace: namespace,
            logo: logo,
            hasInstance: true
        )
    }

    /// The same relay with `builtIn` set as given.
    func withBuiltIn(_ value: Bool, serverUrl: String? = nil) -> RelayProfile {
        copyWith(serverUrl: serverUrl, builtIn: value)
    }

    /// Build a profile from whatever the user typed: a bare host, a `host:port` pair, an
    /// `https://` or `http://` origin, or a full `ws(s)://` relay URL. nil when nothing usable can
    /// be read out of it (the Add Relay sheet's "not a URL yet").
    ///
    /// A profile parsed here is a *new* project, so it gets its own storage namespace: nothing it
    /// stores can land in another project's slots.
    public static func tryParse(
        _ input: String, label: String = "", worldUrl: String = "", now: Int64 = Date().epochMillis
    ) -> RelayProfile? {
        guard let origin = normalizeOrigin(input) else { return nil }
        let cleanLabel = label.dartTrimmed
        return RelayProfile(
            id: origin,
            label: cleanLabel.isEmpty ? origin : cleanLabel,
            serverUrl: AppEnv.relayUrlForHost(origin),
            addedAt: now,
            worldUrl: normalizeWorldUrl(worldUrl),
            namespace: ProjectScope.namespaceFor(origin)
        )
    }

    /// `https://host/` for whatever was typed into a World host field, or empty when nothing
    /// usable was (the World URL is then derived).
    public static func normalizeWorldUrl(_ input: String) -> String {
        guard let origin = hostOrigin(input) else { return "" }
        return "https://\(origin)/"
    }

    /// Reduce anything URL-shaped to `host[:port]`, lowercased, or nil. A default port for the
    /// scheme is dropped, so `https://example.com:443` and `example.com` are the same relay.
    public static func normalizeOrigin(_ input: String) -> String? {
        hostOrigin(input).map(AppEnv.canonicalProjectId)
    }

    public static func hostOrigin(_ input: String) -> String? {
        var raw = input.dartTrimmed
        if raw.isEmpty { return nil }
        // A bare host has no scheme to find an authority behind; give it one.
        if !raw.contains("://") { raw = "https://" + raw }
        guard let parts = authorityParts(raw) else { return nil }
        let host = parts.host.dartTrimmed.lowercased()
        if host.isEmpty || !isHostShaped(host) { return nil }
        guard let port = parts.port else { return host }
        let defaults = ["https": 443, "wss": 443, "http": 80, "ws": 80]
        if defaults[parts.scheme] == port { return host }
        return "\(host):\(port)"
    }

    /// Scheme, host and explicit port of `url`; nil where the URL does not parse (a non-numeric
    /// port). An IPv6 literal loses its brackets, which means the host
    /// pattern's bracketed branch never matches and IPv6 relays are refused.
    private static func authorityParts(_ url: String) -> (scheme: String, host: String, port: Int?)? {
        guard let schemeEnd = url.range(of: "://") else { return nil }
        let scheme = url[url.startIndex..<schemeEnd.lowerBound].lowercased()
        let after = url[schemeEnd.upperBound...]
        let end = after.firstIndex(where: { $0 == "/" || $0 == "?" || $0 == "#" }) ?? after.endIndex
        let authority = after[after.startIndex..<end]
        let hostPort = authority.lastIndex(of: "@").map { authority[authority.index(after: $0)...] } ?? authority
        var host: Substring
        var portText: Substring = ""
        if hostPort.hasPrefix("[") {
            guard let close = hostPort.firstIndex(of: "]") else { return nil }
            host = hostPort[hostPort.index(after: hostPort.startIndex)..<close]
            let rest = hostPort[hostPort.index(after: close)...]
            if rest.hasPrefix(":") {
                portText = rest.dropFirst()
            } else if !rest.isEmpty {
                return nil
            }
        } else if let colon = hostPort.lastIndex(of: ":") {
            host = hostPort[hostPort.startIndex..<colon]
            portText = hostPort[hostPort.index(after: colon)...]
        } else {
            host = hostPort
        }
        if portText.isEmpty { return (scheme, DartURI.decodePercent(String(host)), nil) }
        guard portText.allSatisfy({ $0.isASCII && $0.isNumber }), let port = Int(portText) else { return nil }
        return (scheme, DartURI.decodePercent(String(host)), port)
    }

    /// A hostname, an IPv4 literal, or a bracketed IPv6 literal:
    /// `^(?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]*[a-z0-9])?)*|\[[0-9a-f:]+\])$`.
    static func isHostShaped(_ host: String) -> Bool {
        let bytes = Array(host.utf8)
        if bytes.first == 0x5b {
            guard bytes.count > 2, bytes.last == 0x5d else { return false }
            return bytes[1..<(bytes.count - 1)].allSatisfy { isHexDigit($0) || $0 == 0x3a }
        }
        let labels = bytes.split(separator: 0x2e, omittingEmptySubsequences: false)
        for label in labels {
            guard let first = label.first, let last = label.last else { return false }
            if !isAlnum(first) || !isAlnum(last) { return false }
            if !label.allSatisfy({ isAlnum($0) || $0 == 0x2d }) { return false }
        }
        return true
    }

    private static func isAlnum(_ c: UInt8) -> Bool { (c >= 0x61 && c <= 0x7a) || (c >= 0x30 && c <= 0x39) }
    private static func isHexDigit(_ c: UInt8) -> Bool { (c >= 0x61 && c <= 0x66) || (c >= 0x30 && c <= 0x39) }

    public static func fromJSON(_ json: JSONObject) -> RelayProfile {
        let id = json.str("id")
        let url = json.str("serverUrl")
        let label = json.str("label")
        let hasInstance = json["hasInstance"] == nil ? true : json.bool("hasInstance")
        return RelayProfile(
            id: id,
            label: label.isEmpty ? id : label,
            serverUrl: !hasInstance ? "" : url.isEmpty ? AppEnv.relayUrlForHost(id) : url,
            addedAt: asInt(json["addedAt"]),
            lastUsedAt: asInt(json["lastUsedAt"]),
            verifiedAt: asInt(json["verifiedAt"]),
            verifiedRev: json.str("verifiedRev"),
            builtIn: json.bool("builtIn"),
            features: RelayFeatureSet.fromJSON(json["features"]),
            worldUrl: json.str("worldUrl"),
            homepageUrl: json.str("homepageUrl"),
            // A row saved before projects existed has no namespace field. Those rows all shared
            // the one legacy store, so that is where they stay.
            namespace: json.str("namespace"),
            logo: json.str("logo"),
            hasInstance: hasInstance
        )
    }

    public func toJSON() -> JSONObject {
        var json: JSONObject = [
            "id": .string(id),
            "label": .string(label),
            "serverUrl": .string(serverUrl),
            "addedAt": .int(addedAt),
            "lastUsedAt": .int(lastUsedAt),
            "verifiedAt": .int(verifiedAt),
            "verifiedRev": .string(verifiedRev),
            "builtIn": .bool(builtIn),
            "features": features.jsonValue,
            "worldUrl": .string(worldUrl),
            "homepageUrl": .string(homepageUrl),
            "namespace": .string(namespace),
        ]
        if !logo.isEmpty { json["logo"] = .string(logo) }
        if !hasInstance { json["hasInstance"] = .bool(false) }
        return json
    }
}

/// An int, a number truncated, or a string holding an integer; else 0.
func asInt(_ value: JSONValue?) -> Int64 {
    guard let value else { return 0 }
    switch value {
    case .int(let v): return v
    case .double(let d): return d.isFinite ? Int64(exactly: d.rounded(.towardZero)) ?? 0 : 0
    case .string(let s): return Int64(s.dartTrimmed) ?? 0
    default: return 0
    }
}
