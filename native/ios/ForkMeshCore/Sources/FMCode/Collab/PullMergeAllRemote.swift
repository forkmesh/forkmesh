import FMBase
import FMChat
import FMControl
import Foundation

public struct PullMergeAllConfirmation: Equatable, Sendable {
    public let token: String
    public let summary: String
    public let expiresInMs: Int

    public init(json j: JSONObject) {
        token = j.str("token")
        summary = j.str("summary")
        expiresInMs = j.int("expiresInMs", 60_000)
    }
}

public struct PullMergeAllResult: Equatable, Sendable {
    public let message: String
    public let sessionId: Int
    public let pulls: Int
    public let skipped: [String]

    public init(json j: JSONObject) {
        message = j.str("message")
        sessionId = j.int("sessionId")
        pulls = j.int("pulls")
        skipped = Array(j.strings("skipped").prefix(PullMergeAllRules.maxSkipped))
    }
}

public enum PullMergeAllRules {
    public static let action = "merge-all-ai"
    public static let title = "Merge all with AI"
    public static let maxSkipped = 50
    public static let unpaired = "Pair this phone with your desktop to merge every pull request with AI."

    public static func params(owner: String, name: String) -> JSONObject {
        ["owner": .string(owner), "name": .string(name), "action": .string(action)]
    }

    public static func errorMessage(_ code: String) -> String {
        switch code {
        case "unsupported", "unknown_method":
            return "This desktop cannot merge every pull request with AI yet. Update ForkMesh on the desktop."
        case "not_owner": return "Only the desktop's owner can merge every pull request with AI."
        case "not_repo_owner": return "Only the repository's owner can merge every pull request at once."
        case "unknown_repo": return "The desktop does not have that repository."
        case "read_only": return "This repository is read-only on the desktop."
        case "busy": return "An agent is already merging this repository's pull requests."
        case "no_open_pulls": return "No open pull requests to merge."
        case "nothing_mergeable": return "None of the open pull requests can be merged by an agent on the desktop."
        case "launch_failed": return "The desktop could not start the merge agent."
        case "bad_request": return "The desktop could not read that request."
        case "missing", "expired", "mismatch", "confirmation_missing", "confirmation_expired", "confirmation_mismatch":
            return "The confirmation expired. Try again."
        default: return code.isEmpty ? "The desktop refused." : code.replacingOccurrences(of: "_", with: " ")
        }
    }
}

@MainActor
public final class PullMergeAllRemote {

    let family: DesktopControlFamily

    public init(channel: DesktopControlCalling) {
        family = DesktopControlFamily(.pulls, channel: channel)
    }

    public func prepare(owner: String, name: String) async throws -> PullMergeAllConfirmation {
        let confirmation = PullMergeAllConfirmation(
            json: try await family.call("prepare", PullMergeAllRules.params(owner: owner, name: name)))
        if confirmation.token.isEmpty { throw LocalControlException("confirmation_missing") }
        return confirmation
    }

    public func run(owner: String, name: String, token: String) async throws -> PullMergeAllResult {
        var params = PullMergeAllRules.params(owner: owner, name: name)
        params["token"] = .string(token)
        return PullMergeAllResult(json: try await family.call("run", params, timeout: 90))
    }

    public nonisolated static func message(for error: Error) -> String {
        if let error = error as? LocalControlException { return PullMergeAllRules.errorMessage(error.code) }
        return "\(error)"
    }
}
