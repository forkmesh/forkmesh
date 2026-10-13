import FMApi
import FMBase
import FMChat
import Foundation

public struct PingQuickFix: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var label: String
    public var detail: String
    public var icon: String
    public var score: Double
    public var trusted: Bool
    public var primary: Bool

    public init(id: String, label: String, detail: String = "", icon: String = "", score: Double = 0,
                trusted: Bool = false, primary: Bool = false)
    {
        self.id = id
        self.label = label
        self.detail = detail
        self.icon = icon
        self.score = score
        self.trusted = trusted
        self.primary = primary
    }

    public init(json: JSONObject) {
        self.init(
            id: json.str("id"), label: json.str("label"), detail: json.str("detail"), icon: json.str("icon"),
            score: json.double("score"), trusted: json.bool("trusted"), primary: json.bool("primary"))
    }

    public var systemImage: String {
        switch icon {
        case "pause": return "pause.circle"
        case "play": return "play.circle"
        case "rerun": return "arrow.clockwise"
        case "agent": return "sparkles"
        case "approve": return "checkmark.seal"
        case "reject": return "xmark.octagon"
        default: return "wrench.and.screwdriver"
        }
    }
}

public struct PingQuickFixes: Equatable, Sendable {
    public var found: Bool
    public var fixes: [PingQuickFix]
    public var event: String
    public var cause: String
    public var message: String

    public init(found: Bool = false, fixes: [PingQuickFix] = [], event: String = "", cause: String = "",
                message: String = "")
    {
        self.found = found
        self.fixes = fixes
        self.event = event
        self.cause = cause
        self.message = message
    }

    public init(json: JSONObject) {
        let fixes = json.maps("fixes").map(PingQuickFix.init(json:)).filter { !$0.id.isEmpty && !$0.label.isEmpty }
        self.init(
            found: json.bool("found") && !fixes.isEmpty, fixes: fixes, event: json.str("event"),
            cause: json.str("cause"), message: json.str("message"))
    }

    public var primary: PingQuickFix? { fixes.first(where: \.primary) ?? fixes.first }
}

@MainActor
public final class DesktopPingFixes {
    private let family: DesktopControlFamily

    public static let timeout: TimeInterval = 8

    public init(channel: DesktopControlCalling) {
        family = DesktopControlFamily(.pings, channel: channel)
    }

    public nonisolated static func params(for item: InboxItem) -> JSONObject? {
        guard item.fromDesktop else { return nil }
        var params: JSONObject = ["title": .string(item.title), "body": .string(item.body), "ts": .int(Int64(item.ts))]
        if !item.desktopPingId.isEmpty { params["pingId"] = .string(item.desktopPingId) }
        return params
    }

    public func fixes(for item: InboxItem) async throws -> PingQuickFixes {
        guard let params = DesktopPingFixes.params(for: item) else { return PingQuickFixes() }
        return PingQuickFixes(json: try await family.call("fixes", params, timeout: DesktopPingFixes.timeout))
    }

    public func apply(_ fix: PingQuickFix, to item: InboxItem) async throws -> PingQuickFixes {
        guard var params = DesktopPingFixes.params(for: item) else { throw LocalControlException("unknown_ping") }
        params["fix"] = .string(fix.id)
        return PingQuickFixes(json: try await family.call("fix", params, timeout: DesktopPingFixes.timeout))
    }

    public nonisolated static func explain(_ error: Error) -> String {
        switch (error as? LocalControlException)?.code ?? "" {
        case "unknown_ping": return "Your desktop no longer has this ping."
        case "fix_unavailable": return "That fix no longer applies."
        case "unknown_repo": return "Your desktop does not have this repository."
        case "unknown_run": return "The desktop no longer has that run."
        case "unknown_agent": return "The desktop no longer has that agent."
        case "wrong_state": return "That run has already moved on."
        case "not_started": return "The desktop could not start it."
        case "unsupported", "unknown_method": return "Update the desktop to use quick fixes."
        case "not_owner": return "The desktop is signed in to another account."
        case "not_paired": return "Pair this phone with your desktop first."
        case "": return "The desktop did not answer."
        case let code: return code.replacingOccurrences(of: "_", with: " ").capitalized + "."
        }
    }
}
