import FMAccount
import FMBase
import Foundation
import Security

/// A Keychain call that did not succeed, with its `OSStatus`.
struct KeychainError: Error, CustomStringConvertible {
    let operation: String
    let status: OSStatus

    var description: String {
        let text: String = (SecCopyErrorMessageString(status, nil) as String?) ?? "OSStatus \(status)"
        return "Keychain \(operation) failed: \(text)"
    }
}

/// Generic-password Keychain items under one service, reporting failures
/// instead of hiding them, so FMAccount's `FallbackSecureStore` can move to
/// memory.
///
/// Items are `AfterFirstUnlockThisDeviceOnly`: the relay reconnects and the
/// account event loop read the identity and the session while the phone is
/// locked, and none of it may travel to another device in a backup.
final class KeychainBackend: ThrowingSecureBackend, @unchecked Sendable {
    private let service: String

    init(service: String = KeychainStore.defaultService) {
        self.service = service
    }

    static func eraseAll() throws {
        let classes: [CFString] = [
            kSecClassGenericPassword, kSecClassInternetPassword, kSecClassKey, kSecClassCertificate,
            kSecClassIdentity,
        ]
        for itemClass in classes {
            let status: OSStatus = SecItemDelete([kSecClass as String: itemClass] as CFDictionary)
            if status != errSecSuccess && status != errSecItemNotFound {
                throw KeychainError(operation: "erase", status: status)
            }
        }
    }

    static func isLockedUntilFirstUnlock() -> Bool {
        let lookup: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: KeychainStore.defaultService,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var found: CFTypeRef?
        return SecItemCopyMatching(lookup as CFDictionary, &found) == errSecInteractionNotAllowed
    }

    private func query(_ key: String?) -> [String: Any] {
        var query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
        ]
        if let key = key {
            query[kSecAttrAccount as String] = key
            query[kSecAttrAccessGroup as String] = KeychainPlacement.group(forSecureKey: key)
        }
        return query
    }

    static func migrateAccessGroups() {
        for service in [KeychainPlacement.secureService, KeychainPlacement.pushService] {
            let lookup: [String: Any] = [
                kSecClass as String: kSecClassGenericPassword,
                kSecAttrService as String: service,
                kSecAttrAccessGroup as String: KeychainPlacement.sharedGroup,
                kSecReturnAttributes as String: true,
                kSecReturnData as String: true,
                kSecMatchLimit as String: kSecMatchLimitAll,
            ]
            var found: CFTypeRef?
            guard SecItemCopyMatching(lookup as CFDictionary, &found) == errSecSuccess, let rows = found as? [[String: Any]]
            else { continue }
            for row in rows {
                guard let account = row[kSecAttrAccount as String] as? String, let data = row[kSecValueData as String] as? Data,
                    let target = KeychainPlacement.destination(
                        service: service, account: account, group: KeychainPlacement.sharedGroup)
                else { continue }
                let insert: [String: Any] = [
                    kSecClass as String: kSecClassGenericPassword,
                    kSecAttrService as String: service,
                    kSecAttrAccount as String: account,
                    kSecAttrAccessGroup as String: target,
                    kSecValueData as String: data,
                    kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
                ]
                let added: OSStatus = SecItemAdd(insert as CFDictionary, nil)
                guard added == errSecSuccess || added == errSecDuplicateItem else { continue }
                let old: [String: Any] = [
                    kSecClass as String: kSecClassGenericPassword,
                    kSecAttrService as String: service,
                    kSecAttrAccount as String: account,
                    kSecAttrAccessGroup as String: KeychainPlacement.sharedGroup,
                ]
                SecItemDelete(old as CFDictionary)
            }
        }
    }

    func read(_ key: String) throws -> String? {
        var lookup: [String: Any] = query(key)
        lookup[kSecReturnData as String] = true
        lookup[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        let status: OSStatus = SecItemCopyMatching(lookup as CFDictionary, &item)
        if status == errSecItemNotFound {
            return nil
        }
        guard status == errSecSuccess else {
            throw KeychainError(operation: "read", status: status)
        }
        guard let data = item as? Data else {
            return nil
        }
        return String(data: data, encoding: .utf8)
    }

    func write(_ key: String, _ value: String) throws {
        // Delete-then-add keeps the accessibility class authoritative if it
        // ever changes between versions; SecItemUpdate would keep the old one.
        let removed: OSStatus = SecItemDelete(query(key) as CFDictionary)
        if removed != errSecSuccess && removed != errSecItemNotFound {
            throw KeychainError(operation: "write", status: removed)
        }
        var insert: [String: Any] = query(key)
        insert[kSecValueData as String] = Data(value.utf8)
        insert[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        let added: OSStatus = SecItemAdd(insert as CFDictionary, nil)
        guard added == errSecSuccess else {
            throw KeychainError(operation: "write", status: added)
        }
    }

    func delete(_ key: String) throws {
        let status: OSStatus = SecItemDelete(query(key) as CFDictionary)
        if status != errSecSuccess && status != errSecItemNotFound {
            throw KeychainError(operation: "delete", status: status)
        }
    }

    func readAll() throws -> [String: String] {
        var lookup: [String: Any] = query(nil)
        lookup[kSecReturnAttributes as String] = true
        lookup[kSecReturnData as String] = true
        lookup[kSecMatchLimit as String] = kSecMatchLimitAll
        var items: CFTypeRef?
        let status: OSStatus = SecItemCopyMatching(lookup as CFDictionary, &items)
        if status == errSecItemNotFound {
            return [:]
        }
        guard status == errSecSuccess else {
            throw KeychainError(operation: "readAll", status: status)
        }
        guard let rows = items as? [[String: Any]] else {
            return [:]
        }
        var values: [String: String] = [:]
        for row in rows {
            guard let account = row[kSecAttrAccount as String] as? String,
                let data = row[kSecValueData as String] as? Data,
                let text = String(data: data, encoding: .utf8)
            else { continue }
            values[account] = text
        }
        return values
    }
}

/// `SecureStore` over the Keychain: the first Keychain failure moves reads and
/// writes to memory for the rest of the session (FMAccount's
/// `FallbackSecureStore`), while deletes keep reaching the Keychain. This is
/// what AppServices registers as the app's SecureStore
/// (`Services.get(SecureStore.self)`).
///
/// Every key except the shared ones (sessions, coding-agent keys) is routed to
/// the active project's namespace (FMAccount's `ProjectRoutedSecureStore`), so
/// the node key, owner key, vault copy and secrets of one project are never
/// read through another's services. The first project keeps the unprefixed
/// (legacy) keys; the others use `p/<projectId>/`.
final class KeychainStore: SecureStore, @unchecked Sendable {
    static let defaultService: String = "com.forkmesh.forkmesh.secure"

    private let store: FallbackSecureStore
    private let routed: ProjectRoutedSecureStore

    private static let migrated: Void = KeychainBackend.migrateAccessGroups()

    init(service: String = KeychainStore.defaultService, router: ProjectScopeRouter = AppProjectScope.router) {
        _ = KeychainStore.migrated
        let fallback: FallbackSecureStore = FallbackSecureStore(backend: KeychainBackend(service: service))
        store = fallback
        routed = ProjectRoutedSecureStore(base: fallback, router: router)
    }

    /// True once the Keychain failed and secrets live in memory until the
    /// app quits.
    var isUsingFallback: Bool {
        return store.isUsingFallback
    }

    /// The Keychain items underneath, addressed by full keys (`ProjectStorage.erase`).
    var base: SecureStore { store }

    var router: ProjectScopeRouter { routed.router }

    func read(_ key: String) -> String? {
        return routed.read(key)
    }

    func write(_ key: String, _ value: String) {
        routed.write(key, value)
    }

    func delete(_ key: String) {
        routed.delete(key)
    }

    func readAll() -> [String: String] {
        return routed.readAll()
    }
}
