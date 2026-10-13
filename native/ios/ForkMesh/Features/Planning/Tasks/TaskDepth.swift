import FMApi
import FMBase
import FMCode
import FMPlanning
import FMTools
import SwiftUI

@MainActor
enum PlanningServices {
    static var api: ApiService? { Services.get(ApiService.self) }

    static var issueFiler: IssueFiler? {
        guard let inbox = Services.get(InboxService.self) else {
            return nil
        }
        return { (owner: String, name: String, title: String, body: String) in
            try await inbox.submitNewIssue(owner, name, title: title, body: body)
        }
    }
}

enum TaskDepthPrompt: String, Identifiable {
    case followUp, assignAgent, moveToIssue, priority, startAgent
    var id: String { rawValue }

    var title: String {
        switch self {
        case .followUp: return "Follow-up task"
        case .assignAgent: return "Assign to an agent"
        case .moveToIssue: return "Move to an issue"
        case .priority: return "Priority"
        case .startAgent: return "Start an agent"
        }
    }

    var placeholder: String {
        switch self {
        case .followUp: return "Title"
        case .assignAgent, .moveToIssue, .startAgent: return "owner/repository"
        case .priority: return "1 to 99"
        }
    }
}

struct TaskDepthMenu: View {
    let detail: TaskDetailModel
    let onNotice: (FmNotice) -> Void
    let onClose: () -> Void
    @State private var ops: TaskOperations?
    @State private var prompt: TaskDepthPrompt?
    @State private var input: String = ""
    @State private var showingHistory: Bool = false
    @State private var composing: TaskComposeBox?

    init(detail: TaskDetailModel, onNotice: @escaping (FmNotice) -> Void, onClose: @escaping () -> Void) {
        self.detail = detail
        self.onNotice = onNotice
        self.onClose = onClose
        if let api = PlanningServices.api {
            _ops = State(initialValue: TaskOperations(api: api))
        } else {
            _ops = State(initialValue: nil)
        }
    }

    private var task: JSONObject { detail.task ?? [:] }

    var body: some View {
        Menu {
            Button("History") { showingHistory = true }
            Button("Follow-up task") { open(.followUp, prefill: "") }
            Button("Start an agent") { open(.startAgent, prefill: task.str("repository")) }
            if detail.canManage {
                Button("Assign to an agent") { open(.assignAgent, prefill: task.str("repository")) }
                Button("Priority") { open(.priority, prefill: "\(task.int("priority", 50))") }
                Button("Move to an issue") { open(.moveToIssue, prefill: task.str("repository")) }
            }
        } label: {
            Image(systemName: "ellipsis.circle")
        }
        .accessibilityLabel("Task tools")
        .disabled(ops == nil || detail.task == nil)
        .alert(prompt?.title ?? "", isPresented: Binding(get: { prompt != nil }, set: { if !$0 { prompt = nil } })) {
            TextField(prompt?.placeholder ?? "", text: $input)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            Button("Cancel", role: .cancel) { prompt = nil }
            Button("OK") { submit() }
        }
        .sheet(isPresented: $showingHistory) {
            if let ops = ops {
                TaskHistorySheet(ops: ops, taskId: task.str("id"))
            }
        }
        .sheet(item: $composing) { (box: TaskComposeBox) in
            NavigationStack {
                RouteDestinationView(route: .compose(box.target))
                    .toolbar {
                        ToolbarItem(placement: .cancellationAction) {
                            Button("Close") { composing = nil }
                        }
                    }
            }
        }
    }

    private func open(_ kind: TaskDepthPrompt, prefill: String) {
        input = prefill
        prompt = kind
    }

    private func submit() {
        guard let kind = prompt, let ops = ops else {
            return
        }
        prompt = nil
        let value: String = input.trimmingCharacters(in: .whitespacesAndNewlines)
        let id: String = task.str("id")
        Task {
            switch kind {
            case .followUp:
                report(await ops.followUp(parentId: id, title: value), success: "Follow-up created")
            case .assignAgent:
                report(await ops.assignToAgent(id, repository: value), success: "Assigned to an agent")
            case .priority:
                report(await ops.setPriority(id, Int(value) ?? 0), success: "Priority changed")
            case .startAgent:
                if let repo = PlanningConversion.validRepository(value) {
                    let target: ComposeTarget = ComposeTarget(repo: repo, text: PlanningConversion.agentPrompt(task: task))
                    if let compose = NotificationActions.onCompose {
                        compose(target)
                    } else {
                        composing = TaskComposeBox(target: target)
                    }
                } else {
                    onNotice(FmNotice.error("Choose a repository as owner/name."))
                }
            case .moveToIssue:
                guard let filer = PlanningServices.issueFiler else {
                    onNotice(FmNotice.error("Sign in on this device to file issues."))
                    return
                }
                if let problem = await ops.moveToIssue(task, repository: value, file: filer) {
                    onNotice(FmNotice.error(problem))
                } else {
                    onNotice(FmNotice("Moved to an issue"))
                    onClose()
                }
            }
        }
    }

    private func report(_ result: Result<JSONObject, TaskOperationError>, success: String) {
        switch result {
        case .success(let updated):
            if !updated.isEmpty && updated.str("id") == task.str("id") {
                detail.applyEdited(updated)
            }
            onNotice(FmNotice(success))
        case .failure(let error):
            if !error.message.isEmpty {
                onNotice(FmNotice.error(error.message))
            }
        }
    }
}

struct TaskComposeBox: Identifiable {
    let target: ComposeTarget
    var id: String { target.text ?? "" }
}

struct TaskHistorySheet: View {
    @Environment(\.dismiss) private var dismiss
    let ops: TaskOperations
    let taskId: String
    @State private var error: String?

    var body: some View {
        NavigationStack {
            List {
                if let error = error {
                    Text(error).foregroundStyle(.red)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(error)
                }
                ForEach(ops.history) { (entry: TaskHistoryEntry) in
                    VStack(alignment: .leading, spacing: 2) {
                        LinkedText(entry.summary.isEmpty ? entry.kind : entry.summary, repo: ReferenceLinks.chatRepo, commits: false)
                        Text("@\(entry.actor) · \(Format.relativeTime(entry.createdAtMs, nowMillis: SystemClock().nowMillis))")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .overlay {
                if ops.busy && ops.history.isEmpty {
                    ProgressView()
                }
            }
            .navigationTitle("History")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .task { error = await ops.loadHistory(taskId) }
        }
    }
}

struct TaskAgentStatusRow: View {
    @Environment(\.fmPalette) private var fm
    let task: JSONObject?

    var body: some View {
        if let task = task, let info = TaskAgentInfo(task: task) {
            HStack(spacing: FmSpace.x2) {
                Image(systemName: "sparkles")
                Text(info.label).fmText(.bodyMedium)
                Spacer()
                if !info.sessionId.isEmpty {
                    NavigationLink {
                        RouteDestinationView(route: .agent(id: info.sessionId))
                            .shellPageClearance()
                    } label: {
                        Text("Open").fmText(.labelLarge, color: fm.accent)
                    }
                }
            }
            .padding(.top, FmSpace.x3)
        }
    }
}

struct TasksToolsMenu: View {
    let model: TasksListModel
    let onNotice: (FmNotice) -> Void
    @State private var ops: TaskOperations?
    @State private var quickAdding: Bool = false
    @State private var title: String = ""
    @State private var confirmDeleteClosed: Bool = false

    init(model: TasksListModel, onNotice: @escaping (FmNotice) -> Void) {
        self.model = model
        self.onNotice = onNotice
        if let api = PlanningServices.api {
            _ops = State(initialValue: TaskOperations(api: api))
        } else {
            _ops = State(initialValue: nil)
        }
    }

    var body: some View {
        Menu {
            Button("Quick add") {
                title = ""
                quickAdding = true
            }
            if model.canManage && !model.done.isEmpty {
                Button("Delete closed tasks", role: .destructive) { confirmDeleteClosed = true }
            }
        } label: {
            Image(systemName: "plus.circle")
        }
        .accessibilityLabel("Task tools")
        .disabled(ops == nil)
        .alert("Quick add", isPresented: $quickAdding) {
            TextField("Task title", text: $title)
            Button("Cancel", role: .cancel) {}
            Button("Add") { quickAdd() }
        }
        .confirmationDialog(
            "Delete \(model.done.count) closed tasks?", isPresented: $confirmDeleteClosed, titleVisibility: .visible
        ) {
            Button("Delete", role: .destructive) { deleteClosed() }
        }
    }

    private func quickAdd() {
        guard let ops = ops else {
            return
        }
        let text: String = title
        Task {
            switch await ops.quickAdd(title: text) {
            case .success:
                onNotice(FmNotice("Task added"))
                await model.load()
            case .failure(let error):
                if !error.message.isEmpty {
                    onNotice(FmNotice.error(error.message))
                }
            }
        }
    }

    private func deleteClosed() {
        guard let ops = ops else {
            return
        }
        let closed: [JSONObject] = model.done
        Task {
            let result: (deleted: Int, failed: Int) = await ops.deleteClosed(closed)
            onNotice(result.failed == 0
                ? FmNotice("Deleted \(result.deleted) tasks")
                : FmNotice.error("Deleted \(result.deleted), \(result.failed) could not be deleted"))
            await model.load()
        }
    }
}
