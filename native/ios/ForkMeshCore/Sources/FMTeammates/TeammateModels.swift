import FMBase
import Foundation

public enum TeammateStatus: String, Equatable, Sendable, CaseIterable {
    case working = "Working"
    case queued = "Queued"
    case waiting = "Waiting"
    case scheduled = "Scheduled"
    case paused = "Paused"
    case idle = "Idle"

    public init(wire: String) { self = TeammateStatus(rawValue: wire) ?? .idle }

    public var isWorking: Bool { self == .working || self == .queued || self == .waiting }

    public var sortRank: Int {
        switch self {
        case .working: return 0
        case .waiting: return 1
        case .queued: return 2
        case .scheduled: return 3
        case .idle: return 4
        case .paused: return 5
        }
    }
}

public struct TeammateSchedule: Equatable, Sendable {
    public var mode: String
    public var time: String
    public var days: [Int]
    public var events: [String]
    public var active: Bool
    public var human: String
    public var next: String
    public var progress: Double

    public init(
        mode: String = "on-demand", time: String = "09:00", days: [Int] = [], events: [String] = [], active: Bool = false,
        human: String = "", next: String = "", progress: Double = 0
    ) {
        self.mode = mode
        self.time = time
        self.days = days
        self.events = events
        self.active = active
        self.human = human
        self.next = next
        self.progress = progress
    }

    public init(json: JSONObject) {
        mode = json.str("mode", "on-demand")
        time = json.str("time", "09:00")
        days = json.list("days").map { $0.int(or: 0) }.filter { (1...7).contains($0) }
        events = json.strings("events")
        active = json.bool("active")
        human = json.str("human")
        next = json.str("next")
        progress = json.double("progress")
    }

    public var isAutomatic: Bool { active && mode != "on-demand" }

    public var wire: JSONObject {
        [
            "mode": .string(mode), "time": .string(time), "days": .array(days.map { .int(Int64($0)) }),
            "events": .array(events.map { .string($0) }), "active": .bool(active),
        ]
    }
}

public struct AgentChoiceRow: Equatable, Sendable, Identifiable, Hashable {
    public var id = UUID()
    public var taskType: String
    public var provider: String
    public var model: String

    public init(taskType: String = "", provider: String, model: String) {
        self.taskType = taskType
        self.provider = provider
        self.model = model
    }

    public static func == (lhs: AgentChoiceRow, rhs: AgentChoiceRow) -> Bool {
        lhs.taskType == rhs.taskType && lhs.provider == rhs.provider && lhs.model == rhs.model
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(taskType)
        hasher.combine(provider)
        hasher.combine(model)
    }
}

public struct TeammateMood: Equatable, Sendable {
    public var key: String
    public var emoji: String
    public var label: String
    public var motion: String
    public var attention: Bool
    public var intensity: Double
    public var reasons: [String]

    public init(json: JSONObject) {
        key = json.str("key")
        emoji = json.str("emoji")
        label = json.str("label")
        motion = json.str("motion", "still")
        attention = json.bool("attention")
        intensity = json.double("intensity")
        reasons = json.strings("reasons")
    }

    public var isEmpty: Bool { emoji.isEmpty }

    public var line: String {
        reasons.isEmpty ? label : "\(label): \(reasons.joined(separator: ", "))"
    }
}

public struct TeammateRow: Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var avatar: String
    public var specialty: String
    public var focus: String
    public var provider: String
    public var model: String
    public var status: TeammateStatus
    public var enabled: Bool
    public var schedule: TeammateSchedule
    public var skills: [String]
    public var monitors: Int
    public var lastRunAtMs: Int
    public var lastActive: String
    public var lastSessionId: Int
    public var lastTrigger: String
    public var workflow: String
    public var improvementCycles: Int
    public var mood: TeammateMood

    public init(json: JSONObject) {
        id = json.str("id")
        name = json.str("name")
        avatar = json.str("avatar")
        specialty = json.str("specialty")
        focus = json.str("focus")
        provider = json.str("provider")
        model = json.str("model")
        status = TeammateStatus(wire: json.str("status"))
        enabled = json.optBool("enabled") ?? true
        schedule = TeammateSchedule(json: json.obj("schedule"))
        skills = json.strings("skills")
        monitors = json.int("monitors")
        lastRunAtMs = json.int("lastRunAtMs")
        lastActive = json.str("lastActive")
        lastSessionId = json.int("lastSessionId")
        lastTrigger = json.str("lastTrigger")
        workflow = json.str("workflow")
        improvementCycles = json.int("improvementCycles")
        mood = TeammateMood(json: json.obj("mood"))
    }

    public var initials: String {
        let words = name.split(separator: " ").prefix(2)
        let letters = words.compactMap(\.first).map { String($0).uppercased() }.joined()
        return letters.isEmpty ? "?" : letters
    }

    public var takesPrompts: Bool { workflow.isEmpty }

    public var modelLine: String { [provider, model].filter { !$0.isEmpty }.joined(separator: " · ") }
}

public struct TeammatePermission: Equatable, Sendable, Identifiable {
    public var id: String
    public var label: String
}

public struct TeammateLogMonitor: Equatable, Sendable, Identifiable {
    public var badge: String
    public var threshold: Int
    public var taskType: String
    public var active: Bool

    public var id: String { badge + taskType }
}

public struct TeammateDetail: Equatable, Sendable {
    public var row: TeammateRow
    public var description: String
    public var instructions: String
    public var mode: String
    public var backupModels: [AgentChoiceRow]
    public var taskAgents: [AgentChoiceRow]
    public var logMonitors: [TeammateLogMonitor]
    public var permissions: [TeammatePermission]
    public var relayChannel: String
    public var selfImproving: Bool
    public var improvementNotes: [String]
    public var activity: [String]
    public var shortcut: String
    public var mention: String
    public var latestOutput: String

    public init(json: JSONObject) {
        row = TeammateRow(json: json)
        description = json.str("description")
        instructions = json.str("instructions")
        mode = json.str("mode")
        backupModels = json.maps("backupModels").map { AgentChoiceRow(provider: $0.str("provider"), model: $0.str("model")) }
        taskAgents = json.maps("taskAgents").map {
            AgentChoiceRow(taskType: $0.str("taskType"), provider: $0.str("provider"), model: $0.str("model"))
        }
        logMonitors = json.maps("logMonitors").map {
            TeammateLogMonitor(
                badge: $0.str("badge"), threshold: $0.int("threshold"), taskType: $0.str("taskType"),
                active: $0.bool("active"))
        }
        permissions = json.maps("permissions").map { TeammatePermission(id: $0.str("id"), label: $0.str("label")) }
        relayChannel = json.str("relayChannel")
        selfImproving = json.optBool("selfImproving") ?? true
        improvementNotes = json.strings("improvementNotes")
        activity = json.strings("activity")
        shortcut = json.str("shortcut")
        mention = json.str("mention")
        latestOutput = json.str("latestOutput")
    }
}

public struct TeamSummary: Equatable, Sendable {
    public var total: Int
    public var working: Int
    public var scheduled: Int
    public var idle: Int

    public init(json: JSONObject) {
        total = json.int("total")
        working = json.int("working")
        scheduled = json.int("scheduled")
        idle = json.int("idle")
    }

    public var line: String {
        "\(total) special agent\(total == 1 ? "" : "s") · \(working) working · \(scheduled) scheduled · \(idle) idle"
    }
}

public struct TeammatesSnapshot: Equatable, Sendable {
    public var repo: String
    public var repos: [String]
    public var available: Bool
    public var teammates: [TeammateRow]
    public var errors: [String]
    public var summary: TeamSummary

    public init(json: JSONObject) {
        repo = json.str("repo")
        repos = json.strings("repos")
        available = json.bool("available")
        teammates = json.maps("teammates").map(TeammateRow.init(json:))
        errors = json.strings("errors")
        summary = TeamSummary(json: json.obj("summary"))
    }

    public var sorted: [TeammateRow] {
        teammates.sorted {
            $0.status.sortRank != $1.status.sortRank
                ? $0.status.sortRank < $1.status.sortRank
                : $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending
        }
    }

    public func filtered(_ status: TeammateStatus?) -> [TeammateRow] {
        guard let status else { return sorted }
        if status == .working { return sorted.filter { $0.status.isWorking } }
        return sorted.filter { $0.status == status }
    }

    public var automaticSchedules: Int { teammates.filter { $0.schedule.isAutomatic }.count }
}

public struct TaskTypeOption: Equatable, Sendable, Identifiable, Hashable {
    public var id: String
    public var label: String
}

public struct TeammateOptions: Equatable, Sendable {
    public var providers: [String]
    public var modes: [String]
    public var scheduleModes: [String]
    public var taskTypes: [TaskTypeOption]
    public var editable: [String]

    public init(json: JSONObject) {
        providers = json.strings("providers")
        modes = json.strings("modes")
        scheduleModes = json.strings("scheduleModes")
        taskTypes = json.maps("taskTypes").map { TaskTypeOption(id: $0.str("id"), label: $0.str("label")) }
        editable = json.strings("editable")
    }

    public static let fallback = TeammateOptions(json: [
        "providers": ["codex", "claude-code", "openai", "claude-api"],
        "modes": ["", "Auto", "Plan", "Ask"],
        "scheduleModes": ["on-demand", "continuous", "hourly", "daily", "weekly", "weekdays", "custom", "events"],
    ])
}

public struct BriefingEntry: Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var avatar: String
    public var status: TeammateStatus
    public var sessionId: Int
    public var sessionStatus: String
    public var updatedAtMs: Int
    public var output: String

    public init(json: JSONObject) {
        id = json.str("id")
        name = json.str("name")
        avatar = json.str("avatar")
        status = TeammateStatus(wire: json.str("status"))
        sessionId = json.int("sessionId")
        sessionStatus = json.str("sessionStatus")
        updatedAtMs = json.int("updatedAtMs")
        output = json.str("output")
    }

    public var hasUpdate: Bool { sessionId > 0 }
}

public struct TeamBriefing: Equatable, Sendable {
    public var repo: String
    public var date: String
    public var updates: Int
    public var completed: Int
    public var inProgress: Int
    public var entries: [BriefingEntry]

    public init(json: JSONObject) {
        repo = json.str("repo")
        date = json.str("date")
        updates = json.int("updates")
        completed = json.int("completed")
        inProgress = json.int("inProgress")
        entries = json.maps("entries").map(BriefingEntry.init(json:))
    }

    public var headline: String {
        if updates == 0 { return "No updates yet today" }
        return "\(updates) update\(updates == 1 ? "" : "s") · \(completed) done · \(inProgress) in progress"
    }
}

public struct TeammateStatsRow: Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var avatar: String
    public var runs: Int
    public var succeeded: Int
    public var failed: Int
    public var graded: Int
    public var averageGrade: Double
    public var costUsd: Double

    public init(json: JSONObject) {
        id = json.str("id")
        name = json.str("name")
        avatar = json.str("avatar")
        runs = json.int("runs")
        succeeded = json.int("succeeded")
        failed = json.int("failed")
        graded = json.int("graded")
        averageGrade = json.double("averageGrade")
        costUsd = json.double("costUsd")
    }

    public var successRate: Double { runs > 0 ? Double(succeeded) / Double(runs) : 0 }
}

public struct TeamTask: Equatable, Sendable, Identifiable {
    public var session: Int
    public var teammates: [String]
    public var title: String
    public var status: String
    public var grade: Int
    public var gradable: Bool
    public var finishedAtMs: Int
    public var costUsd: Double

    public var id: Int { session }

    public init(json: JSONObject) {
        session = json.int("session")
        teammates = json.strings("teammates")
        title = json.str("title")
        status = json.str("status")
        grade = json.int("grade")
        gradable = json.bool("gradable")
        finishedAtMs = json.int("finishedAtMs")
        costUsd = json.double("costUsd")
    }
}

public struct TeamStats: Equatable, Sendable {
    public var repo: String
    public var teammates: [TeammateStatsRow]
    public var tasks: [TeamTask]
    public var totalTasks: Int

    public init(json: JSONObject) {
        repo = json.str("repo")
        teammates = json.maps("teammates").map(TeammateStatsRow.init(json:))
        tasks = json.maps("tasks").map(TeamTask.init(json:))
        totalTasks = json.int("totalTasks")
    }

    public var totalCost: Double { teammates.reduce(0) { $0 + $1.costUsd } }
}

public struct TeamRoomMessage: Equatable, Sendable, Identifiable {
    public var id: String
    public var sender: String
    public var text: String
    public var timestampMs: Int
    public var isSelf: Bool
    public var threadRootId: String

    public init(json: JSONObject) {
        id = json.str("id")
        sender = json.str("sender")
        text = json.str("text")
        timestampMs = json.int("t")
        isSelf = json.bool("self")
        threadRootId = json.str("threadRootId")
    }
}
