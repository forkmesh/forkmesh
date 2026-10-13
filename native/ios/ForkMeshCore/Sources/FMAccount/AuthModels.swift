// The account models: AuthSession, AccountDevice, AccountSession, AccountAvailability,
// AuthException and defaultDeviceLabel.

import FMBase
import Foundation

/// The signed-in account on one relay, persisted (as `toJSON`) in the secure store.
public struct AuthSession: Equatable, Sendable {
    public var nodeName: String
    public var email: String
    public var status: String
    public var pubkey: String
    public var emailVerified: Bool
    public var isAdmin: Bool
    public var avatarPng: String
    public var avatarUpdatedAt: Int64
    public var createdAt: Int64
    /// Stored under `at`.
    public var savedAt: Int64
    /// `account` for a plain web-style login, `desktop_node` for the Qt client, `mobile_node` when
    /// this phone's Ed25519 key was bound on login, `preview` for the offline design preview.
    public var sessionKind: String
    public var deviceKind: String
    /// Short id (`device_bi[:16]`) of the `account_devices` row bound to this session, or "".
    /// When set, an invalid/revoked session means the web app revoked this node.
    public var deviceId: String
    public var desktopCapable: Bool
    public var capabilities: [String]
    public var devices: [AccountDevice]
    /// Signed account session token, sent as `Authorization: Bearer`.
    public var sessionToken: String

    public init(
        nodeName: String,
        email: String,
        status: String,
        pubkey: String,
        emailVerified: Bool,
        isAdmin: Bool,
        avatarPng: String,
        avatarUpdatedAt: Int64,
        createdAt: Int64,
        savedAt: Int64,
        sessionKind: String = "account",
        deviceKind: String = "",
        deviceId: String = "",
        desktopCapable: Bool = false,
        capabilities: [String] = [],
        devices: [AccountDevice] = [],
        sessionToken: String = ""
    ) {
        self.nodeName = nodeName
        self.email = email
        self.status = status
        self.pubkey = pubkey
        self.emailVerified = emailVerified
        self.isAdmin = isAdmin
        self.avatarPng = avatarPng
        self.avatarUpdatedAt = avatarUpdatedAt
        self.createdAt = createdAt
        self.savedAt = savedAt
        self.sessionKind = sessionKind
        self.deviceKind = deviceKind
        self.deviceId = deviceId
        self.desktopCapable = desktopCapable
        self.capabilities = capabilities
        self.devices = devices
        self.sessionToken = sessionToken
    }

    public var canSubmitIssue: Bool { capabilities.contains("submit_issue") }
    public var canSubmitPr: Bool { capabilities.contains("submit_pr") }
    public var canHost: Bool { capabilities.contains("host_repo") }
    public var canPublish: Bool { capabilities.contains("publish_repo") }
    public var canOwnerSign: Bool { capabilities.contains("owner_sign") }

    /// True when this session was minted by a mobile-node bind: the phone's key is registered as a
    /// `mobile_node` device and may sign as the owner.
    public var isMobileNode: Bool { sessionKind == "mobile_node" && canOwnerSign }

    public var desktopDevices: [AccountDevice] { devices.filter { $0.kind == "desktop_node" && $0.enabled } }
    public var mobileDevices: [AccountDevice] { devices.filter { $0.kind == "mobile_node" && $0.enabled } }

    /// The `devices[]` entry bound to this session, when the Worker sent one.
    public var thisDevice: AccountDevice? {
        if deviceId.isEmpty { return nil }
        return devices.first { $0.current || $0.id == deviceId }
    }

    /// `AuthSession.fromJson`. `savedAt` falls back to `now` when the payload has no `at`.
    public static func fromJSON(_ json: JSONObject, now: Int64 = Date().epochMillis) -> AuthSession {
        let nodeName: String
        if let value = json["nodeName"], !value.isNull {
            nodeName = value.text
        } else {
            nodeName = json.str("name")
        }
        let at = json["at"].flatMap { $0.isNull ? nil : $0 } ?? .int(now)
        return AuthSession(
            nodeName: nodeName,
            email: json.str("email"),
            status: json.str("status"),
            pubkey: json.str("pubkey"),
            emailVerified: json.bool("emailVerified"),
            isAdmin: json.bool("isAdmin"),
            avatarPng: json.str("avatarPng"),
            avatarUpdatedAt: asInt(json["avatarUpdatedAt"]),
            createdAt: asInt(json["createdAt"]),
            savedAt: asInt(at),
            sessionKind: json.str("sessionKind", "account"),
            deviceKind: json.str("deviceKind"),
            deviceId: json.str("deviceId"),
            desktopCapable: json.bool("desktopCapable"),
            capabilities: json["capabilities"]?.arrayValue.map { $0.map(\.text) } ?? [],
            devices: json["devices"]?.arrayValue.map { $0.compactMap(\.objectValue).map(AccountDevice.fromJSON) } ?? [],
            sessionToken: json.str("sessionToken")
        )
    }

    public func toJSON() -> JSONObject {
        [
            "nodeName": .string(nodeName),
            "email": .string(email),
            "status": .string(status),
            "pubkey": .string(pubkey),
            "emailVerified": .bool(emailVerified),
            "isAdmin": .bool(isAdmin),
            "avatarPng": .string(avatarPng),
            "avatarUpdatedAt": .int(avatarUpdatedAt),
            "createdAt": .int(createdAt),
            "at": .int(savedAt),
            "sessionKind": .string(sessionKind),
            "deviceKind": .string(deviceKind),
            "deviceId": .string(deviceId),
            "desktopCapable": .bool(desktopCapable),
            "capabilities": JSONValue(capabilities),
            "devices": .array(devices.map { .object($0.toJSON()) }),
            "sessionToken": .string(sessionToken),
        ]
    }
}

/// One row of the account's node registry (`devices[]` on login, GET /api/accounts/devices).
public struct AccountDevice: Equatable, Sendable, Identifiable {
    public var id: String
    public var pubkey: String
    public var kind: String
    public var label: String
    public var capabilities: [String]
    public var enabled: Bool
    public var lastSeen: Int64
    public var createdAt: Int64
    /// Unix ms when the web app revoked this node; 0 while active.
    public var revokedAt: Int64
    /// True for the device bound to the session that fetched this list.
    public var current: Bool

    public init(
        id: String,
        pubkey: String,
        kind: String,
        label: String,
        capabilities: [String],
        enabled: Bool,
        lastSeen: Int64,
        createdAt: Int64 = 0,
        revokedAt: Int64 = 0,
        current: Bool = false
    ) {
        self.id = id
        self.pubkey = pubkey
        self.kind = kind
        self.label = label
        self.capabilities = capabilities
        self.enabled = enabled
        self.lastSeen = lastSeen
        self.createdAt = createdAt
        self.revokedAt = revokedAt
        self.current = current
    }

    public var canOwnerSign: Bool { capabilities.contains("owner_sign") }
    public var canHost: Bool { capabilities.contains("host_repo") }
    public var canPublish: Bool { capabilities.contains("publish_repo") }
    public var isRevoked: Bool { revokedAt > 0 || !enabled }
    public var isMobileNode: Bool { kind == "mobile_node" }
    public var isDesktopNode: Bool { kind == "desktop_node" }

    public var displayName: String {
        let clean = label.dartTrimmed
        return clean.isEmpty ? kindLabel : clean
    }

    public var kindLabel: String {
        switch kind {
        case "desktop_node": return "Desktop node"
        case "mobile_node": return "Mobile node"
        case "mobile_client": return "Mobile client"
        default: return "Device"
        }
    }

    public var statusLabel: String {
        if revokedAt > 0 { return "Revoked" }
        if !enabled { return "Disabled" }
        return canOwnerSign ? "Ready for signed controls" : "Browse only"
    }

    /// Short human summary of what this node may do, e.g. "Publishes · signs as owner".
    public var capabilitySummary: String {
        var parts: [String] = []
        if canHost { parts.append("Hosts") }
        if canPublish { parts.append("Publishes") }
        if canOwnerSign { parts.append("signs as owner") }
        if parts.isEmpty { return "Browse only" }
        return capitalizeFirst(parts.joined(separator: " · "))
    }

    /// First and last characters of the device id, for compact display.
    public var shortId: String {
        let clean = id.dartTrimmed
        let units = Array(clean.utf16)
        if units.count <= 10 { return clean }
        let head = String(decoding: units[0..<6], as: UTF16.self)
        let tail = String(decoding: units[(units.count - 4)...], as: UTF16.self)
        return "\(head)…\(tail)"
    }

    public var capabilityLabel: String {
        var labels: [String] = []
        if canHost { labels.append("Host repo") }
        if canPublish { labels.append("Publish repo") }
        if canOwnerSign { labels.append("Owner signing") }
        if labels.isEmpty { labels.append(contentsOf: capabilities.map(AccountDevice.capabilityTitle)) }
        return labels.isEmpty ? "No capabilities" : labels.joined(separator: " · ")
    }

    public func toJSON() -> JSONObject {
        [
            "id": .string(id),
            "pubkey": .string(pubkey),
            "kind": .string(kind),
            "label": .string(label),
            "capabilities": JSONValue(capabilities),
            "enabled": .bool(enabled),
            "lastSeen": .int(lastSeen),
            "createdAt": .int(createdAt),
            "revokedAt": .int(revokedAt),
            "current": .bool(current),
        ]
    }

    public static func fromJSON(_ json: JSONObject) -> AccountDevice {
        let capabilities: [String]
        switch json["capabilities"] {
        case .array(let items)?: capabilities = items.map(\.text)
        case .string(let text)?: capabilities = text.split(separator: ",", omittingEmptySubsequences: true).map(String.init)
        default: capabilities = []
        }
        let enabled = json["enabled"]
        return AccountDevice(
            id: json.str("id"),
            pubkey: json.str("pubkey"),
            kind: json.str("kind"),
            label: json.str("label"),
            capabilities: capabilities,
            // Disabled only by `false` or a numeric zero (0 and 0.0 alike).
            enabled: enabled != .bool(false) && !(enabled?.numEquals(0) ?? false),
            lastSeen: asInt(firstNonNull(json, "lastSeen", "last_seen")),
            createdAt: asInt(firstNonNull(json, "createdAt", "created_at")),
            revokedAt: asInt(firstNonNull(json, "revokedAt", "revoked_at")),
            current: json.bool("current")
        )
    }

    static func capabilityTitle(_ raw: String) -> String {
        let words = raw.split(separator: "_", omittingEmptySubsequences: true).map(String.init)
        if words.isEmpty { return raw }
        return words.map(capitalizeFirst).joined(separator: " ")
    }
}

/// One live sign-in on the account, from GET /api/accounts/sessions. `current` marks the session
/// the listing was fetched with.
public struct AccountSession: Equatable, Sendable, Identifiable {
    public var id: String
    public var deviceLabel: String
    public var ipAddress: String
    public var createdAt: Int64
    public var lastSeenAt: Int64
    public var expiresAt: Int64
    public var current: Bool

    public init(
        id: String,
        deviceLabel: String,
        ipAddress: String = "",
        createdAt: Int64 = 0,
        lastSeenAt: Int64 = 0,
        expiresAt: Int64 = 0,
        current: Bool = false
    ) {
        self.id = id
        self.deviceLabel = deviceLabel
        self.ipAddress = ipAddress
        self.createdAt = createdAt
        self.lastSeenAt = lastSeenAt
        self.expiresAt = expiresAt
        self.current = current
    }

    public static func fromJSON(_ json: JSONObject) -> AccountSession {
        let label = json.str("deviceLabel").dartTrimmed
        return AccountSession(
            id: json.str("id"),
            deviceLabel: label.isEmpty ? "Unknown device" : label,
            ipAddress: json.str("ipAddress").dartTrimmed,
            createdAt: asInt(json["createdAt"]),
            lastSeenAt: asInt(json["lastSeenAt"]),
            expiresAt: asInt(json["expiresAt"]),
            current: json.bool("current")
        )
    }
}

/// The answer of the username availability check on the signup screen.
public struct AccountAvailability: Equatable, Sendable {
    public var ok: Bool
    public var exists: Bool
    public var available: Bool
    public var name: String
    public var message: String

    public init(ok: Bool, exists: Bool, available: Bool, name: String, message: String = "") {
        self.ok = ok
        self.exists = exists
        self.available = available
        self.name = name
        self.message = message
    }
}

/// A failed auth request: `code` is the Worker's `error` (or `http_<status>`), `message` the copy
/// shown to the user.
public struct AuthException: Error, Equatable, Sendable, CustomStringConvertible, LocalizedError {
    public let code: String
    public let message: String

    public init(_ code: String, _ message: String) {
        self.code = code
        self.message = message
    }

    public var description: String { message }
    public var errorDescription: String? { message }
}

/// What the app target knows about this device (UIDevice / utsname). Supplied at startup; the
/// core never asks the OS itself.
public struct DeviceInfo: Equatable, Sendable {
    /// "ios", "android", "macos", "web", ... (`AppBuild.platform`).
    public var platform: String
    /// Marketing name, e.g. "iPhone 15 Pro". Empty when unknown.
    public var modelName: String
    /// `UIDevice.model` or the machine identifier, e.g. "iPhone" / "iPhone16,1".
    public var model: String
    /// `UIDevice.systemName`, e.g. "iOS".
    public var systemName: String
    /// `UIDevice.systemVersion`, e.g. "19.0".
    public var systemVersion: String
    /// `identifierForVendor`, when the app target has one.
    public var vendorId: String

    public init(
        platform: String = "ios",
        modelName: String = "",
        model: String = "",
        systemName: String = "",
        systemVersion: String = "",
        vendorId: String = ""
    ) {
        self.platform = platform
        self.modelName = modelName
        self.model = model
        self.systemName = systemName
        self.systemVersion = systemVersion
        self.vendorId = vendorId
    }

    /// `defaultDeviceLabel()`: "iPhone 15 Pro · iOS 19.0" for the mobile-node bind, never the
    /// user-assigned device name; "Mobile device" when nothing is known.
    public var label: String {
        let fallback = "Mobile device"
        switch platform {
        case "web":
            return "Web browser"
        case "ios":
            let name = modelName.dartTrimmed
            let device = name.isEmpty ? model.dartTrimmed : name
            let os = "\(systemName) \(systemVersion)".dartTrimmed
            return DeviceInfo.joinLabel(device, os, fallback)
        case "android":
            // There is no Android build of this app; on Android the manufacturer travels in
            // `systemName`.
            let brand = systemName.dartTrimmed
            let device = model.dartTrimmed
            let named = device.lowercased().hasPrefix(brand.lowercased()) ? device : "\(brand) \(device)".dartTrimmed
            return DeviceInfo.joinLabel(named, "Android \(systemVersion)", fallback)
        case "macos":
            return DeviceInfo.joinLabel(model, "macOS \(systemVersion)", fallback)
        default:
            return fallback
        }
    }

    static func joinLabel(_ device: String, _ os: String, _ fallback: String) -> String {
        let d = device.dartTrimmed
        let o = os.dartTrimmed
        if d.isEmpty && o.isEmpty { return fallback }
        if d.isEmpty { return o }
        if o.isEmpty { return d }
        let label = "\(d) · \(o)"
        let units = Array(label.utf16)
        return units.count > 120 ? String(decoding: units[0..<120], as: UTF16.self) : label
    }
}

func capitalizeFirst(_ text: String) -> String {
    guard let first = text.first else { return text }
    return first.uppercased() + text.dropFirst()
}

func firstNonNull(_ json: JSONObject, _ keys: String...) -> JSONValue? {
    for key in keys {
        if let value = json[key], !value.isNull { return value }
    }
    return nil
}
