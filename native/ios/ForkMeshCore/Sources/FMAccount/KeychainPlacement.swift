import Foundation

public enum KeychainPlacement {
    public static let teamPrefix = "742ZANCB57"
    public static let appOnlyGroup = "\(teamPrefix).com.forkmesh.forkmesh.private"
    public static let pushGroup = "\(teamPrefix).com.forkmesh.forkmesh.push"
    public static let sharedGroup = "\(teamPrefix).com.forkmesh.forkmesh"
    public static let secureService = "com.forkmesh.forkmesh.secure"
    public static let pushService = "com.forkmesh.forkmesh.push"
    public static let chatKeyPrefix = "chat/e2ee/"

    public static func unscoped(_ key: String) -> Substring {
        guard ProjectKeyPolicy.isNamespaced(key) else { return Substring(key) }
        let rest = key.dropFirst(ProjectScope.namespaceRoot.count)
        guard let slash = rest.firstIndex(of: "/") else { return Substring(key) }
        return rest[rest.index(after: slash)...]
    }

    public static func isChatKey(_ key: String) -> Bool { unscoped(key).hasPrefix(chatKeyPrefix) }

    public static func group(forSecureKey key: String) -> String { isChatKey(key) ? pushGroup : appOnlyGroup }

    public static func destination(service: String, account: String, group: String) -> String? {
        guard group == sharedGroup else { return nil }
        switch service {
        case secureService: return self.group(forSecureKey: account)
        case pushService: return pushGroup
        default: return nil
        }
    }
}
