// The deployment this build targets: a struct the app target fills from
// Info.plist at startup and hands to the services.

import Foundation

/// Which deployment this build talks to.
public enum AppEnvironment: String, Sendable {
    /// Local development: a `wrangler dev` Worker on the developer's machine,
    /// or whatever `FORKMESH_API_HOST` points at.
    case dev
    /// The deployed Worker at `app.forkmesh.com`.
    case prod
}

/// With nothing set the app targets **prod**, and anything but an explicit dev marker resolves to
/// prod, so a typo cannot ship a build aimed at a developer machine.
public struct AppEnv: Equatable, Sendable {
    public static let prodHost = "app.forkmesh.com"
    public static let devHost = "localhost"
    public static let androidEmulatorHost = "10.0.2.2"
    public static let devPort = 8787

    /// Relay endpoint for the shared world room. `ApiService` derives the REST
    /// host from this URL, so one host setting moves both transports.
    public static let relayPath = "/api/repo/mainnode/forkmesh/rooms/world-general/ws"

    /// Relay URL for the deployed Worker.
    public static let prodServerUrl = "wss://\(prodHost)\(relayPath)"

    /// Relay URL for `wrangler dev` on the host machine.
    public static let devServerUrl = "ws://\(devHost):\(devPort)\(relayPath)"

    public static let defaultWorldSiteUrl = "https://world.forkmesh.com/"

    public static let nextHost = "app.next.forkmesh.com"

    public static let prodAliasHosts: Set<String> = [
        "forkmesh.com", "www.forkmesh.com", "api.forkmesh.com", "next.forkmesh.com", nextHost,
    ]

    public static func isProdAlias(_ origin: String) -> Bool {
        prodAliasHosts.contains(origin.dartTrimmed.lowercased())
    }

    public static func canonicalProjectId(_ origin: String) -> String {
        isProdAlias(origin) ? prodHost : origin
    }

    /// The production build configuration.
    public static let prod = AppEnv()

    /// `FORKMESH_ENV`: `prod` (default) or `dev` / `development` / `local`.
    public var rawEnvironment: String
    /// `FORKMESH_API_HOST`: host override, e.g. `staging.forkmesh.com` or `host:port`.
    public var apiHostOverride: String
    /// `FORKMESH_SERVER_URL`: full relay WebSocket URL; wins over everything above.
    public var serverUrlOverride: String
    /// `FORKMESH_WORLD_URL`: public World site opened from the home shell.
    public var worldSiteUrl: String

    public init(
        rawEnvironment: String = "prod",
        apiHostOverride: String = "",
        serverUrlOverride: String = "",
        worldSiteUrl: String = AppEnv.defaultWorldSiteUrl
    ) {
        self.rawEnvironment = rawEnvironment
        self.apiHostOverride = apiHostOverride
        self.serverUrlOverride = serverUrlOverride
        self.worldSiteUrl = worldSiteUrl
    }

    /// From `Bundle.main.infoDictionary`. A missing or empty key keeps its
    /// default; so does a build setting
    /// that was never substituted ("$(FORKMESH_ENV)").
    public init(infoDictionary: [String: Any]?) {
        func value(_ key: String) -> String {
            let text = (infoDictionary?[key] as? String ?? "").dartTrimmed
            return text.hasPrefix("$(") ? "" : text
        }
        let env = value("FORKMESH_ENV")
        let world = value("FORKMESH_WORLD_URL")
        self.init(
            rawEnvironment: env.isEmpty ? "prod" : env,
            // The prod host as an override resolves to the same relay URL as
            // no override at all.
            apiHostOverride: value("FORKMESH_API_HOST"),
            serverUrlOverride: value("FORKMESH_SERVER_URL"),
            worldSiteUrl: world.isEmpty ? AppEnv.defaultWorldSiteUrl : world
        )
    }

    public var environment: AppEnvironment {
        ["dev", "development", "local"].contains(rawEnvironment.lowercased()) ? .dev : .prod
    }

    public var isDev: Bool { environment == .dev }

    /// The relay URL this build uses unless the user overrides it in Settings.
    public var defaultServerUrl: String {
        if !serverUrlOverride.isEmpty { return serverUrlOverride }
        if !apiHostOverride.isEmpty { return AppEnv.relayUrlForHost(apiHostOverride) }
        return isDev ? AppEnv.devServerUrl : AppEnv.prodServerUrl
    }

    /// Short label for the Settings screen, e.g. `prod · app.forkmesh.com`.
    public func describe(_ serverUrl: String) -> String {
        let host = AppEnv.host(of: serverUrl)
        let name = environment.rawValue
        return host.isEmpty ? name : "\(name) · \(host)"
    }

    /// Build a relay URL for `host`, choosing `ws` for loopback and `wss`
    /// otherwise. A `host:port` pair is accepted.
    public static func relayUrlForHost(_ host: String) -> String {
        let trimmed = host.dartTrimmed
        let name = trimmed.split(separator: ":", omittingEmptySubsequences: false).first.map(String.init) ?? ""
        let scheme = isLoopbackHost(name) ? "ws" : "wss"
        return "\(scheme)://\(trimmed)\(relayPath)"
    }

    public static func isLoopbackHost(_ host: String) -> Bool {
        ["localhost", "127.0.0.1", "::1", androidEmulatorHost].contains(host.dartTrimmed.lowercased())
    }

    /// True when `url` points at a Worker on the developer's machine.
    public static func isLoopbackUrl(_ url: String) -> Bool {
        let host = AppEnv.host(of: url.dartTrimmed)
        return !host.isEmpty && isLoopbackHost(host)
    }

    /// `Uri.tryParse(url)?.host ?? ''`: lower-case, an IPv6 literal without its brackets.
    static func host(of url: String) -> String {
        var host = (URLComponents(string: url)?.host ?? "").lowercased()
        if host.hasPrefix("["), host.hasSuffix("]") { host = String(host.dropFirst().dropLast()) }
        return host
    }
}
