import FMBase
import FMNet
import Foundation

public struct UpdateRequirement: Equatable, Sendable {
    public let running: String
    public let minimum: String
    public let storeURL: URL?

    public init(running: String, minimum: String, storeURL: URL? = nil) {
        self.running = running
        self.minimum = minimum
        self.storeURL = storeURL
    }

    public var summary: String {
        let shown = running.trimmingCharacters(in: .whitespaces).isEmpty ? "(unknown build)" : running
        return "ForkMesh \(shown) is older than the minimum supported version \(minimum)"
    }

    public static let advice = "Update ForkMesh from the App Store to keep using it."
}

public struct AvailableUpdate: Equatable, Sendable {
    public let version: String
    public let storeURL: URL?

    public init(version: String, storeURL: URL? = nil) {
        self.version = version
        self.storeURL = storeURL
    }

    public var label: String {
        version.hasPrefix("v") ? version : "v\(version)"
    }
}

public enum ClientVersionGate {
    public static let platformKey = "ios"
    public static let defaultStoreURL = URL(string: "https://apps.apple.com/app/id6803060735")
    public static let storeLookupURL = URL(string: "https://itunes.apple.com/lookup?id=6803060735")

    public static func versionNumbers(_ version: String) -> [Int]? {
        var text = version.trimmingCharacters(in: .whitespaces)
        if text.hasPrefix("v") || text.hasPrefix("V") { text.removeFirst() }
        let release = text.prefix { $0.isNumber || $0 == "." }
        let parts = release.split(separator: ".", omittingEmptySubsequences: false)
        guard !parts.isEmpty, parts.count <= 4 else { return nil }
        var numbers: [Int] = []
        for part in parts {
            guard let value = Int(part) else { return nil }
            numbers.append(value)
        }
        return numbers
    }

    public static func isTooOld(running: String, minimum: String) -> Bool {
        guard let have = versionNumbers(running), let floor = versionNumbers(minimum) else { return false }
        let count = max(have.count, floor.count)
        for index in 0..<count {
            let left = index < have.count ? have[index] : 0
            let right = index < floor.count ? floor[index] : 0
            if left != right { return left < right }
        }
        return false
    }

    public static func minimum(in body: JSONObject) -> (version: String, storeURL: URL?)? {
        if let clients = body["minimumClientVersions"]?.objectValue,
           let value = clients[platformKey]?.stringValue, !value.isEmpty {
            let store = body["appStoreUrl"]?.stringValue.flatMap { URL(string: $0) }
            return (value, store)
        }
        if let value = body["minimumIosVersion"]?.stringValue, !value.isEmpty {
            let store = body["appStoreUrl"]?.stringValue.flatMap { URL(string: $0) }
            return (value, store)
        }
        return nil
    }

    public static func requirement(body: JSONObject, running: String) -> UpdateRequirement? {
        guard let floor = minimum(in: body), isTooOld(running: running, minimum: floor.version) else { return nil }
        return UpdateRequirement(running: running, minimum: floor.version, storeURL: floor.storeURL ?? defaultStoreURL)
    }

    public static func requirement(status: Int, running: String) -> UpdateRequirement? {
        guard status == 426 else { return nil }
        return UpdateRequirement(running: running, minimum: "a newer version", storeURL: defaultStoreURL)
    }

    public static func check(http: HTTPClient, origin: String, running: String, timeout: TimeInterval = 8) async
        -> UpdateRequirement?
    {
        guard let url = URL(string: "\(origin)/api/version") else { return nil }
        let request = HTTPRequest(method: "GET", url: url, headers: ["Accept": "application/json"], timeout: timeout)
        guard let response = try? await http.send(request) else { return nil }
        if let required = requirement(status: response.status, running: running) { return required }
        guard response.status == 200 else { return nil }
        return requirement(body: response.jsonObject, running: running)
    }

    public static func availableUpdate(lookup body: JSONObject, running: String) -> AvailableUpdate? {
        guard let release = body["results"]?.arrayValue?.first?.objectValue,
              let version = release["version"]?.stringValue, !version.isEmpty,
              isTooOld(running: running, minimum: version) else { return nil }
        let store = release["trackViewUrl"]?.stringValue.flatMap { URL(string: $0) }
        return AvailableUpdate(version: version, storeURL: store ?? defaultStoreURL)
    }

    public static func availableUpdate(http: HTTPClient, running: String, timeout: TimeInterval = 8) async
        -> AvailableUpdate?
    {
        guard let url = storeLookupURL else { return nil }
        let request = HTTPRequest(method: "GET", url: url, headers: ["Accept": "application/json"], timeout: timeout)
        guard let response = try? await http.send(request), response.status == 200 else { return nil }
        return availableUpdate(lookup: response.jsonObject, running: running)
    }
}
