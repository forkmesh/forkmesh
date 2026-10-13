import FMBase
import Foundation
import Observation

public struct McpTokenRow: Equatable, Sendable, Identifiable {
    public let id: String
    public let provider: String
    public let label: String
    public let deviceName: String
    public let scopes: [String]
    public let createdBy: String
    public let createdAt: Int64
    public let lastUsedAt: Int64
    public let expiresAt: Int64
    public let revokedAt: Int64

    public init(
        id: String, provider: String, label: String, deviceName: String = "", scopes: [String],
        createdBy: String = "", createdAt: Int64 = 0, lastUsedAt: Int64 = 0, expiresAt: Int64 = 0, revokedAt: Int64 = 0
    ) {
        self.id = id
        self.provider = provider
        self.label = label
        self.deviceName = deviceName
        self.scopes = scopes
        self.createdBy = createdBy
        self.createdAt = createdAt
        self.lastUsedAt = lastUsedAt
        self.expiresAt = expiresAt
        self.revokedAt = revokedAt
    }

    public static func parse(_ map: JSONObject) -> McpTokenRow? {
        let id = map.str("id").lowercased()
        guard McpConnector.isTokenId(id) else { return nil }
        return McpTokenRow(
            id: id, provider: map.str("provider"), label: String(map.str("label").prefix(80)),
            deviceName: String(map.str("deviceName").prefix(80)), scopes: Array(map.strings("scopes").prefix(32)),
            createdBy: map.str("createdBy"), createdAt: map.int64("createdAt"), lastUsedAt: map.int64("lastUsedAt"),
            expiresAt: map.int64("expiresAt"), revokedAt: map.int64("revokedAt"))
    }

    public func isActive(nowMillis: Int64) -> Bool {
        revokedAt == 0 && (expiresAt == 0 || expiresAt > nowMillis)
    }

    public var title: String { label.isEmpty ? McpConnector.providerLabel(provider) : label }

    public var tasksOnly: Bool { !scopes.isEmpty && Set(scopes).isSubset(of: Set(McpConnector.taskScopes)) }

    public func status(nowMillis: Int64) -> String {
        if revokedAt > 0 { return "Revoked" }
        if expiresAt > 0 && expiresAt <= nowMillis { return "Expired" }
        if expiresAt > 0 {
            let days = max(0, (expiresAt - nowMillis) / 86_400_000)
            return days == 0 ? "Expires today" : "Expires in \(days) day\(days == 1 ? "" : "s")"
        }
        return "Active"
    }
}

public struct MintedMcpToken: Equatable, Sendable {
    public let token: String
    public let tokenId: String
    public let label: String
    public let expiresAt: Int64
}

public enum McpConnector {
    public static let providers: [String] = ["claude-code", "codex"]
    public static let taskScopes: [String] = ["organization.tasks.read", "organization.tasks.write"]
    public static let defaultDays = 90
    public static let maxDays = 365

    public static func providerLabel(_ provider: String) -> String {
        switch provider {
        case "claude-code": return "Claude Code"
        case "codex": return "Codex"
        default: return provider.isEmpty ? "Bot" : provider
        }
    }

    public static func isTokenId(_ id: String) -> Bool {
        id.count == 32 && id.unicodeScalars.allSatisfy { ("0"..."9").contains($0) || ("a"..."f").contains($0) }
    }

    public static func isToken(_ token: String) -> Bool {
        let parts = token.split(separator: "_", maxSplits: 2, omittingEmptySubsequences: false)
        guard parts.count == 3, parts[0] == "fmbot", isTokenId(String(parts[1])) else { return false }
        return parts[2].count == 43
    }

    public static func path(_ org: String) -> String {
        let segment = org.lowercased().filter { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" || $0 == "." }
        return "/api/orgs/\(segment)/bot-tokens"
    }

    public static func serverURL(origin: String) -> String {
        let trimmed = origin.hasSuffix("/") ? String(origin.dropLast()) : origin
        return trimmed.isEmpty ? "" : trimmed + "/mcp"
    }

    public static func claudeCommand(serverURL: String, token: String) -> String {
        "claude mcp add --transport http forkmesh \(serverURL) --header \"Authorization: Bearer \(token)\""
    }

    public static func configJson(serverURL: String, token: String) -> String {
        let config: JSONValue = [
            "mcpServers": [
                "forkmesh": [
                    "type": "http",
                    "url": .string(serverURL),
                    "headers": ["Authorization": .string("Bearer \(token)")],
                ],
            ],
        ]
        return JSON.encodePretty(config)
    }

    public static func mask(_ token: String) -> String {
        guard token.count > 16 else { return String(repeating: "•", count: token.count) }
        return "\(token.prefix(12))…\(token.suffix(4))"
    }

    public static func clampDays(_ days: Int) -> Int { min(maxDays, max(1, days)) }
}

@MainActor
@Observable
public final class McpConnectorModel {
    public let org: String
    public private(set) var tokens: [McpTokenRow] = []
    public private(set) var loading = false
    public private(set) var busy = false
    public private(set) var error = ""
    public private(set) var minted: MintedMcpToken?
    public var provider = "claude-code"
    public var label = ""
    public var days = McpConnector.defaultDays

    @ObservationIgnored private let client: AccountEndpointCalling
    @ObservationIgnored private let clock: Clock

    public init(org: String, client: AccountEndpointCalling, clock: Clock = SystemClock()) {
        self.org = org
        self.client = client
        self.clock = clock
    }

    public var serverURL: String { McpConnector.serverURL(origin: client.serverOrigin) }

    public var activeTokens: [McpTokenRow] { tokens.filter { $0.isActive(nowMillis: clock.nowMillis) } }

    public func load() async {
        loading = true
        defer { loading = false }
        do {
            let map = try await client.accountCall("GET", McpConnector.path(org), nil)
            tokens = Array(map.maps("tokens").compactMap(McpTokenRow.parse).prefix(64))
            error = ""
        } catch {
            self.error = AccountErrorText.text(error, fallback: "Could not load the tokens.")
        }
    }

    @discardableResult
    public func mint() async -> MintedMcpToken? {
        guard McpConnector.providers.contains(provider) else { return nil }
        busy = true
        defer { busy = false }
        do {
            var body: JSONObject = [
                "provider": .string(provider),
                "scopes": .array(McpConnector.taskScopes.map { .string($0) }),
                "expiresDays": .int(Int64(McpConnector.clampDays(days))),
            ]
            let trimmed = label.trimmingCharacters(in: .whitespacesAndNewlines)
            if !trimmed.isEmpty { body["label"] = .string(String(trimmed.prefix(80))) }
            let map = try await client.accountCall("POST", McpConnector.path(org), body)
            let token = map.str("token")
            guard McpConnector.isToken(token) else {
                error = "The server answered without a token."
                return nil
            }
            let result = MintedMcpToken(
                token: token, tokenId: map.str("tokenId"), label: map.str("label"), expiresAt: map.int64("expiresAt"))
            minted = result
            label = ""
            error = ""
            await load()
            return result
        } catch {
            self.error = AccountErrorText.text(error, fallback: "Could not create a token.")
            return nil
        }
    }

    @discardableResult
    public func revoke(_ id: String) async -> Bool {
        guard McpConnector.isTokenId(id) else { return false }
        busy = true
        defer { busy = false }
        do {
            _ = try await client.accountCall("DELETE", McpConnector.path(org), ["tokenId": .string(id)])
            if minted?.tokenId == id { minted = nil }
            error = ""
            await load()
            return true
        } catch {
            self.error = AccountErrorText.text(error, fallback: "Could not revoke the token.")
            return false
        }
    }

    @discardableResult
    public func rotate(_ row: McpTokenRow) async -> MintedMcpToken? {
        if McpConnector.providers.contains(row.provider) { provider = row.provider }
        label = row.label
        guard let fresh = await mint() else { return nil }
        guard await revoke(row.id) else { return fresh }
        minted = fresh
        return fresh
    }

    public func dismissMinted() {
        minted = nil
    }
}
