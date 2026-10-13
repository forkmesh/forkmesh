import FMApi
import FMBase
import Foundation
#if canImport(Security)
import Security
#endif

public enum ShareDrop {
    public static let service = "com.forkmesh.forkmesh.share"
    public static let manifestItem = "manifest"
    public static let maxImageBytes = 8 * 1_024 * 1_024
    public static let clockSkewMs: Int64 = 5 * 60 * 1_000

    public static func imageItem(_ index: Int) -> String { "image-\(index)" }

    public static func items(
        text: String, urls: [String], images: [Data], id: String, createdAtMs: Int64
    ) throws -> (handoff: ShareHandoff, items: [String: Data]) {
        let clean = try ShareHandoffStore.clean(text: text, urls: urls, images: images, repo: "")
        if clean.images.contains(where: { $0.count > maxImageBytes }) {
            throw ShareHandoffError("One of those images is too large to send.")
        }
        guard ShareHandoffStore.validId(id) else { throw ShareHandoffError("Could not save what you shared.") }
        var items: [String: Data] = [:]
        var names: [String] = []
        for (index, data) in clean.images.enumerated() {
            let name = imageItem(index)
            items[name] = data
            names.append(name)
        }
        let handoff = ShareHandoff(
            id: id, createdAtMs: createdAtMs, text: clean.text, urls: clean.urls, images: names, repo: clean.repo)
        guard let manifest = try? JSONEncoder().encode(handoff), manifest.count <= ShareHandoffStore.manifestMaxBytes
        else { throw ShareHandoffError("Could not save what you shared.") }
        items[manifestItem] = manifest
        return (handoff, items)
    }

    public static func read(_ load: (String) -> Data?, nowMs: Int64) -> (handoff: ShareHandoff, images: [Data])? {
        guard let manifest = load(manifestItem), manifest.count <= ShareHandoffStore.manifestMaxBytes,
              var handoff = try? JSONDecoder().decode(ShareHandoff.self, from: manifest),
              ShareHandoffStore.validId(handoff.id),
              nowMs - handoff.createdAtMs < ShareHandoffStore.maxAgeMs,
              handoff.createdAtMs - nowMs < clockSkewMs
        else { return nil }
        var images: [Data] = []
        for (index, name) in handoff.images.prefix(ShareHandoffStore.maxImages).enumerated() {
            guard name == imageItem(index), let data = load(name), !data.isEmpty, data.count <= maxImageBytes else { continue }
            images.append(data)
        }
        handoff.text = String(handoff.text.prefix(ShareHandoffStore.maxTextLength))
        handoff.urls = ShareHandoffStore.cleanURLs(handoff.urls)
        handoff.repo = RepoRef(slug: handoff.repo)?.slug ?? ""
        return (handoff, images)
    }
}

#if canImport(Security)
public struct ShareKeychainDrop: Sendable {
    public static let teamGroup = "742ZANCB57.com.forkmesh.forkmesh"

    public let accessGroup: String

    public init(accessGroup: String = ShareKeychainDrop.teamGroup) {
        self.accessGroup = accessGroup
    }

    @discardableResult
    public func save(text: String, urls: [String], images: [Data]) throws -> ShareHandoff {
        let (handoff, items) = try ShareDrop.items(
            text: text, urls: urls, images: images, id: UUID().uuidString, createdAtMs: SystemClock().nowMillis)
        clear()
        for (name, data) in items where name != ShareDrop.manifestItem {
            if !write(data, item: name) {
                clear()
                throw ShareHandoffError("Could not save what you shared.")
            }
        }
        guard let manifest = items[ShareDrop.manifestItem], write(manifest, item: ShareDrop.manifestItem) else {
            clear()
            throw ShareHandoffError("Could not save what you shared.")
        }
        return handoff
    }

    public func take() -> (handoff: ShareHandoff, images: [Data])? {
        guard let drop = ShareDrop.read({ load(item: $0) }, nowMs: SystemClock().nowMillis) else {
            clear()
            return nil
        }
        clear()
        return drop
    }

    public func clear() {
        SecItemDelete(query() as CFDictionary)
    }

    private func write(_ data: Data, item: String) -> Bool {
        var insert = query(item: item)
        insert[kSecValueData as String] = data
        insert[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        return SecItemAdd(insert as CFDictionary, nil) == errSecSuccess
    }

    private func load(item: String) -> Data? {
        var lookup = query(item: item)
        lookup[kSecReturnData as String] = true
        lookup[kSecMatchLimit as String] = kSecMatchLimitOne
        var found: CFTypeRef?
        guard SecItemCopyMatching(lookup as CFDictionary, &found) == errSecSuccess else { return nil }
        return found as? Data
    }

    private func query(item: String? = nil) -> [String: Any] {
        var query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: ShareDrop.service,
            kSecAttrAccessGroup as String: accessGroup,
        ]
        if let item {
            query[kSecAttrAccount as String] = item
        }
        return query
    }
}
#endif
