import FMBase
import Foundation

public struct TeammateDraft: Equatable, Sendable {
    public var name = ""
    public var avatar = "sol"
    public var specialty = ""
    public var focus = ""
    public var description = ""
    public var instructions = ""
    public var provider = "codex"
    public var model = ""
    public var mode = ""
    public var skills: [String] = []
    public var backupModels: [AgentChoiceRow] = []
    public var taskAgents: [AgentChoiceRow] = []
    public var schedule = TeammateSchedule()
    public var relayChannel = ""
    public var enabled = true
    public var selfImproving = true

    public init() {}

    public init(detail: TeammateDetail) {
        name = detail.row.name
        avatar = detail.row.avatar.isEmpty ? "sol" : detail.row.avatar
        specialty = detail.row.specialty
        focus = detail.row.focus
        description = detail.description
        instructions = detail.instructions
        provider = detail.row.provider
        model = detail.row.model
        mode = detail.mode
        skills = detail.row.skills
        backupModels = detail.backupModels
        taskAgents = detail.taskAgents
        schedule = detail.row.schedule
        relayChannel = detail.relayChannel
        enabled = detail.row.enabled
        selfImproving = detail.selfImproving
    }
}

public enum TeammateDraftIssue: Equatable, Sendable {
    case name
    case avatar
    case provider
    case model
    case mode
    case schedule
    case relayChannel
    case skills
    case choices
    case tooLong(String)

    public var message: String {
        switch self {
        case .name: return "Give the special agent a name."
        case .avatar: return "The avatar is a short lowercase word."
        case .provider: return "Pick a provider."
        case .model: return "Enter a model id."
        case .mode: return "Pick Auto, Plan, Ask or the default."
        case .schedule: return "Check the schedule's time and days."
        case .relayChannel: return "A relay room starts with #, like #team."
        case .skills: return "Skills are short names, up to 40."
        case .choices: return "Every model choice needs a provider and a model."
        case .tooLong(let field): return "\(field) is too long."
        }
    }
}

public enum TeammateEditorLogic {
    static func matches(_ text: String, _ pattern: String) -> Bool {
        text.range(of: pattern, options: .regularExpression) != nil
    }

    public static func validate(_ draft: TeammateDraft) -> [TeammateDraftIssue] {
        var issues: [TeammateDraftIssue] = []
        let name = draft.name.trimmingCharacters(in: .whitespacesAndNewlines)
        if name.isEmpty || name.count > 80 { issues.append(.name) }
        if !matches(draft.avatar, "^[a-z0-9-]{1,32}$") { issues.append(.avatar) }
        if !matches(draft.provider, "^[a-z0-9][a-z0-9-]{0,31}$") { issues.append(.provider) }
        if !matches(draft.model.trimmingCharacters(in: .whitespaces), "^[A-Za-z0-9][A-Za-z0-9._:/@-]{0,99}$") {
            issues.append(.model)
        }
        if !["", "Auto", "Plan", "Ask"].contains(draft.mode) { issues.append(.mode) }
        if !matches(draft.schedule.time, "^([01][0-9]|2[0-3]):[0-5][0-9]$")
            || draft.schedule.days.contains(where: { !(1...7).contains($0) })
        {
            issues.append(.schedule)
        }
        let relay = draft.relayChannel.trimmingCharacters(in: .whitespaces)
        if !relay.isEmpty && !matches(relay, "^#[a-z0-9_-]{1,64}$") { issues.append(.relayChannel) }
        if draft.skills.count > 40 || draft.skills.contains(where: { $0.trimmingCharacters(in: .whitespaces).isEmpty || $0.count > 100 }) {
            issues.append(.skills)
        }
        let choices = draft.backupModels + draft.taskAgents
        if draft.backupModels.count > 10 || draft.taskAgents.count > 10
            || choices.contains(where: { $0.provider.isEmpty || $0.model.isEmpty })
            || draft.taskAgents.contains(where: { $0.taskType.isEmpty })
        {
            issues.append(.choices)
        }
        if draft.specialty.count > 200 { issues.append(.tooLong("Specialty")) }
        if draft.focus.count > 200 { issues.append(.tooLong("Focus")) }
        if draft.description.count > 2000 { issues.append(.tooLong("Description")) }
        if draft.instructions.count > 20000 { issues.append(.tooLong("Instructions")) }
        return issues
    }

    static func choiceArray(_ rows: [AgentChoiceRow], withTask: Bool) -> JSONValue {
        .array(
            rows.map { row in
                var object: JSONObject = [
                    "provider": .string(row.provider.trimmingCharacters(in: .whitespaces)),
                    "model": .string(row.model.trimmingCharacters(in: .whitespaces)),
                ]
                if withTask { object["taskType"] = .string(row.taskType) }
                return .object(object)
            })
    }

    public static func changes(from original: TeammateDraft?, to draft: TeammateDraft) -> JSONObject {
        var out: JSONObject = [:]
        let base = original
        func text(_ key: String, _ value: String, _ old: String?) {
            let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
            if base == nil ? !trimmed.isEmpty : trimmed != old?.trimmingCharacters(in: .whitespacesAndNewlines) {
                out[key] = .string(trimmed)
            }
        }
        text("name", draft.name, base?.name)
        if base == nil || draft.avatar != base?.avatar { out["avatar"] = .string(draft.avatar) }
        text("specialty", draft.specialty, base?.specialty)
        text("focus", draft.focus, base?.focus)
        text("description", draft.description, base?.description)
        text("instructions", draft.instructions, base?.instructions)
        if base == nil || draft.provider != base?.provider { out["provider"] = .string(draft.provider) }
        text("model", draft.model, base?.model)
        if base == nil ? !draft.mode.isEmpty : draft.mode != base?.mode { out["mode"] = .string(draft.mode) }
        let skills = draft.skills.map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        if base == nil ? !skills.isEmpty : skills != base?.skills {
            out["skills"] = .array(skills.map { .string($0) })
        }
        if base == nil ? !draft.backupModels.isEmpty : draft.backupModels != base?.backupModels {
            out["backupModels"] = choiceArray(draft.backupModels, withTask: false)
        }
        if base == nil ? !draft.taskAgents.isEmpty : draft.taskAgents != base?.taskAgents {
            out["taskAgents"] = choiceArray(draft.taskAgents, withTask: true)
        }
        if base == nil || !sameSchedule(draft.schedule, base!.schedule) { out["schedule"] = .object(draft.schedule.wire) }
        text("relayChannel", draft.relayChannel, base?.relayChannel)
        if base == nil ? !draft.enabled : draft.enabled != base?.enabled { out["enabled"] = .bool(draft.enabled) }
        if base == nil ? !draft.selfImproving : draft.selfImproving != base?.selfImproving {
            out["selfImproving"] = .bool(draft.selfImproving)
        }
        return out
    }

    static func sameSchedule(_ a: TeammateSchedule, _ b: TeammateSchedule) -> Bool {
        a.mode == b.mode && a.time == b.time && a.days.sorted() == b.days.sorted() && a.events == b.events
            && a.active == b.active
    }

    public static let weekdays: [(day: Int, short: String)] = [
        (1, "Mon"), (2, "Tue"), (3, "Wed"), (4, "Thu"), (5, "Fri"), (6, "Sat"), (7, "Sun"),
    ]

    public static func scheduleModeLabel(_ mode: String) -> String {
        switch mode {
        case "on-demand": return "On demand"
        case "continuous": return "Continuously"
        case "hourly": return "Every hour"
        case "daily": return "Every day"
        case "weekly": return "Every week"
        case "weekdays": return "Weekdays"
        case "custom": return "Chosen days"
        case "events": return "On events"
        default: return mode
        }
    }

    public static func usesTime(_ mode: String) -> Bool { ["daily", "weekly", "weekdays", "custom"].contains(mode) }

    public static func usesDays(_ mode: String) -> Bool { ["weekly", "custom"].contains(mode) }

    public static func usesEvents(_ mode: String) -> Bool { mode == "events" }
}
