// Push credentials in the keychain, per-project items keyed by host
// (2454591cb). It lives in Shared/ because both the app and the notification
// service extension compile it. The group and service are the ones the retired
// Flutter app wrote, so its items are read as they are: the format is the same.

import FMBase
import FMCrypto
import FMPush
import Foundation
import Security

/// What the notification service extension needs to turn a blind push into a
/// readable banner: which relay to ask, which account to ask about, and the
/// session that authorises the read.
struct PushCredentials {
    let origin: String
    let account: String
    let session: String
    var deviceKeyName: String = ""
    var preview: String = ""
}

/// The one place the app and its notification extension exchange a session.
///
/// They are separate processes in separate sandboxes, so the extension cannot
/// see the app's keychain items at all. A shared keychain group is the narrow
/// bridge between them — narrow deliberately: one item, holding the same
/// session token the app already sends as a bearer header, and nothing else.
///
/// Two details matter and neither is obvious.
///
/// `kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly` is required, not preferred. A push
/// routinely arrives while the phone is sitting locked on a desk, and the
/// extension runs right then; the default `WhenUnlocked` class would make
/// every one of those reads fail with `errSecInteractionNotAllowed` and the
/// banner would silently fall back to the generic text — the exact symptom
/// this whole feature exists to remove.
///
/// The access group must name the team prefix explicitly. Without it each
/// target defaults to a group derived from its own bundle id
/// (`com.forkmesh.forkmesh` versus `com.forkmesh.forkmesh.NotificationService`)
/// and the two would write to, and read from, different keychains while every
/// call still returned `errSecSuccess`.
enum PushCredentialStore {
    /// Matches `keychain-access-groups` in both targets' entitlements, where it
    /// is written as `$(AppIdentifierPrefix)com.forkmesh.forkmesh`. The prefix
    /// is the Apple team id, the same `TEAM_ID` tools/generate_project.rb signs
    /// with.
    static let accessGroup = "742ZANCB57.com.forkmesh.forkmesh"
    static let service = "com.forkmesh.forkmesh.push"
    static let account = "session"
    static let secureService = "com.forkmesh.forkmesh.secure"

    /// Each project (ForkMesh, ForkMesh Next, a self-hosted instance) the app
    /// is signed in to keeps its own item, keyed by its host, so a push from
    /// one instance is expanded with that instance's session and never with
    /// another's. The unkeyed item is still written for the most recent
    /// project: it is what a push from a relay that names no origin falls back
    /// to, as before projects existed.
    static func save(
        origin: String, account accountName: String, session: String, deviceKeyName: String = "", preview: String = ""
    ) {
        var payload: [String: String] = [
            "origin": origin,
            "account": accountName,
            "session": session,
        ]
        if !deviceKeyName.isEmpty { payload["deviceKeyName"] = deviceKeyName }
        if !preview.isEmpty { payload["preview"] = preview }
        guard !origin.isEmpty, !accountName.isEmpty, !session.isEmpty,
            let data = try? JSONSerialization.data(withJSONObject: payload)
        else {
            clear(origin: origin)
            return
        }
        write(data, item: account)
        if let key = hostKey(origin) { write(data, item: key) }
    }

    /// The credentials for the instance at [origin] (a URL or a bare host), or
    /// for the most recent project when the push named none.
    static func load(origin: String) -> PushCredentials? {
        guard let key = hostKey(origin) else { return load() }
        if let scoped = load(item: key) { return scoped }
        // A session shared before items were keyed by host: only good for the
        // host it names.
        guard let latest = load(), hostKey(latest.origin) == key else { return nil }
        return latest
    }

    static func load() -> PushCredentials? { load(item: account) }

    /// Called on sign-out of one project: drops that project's item, and the
    /// unkeyed one when it was that project's.
    static func clear(origin: String) {
        guard let key = hostKey(origin) else {
            clear()
            return
        }
        SecItemDelete(query(item: key) as CFDictionary)
        if let latest = load(), hostKey(latest.origin) == key {
            SecItemDelete(query() as CFDictionary)
        }
    }

    /// `session@<host[:port]>`, lowercased, for a URL or a bare host.
    static func hostKey(_ origin: String) -> String? {
        let trimmed = origin.trimmingCharacters(in: .whitespaces).lowercased()
        if trimmed.isEmpty { return nil }
        let url = trimmed.contains("://") ? trimmed : "https://" + trimmed
        guard let components = URLComponents(string: url), let host = components.host,
            !host.isEmpty
        else { return nil }
        if let port = components.port, port != 443, port != 80 {
            return "\(account)@\(host):\(port)"
        }
        return "\(account)@\(host)"
    }

    private static func write(_ data: Data, item: String) {
        // Delete-then-add rather than SecItemUpdate: an update against a missing
        // item is an error, and the two-step keeps the accessibility class
        // authoritative when it changes between app versions.
        SecItemDelete(query(item: item) as CFDictionary)
        var insert = query(item: item)
        insert[kSecValueData as String] = data
        insert[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        SecItemAdd(insert as CFDictionary, nil)
    }

    private static func load(item: String) -> PushCredentials? {
        var lookup = query(item: item)
        lookup[kSecReturnData as String] = true
        lookup[kSecMatchLimit as String] = kSecMatchLimitOne
        var found: CFTypeRef?
        guard SecItemCopyMatching(lookup as CFDictionary, &found) == errSecSuccess,
            let data = found as? Data,
            let fields = try? JSONSerialization.jsonObject(with: data) as? [String: String]
        else {
            return nil
        }
        let origin = fields["origin"] ?? ""
        let account = fields["account"] ?? ""
        let session = fields["session"] ?? ""
        if origin.isEmpty || account.isEmpty || session.isEmpty { return nil }
        return PushCredentials(
            origin: origin, account: account, session: session, deviceKeyName: fields["deviceKeyName"] ?? "",
            preview: fields["preview"] ?? "")
    }

    static func setPreview(_ value: String) {
        let lookup: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccessGroup as String: accessGroup,
            kSecReturnAttributes as String: true,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitAll,
        ]
        var found: CFTypeRef?
        guard SecItemCopyMatching(lookup as CFDictionary, &found) == errSecSuccess, let rows = found as? [[String: Any]]
        else { return }
        for row in rows {
            guard let item = row[kSecAttrAccount as String] as? String, item.hasPrefix(account),
                let data = row[kSecValueData as String] as? Data,
                var fields = (try? JSONSerialization.jsonObject(with: data)) as? [String: String]
            else { continue }
            fields["preview"] = value
            guard let updated = try? JSONSerialization.data(withJSONObject: fields) else { continue }
            write(updated, item: item)
        }
    }

    static let legacyDeviceKeyNames = ["chat/e2ee/x25519PrivateV2"]

    static func secureItem(_ name: String) -> String? {
        guard !name.isEmpty else { return nil }
        let lookup: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: secureService,
            kSecAttrAccount as String: name,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var found: CFTypeRef?
        guard SecItemCopyMatching(lookup as CFDictionary, &found) == errSecSuccess, let data = found as? Data else {
            return nil
        }
        return String(data: data, encoding: .utf8)
    }

    static func chatKeyPrefix(deviceKeyName name: String) -> String {
        for suffix in [RoomSeal.privateKeyStoreKey] + legacyDeviceKeyNames where name.hasSuffix(suffix) {
            return String(name.dropLast(suffix.count))
        }
        return ""
    }

    static func chatStore(deviceKeyName: String) -> SecureStore {
        PushSecureReader(prefix: chatKeyPrefix(deviceKeyName: deviceKeyName))
    }

    /// Called on sign-out. A stale session would keep fetching a signed-out
    /// account's notifications into banners on a handset that has left it.
    static func clear() {
        SecItemDelete(query() as CFDictionary)
        SecItemDelete(query(item: PushRoomKeys.item) as CFDictionary)
    }

    private static func query(item: String = account) -> [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: item,
            kSecAttrAccessGroup as String: accessGroup,
        ]
    }
}

final class PushSecureReader: SecureStore, @unchecked Sendable {
    let prefix: String

    init(prefix: String) {
        self.prefix = prefix
    }

    func read(_ key: String) -> String? { PushCredentialStore.secureItem(prefix + key) }

    func write(_ key: String, _ value: String) {}

    func delete(_ key: String) {}

    func readAll() -> [String: String] { [:] }
}
