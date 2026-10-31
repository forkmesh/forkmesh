// The organization task board (`/api/tasks`), one task with its timer, check-ins, completion and
// replies, and the create/edit form. State: FMTools' TasksListModel, TaskDetailModel, TaskFormModel
// and TaskCheckinModel.

import FMApi
import FMBase
import FMCode
import FMPlanning
import FMTools
import SwiftUI

struct TasksScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: TasksListModel?
    @State private var detail: TaskDetailModel?
    @State private var detailOpen: Bool = false
    @State private var form: ToolsSheet<TaskFormModel>?
    @State private var notice: FmNotice?

    init(filter: String? = nil) {
        if let api = Services.get(ApiService.self) {
            let list: TasksListModel = TasksListModel(api: api)
            list.filter = TaskListFilter(route: filter).index
            _model = State(initialValue: list)
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        content
            .navigationTitle(TasksListModel.title)
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
            .shellDetail(isPresented: $detailOpen, id: detail.map { ObjectIdentifier($0) }) {
                if let detail = detail {
                    TaskDetailScreen(model: detail)
                        .shellPageClearance()
                }
            }
            .onChange(of: detailOpen) { (_: Bool, open: Bool) in
                if !open, let model = model {
                    Task { await model.load() }
                }
            }
            .onChange(of: model?.loading ?? true) { (_: Bool, loading: Bool) in
                if !loading, let model = model {
                    ShellNavigatorCounts.shared.adoptTasks(model)
                }
            }
            .sheet(item: $form) { (box: ToolsSheet<TaskFormModel>) in
                TaskFormSheet(model: box.value) { (task: JSONObject) in
                    form = nil
                    openTask(ToolsJSONText.id(task))
                }
            }
    }

    @ViewBuilder private var content: some View {
        if let model = model {
            TasksList(model: model, onOpen: openTask, notice: $notice)
                .task { await model.load() }
                .toolbar {
                    ToolbarItem(placement: .primaryAction) {
                        FmIconButton(icon: "refresh", label: "Refresh", onPressed: { Task { await model.load() } })
                    }
                    ToolbarItem(placement: .primaryAction) {
                        TasksToolsMenu(model: model, onNotice: { (next: FmNotice) in notice = next })
                    }
                }
                .safeAreaInset(edge: .bottom, alignment: .trailing, spacing: 0) {
                    if model.showsNewTask {
                        FmFloatingActionButton(icon: "add", label: "New task", accessibilityLabel: "New task", onPressed: {
                            form = ToolsSheet(model.formModel())
                        })
                        .padding(FmSpace.x4)
                        .accessibilityIdentifier("tasks-new")
                    }
                }
        } else {
            FmEmptyState(icon: "task_alt", title: TasksListModel.unavailableTitle)
        }
    }

    private func openTask(_ id: String) {
        guard let model = model else {
            return
        }
        detail = model.detailModel(taskId: id)
        detailOpen = true
    }
}

/// The id of a task object, for the views.
enum ToolsJSONText {
    static func id(_ object: JSONObject) -> String {
        guard let value = object["id"] else {
            return ""
        }
        return value.text
    }
}

private struct TasksList: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: TasksListModel
    let onOpen: (String) -> Void
    @Binding var notice: FmNotice?
    @State private var loadMoreNotice: FmNotice?

    var body: some View {
        if model.showsSpinner {
            FmSkeletonList()
        } else if model.showsError {
            FmEmptyState(icon: "task_alt", title: TasksListModel.unavailableTitle, message: model.error)
        } else {
            VStack(spacing: 0) {
                FmSegmentTabBar(tabs: tabs, index: model.filter, onChanged: { (index: Int) in model.selectFilter(index) })
                people
                if notice != nil {
                    FmNoticeLine($notice)
                        .padding(.horizontal, FmSpace.x4)
                }
                list
            }
        }
    }

    private var tabs: [FmSegmentTab] {
        model.filterTabs.map { (tab: TaskFilterTab) -> FmSegmentTab in
            FmSegmentTab(label: tab.label, count: tab.count, icon: tab.icon)
        }
    }

    private var people: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: FmSpace.x2) {
                ForEach(model.people) { (person: TaskPerson) in
                    TaskPersonChip(person: person, selected: person.id == model.person) {
                        model.selectPerson(person.id)
                    }
                }
            }
            .padding(.horizontal, FmSpace.x3)
            .padding(.vertical, FmSpace.x2)
        }
    }

    @ViewBuilder private var list: some View {
        if model.showsEmpty {
            ScrollView {
                FmEmptyState(icon: "task_alt", title: model.emptyTitle)
            }
            .fmRefreshable { await model.load() }
        } else {
            ScrollView {
                LazyVStack(spacing: 0) {
                    ForEach(Array(model.rows.enumerated()), id: \.element.id) { (pair: (offset: Int, element: TaskRow)) in
                        if pair.offset > 0 {
                            FmRowDivider()
                        }
                        row(pair.element)
                    }
                    if loadMoreNotice != nil {
                        FmNoticeLine($loadMoreNotice)
                            .padding(.horizontal, FmSpace.x4)
                            .padding(.top, FmSpace.x2)
                    }
                    if model.showsLoadMore {
                        FmOutlinedButton(model.loadMoreLabel, fillWidth: true, onPressed: model.loadingMore ? nil : loadMore)
                            .padding(FmSpace.x4)
                            .accessibilityIdentifier("tasks-load-more")
                    }
                }
                .padding(.bottom, 96)
            }
            .fmRefreshable { await model.load() }
        }
    }

    private func row(_ row: TaskRow) -> some View {
        let tint: Color = row.tone.color(fm)
        return FmListRow(
            title: row.title, subtitle: row.subtitle,
            leading: FmIconTile(icon: row.icon, tint: tint).fmAny,
            trailing: FmStatusBadge(label: row.status, color: tint).fmAny,
            onTap: { onOpen(row.id) })
            .accessibilityIdentifier("task-\(row.id)")
    }

    private func loadMore() {
        Task {
            if let failure = await model.loadMore() {
                loadMoreNotice = FmNotice(failure.message, isError: failure.isError)
            } else {
                loadMoreNotice = nil
            }
        }
    }
}

private struct TaskPersonChip: View {
    @Environment(\.fmPalette) private var fm
    let person: TaskPerson
    let selected: Bool
    let onTap: () -> Void

    var body: some View {
        Button(action: {
            if !selected {
                FmHaptics.selection()
            }
            onTap()
        }) {
            HStack(spacing: 6) {
                avatar
                if selected {
                    Text(person.label)
                        .fmText(.labelMedium, color: fm.textPrimary, weight: FmType.emphasis)
                        .lineLimit(1)
                }
                FmCountPill(count: person.openCount, active: selected)
            }
            .padding(.leading, 3)
            .padding(.trailing, FmSpace.x2)
            .padding(.vertical, 3)
            .background(selected ? fm.accent.opacity(0.14) : Color.clear, in: Capsule())
            .shellGlass(Capsule())
            .overlay {
                Capsule().strokeBorder(selected ? fm.accent : Color.clear, lineWidth: 1.5)
            }
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(person.accessibilityLabel))
        .accessibilityAddTraits(selected ? [.isButton, .isSelected] : [.isButton])
        .accessibilityIdentifier("tasks-person-\(person.id)")
    }

    @ViewBuilder private var avatar: some View {
        if person.isMember {
            MessagesPersonAvatar(name: person.name, size: 28)
        } else {
            FmIcon(person.icon, size: 18, color: selected ? fm.accent : fm.textSecondary)
                .frame(width: 28, height: 28)
                .background(fm.bgRaised, in: Circle())
        }
    }
}

/// One task: its details, timer and check-ins, completion and reopening, and the reply thread.
struct TaskDetailScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @Bindable var model: TaskDetailModel
    @State private var notice: FmNotice?
    @State private var replyNotice: FmNotice?
    @State private var confirmDelete: Bool = false
    @State private var finishing: Bool = false
    @State private var finishNote: String = ""
    @State private var reminding: Bool = false
    @State private var reminderNote: String = ""
    @State private var checkin: ToolsSheet<TaskCheckinModel>?
    @State private var edit: ToolsSheet<TaskFormModel>?

    var body: some View {
        content
            .task { await model.load() }
            .navigationTitle(TaskDetailModel.title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    if model.showsMenu {
                        menu
                    }
                }
                ToolbarItem(placement: .primaryAction) {
                    TaskDepthMenu(
                        detail: model, onNotice: { (next: FmNotice) in notice = next },
                        onClose: { dismiss() })
                }
            }
            .background(fm.canvas)
            .fmDialog(isPresented: $confirmDelete) {
                FmConfirmDialog(
                    title: TaskDetailModel.deleteConfirmation.title, message: TaskDetailModel.deleteConfirmation.message,
                    confirmLabel: TaskDetailModel.deleteConfirmation.confirm, destructive: true,
                    onCancel: { confirmDelete = false },
                    onConfirm: {
                        confirmDelete = false
                        Task {
                            let outcome: ToolsOutcome = await model.delete()
                            show(outcome.notice)
                            if outcome.close {
                                dismiss()
                            }
                        }
                    })
            }
            .fmDialog(isPresented: $finishing) {
                ToolsPromptDialog(
                    title: TaskDetailModel.finishPrompt.title, hint: TaskDetailModel.finishPrompt.hint,
                    action: TaskDetailModel.finishPrompt.action, multiline: true, text: $finishNote,
                    onCancel: { finishing = false },
                    onSubmit: { (text: String) in
                        finishing = false
                        let note: String = String(text.prefix(4000)).trimmingCharacters(in: .whitespacesAndNewlines)
                        Task { show(await model.complete(note: note)) }
                    })
            }
            .fmDialog(isPresented: $reminding) {
                ToolsPromptDialog(
                    title: TaskDetailModel.remindPrompt.title, hint: TaskDetailModel.remindPrompt.hint,
                    action: TaskDetailModel.remindPrompt.action, multiline: true, text: $reminderNote,
                    onCancel: { reminding = false },
                    onSubmit: { (text: String) in
                        reminding = false
                        Task { show(await model.remind(note: text)) }
                    })
            }
            .sheet(item: $checkin) { (box: ToolsSheet<TaskCheckinModel>) in
                TaskCheckinSheet(model: box.value) { (result: (state: String, note: String)) in
                    checkin = nil
                    Task { show(await model.checkin(state: result.state, note: result.note)) }
                }
            }
            .sheet(item: $edit) { (box: ToolsSheet<TaskFormModel>) in
                TaskFormSheet(model: box.value) { (task: JSONObject) in
                    edit = nil
                    model.applyEdited(task)
                }
            }
    }

    private var menu: some View {
        Menu {
            Button("Edit") {
                if let form = model.editModel() {
                    edit = ToolsSheet(form)
                }
            }
            Button("Delete", role: .destructive) { confirmDelete = true }
        } label: {
            FmIcon("more_vert", size: 24, tone: .primary)
                .frame(width: 44, height: 44)
        }
        .accessibilityLabel(Text("More"))
        .accessibilityIdentifier("task-menu")
    }

    @ViewBuilder private var content: some View {
        if model.loading && model.task == nil {
            FmSkeletonList(rows: 6, hasLeading: false)
        } else if model.task == nil {
            FmEmptyState(icon: "task_alt", title: "Task unavailable", message: model.error)
        } else {
            VStack(spacing: 0) {
                ScrollView {
                    TaskDetailBody(model: model, notice: $notice, onAction: run)
                        .padding(FmSpace.x4)
                }
                .fmRefreshable { await model.load() }
                replyBar
            }
        }
    }

    private var replyBinding: Binding<String> {
        Binding<String>(
            get: { model.reply },
            set: { (value: String) in model.reply = String(value.prefix(TaskDetailModel.replyMaxLength)) })
    }

    private var replyBar: some View {
        FmBottomActionBar {
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                FmNoticeLine($replyNotice)
                HStack(spacing: FmSpace.x2) {
                    FmTextField(text: replyBinding, hint: "Reply", multiline: true)
                        .textInputAutocapitalization(.sentences)
                        .accessibilityIdentifier("task-reply")
                    FmIconButton(icon: "send", label: "Send reply", onPressed: model.busy ? nil : {
                        Task { showReply(await model.sendReply()) }
                    })
                    .accessibilityIdentifier("task-reply-send")
                }
            }
        }
    }

    private func run(_ action: TaskAction) {
        switch action {
        case .complete:
            finishNote = ""
            finishing = true
        case .checkin:
            checkin = ToolsSheet(TaskCheckinModel())
        case .remind:
            reminderNote = ""
            reminding = true
        default:
            Task { show(await model.perform(action)) }
        }
    }

    private func show(_ next: ToolsNotice?) {
        if let next = next {
            notice = FmNotice(next.message, isError: next.isError)
        }
    }

    private func showReply(_ next: ToolsNotice?) {
        replyNotice = next.map { (value: ToolsNotice) -> FmNotice in FmNotice(value.message, isError: value.isError) }
    }
}

private struct TaskDetailBody: View {
    @Environment(\.fmPalette) private var fm
    let model: TaskDetailModel
    @Binding var notice: FmNotice?
    let onAction: (TaskAction) -> Void

    private var linkRepo: RepoRef { RepoRef(slug: model.repository) ?? ReferenceLinks.chatRepo }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: FmSpace.x2) {
                FmStatusBadge(label: model.statusLabel, color: model.statusTone.color(fm))
                Text(model.department)
                    .fmText(.bodySmall, color: fm.textTertiary)
            }
            Text(model.titleText)
                .fmText(.titleLarge)
                .textSelection(.enabled)
                .padding(.top, FmSpace.x2)
            if !model.actions.isEmpty {
                actions
                    .padding(.top, FmSpace.x3)
            }
            if notice != nil {
                FmNoticeLine($notice)
                    .padding(.top, FmSpace.x2)
            }
            facts
                .padding(.top, FmSpace.x3)
            TaskAgentStatusRow(task: model.task)
            sections
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var actions: some View {
        ToolsFlowLayout(spacing: FmSpace.x2, lineSpacing: FmSpace.x2) {
            ForEach(model.actions, id: \.self) { (action: TaskAction) in
                FmToneButton(
                    label: action.label, icon: action.icon, tone: tone(action), filled: action == .start,
                    onPressed: model.busy ? nil : { onAction(action) })
                    .accessibilityIdentifier("task-\(action.rawValue)")
            }
        }
    }

    private func tone(_ action: TaskAction) -> FmTone {
        switch action {
        case .start: return .accent
        case .stop: return .warning
        case .complete: return .success
        default: return .neutral
        }
    }

    private var facts: some View {
        FmCard(padding: EdgeInsets.fmSymmetric(horizontal: FmSpace.x3, vertical: FmSpace.x1)) {
            VStack(spacing: 0) {
                ForEach(Array(model.facts.enumerated()), id: \.offset) { (pair: (offset: Int, element: (label: String, value: String))) in
                    FmKeyValueRow(label: pair.element.label, value: pair.element.value)
                }
            }
        }
    }

    @ViewBuilder private var sections: some View {
        if !model.details.isEmpty {
            section("Details", model.details)
        }
        if let checkin = model.lastCheckin {
            section("Last check-in", checkin)
        }
        if let reminder = model.lastReminder {
            section("Last reminder", reminder)
        }
        if !model.completionNote.isEmpty {
            section("Completion note", model.completionNote)
        }
        FmSectionHeader(title: "Replies", count: model.replies.count)
            .padding(.top, FmSpace.x4)
        if model.replies.isEmpty {
            Text("No replies yet.")
                .fmText(.bodyMedium, color: fm.textSecondary)
        }
        ForEach(Array(model.replies.enumerated()), id: \.offset) { (pair: (offset: Int, element: JSONObject)) in
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                Text(model.replyHeader(pair.element))
                    .fmText(.bodySmall, color: fm.textTertiary)
                LinkedText(model.replyBody(pair.element), repo: linkRepo)
                    .fmText(.bodyMedium)
                    .textSelection(.enabled)
            }
            .padding(.bottom, FmSpace.x3)
        }
    }

    private func section(_ title: String, _ text: String) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            FmSectionHeader(title: title)
            LinkedText(text, repo: linkRepo)
                .fmText(.bodyMedium)
                .textSelection(.enabled)
        }
        .padding(.top, FmSpace.x4)
    }
}

private struct TaskCheckinSheet: View {
    @Bindable var model: TaskCheckinModel
    let onDone: ((state: String, note: String)) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            FmPanelHeader(title: "Check in")
            Picker("", selection: $model.state) {
                ForEach(TaskText.checkinStates, id: \.self) { (state: String) in
                    Text(TaskText.checkinLabel(state)).tag(state)
                }
            }
            .pickerStyle(.inline)
            .labelsHidden()
            .padding(.horizontal, FmSpace.x4)
            FmTextField(text: $model.note, hint: "Note (optional)", multiline: true)
                .padding(.horizontal, FmSpace.x4)
            FmFilledButton("Check in", fillWidth: true, onPressed: { onDone(model.result) })
                .padding(FmSpace.x4)
        }
        .fmSheetStyle()
    }
}

/// Creates a task, or edits the title and details of an existing one.
private struct TaskFormSheet: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: TaskFormModel
    let onSaved: (JSONObject) -> Void
    @State private var notice: FmNotice?

    private var titleBinding: Binding<String> {
        Binding<String>(get: { model.title }, set: { (value: String) in model.title = String(value.prefix(TaskFormModel.titleMaxLength)) })
    }

    private var detailsBinding: Binding<String> {
        Binding<String>(get: { model.details }, set: { (value: String) in model.details = String(value.prefix(TaskFormModel.detailsMaxLength)) })
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                FmPanelHeader(title: model.sheetTitle)
                ToolsLabeledField(label: "Title", text: titleBinding)
                    .accessibilityIdentifier("task-form-title")
                ToolsLabeledField(label: "Details", text: detailsBinding, multiline: true)
                    .accessibilityIdentifier("task-form-details")
                if model.creating {
                    creatingFields
                }
                FmNoticeLine($notice)
                FmFilledButton(model.submitLabel, fillWidth: true, onPressed: model.busy ? nil : submit)
                    .padding(.vertical, FmSpace.x4)
                    .accessibilityIdentifier("task-form-submit")
            }
            .padding(.horizontal, FmSpace.x4)
        }
        .fmSheetStyle(fitsContent: false)
    }

    @ViewBuilder private var creatingFields: some View {
        FmField(label: "Department") {
            Picker("", selection: $model.department) {
                ForEach(model.departments, id: \.self) { (department: String) in
                    Text(TaskText.departmentLabel(department)).tag(department)
                }
            }
            .pickerStyle(.menu)
            .labelsHidden()
        }
        if model.showsAssignee {
            FmField(label: "Assignee") {
                Picker("", selection: $model.assignee) {
                    ForEach(model.assignees, id: \.self) { (name: String) in
                        Text(model.assigneeLabel(name)).tag(name)
                    }
                    Text("Unassigned").tag("")
                }
                .pickerStyle(.menu)
                .labelsHidden()
            }
        }
    }

    private func submit() {
        Task {
            let result: (task: JSONObject?, failure: String?) = await model.submit()
            if let task = result.task {
                onSaved(task)
            } else if let failure = result.failure {
                notice = FmNotice.error(failure)
            }
        }
    }
}
