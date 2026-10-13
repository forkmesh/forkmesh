import FMApi
import FMBase
import Foundation

@MainActor
public protocol PlanningClient: AnyObject {
    func accountApiCall(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject
    func tree(_ owner: String, _ name: String, path: String, ref: String) async throws -> RepoTree
    func blob(_ owner: String, _ name: String, _ path: String, ref: String) async throws -> RepoBlob
    func publishedIssues(_ owner: String, _ name: String) async throws -> [Issue]
    func clearRepoCache(_ owner: String, _ name: String)
}

extension ApiService: PlanningClient {}

public enum PlanningErrors {
    public static func text(_ error: Error, _ fallback: String) -> String {
        if let org = error as? OrgApiException {
            switch org.code {
            case "not_found": return "That was not found. It may have been deleted."
            case "forbidden", "org_member_required": return "You do not have access to that."
            case "unauthorized", "invalid_session": return "Sign in again to continue."
            case "http_0", "": return fallback
            default: return org.message.isEmpty ? fallback : org.message.prefix(1).uppercased() + org.message.dropFirst() + "."
            }
        }
        return fallback
    }
}

public enum TimerPings {
    public static let maxAgeMs: Int64 = 15 * 60 * 1_000

    public static func body(_ timer: PlanningTimer) -> JSONObject? {
        guard timer.kind != .stopwatch, timer.finishedAtMs > 0 else { return nil }
        let id = String(timer.id.filter { $0.isLetter || $0.isNumber || $0 == "-" }.prefix(40))
        guard !id.isEmpty else { return nil }
        return [
            "timerId": .string(id),
            "kind": .string(timer.kind.rawValue),
            "endsAt": .int(timer.finishedAtMs),
            "label": .string(String(timer.label.prefix(80))),
        ]
    }

    @MainActor
    public static func post(_ rung: [PlanningTimer], client: PlanningClient, nowMillis: Int64) async -> Int {
        var sent = 0
        for timer in rung where nowMillis - timer.finishedAtMs <= maxAgeMs {
            guard let body = body(timer) else { continue }
            if (try? await client.accountApiCall("POST", "/api/notifications/timer", body)) != nil { sent += 1 }
        }
        return sent
    }
}
