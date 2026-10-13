import Foundation
#if canImport(Security)
import Security
#endif

public protocol SharedItemStore: AnyObject, Sendable {
    func data(_ item: String) -> Data?
    @discardableResult func set(_ data: Data, _ item: String) -> Bool
    func remove(_ item: String)
}

public final class InMemorySharedItemStore: SharedItemStore, @unchecked Sendable {
    private let lock = NSLock()
    private var items: [String: Data] = [:]

    public init() {}

    public func data(_ item: String) -> Data? {
        lock.lock()
        defer { lock.unlock() }
        return items[item]
    }

    @discardableResult
    public func set(_ data: Data, _ item: String) -> Bool {
        lock.lock()
        items[item] = data
        lock.unlock()
        return true
    }

    public func remove(_ item: String) {
        lock.lock()
        items.removeValue(forKey: item)
        lock.unlock()
    }
}

public final class DirectorySharedItemStore: SharedItemStore, @unchecked Sendable {
    public let directory: URL

    public init(directory: URL) {
        self.directory = directory
    }

    private func url(_ item: String) -> URL { directory.appendingPathComponent(item) }

    public func data(_ item: String) -> Data? {
        try? Data(contentsOf: url(item))
    }

    @discardableResult
    public func set(_ data: Data, _ item: String) -> Bool {
        do {
            #if os(iOS) || os(watchOS)
            try data.write(to: url(item), options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
            #else
            try data.write(to: url(item), options: [.atomic])
            #endif
            return true
        } catch {
            return false
        }
    }

    public func remove(_ item: String) {
        try? FileManager.default.removeItem(at: url(item))
    }
}

#if canImport(Security)
public final class KeychainSharedItemStore: SharedItemStore, @unchecked Sendable {
    public let service: String
    public let accessGroup: String

    public init(service: String, accessGroup: String) {
        self.service = service
        self.accessGroup = accessGroup
    }

    private func query(_ item: String) -> [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: item,
            kSecAttrAccessGroup as String: accessGroup,
            kSecAttrSynchronizable as String: false,
        ]
    }

    public func data(_ item: String) -> Data? {
        var lookup = query(item)
        lookup[kSecReturnData as String] = true
        lookup[kSecMatchLimit as String] = kSecMatchLimitOne
        var found: CFTypeRef?
        guard SecItemCopyMatching(lookup as CFDictionary, &found) == errSecSuccess else { return nil }
        return found as? Data
    }

    @discardableResult
    public func set(_ data: Data, _ item: String) -> Bool {
        let update: [String: Any] = [kSecValueData as String: data]
        let status = SecItemUpdate(query(item) as CFDictionary, update as CFDictionary)
        if status == errSecSuccess { return true }
        guard status == errSecItemNotFound else { return false }
        var insert = query(item)
        insert[kSecValueData as String] = data
        insert[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        return SecItemAdd(insert as CFDictionary, nil) == errSecSuccess
    }

    public func remove(_ item: String) {
        SecItemDelete(query(item) as CFDictionary)
    }
}
#endif
