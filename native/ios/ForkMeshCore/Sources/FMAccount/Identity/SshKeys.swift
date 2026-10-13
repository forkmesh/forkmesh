import FMBase
import Foundation
import Observation

public struct SshKeyRow: Equatable, Sendable, Identifiable {
    public let id: String
    public let label: String
    public let keyType: String
    public let fingerprint: String
    public let createdAt: Int64
    public let lastUsedAt: Int64

    public init(id: String, label: String, keyType: String, fingerprint: String, createdAt: Int64, lastUsedAt: Int64) {
        self.id = id
        self.label = label
        self.keyType = keyType
        self.fingerprint = fingerprint
        self.createdAt = createdAt
        self.lastUsedAt = lastUsedAt
    }

    public static func parse(_ map: JSONObject) -> SshKeyRow? {
        let id = map.str("id")
        guard SshKeys.isKeyId(id) else { return nil }
        return SshKeyRow(
            id: id,
            label: String(map.str("label").prefix(SshKeys.maxLabel)),
            keyType: map.str("keyType"),
            fingerprint: map.str("fingerprint"),
            createdAt: map.int64("createdAt"),
            lastUsedAt: map.int64("lastUsedAt"))
    }

    public var title: String { label.isEmpty ? SshKeys.typeLabel(keyType) : label }
}

public struct SshKeysPage: Equatable, Sendable {
    public let keys: [SshKeyRow]
    public let maximumKeys: Int
    public let gatewayConfigured: Bool
    public let gatewayHost: String
    public let gatewayPort: Int
    public let notice: String

    public static func parse(_ map: JSONObject) -> SshKeysPage {
        SshKeysPage(
            keys: Array(map.maps("keys").compactMap(SshKeyRow.parse).prefix(SshKeys.maxRows)),
            maximumKeys: max(0, map.int("maximumKeys", SshKeys.defaultMaximum)),
            gatewayConfigured: map.bool("gatewayConfigured"),
            gatewayHost: map.str("gatewayHost"),
            gatewayPort: map.int("gatewayPort", 22),
            notice: map.str("notice"))
    }

    public var atLimit: Bool { maximumKeys > 0 && keys.count >= maximumKeys }

    public var gatewayLine: String {
        guard gatewayConfigured, !gatewayHost.isEmpty else {
            return "This server has no SSH gateway yet. Keys are kept for when it does."
        }
        return gatewayPort == 22 ? "git@\(gatewayHost)" : "ssh://git@\(gatewayHost):\(gatewayPort)"
    }
}

public enum SshKeyCheck: Equatable, Sendable {
    case ok(keyType: String, line: String, comment: String)
    case empty
    case privateKey
    case multipleKeys
    case unsupportedType(String)
    case malformed
}

public enum SshKeys {
    public static let path = "/api/accounts/ssh-keys"
    public static let maxLabel = 80
    public static let maxRows = 64
    public static let defaultMaximum = 20
    public static let maxLineBytes = 16 * 1024
    public static let supportedTypes: [String] = [
        "ssh-ed25519",
        "sk-ssh-ed25519@openssh.com",
        "ecdsa-sha2-nistp256",
        "ecdsa-sha2-nistp384",
        "ecdsa-sha2-nistp521",
        "sk-ecdsa-sha2-nistp256@openssh.com",
        "ssh-rsa",
    ]

    public static func isKeyId(_ id: String) -> Bool {
        guard id.hasPrefix("sk_") else { return false }
        let rest = id.dropFirst(3)
        guard rest.count >= 16, rest.count <= 80 else { return false }
        return rest.unicodeScalars.allSatisfy { CharacterSet.alphanumerics.contains($0) && $0.isASCII || $0 == "_" || $0 == "-" }
    }

    public static func typeLabel(_ keyType: String) -> String {
        switch keyType {
        case "ssh-ed25519": return "Ed25519 key"
        case "sk-ssh-ed25519@openssh.com": return "Ed25519 security key"
        case "sk-ecdsa-sha2-nistp256@openssh.com": return "ECDSA security key"
        case "ssh-rsa": return "RSA key"
        default: return keyType.hasPrefix("ecdsa-") ? "ECDSA key" : (keyType.isEmpty ? "SSH key" : keyType)
        }
    }

    public static func check(_ pasted: String) -> SshKeyCheck {
        let raw = pasted.trimmingCharacters(in: .whitespacesAndNewlines)
        if raw.isEmpty { return .empty }
        if raw.contains("PRIVATE KEY") || raw.hasPrefix("-----BEGIN") || raw.hasPrefix("PuTTY-User-Key-File") {
            return .privateKey
        }
        if raw.contains("\n") || raw.contains("\r") { return .multipleKeys }
        if raw.utf8.count > maxLineBytes { return .malformed }
        let parts = raw.split(separator: " ", maxSplits: 2, omittingEmptySubsequences: true).map(String.init)
        guard parts.count >= 2 else { return .malformed }
        guard supportedTypes.contains(parts[0]) else { return .unsupportedType(parts[0]) }
        guard let blob = B64.decodeOrNil(parts[1]), !blob.isEmpty else { return .malformed }
        guard let declared = embeddedType(Array(blob)), declared == parts[0] else { return .malformed }
        let comment = parts.count == 3 ? parts[2].trimmingCharacters(in: .whitespaces) : ""
        return .ok(keyType: parts[0], line: "\(parts[0]) \(parts[1])", comment: comment)
    }

    static func embeddedType(_ blob: [UInt8]) -> String? {
        guard blob.count >= 4 else { return nil }
        let length = Int(blob[0]) << 24 | Int(blob[1]) << 16 | Int(blob[2]) << 8 | Int(blob[3])
        guard length > 0, length <= 64, blob.count >= 4 + length else { return nil }
        return String(bytes: blob[4..<(4 + length)], encoding: .utf8)
    }

    public static func message(_ check: SshKeyCheck) -> String {
        switch check {
        case .ok: return ""
        case .empty: return "Paste the contents of your .pub file."
        case .privateKey: return "That is a private key. Never paste a private key anywhere; paste the .pub file instead."
        case .multipleKeys: return "Paste one key at a time."
        case .unsupportedType(let type): return "\(type) keys are not supported."
        case .malformed: return "That is not an OpenSSH public key line."
        }
    }

    public static func label(_ typed: String, comment: String) -> String {
        let trimmed = typed.trimmingCharacters(in: .whitespacesAndNewlines)
        let chosen = trimmed.isEmpty ? comment : trimmed
        return String(chosen.filter { !$0.isNewline }.prefix(maxLabel))
    }
}

@MainActor
@Observable
public final class SshKeysModel {
    public private(set) var page: SshKeysPage?
    public private(set) var loading = false
    public private(set) var busy = false
    public private(set) var error = ""
    public var draftKey = ""
    public var draftLabel = ""

    @ObservationIgnored private let client: AccountEndpointCalling

    public init(client: AccountEndpointCalling) {
        self.client = client
    }

    public var draftCheck: SshKeyCheck { SshKeys.check(draftKey) }

    public var canAdd: Bool {
        if busy || (page?.atLimit ?? false) { return false }
        if case .ok = draftCheck { return true }
        return false
    }

    public func load() async {
        loading = true
        defer { loading = false }
        do {
            page = SshKeysPage.parse(try await client.accountCall("GET", SshKeys.path, nil))
            error = ""
        } catch {
            self.error = AccountErrorText.text(error, fallback: "Could not load your SSH keys.")
        }
    }

    @discardableResult
    public func add() async -> Bool {
        guard case let .ok(_, line, comment) = draftCheck else {
            error = SshKeys.message(draftCheck)
            return false
        }
        busy = true
        defer { busy = false }
        do {
            let body: JSONObject = [
                "publicKey": .string(line),
                "label": .string(SshKeys.label(draftLabel, comment: comment)),
            ]
            _ = try await client.accountCall("POST", SshKeys.path, body)
            draftKey = ""
            draftLabel = ""
            error = ""
            await load()
            return true
        } catch {
            self.error = AccountErrorText.text(error, fallback: "Could not add the key.")
            return false
        }
    }

    @discardableResult
    public func remove(_ id: String) async -> Bool {
        guard SshKeys.isKeyId(id) else { return false }
        busy = true
        defer { busy = false }
        do {
            _ = try await client.accountCall("DELETE", "\(SshKeys.path)/\(id)", nil)
            if let current = page {
                page = SshKeysPage(
                    keys: current.keys.filter { $0.id != id }, maximumKeys: current.maximumKeys,
                    gatewayConfigured: current.gatewayConfigured, gatewayHost: current.gatewayHost,
                    gatewayPort: current.gatewayPort, notice: current.notice)
            }
            error = ""
            return true
        } catch {
            self.error = AccountErrorText.text(error, fallback: "Could not remove the key.")
            return false
        }
    }
}
