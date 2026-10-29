import FMBase
import FMChat
import FMControl
import Foundation
import Observation

public struct TeamConfirmation: Equatable, Sendable, Identifiable {
    public var token: String
    public var action: String
    public var subject: String
    public var summary: String
    public var expiresInMs: Int
    public var params: JSONObject

    public var id: String { token }

    public init?(json: JSONObject, params: JSONObject) {
        token = json.str("token")
        guard !token.isEmpty else { return nil }
        action = json.str("action")
        subject = json.str("subject")
        summary = json.str("summary")
        expiresInMs = json.int("expiresInMs", 60000)
        self.params = params
    }

    public var runParams: JSONObject {
        var out = params
        out["action"] = .string(action)
        out["token"] = .string(token)
        return out
    }
}

public enum TeammateAction: Equatable, Sendable {
    case runTeam
    case run(id: String)
    case briefing(id: String?)
    case enableSchedule(id: String)
    case disableSchedules
    case grade(session: Int, grade: Int)
    case prompt(id: String, text: String)
    case stop(id: String)
    case continueSession(id: String, text: String)
    case edit(id: String, changes: JSONObject)
    case create(changes: JSONObject)

    public var params: JSONObject {
        switch self {
        case .runTeam: return ["action": "runTeam"]
        case .run(let id): return ["action": "run", "id": .string(id)]
        case .briefing(let id): return id.map { ["action": "briefing", "id": .string($0)] } ?? ["action": "briefing"]
        case .enableSchedule(let id): return ["action": "enableSchedule", "id": .string(id)]
        case .disableSchedules: return ["action": "disableSchedules"]
        case .grade(let session, let grade):
            return ["action": "grade", "session": .int(Int64(session)), "grade": .int(Int64(grade))]
        case .prompt(let id, let text): return ["action": "prompt", "id": .string(id), "text": .string(text)]
        case .stop(let id): return ["action": "stop", "id": .string(id)]
        case .continueSession(let id, let text):
            return ["action": "continue", "id": .string(id), "text": .string(text)]
        case .edit(let id, let changes): return ["action": "edit", "id": .string(id), "changes": .object(changes)]
        case .create(let changes): return ["action": "create", "changes": .object(changes)]
        }
    }

    public var title: String {
        switch self {
        case .runTeam: return "Run the special agents now"
        case .run: return "Run now"
        case .briefing(let id): return id == nil ? "Generate briefing" : "Refresh briefing"
        case .enableSchedule: return "Turn schedule on"
        case .disableSchedules: return "Pause all schedules"
        case .grade: return "Grade task"
        case .prompt: return "Send prompt"
        case .stop: return "Stop"
        case .continueSession: return "Continue"
        case .edit: return "Save changes"
        case .create: return "Add special agent"
        }
    }

    public var isQuiet: Bool {
        switch self {
        case .grade, .prompt: return true
        default: return false
        }
    }
}

public enum TeamErrorText {
    public static func describe(_ error: Error) -> String {
        if let control = error as? LocalControlException { return describe(code: control.code) }
        return ControlErrorText.describe(error)
    }

    public static func describe(code: String) -> String {
        if code.hasPrefix("bad_field:") {
            return "The desktop refused the \(code.dropFirst("bad_field:".count)) field."
        }
        if code.hasPrefix("field_not_editable:") {
            return "\(code.dropFirst("field_not_editable:".count)) is edited on the desktop."
        }
        switch code {
        case "unsupported", "unknown_method": return "This desktop's ForkMesh is too old for that. Update it."
        case "unknown_teammate": return "That special agent is gone."
        case "no_repository": return "Pick a repository with a working tree on the desktop first."
        case "unknown_repo": return "The desktop has no such repository."
        case "not_started": return "The special agent could not start. The desktop says why on its Special Agents page."
        case "not_gradable": return "Only a finished special agent task can be graded."
        case "native_workflow": return "This special agent runs a built-in workflow and takes no prompt."
        case "no_session": return "There is no session to continue."
        case "teammate_exists": return "A special agent with that name already exists."
        case "empty_prompt": return "Type a prompt first."
        case "prompt_too_long": return "That prompt is too long."
        case "no_changes": return "Nothing changed."
        case "bad_family": return "The desktop does not know that tool family."
        case "unknown_shortcut", "bad_shortcut": return "That shortcut is gone."
        case "script_on_desktop": return "Script shortcuts run on the desktop itself."
        case "shortcut_running": return "Another shortcut is already running on the desktop."
        case "shortcut_not_started": return "The desktop could not start that shortcut."
        case "approval_required": return "The desktop has not allowed this shortcut file yet. Click Allow in the window that opened on the desktop, then run it again."
        case "empty_shortcut": return "That shortcut has no prompt."
        case "not_mirrored": return "This repository is not on this iPhone. Mirror it from Code first."
        case "unsupported_agent", "model_mismatch": return "The shortcut names an agent or model the desktop cannot run."
        case "agent_not_started": return "The desktop could not start an agent."
        default: return ControlErrorText.describe(code: code)
        }
    }
}

@MainActor
@Observable
public final class TeammatesStore {
    @ObservationIgnored public let family: DesktopControlFamily
    @ObservationIgnored private var tapId: UUID?
    @ObservationIgnored private var refreshing = false
    @ObservationIgnored private var again = false

    public private(set) var snapshot: TeammatesSnapshot?
    public private(set) var options = TeammateOptions.fallback
    public private(set) var loading = false
    public private(set) var error = ""
    public private(set) var unsupported = false
    public private(set) var revision = 0

    public init(channel: DesktopControlCalling) {
        family = DesktopControlFamily(.teammates, channel: channel)
        tapId = family.addEventTap { [weak self] suffix, _ in
            guard let self, suffix == "changed" else { return }
            self.revision += 1
            Task { await self.refresh() }
        }
    }

    public func refresh() async {
        if refreshing {
            again = true
            return
        }
        refreshing = true
        loading = true
        defer {
            refreshing = false
            loading = false
        }
        repeat {
            again = false
            do {
                snapshot = TeammatesSnapshot(json: try await family.call("list"))
                error = ""
                unsupported = false
            } catch {
                unsupported = DesktopControlFamily.isUnsupported(error)
                self.error = TeamErrorText.describe(error)
            }
        } while again
    }

    public func loadOptions() async {
        if let reply = try? await family.call("options") {
            let parsed = TeammateOptions(json: reply)
            if !parsed.providers.isEmpty { options = parsed }
        }
    }

    public func detail(_ id: String) async throws -> TeammateDetail {
        TeammateDetail(json: try await family.call("get", ["id": .string(id)]))
    }

    public func briefing() async throws -> TeamBriefing {
        TeamBriefing(json: try await family.call("briefing"))
    }

    public func stats() async throws -> TeamStats {
        TeamStats(json: try await family.call("stats"))
    }

    public func room(limit: Int = 100) async throws -> [TeamRoomMessage] {
        try await family.call("room", ["limit": .int(Int64(limit))]).maps("messages").map(TeamRoomMessage.init(json:))
    }

    public func selectRepo(_ repo: String) async {
        do {
            snapshot = TeammatesSnapshot(json: try await family.call("selectRepo", ["repo": .string(repo)]))
            error = ""
        } catch {
            self.error = TeamErrorText.describe(error)
        }
    }

    public func prepare(_ action: TeammateAction) async throws -> TeamConfirmation {
        let params = action.params
        guard let confirmation = TeamConfirmation(json: try await family.call("prepare", params), params: params) else {
            throw LocalControlException("bad_answer")
        }
        return confirmation
    }

    @discardableResult
    public func run(_ confirmation: TeamConfirmation) async throws -> String {
        let reply = try await family.call("run", confirmation.runParams, timeout: 45)
        await refresh()
        return reply.str("message")
    }

    @discardableResult
    public func perform(_ action: TeammateAction) async throws -> String {
        try await run(try await prepare(action))
    }

    public func teammate(_ id: String) -> TeammateRow? { snapshot?.teammates.first { $0.id == id } }

    public func stop() {
        if let tapId { family.removeEventTap(tapId) }
        tapId = nil
    }
}
