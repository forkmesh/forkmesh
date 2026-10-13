import Foundation

#if canImport(Security) && canImport(LocalAuthentication)
import LocalAuthentication
import Security

public enum VaultAutofillShared {
    public static let keychainGroupSuffix = "com.forkmesh.forkmesh"
    public static let keyService = "com.forkmesh.vault.autofill"
    public static let keyAccount = "autofill-cache-key"
    public static let blobAccount = "autofill-cache-blob"
    public static let linksAccount = "autofill-links"
    public static let countKey = "vault.autofill.logins"

    public static func accessGroup() -> String? {
        let probe: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: "com.forkmesh.vault.autofill.probe",
            kSecAttrAccount as String: "probe",
            kSecReturnAttributes as String: true,
        ]
        var result: CFTypeRef?
        var status = SecItemCopyMatching(probe as CFDictionary, &result)
        if status == errSecItemNotFound {
            var insert = probe
            insert.removeValue(forKey: kSecReturnAttributes as String)
            insert[kSecValueData as String] = Data()
            insert[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
            SecItemAdd(insert as CFDictionary, nil)
            status = SecItemCopyMatching(probe as CFDictionary, &result)
        }
        guard status == errSecSuccess, let attributes = result as? [String: Any],
            let group = attributes[kSecAttrAccessGroup as String] as? String
        else { return nil }
        let prefix = group.split(separator: ".").first.map(String.init) ?? ""
        return prefix.isEmpty ? nil : "\(prefix).\(keychainGroupSuffix)"
    }

    public static var loginCount: Int {
        get { UserDefaults.standard.integer(forKey: countKey) }
        set { UserDefaults.standard.set(newValue, forKey: countKey) }
    }

    public static func vault(context: LAContext? = nil) -> VaultAutofillVault {
        let group = accessGroup()
        return VaultAutofillVault(
            keys: VaultKeychainAutofillKeyStore(accessGroup: group, context: context),
            blobs: VaultKeychainAutofillBlobStore(accessGroup: group))
    }

    public static func links() -> VaultKeychainAutofillLinkStore {
        VaultKeychainAutofillLinkStore(accessGroup: accessGroup())
    }
}

public struct VaultKeychainError: Error, Equatable {
    public let status: OSStatus
}

struct VaultKeychainItem {
    let account: String
    let accessGroup: String?

    func base() -> [String: Any] {
        var query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: VaultAutofillShared.keyService,
            kSecAttrAccount as String: account,
            kSecAttrSynchronizable as String: false,
        ]
        if let accessGroup { query[kSecAttrAccessGroup as String] = accessGroup }
        return query
    }

    func read(context: LAContext? = nil) throws -> Data? {
        var query = base()
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        if let context { query[kSecUseAuthenticationContext as String] = context }
        var result: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess, let data = result as? Data else { throw VaultKeychainError(status: status) }
        return data
    }

    func write(_ data: Data, accessControl: SecAccessControl? = nil, accessible: CFString = kSecAttrAccessibleWhenPasscodeSetThisDeviceOnly) throws {
        try delete()
        var insert = base()
        insert[kSecValueData as String] = data
        if let accessControl {
            insert[kSecAttrAccessControl as String] = accessControl
        } else {
            insert[kSecAttrAccessible as String] = accessible
        }
        let status = SecItemAdd(insert as CFDictionary, nil)
        guard status == errSecSuccess else { throw VaultKeychainError(status: status) }
    }

    func delete() throws {
        let status = SecItemDelete(base() as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else { throw VaultKeychainError(status: status) }
    }
}

public final class VaultKeychainAutofillKeyStore: VaultAutofillKeyStore {
    private let item: VaultKeychainItem
    private let context: LAContext?

    public init(accessGroup: String?, context: LAContext? = nil) {
        item = VaultKeychainItem(account: VaultAutofillShared.keyAccount, accessGroup: accessGroup)
        self.context = context
    }

    public func readKey() throws -> [UInt8]? {
        try item.read(context: context).map { [UInt8]($0) }
    }

    public func writeKey(_ key: [UInt8]) throws {
        var error: Unmanaged<CFError>?
        guard let access = SecAccessControlCreateWithFlags(
            nil, kSecAttrAccessibleWhenPasscodeSetThisDeviceOnly, .userPresence, &error)
        else { throw VaultKeychainError(status: errSecParam) }
        try item.write(Data(key), accessControl: access)
    }

    public func deleteKey() throws {
        try item.delete()
    }
}

public final class VaultKeychainAutofillBlobStore: VaultAutofillBlobStore {
    private let item: VaultKeychainItem

    public init(accessGroup: String?) {
        item = VaultKeychainItem(account: VaultAutofillShared.blobAccount, accessGroup: accessGroup)
    }

    public func readBlob() throws -> Data? { try item.read() }

    public func writeBlob(_ data: Data) throws {
        try item.write(data, accessible: kSecAttrAccessibleWhenPasscodeSetThisDeviceOnly)
    }

    public func deleteBlob() throws { try item.delete() }
}

public final class VaultKeychainAutofillLinkStore: VaultAutofillLinkStore {
    private let item: VaultKeychainItem

    public init(accessGroup: String?) {
        item = VaultKeychainItem(account: VaultAutofillShared.linksAccount, accessGroup: accessGroup)
    }

    public func readLinks() -> VaultAutofillLinks {
        guard let data = try? item.read() else { return VaultAutofillLinks() }
        return VaultAutofillLinks(data: data)
    }

    public func writeLinks(_ links: VaultAutofillLinks) {
        if links.isEmpty {
            try? item.delete()
        } else {
            try? item.write(links.data, accessible: kSecAttrAccessibleWhenUnlockedThisDeviceOnly)
        }
    }
}
#endif
