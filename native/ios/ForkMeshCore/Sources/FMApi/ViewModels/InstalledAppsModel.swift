import FMBase
import Foundation
import Observation

extension ApiService {
    public var appsClient: AppsClient {
        AppsClient { [weak self] method, path, body in
            guard let self else { throw OrgApiException("network_error", "Could not reach ForkMesh.") }
            return try await self.accountApiCall(method, path, body)
        }
    }
}

public enum InstalledAppsCopy {
    public static let title = "Installed apps"
    public static let empty = "No apps installed"
    public static let emptyDetail = "Apps you or your organizations install through the App Directory show up here."
    public static let directory = "Browse the App Directory"
    public static let directoryPath = "/dashboard/apps"
    public static let signedOut = "Sign in to see the apps installed for you and your organizations."

    public static func revokeTitle(_ app: String) -> String { "Revoke \(app)?" }
    public static let revokeMessage = "It loses access at once and every token it holds is revoked."

    public static func revoked(_ app: String) -> String { "\(app) was revoked." }

    public static func decided(_ app: String, approved: Bool) -> String {
        approved ? "\(app) was approved. The requester can install it now." : "\(app) was denied."
    }

    public static func errorText(_ error: Error) -> String {
        guard let refusal = error as? OrgApiException else { return "Could not reach ForkMesh. Try again." }
        switch refusal.code {
        case "forbidden", "not_org_admin": return "Only an organization owner or admin can do that."
        case "not_found": return "That app or request is gone. Pull to refresh."
        case "invalid_session": return "Sign in again to manage your apps."
        default: return refusal.message.isEmpty ? "The server refused that." : refusal.message
        }
    }
}

@MainActor
@Observable
public final class InstalledAppsModel {
    public private(set) var apps = InstalledApps()
    public private(set) var loaded = false
    public private(set) var loading = false
    public private(set) var failure: String?
    public private(set) var busy: Set<String> = []
    public var notice: String?

    @ObservationIgnored private let client: AppsClient?
    @ObservationIgnored private var generation = 0

    public init(client: AppsClient?) {
        self.client = client
    }

    public var available: Bool { client != nil }

    public func reload() async {
        guard let client else { return }
        generation += 1
        let mine = generation
        loading = true
        failure = nil
        do {
            let fresh = try await client.installed()
            guard mine == generation else { return }
            apps = fresh
            loaded = true
        } catch {
            guard mine == generation else { return }
            failure = InstalledAppsCopy.errorText(error)
        }
        loading = false
    }

    public func revoke(_ install: AppInstall) async {
        guard let client, install.canRevoke, !busy.contains(install.installId) else { return }
        busy.insert(install.installId)
        defer { busy.remove(install.installId) }
        do {
            try await client.revoke(installId: install.installId)
            apps.installs.removeAll { $0.installId == install.installId }
            for index in apps.organizations.indices {
                apps.organizations[index].installs.removeAll { $0.installId == install.installId }
            }
            notice = InstalledAppsCopy.revoked(install.app.title)
        } catch {
            notice = InstalledAppsCopy.errorText(error)
        }
    }

    public func decide(_ request: AppInstallRequest, org: String, approve: Bool) async {
        guard let client, !busy.contains(request.id) else { return }
        busy.insert(request.id)
        defer { busy.remove(request.id) }
        do {
            _ = try await client.decide(org: org, requestId: request.id, approve: approve)
            for index in apps.organizations.indices where apps.organizations[index].name == org {
                apps.organizations[index].requests.removeAll { $0.id == request.id }
            }
            notice = InstalledAppsCopy.decided(request.app.title, approved: approve)
        } catch {
            notice = InstalledAppsCopy.errorText(error)
        }
    }
}
