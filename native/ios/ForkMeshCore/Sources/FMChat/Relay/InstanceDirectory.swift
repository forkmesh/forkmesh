import FMAccount
import FMBase
import FMNet
import Foundation

public struct InstanceDirectoryEntry: Equatable, Sendable, Identifiable {
    public let id: String
    public let name: String
    public let logoUrl: String
    public let online: Bool

    public init(id: String, name: String, logoUrl: String = "", online: Bool = true) {
        self.id = id
        self.name = name
        self.logoUrl = logoUrl
        self.online = online
    }

    public var initial: String { String((name.isEmpty ? id : name).prefix(1)).uppercased() }
}

public struct InstanceDirectory: Sendable {
    let http: HTTPClient
    let origin: String
    let timeout: TimeInterval

    public init(http: HTTPClient, origin: String = "https://\(AppEnv.prodHost)", timeout: TimeInterval = 8) {
        self.http = http
        self.origin = origin
        self.timeout = timeout
    }

    public static let flagship = InstanceDirectoryEntry(id: AppEnv.prodHost, name: "ForkMesh")

    static let logoPath: String = #"^/api/start/instances/[0-9a-f]{32}/logo\?v=\d+$"#

    public func list() async -> [InstanceDirectoryEntry] {
        guard let url = URL(string: origin + "/api/start/instances") else { return [InstanceDirectory.flagship] }
        let request = HTTPRequest(
            method: "GET", url: url, headers: ["Accept": "application/json"], timeout: timeout, followRedirects: true)
        guard let response = try? await Deadline.send(http, request, seconds: timeout), response.status == 200 else {
            return [InstanceDirectory.flagship]
        }
        return entries(response.jsonObject["instances"]?.arrayValue ?? [])
    }

    func entries(_ rows: [JSONValue]) -> [InstanceDirectoryEntry] {
        var seen: Set<String> = [InstanceDirectory.flagship.id]
        var found: [InstanceDirectoryEntry] = [InstanceDirectory.flagship]
        for row in rows where row["listed"]?.boolValue == true {
            let url: String = row["url"]?.stringValue?.dartTrimmed ?? ""
            guard url.lowercased().hasPrefix("https://"), let id = RelayProfile.normalizeOrigin(url),
                !seen.contains(id)
            else { continue }
            seen.insert(id)
            let logo: String = row["logo"]?.stringValue ?? ""
            let name: String = String((row["name"]?.stringValue?.dartTrimmed ?? "").prefix(40))
            found.append(
                InstanceDirectoryEntry(
                    id: id, name: name.isEmpty ? id : name,
                    logoUrl: logo.range(of: InstanceDirectory.logoPath, options: .regularExpression) == nil
                        ? "" : origin + logo,
                    online: row["status"]?.stringValue == "online"))
        }
        return found
    }
}
