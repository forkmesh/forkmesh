// ApiService `pingSettings` / `savePingSettings`: the account's
// Pings settings (issue #347), read and written through the notification inbox route.

import FMBase
import Foundation

extension ApiService {
    // MARK: Ping settings

    /// The account's Pings settings: which kinds are on, email, the pause, quiet hours, code
    /// pushes, and (administrators only) the /status monitor alerts
    /// (`GET /api/notifications?node=…&settings=1`).
    public func pingSettings(node: String) async throws -> PingSettings {
        let cleanNode = node.dartTrimmed.lowercased()
        let data = try await getJson(url("/api/notifications", [("node", cleanNode), ("settings", "1")]))
        return PingSettings(json: data.objectValue ?? JSONObject())
    }

    /// Saves any subset of the Pings settings (`pingSettings` on `POST /api/notifications`) and
    /// answers with everything the Worker stored, so the page re-renders from the server's view.
    /// Throws when the Worker refuses, so the caller can put the switch back.
    public func savePingSettings(node: String, patch: JSONObject) async throws -> PingSettings {
        let cleanNode = node.dartTrimmed.lowercased()
        let data = try await postJson(url("/api/notifications"), [
            "node": .string(cleanNode),
            "pingSettings": .object(patch),
        ])
        return PingSettings(json: data.objectValue ?? JSONObject())
    }
}
