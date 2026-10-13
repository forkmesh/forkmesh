// The state and decisions of the Tasks screen: the organization task board,
// one task, the check-in sheet and the create/edit form, over
// `/api/tasks` (ApiService.tasksApi). The Worker decides which organization's board it is and
// authorizes every action; the buttons follow its rules.

import FMApi
import FMBase
import Foundation
import Observation

public enum TaskText {
    static let errorMessages: [String: String] = [
        "org_member_required":
            "Tasks live on the organization task board. Ask an owner to add you as a member to see them.",
        "office_organization_unavailable": "The organization task board is unavailable right now.",
        "active_task_exists": "You already have a task running. Stop it before starting another.",
        "assignee_only": "Only the assignee can do that.",
        "reminder_too_soon": "The assignee was reminded less than 10 minutes ago. Try again later.",
        "assignee_or_manager_only": "Only the assignee or a manager can do that.",
        "task_owner_or_reviewer_only": "Only the creator, assignee, or reviewer can reopen this task.",
        "forbidden": "You do not have permission to do that.",
    ]

    /// `_errorText`: the known sentence for a refusal code, the Worker's message otherwise, and
    /// `fallback` for anything that is not a refusal.
    public static func errorText(_ error: Error, _ fallback: String) -> String {
        guard let error = error as? OrgApiException else { return fallback }
        return errorMessages[error.code] ?? error.message
    }

    /// "under a minute", "45m", "2h", "1h 30m".
    public static func elapsed(_ ms: Int) -> String {
        let minutes = ms / 60000
        if minutes < 1 { return "under a minute" }
        if minutes < 60 { return "\(minutes)m" }
        let hours = minutes / 60, rest = minutes % 60
        return rest == 0 ? "\(hours)h" : "\(hours)h \(rest)m"
    }

    /// "product-design" -> "Product design".
    public static func departmentLabel(_ value: String) -> String {
        ToolsText.cap(value.replacingOccurrences(of: "-", with: " "))
    }

    public static func statusLabel(_ task: JSONObject) -> String {
        switch ToolsJSON.show(task["status"]) {
        case "active": return "Active"
        case "done": return "Done"
        default: return "Idle"
        }
    }

    public static func statusTone(_ task: JSONObject) -> ToolsTone {
        switch ToolsJSON.show(task["status"]) {
        case "active": return .accent
        case "done": return .success
        default: return .tertiary
        }
    }

    public static func statusIcon(_ task: JSONObject) -> String {
        switch ToolsJSON.show(task["status"]) {
        case "active": return "timelapse"
        case "done": return "task_alt"
        default: return "radio_button_unchecked"
        }
    }

    public static func assigneeLabel(_ task: JSONObject) -> String {
        let kind = ToolsJSON.show(task["assigneeKind"])
        if kind == "agent" { return "Agent" }
        if kind == "unassigned" { return "Unassigned" }
        let name = ToolsJSON.text(task["assignee"], "")
        return name.isEmpty ? "Unassigned" : "@\(name)"
    }

    public static func checkinLabel(_ state: String) -> String {
        switch state {
        case "going_well": return "Going well"
        case "blocked": return "Blocked"
        case "needs_help": return "Needs help"
        default: return ToolsText.cap(state.replacingOccurrences(of: "_", with: " "))
        }
    }

    public static let checkinStates = ["going_well", "blocked", "needs_help"]

    static func department(_ task: JSONObject) -> String {
        departmentLabel(ToolsJSON.text(task["department"], "general"))
    }

    public static func assigneeKind(_ task: JSONObject) -> String {
        let kind = ToolsJSON.text(task["assigneeKind"], "").dartTrimmed.lowercased()
        if ["agent", "bot", "claude", "codex"].contains(kind) { return "agent" }
        return kind == "unassigned" ? "unassigned" : "user"
    }

    public static func personId(_ task: JSONObject) -> String {
        let kind = assigneeKind(task)
        if kind != "user" { return kind }
        let name = ToolsJSON.text(task["assignee"], "").dartTrimmed
        return name.isEmpty ? TaskPerson.unassigned : TaskPerson.memberId(name)
    }
}

public struct TaskRow: Equatable, Identifiable, Sendable {
    public let id: String
    public let title: String
    public let subtitle: String
    public let icon: String
    public let tone: ToolsTone
    public let status: String
}

public struct TaskFilterTab: Equatable, Sendable {
    public let label: String
    public let icon: String
    public let count: Int

    public var accessibilityLabel: String { "\(label), \(count)" }
}

public struct TaskPerson: Equatable, Identifiable, Sendable {
    public static let everyone = "all"
    public static let bot = "agent"
    public static let unassigned = "unassigned"

    public let id: String
    public let name: String
    public let label: String
    public let icon: String
    public let openCount: Int

    public static func memberId(_ name: String) -> String { "user:\(name.dartTrimmed.lowercased())" }

    public var isMember: Bool { !name.isEmpty }
    public var accessibilityLabel: String { "\(label), \(openCount) open" }
}

@MainActor
@Observable
public final class TasksListModel {
    public static let title = "Tasks"
    public static let unavailableTitle = "Tasks are unavailable"
    public static let filters = ["Mine", "Open", "Done"]
    public static let filterIcons = ["person_outline", "radio_button_checked", "check_circle_outline"]

    @ObservationIgnored public let call: ToolsApiCall
    @ObservationIgnored private let clock: Clock

    public private(set) var tasks: [JSONObject] = []
    public private(set) var actor = ""
    public private(set) var canManage = false
    public private(set) var members: [String] = []
    public private(set) var departments: [String] = []
    public private(set) var nextOffset: Int?
    public private(set) var loading = true
    public private(set) var loadingMore = false
    public private(set) var error = ""
    /// 0 = Mine, 1 = Open, 2 = Done.
    public var filter = 0
    public private(set) var person = TaskPerson.everyone

    public init(call: @escaping ToolsApiCall, clock: Clock = SystemClock()) {
        self.call = call
        self.clock = clock
    }

    public convenience init(api: ApiService) {
        self.init(call: { method, suffix, body in try await api.tasksApi(method, suffix, body) })
    }

    public func load() async {
        loading = true
        error = ""
        do {
            let data = try await call("GET", "", nil)
            tasks = ToolsJSON.maps(data["tasks"])
            actor = ToolsJSON.text(data["actor"], "").lowercased()
            canManage = data["canManage"]?.boolValue == true
            members = ToolsJSON.strings(data["members"])
            departments = ToolsJSON.strings(data["departments"])
            nextOffset = data["hasMore"]?.boolValue == true ? ToolsJSON.int(data["nextOffset"]) : nil
            if !people.contains(where: { $0.id == person }) { person = TaskPerson.everyone }
            loading = false
        } catch {
            loading = false
            self.error = TaskText.errorText(error, "Tasks are unavailable right now.")
        }
    }

    /// Returns the failure to show, if any.
    public func loadMore() async -> ToolsNotice? {
        guard let offset = nextOffset, !loadingMore else { return nil }
        loadingMore = true
        do {
            let data = try await call("GET", "?offset=\(offset)", nil)
            let seen = Set(tasks.map { $0["id"] ?? .null })
            tasks += ToolsJSON.maps(data["tasks"]).filter { !seen.contains($0["id"] ?? .null) }
            nextOffset = data["hasMore"]?.boolValue == true ? ToolsJSON.int(data["nextOffset"]) : nil
            loadingMore = false
            return nil
        } catch {
            loadingMore = false
            return ToolsNotice(TaskText.errorText(error, "More tasks could not be loaded."), isError: true)
        }
    }

    private func mine(_ task: JSONObject) -> Bool {
        !actor.isEmpty && TaskText.personId(task) == TaskPerson.memberId(actor)
    }

    private func isDone(_ task: JSONObject) -> Bool { task["status"]?.stringValue == "done" }

    private func belongs(_ task: JSONObject) -> Bool {
        person == TaskPerson.everyone || TaskText.personId(task) == person
    }

    public var mineOpen: [JSONObject] { tasks.filter { !isDone($0) && mine($0) } }
    public var open: [JSONObject] { tasks.filter { !isDone($0) } }
    public var done: [JSONObject] { tasks.filter(isDone) }

    public var lists: [[JSONObject]] { [mineOpen, open.filter(belongs), done.filter(belongs)] }
    public var visible: [JSONObject] { lists[filter] }

    public var filterTabs: [TaskFilterTab] {
        let counts = lists.map(\.count)
        return Self.filters.indices.map {
            TaskFilterTab(label: Self.filters[$0], icon: Self.filterIcons[$0], count: counts[$0])
        }
    }

    public var memberNames: [String] {
        var seen = Set<String>()
        var names = [String]()
        for raw in members {
            let name = raw.dartTrimmed
            if name.isEmpty || !seen.insert(name.lowercased()).inserted { continue }
            names.append(name)
        }
        return names.sorted { a, b in
            let aActor = a.lowercased() == actor, bActor = b.lowercased() == actor
            if aActor != bActor { return aActor }
            let la = a.lowercased(), lb = b.lowercased()
            return la == lb ? a < b : la < lb
        }
    }

    public var personOpenCounts: [String: Int] {
        var counts = [String: Int]()
        for task in open {
            counts[TaskText.personId(task), default: 0] += 1
            counts[TaskPerson.everyone, default: 0] += 1
        }
        return counts
    }

    public var people: [TaskPerson] {
        let counts = personOpenCounts
        var out = [TaskPerson(id: TaskPerson.everyone, name: "", label: "Everyone", icon: "groups_outlined",
                              openCount: counts[TaskPerson.everyone] ?? 0)]
        for name in memberNames {
            let id = TaskPerson.memberId(name)
            let label = name.lowercased() == actor ? "@\(name) (you)" : "@\(name)"
            out.append(TaskPerson(id: id, name: name, label: label, icon: "", openCount: counts[id] ?? 0))
        }
        out.append(TaskPerson(id: TaskPerson.bot, name: "", label: "Bot", icon: "smart_toy_outlined",
                              openCount: counts[TaskPerson.bot] ?? 0))
        out.append(TaskPerson(id: TaskPerson.unassigned, name: "", label: "Unassigned", icon: "inbox_outlined",
                              openCount: counts[TaskPerson.unassigned] ?? 0))
        return out
    }

    public func selectPerson(_ id: String) {
        person = id == person ? TaskPerson.everyone : id
        if person != TaskPerson.everyone && filter == 0 { filter = 1 }
    }

    public func selectFilter(_ index: Int) {
        filter = index
        if index == 0 { person = TaskPerson.everyone }
    }

    public var rows: [TaskRow] { visible.map(row) }

    public var showsSpinner: Bool { loading && tasks.isEmpty }
    public var showsError: Bool { !error.isEmpty && tasks.isEmpty }
    /// The "New task" button exists only once the board loaded.
    public var showsNewTask: Bool { error.isEmpty && !loading }
    public var showsEmpty: Bool { visible.isEmpty && nextOffset == nil }
    public var showsLoadMore: Bool { nextOffset != nil }
    public var loadMoreLabel: String { loadingMore ? "Loading…" : "Load more tasks" }

    public var emptyTitle: String {
        switch filter {
        case 0: return "Nothing assigned to you"
        case 1: return "No open tasks"
        default: return "No finished tasks yet"
        }
    }

    public func row(_ task: JSONObject) -> TaskRow {
        let replies = ToolsJSON.int(task["replyCount"])
        var parts = [TaskText.assigneeLabel(task), TaskText.department(task)]
        if isDone(task) {
            parts.append("done \(Format.relativeTime(ToolsJSON.epoch(task["completedAt"]), nowMillis: clock.nowMillis))")
        } else {
            parts.append("updated \(Format.relativeTime(ToolsJSON.epoch(task["updatedAt"]), nowMillis: clock.nowMillis))")
        }
        if replies > 0 { parts.append("\(replies) \(replies == 1 ? "reply" : "replies")") }
        return TaskRow(
            id: ToolsJSON.show(task["id"]),
            title: ToolsJSON.text(task["title"], "Organization task"),
            subtitle: parts.joined(separator: " · "),
            icon: TaskText.statusIcon(task),
            tone: TaskText.statusTone(task),
            status: TaskText.statusLabel(task))
    }

    public func detailModel(taskId: String) -> TaskDetailModel {
        TaskDetailModel(taskId: taskId, actor: actor, canManage: canManage, call: call, clock: clock)
    }

    public func formModel() -> TaskFormModel {
        TaskFormModel(actor: actor, canManage: canManage, members: members, departments: departments, existing: nil, call: call)
    }
}

/// The actions a task offers the signed-in member, in display order.
public enum TaskAction: String, Equatable, Sendable {
    case start, stop, checkin, remind, complete, reopen

    public var label: String {
        switch self {
        case .start: return "Start"
        case .stop: return "Stop"
        case .checkin: return "Check in"
        case .remind: return "Remind"
        case .complete: return "Finish"
        case .reopen: return "Reopen"
        }
    }

    public var icon: String {
        switch self {
        case .start: return "play_arrow"
        case .stop: return "pause"
        case .checkin: return "flag_outlined"
        case .remind: return "notifications_active_outlined"
        case .complete: return "check"
        case .reopen: return "undo"
        }
    }
}

@MainActor
@Observable
public final class TaskDetailModel {
    public static let title = "Task"
    public static let deleteConfirmation = ToolsConfirmation(
        title: "Delete task?", message: "The task, its replies, and its history go away.",
        confirm: "Delete", cancel: "Cancel", destructive: true)
    public static let replyMaxLength = 2000
    public static let finishPrompt = (title: "Finish task", hint: "What did you do? (optional)", action: "Finish")
    public static let remindPrompt = (title: "Remind assignee", hint: "Note (optional)", action: "Send reminder")
    public static let reminderNoteMaxLength = 500

    @ObservationIgnored public let call: ToolsApiCall
    @ObservationIgnored private let clock: Clock
    public let taskId: String
    public let actor: String
    public let canManage: Bool

    public private(set) var task: JSONObject?
    public private(set) var loading = true
    public private(set) var busy = false
    public private(set) var error = ""
    public var reply = ""

    public init(taskId: String, actor: String, canManage: Bool, call: @escaping ToolsApiCall, clock: Clock = SystemClock()) {
        self.taskId = taskId
        self.actor = actor
        self.canManage = canManage
        self.call = call
        self.clock = clock
    }

    public convenience init(taskId: String, actor: String, canManage: Bool, api: ApiService) {
        self.init(taskId: taskId, actor: actor, canManage: canManage,
                  call: { method, suffix, body in try await api.tasksApi(method, suffix, body) })
    }

    private var path: String { "/\(taskId)" }

    public func load() async {
        loading = true
        error = ""
        do {
            task = ToolsJSON.object(try await call("GET", path, nil)["task"])
            loading = false
        } catch {
            loading = false
            self.error = TaskText.errorText(error, "This task could not be opened.")
        }
    }

    /// Runs one task action; every action answers with the updated task. Returns whether it went
    /// through, and the notice to show.
    @discardableResult
    public func act(_ method: String, _ suffix: String, _ body: JSONObject = [:], success: String = "") async -> (ok: Bool, notice: ToolsNotice?) {
        if busy { return (false, nil) }
        busy = true
        do {
            let data = try await call(method, "\(path)\(suffix)", body)
            if let updated = data["task"]?.objectValue { task = updated }
            busy = false
            return (true, success.isEmpty ? nil : ToolsNotice(success))
        } catch {
            busy = false
            return (false, ToolsNotice(TaskText.errorText(error, "That did not go through."), isError: true))
        }
    }

    // MARK: State

    public var status: String { ToolsJSON.show(task?["status"]) }

    public var isAssignee: Bool {
        guard let task else { return false }
        return ToolsJSON.show(task["assigneeKind"]) == "user" && ToolsJSON.show(task["assignee"]).lowercased() == actor
    }

    public var canReopen: Bool {
        guard let task else { return false }
        let reviewer = ToolsJSON.text(task["qa"]?.objectValue?["reviewer"], "")
        return canManage || isAssignee || ToolsJSON.show(task["createdBy"]) == actor || reviewer == actor
    }

    public var assignee: String {
        guard let task, ToolsJSON.show(task["assigneeKind"]) == "user" else { return "" }
        return ToolsJSON.text(task["assignee"], "").lowercased()
    }

    public var canRemind: Bool { status != "done" && !assignee.isEmpty && assignee != actor }

    /// Editing and deleting are manager-only on the Worker.
    public var showsMenu: Bool { task != nil && canManage }

    public var actions: [TaskAction] {
        var out = [TaskAction]()
        if isAssignee && status == "idle" { out.append(.start) }
        if isAssignee && status == "active" { out.append(contentsOf: [.stop, .checkin]) }
        if canRemind { out.append(.remind) }
        if status != "done" && (isAssignee || canManage) { out.append(.complete) }
        if status == "done" && canReopen { out.append(.reopen) }
        return out
    }

    public var statusLabel: String { task.map(TaskText.statusLabel) ?? "" }
    public var statusTone: ToolsTone { task.map(TaskText.statusTone) ?? .tertiary }
    public var department: String { task.map(TaskText.department) ?? "" }
    public var titleText: String { ToolsJSON.text(task?["title"], "Organization task") }
    public var details: String { ToolsJSON.text(task?["details"], "").dartTrimmed }
    public var completionNote: String { ToolsJSON.text(task?["completionNote"], "").dartTrimmed }

    public var repository: String { ToolsJSON.text(task?["repository"], "").dartTrimmed }

    /// The key/value card: (label, value).
    public var facts: [(label: String, value: String)] {
        guard let task else { return [] }
        let now = clock.nowMillis
        var out: [(label: String, value: String)] = [("Assignee", TaskText.assigneeLabel(task))]
        let createdBy = ToolsJSON.text(task["createdBy"], "")
        if !createdBy.isEmpty { out.append(("Created by", "@\(createdBy)")) }
        out.append(("Priority", "\(ToolsJSON.int(task["priority"]))"))
        let repository = ToolsJSON.text(task["repository"], "")
        if !repository.isEmpty { out.append(("Repository", repository)) }
        out.append(("Time spent", TaskText.elapsed(ToolsJSON.int(task["elapsedMs"]))))
        out.append(("Created", Format.relativeTime(ToolsJSON.epoch(task["createdAt"]), nowMillis: now)))
        if ToolsJSON.int(task["completedAt"]) > 0 {
            out.append(("Finished", Format.relativeTime(ToolsJSON.epoch(task["completedAt"]), nowMillis: now)))
        }
        return out
    }

    /// "Going well · 5m ago · note", or nil without a check-in.
    public var lastCheckin: String? {
        guard let checkin = task?["lastCheckin"]?.objectValue else { return nil }
        let state = ToolsJSON.text(checkin["state"], "")
        if state.isEmpty { return nil }
        var parts = [TaskText.checkinLabel(state), Format.relativeTime(ToolsJSON.epoch(checkin["at"]), nowMillis: clock.nowMillis)]
        let note = ToolsJSON.text(checkin["note"], "")
        if !note.isEmpty { parts.append(note) }
        return parts.joined(separator: " · ")
    }

    var lastReminderDelivered: Bool? {
        task?["lastReminder"]?.objectValue?["delivered"]?.boolValue
    }

    public var lastReminder: String? {
        guard let reminder = task?["lastReminder"]?.objectValue else { return nil }
        if ToolsJSON.int(reminder["at"]) <= 0 { return nil }
        var parts = [String]()
        let by = ToolsJSON.text(reminder["by"], "")
        if !by.isEmpty { parts.append("@\(by)") }
        parts.append(Format.relativeTime(ToolsJSON.epoch(reminder["at"]), nowMillis: clock.nowMillis))
        let note = ToolsJSON.text(reminder["note"], "")
        if !note.isEmpty { parts.append(note) }
        if let status = reminderStatus(reminder) { parts.append(status) }
        return parts.joined(separator: " · ")
    }

    func reminderStatus(_ reminder: JSONObject) -> String? {
        guard reminder["delivered"] != nil || reminder["seenAt"] != nil else { return nil }
        let to = ToolsJSON.text(reminder["to"], "")
        let who = to.isEmpty ? "the assignee" : "@\(to)"
        let seenAt = ToolsJSON.epoch(reminder["seenAt"])
        if seenAt > 0 {
            let how = ToolsJSON.text(reminder["seenVia"], "") == "ping" ? "opened the ping" : "opened the task"
            return "Seen: \(who) \(how) \(Format.relativeTime(seenAt, nowMillis: clock.nowMillis))"
        }
        if reminder["delivered"]?.boolValue == false { return "Not delivered: the ping could not be filed" }
        let phones = ToolsJSON.int(reminder["phones"])
        let reach = phones > 0 ? "pinged and pushed to \(phones) phone\(phones == 1 ? "" : "s")" : "pinged"
        return "Not seen yet: \(who) was \(reach)"
    }

    public var replies: [JSONObject] { ToolsJSON.maps(task?["replies"]) }

    public func replyHeader(_ reply: JSONObject) -> String {
        "@\(ToolsJSON.show(reply["author"])) · \(Format.relativeTime(ToolsJSON.epoch(reply["createdAt"]), nowMillis: clock.nowMillis))"
    }

    public func replyBody(_ reply: JSONObject) -> String { ToolsJSON.show(reply["body"]) }

    // MARK: Actions

    public func perform(_ action: TaskAction) async -> ToolsNotice? {
        switch action {
        case .start: return await act("POST", "/start", [:], success: "Timer started").notice
        case .stop: return await act("POST", "/stop", [:], success: "Timer stopped").notice
        case .reopen: return await act("POST", "/undo", [:], success: "Task reopened").notice
        case .checkin, .remind, .complete: return nil
        }
    }

    /// After the finish prompt (nil = cancelled).
    public func complete(note: String?) async -> ToolsNotice? {
        guard let note else { return nil }
        var body = JSONObject()
        if !note.isEmpty { body["completionNote"] = .string(note) }
        return await act("POST", "/complete", body, success: "Task finished and sent to review").notice
    }

    /// After the check-in sheet.
    public func checkin(state: String, note: String) async -> ToolsNotice? {
        var body: JSONObject = ["state": .string(state)]
        if !note.isEmpty { body["note"] = .string(note) }
        return await act("POST", "/checkin", body, success: "Checked in").notice
    }

    /// After the remind prompt (nil = cancelled).
    public func remind(note: String?) async -> ToolsNotice? {
        guard let note else { return nil }
        var body = JSONObject()
        let trimmed = String(note.dartTrimmed.prefix(Self.reminderNoteMaxLength))
        if !trimmed.isEmpty { body["note"] = .string(trimmed) }
        let result = await act("POST", "/remind", body, success: "Reminder sent to @\(assignee)")
        if result.ok, lastReminderDelivered == false {
            return ToolsNotice("The reminder was saved, but @\(assignee) could not be pinged", isError: true)
        }
        return result.notice
    }

    public func sendReply() async -> ToolsNotice? {
        let body = reply.dartTrimmed
        if body.isEmpty { return nil }
        let result = await act("POST", "/replies", ["body": .string(body)])
        if result.ok { reply = "" }
        return result.notice
    }

    /// After the person confirmed `deleteConfirmation`.
    public func delete() async -> ToolsOutcome {
        do {
            _ = try await call("DELETE", path, [:])
            return ToolsOutcome(close: true)
        } catch {
            return .failed(TaskText.errorText(error, "The task was not deleted."))
        }
    }

    public func editModel() -> TaskFormModel? {
        guard let task else { return nil }
        return TaskFormModel(actor: actor, canManage: canManage, members: [], departments: [], existing: task, call: call)
    }

    /// The form's saved copy replaces the task.
    public func applyEdited(_ updated: JSONObject) { task = updated }
}

/// The check-in sheet.
@MainActor
@Observable
public final class TaskCheckinModel {
    public var state = "going_well"
    public var note = ""

    public init() {}

    public var result: (state: String, note: String) { (state, note.dartTrimmed) }
}

/// Creates a task, or edits the title and details of `existing`.
@MainActor
@Observable
public final class TaskFormModel {
    public static let titleMaxLength = 160
    public static let detailsMaxLength = 4000

    @ObservationIgnored private let call: ToolsApiCall
    public let actor: String
    public let canManage: Bool
    public let existing: JSONObject?
    public let departments: [String]
    public let assignees: [String]
    public var title: String
    public var details: String
    public var department = "general"
    /// "" = unassigned.
    public var assignee: String
    public private(set) var busy = false

    public init(actor: String, canManage: Bool, members: [String], departments: [String], existing: JSONObject?,
                call: @escaping ToolsApiCall) {
        self.actor = actor
        self.canManage = canManage
        self.existing = existing
        self.call = call
        self.departments = departments.isEmpty ? ["general"] : departments
        var names = [String]()
        if !actor.isEmpty { names.append(actor) }
        if canManage { for m in members where !names.contains(m) { names.append(m) } }
        assignees = names
        title = ToolsJSON.text(existing?["title"], "")
        details = ToolsJSON.text(existing?["details"], "")
        assignee = actor
    }

    public var creating: Bool { existing == nil }
    public var sheetTitle: String { creating ? "New task" : "Edit task" }
    public var submitLabel: String { busy ? "Saving…" : creating ? "Create task" : "Save" }
    public var showsAssignee: Bool { creating && canManage }

    public func assigneeLabel(_ name: String) -> String {
        if name.isEmpty { return "Unassigned" }
        return name == actor ? "@\(name) (you)" : "@\(name)"
    }

    /// The Worker's copy of the saved task, or nil with the failure to show (nil failure: nothing to do).
    public func submit() async -> (task: JSONObject?, failure: String?) {
        let trimmed = title.dartTrimmed
        if trimmed.isEmpty || busy { return (nil, nil) }
        busy = true
        do {
            let data: JSONObject
            if let existing {
                data = try await call("PATCH", "/\(ToolsJSON.show(existing["id"]))", [
                    "title": .string(trimmed), "details": .string(details.dartTrimmed),
                ])
            } else {
                var body: JSONObject = [
                    "title": .string(trimmed),
                    "details": .string(details.dartTrimmed),
                    "department": .string(department),
                ]
                if assignee.isEmpty {
                    body["assigneeKind"] = "unassigned"
                } else {
                    body["assigneeKind"] = "user"
                    body["assignee"] = .string(assignee)
                }
                data = try await call("POST", "", body)
            }
            return (ToolsJSON.object(data["task"]), nil)
        } catch {
            busy = false
            return (nil, TaskText.errorText(error, "The task was not saved."))
        }
    }
}
