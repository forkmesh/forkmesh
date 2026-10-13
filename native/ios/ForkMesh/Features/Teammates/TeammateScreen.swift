import FMApi
import FMBase
import FMTeammates
import SwiftUI

struct TeammateDestination: View {
    let id: String

    var body: some View {
        if let store = TeammatesFeature.teammates {
            TeammateScreen(store: store, id: id)
        } else {
            TeamUnavailableView()
        }
    }
}

struct TeammateScreen: View {
    @Bindable var store: TeammatesStore
    let id: String

    @State private var detail: TeammateDetail?
    @State private var error = ""
    @State private var banner = ""
    @State private var prompt = ""
    @State private var sending = false
    @State private var pending: PendingTeamConfirmation?
    @State private var showEditor = false

    var body: some View {
        List {
            if let detail {
                let row = detail.row
                Section {
                    HStack(spacing: 12) {
                        TeammateAvatar(name: row.name, avatar: row.avatar, size: 52)
                        VStack(alignment: .leading, spacing: 4) {
                            Text(row.name).font(.title3.weight(.semibold))
                            if !row.specialty.isEmpty { Text(row.specialty).font(.subheadline).foregroundStyle(.secondary) }
                            TeammateStatusBadge(status: row.status)
                        }
                    }
                    if !row.focus.isEmpty { LabeledContent("Focus", value: row.focus) }
                    LabeledContent("Model", value: row.modelLine)
                    LabeledContent("Schedule", value: row.schedule.human.isEmpty ? "On demand" : row.schedule.human)
                    if row.schedule.isAutomatic && !row.schedule.next.isEmpty {
                        LabeledContent("Next", value: row.schedule.next)
                    }
                    if !detail.mention.isEmpty { LabeledContent("Mention", value: detail.mention) }
                }
                Section("Work") {
                    actionButton(.run(id: id), icon: "play.fill")
                    if row.status.isWorking {
                        actionButton(.stop(id: id), icon: "stop.fill", destructive: true)
                    }
                    if row.lastSessionId > 0 && !row.status.isWorking {
                        actionButton(.continueSession(id: id, text: ""), icon: "arrow.clockwise")
                    }
                    if !row.schedule.isAutomatic && row.schedule.mode != "on-demand" {
                        actionButton(.enableSchedule(id: id), icon: "clock")
                    }
                    actionButton(.briefing(id: id), icon: "newspaper")
                }
                if row.takesPrompts {
                    Section {
                        TextField("Ask \(row.name) to…", text: $prompt, axis: .vertical)
                            .lineLimit(2...6)
                        Button {
                            Task { await send() }
                        } label: {
                            HStack {
                                Label(row.status.isWorking ? "Steer the live run" : "Send", systemImage: "paperplane.fill")
                                Spacer()
                                if sending { ProgressView() }
                            }
                        }
                        .disabled(sending || prompt.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    } header: {
                        Text("Prompt")
                    }
                }
                if !detail.latestOutput.isEmpty {
                    Section("Latest output") {
                        Text(detail.latestOutput)
                            .font(.callout)
                            .textSelection(.enabled)
                    }
                }
                if !detail.activity.isEmpty {
                    Section("Activity") {
                        ForEach(Array(detail.activity.reversed().enumerated()), id: \.offset) { _, line in
                            Text(line).font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
                if !row.skills.isEmpty {
                    Section("Skills") {
                        Text(row.skills.joined(separator: " · ")).font(.footnote)
                    }
                }
                if !detail.improvementNotes.isEmpty {
                    Section("What it has learned") {
                        ForEach(Array(detail.improvementNotes.enumerated()), id: \.offset) { _, note in
                            Text(note).font(.caption)
                        }
                    }
                }
                if !detail.logMonitors.isEmpty || !detail.permissions.isEmpty {
                    Section {
                        ForEach(detail.logMonitors) { monitor in
                            LabeledContent(monitor.badge, value: "\(monitor.threshold)+ · \(monitor.active ? "on" : "off")")
                                .font(.footnote)
                        }
                        ForEach(detail.permissions) { permission in
                            Label(permission.label, systemImage: "checkmark.shield").font(.footnote)
                        }
                    } header: {
                        Text("Monitors and permissions")
                    } footer: {
                        Text("These are changed on the desktop.")
                    }
                }
            }
            if !banner.isEmpty {
                Section { Label(banner, systemImage: "checkmark.circle").foregroundStyle(.green).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(banner) }
            }
            if !error.isEmpty {
                Section {
                    Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red).font(.footnote).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(error)
                }
            }
        }
        .overlay {
            if detail == nil && error.isEmpty { ProgressView() }
        }
        .navigationTitle(detail?.row.name ?? "Special Agent")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button("Edit") { showEditor = true }
                    .disabled(detail == nil)
            }
        }
        .fmRefreshable { await load() }
        .task { await load() }
        .onChange(of: store.revision) { _, _ in Task { await load() } }
        .sheet(item: $pending) { item in
            NavigationStack {
                TeamConfirmSheet(pending: item) { confirmation in
                    try await store.run(confirmation)
                } onDone: { message in
                    banner = message
                    Task { await load() }
                }
            }
        }
        .sheet(isPresented: $showEditor, onDismiss: { Task { await load() } }) {
            NavigationStack {
                TeammateEditorScreen(store: store, id: id)
            }
        }
    }

    private func actionButton(_ action: TeammateAction, icon: String, destructive: Bool = false) -> some View {
        Button(role: destructive ? .destructive : nil) {
            Task { await prepare(action, destructive: destructive) }
        } label: {
            Label(action.title, systemImage: icon)
        }
    }

    private func load() async {
        do {
            detail = try await store.detail(id)
            error = ""
        } catch {
            self.error = TeamErrorText.describe(error)
        }
    }

    private func prepare(_ action: TeammateAction, destructive: Bool) async {
        do {
            pending = PendingTeamConfirmation(
                title: action.title, confirmation: try await store.prepare(action), destructive: destructive)
        } catch {
            self.error = TeamErrorText.describe(error)
        }
    }

    private func send() async {
        sending = true
        defer { sending = false }
        do {
            banner = try await store.perform(.prompt(id: id, text: prompt))
            prompt = ""
            error = ""
            await load()
        } catch {
            self.error = TeamErrorText.describe(error)
        }
    }
}

struct TeammateEditorDestination: View {
    let id: String?

    var body: some View {
        if let store = TeammatesFeature.teammates {
            TeammateEditorScreen(store: store, id: id)
        } else {
            TeamUnavailableView()
        }
    }
}

struct TeammateEditorScreen: View {
    @Environment(\.dismiss) private var dismiss
    @Bindable var store: TeammatesStore
    let id: String?

    @State private var original: TeammateDraft?
    @State private var draft = TeammateDraft()
    @State private var loaded = false
    @State private var saving = false
    @State private var error = ""
    @State private var skillsText = ""

    private var issues: [TeammateDraftIssue] { TeammateEditorLogic.validate(currentDraft) }

    private var currentDraft: TeammateDraft {
        var copy = draft
        copy.skills = skillsText.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        return copy
    }

    private var changes: JSONObject { TeammateEditorLogic.changes(from: original, to: currentDraft) }

    var body: some View {
        Form {
            Section("Identity") {
                TextField("Name", text: $draft.name)
                TextField("Specialty", text: $draft.specialty)
                TextField("Focus", text: $draft.focus)
                TextField("Description", text: $draft.description, axis: .vertical).lineLimit(2...5)
                TextField("Avatar", text: $draft.avatar)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
            }
            Section("Agent") {
                Picker("Provider", selection: $draft.provider) {
                    ForEach(providers, id: \.self) { Text($0).tag($0) }
                }
                TextField("Model", text: $draft.model)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Picker("Mode", selection: $draft.mode) {
                    ForEach(store.options.modes, id: \.self) { mode in
                        Text(mode.isEmpty ? "Desktop default" : mode).tag(mode)
                    }
                }
                Toggle("Enabled", isOn: $draft.enabled)
                Toggle("Learns from its runs", isOn: $draft.selfImproving)
            }
            Section {
                ForEach($draft.backupModels) { $choice in
                    HStack {
                        TextField("provider", text: $choice.provider).textInputAutocapitalization(.never)
                        TextField("model", text: $choice.model).textInputAutocapitalization(.never)
                    }
                    .font(.footnote)
                }
                .onDelete { draft.backupModels.remove(atOffsets: $0) }
                Button("Add a backup model") {
                    draft.backupModels.append(AgentChoiceRow(provider: draft.provider, model: ""))
                }
                .disabled(draft.backupModels.count >= 10)
            } header: {
                Text("Backup models")
            } footer: {
                Text("Used in order when the main model runs out of credits.")
            }
            Section("Task agents") {
                ForEach($draft.taskAgents) { $route in
                    VStack(alignment: .leading) {
                        Picker("Task", selection: $route.taskType) {
                            ForEach(store.options.taskTypes) { Text($0.label).tag($0.id) }
                        }
                        HStack {
                            TextField("provider", text: $route.provider).textInputAutocapitalization(.never)
                            TextField("model", text: $route.model).textInputAutocapitalization(.never)
                        }
                        .font(.footnote)
                    }
                }
                .onDelete { draft.taskAgents.remove(atOffsets: $0) }
                Button("Add a task agent") {
                    draft.taskAgents.append(
                        AgentChoiceRow(taskType: store.options.taskTypes.first?.id ?? "scheduled", provider: draft.provider, model: ""))
                }
                .disabled(draft.taskAgents.count >= 10 || store.options.taskTypes.isEmpty)
            }
            Section("Skills") {
                TextField("Comma-separated skills", text: $skillsText, axis: .vertical)
                    .textInputAutocapitalization(.never)
            }
            Section("Schedule") {
                Picker("Runs", selection: $draft.schedule.mode) {
                    ForEach(store.options.scheduleModes, id: \.self) { mode in
                        Text(TeammateEditorLogic.scheduleModeLabel(mode)).tag(mode)
                    }
                }
                if TeammateEditorLogic.usesTime(draft.schedule.mode) {
                    TextField("Time (HH:mm)", text: $draft.schedule.time)
                        .keyboardType(.numbersAndPunctuation)
                }
                if TeammateEditorLogic.usesDays(draft.schedule.mode) {
                    HStack {
                        ForEach(TeammateEditorLogic.weekdays, id: \.day) { item in
                            let on = draft.schedule.days.contains(item.day)
                            Button(item.short) {
                                if on { draft.schedule.days.removeAll { $0 == item.day } } else { draft.schedule.days.append(item.day) }
                            }
                            .buttonStyle(.bordered)
                            .tint(on ? Color.accentColor : Color.secondary)
                            .font(.caption2)
                        }
                    }
                }
                if TeammateEditorLogic.usesEvents(draft.schedule.mode) {
                    ForEach(store.options.taskTypes) { type in
                        Toggle(type.label, isOn: Binding(
                            get: { draft.schedule.events.contains(type.id) },
                            set: { on in
                                if on { draft.schedule.events.append(type.id) } else { draft.schedule.events.removeAll { $0 == type.id } }
                            }))
                    }
                }
                Toggle("Schedule on", isOn: $draft.schedule.active)
            }
            Section {
                TextField("Relay room, like #team", text: $draft.relayChannel)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
            } header: {
                Text("Updates")
            }
            Section("Instructions") {
                TextField("Instructions", text: $draft.instructions, axis: .vertical).lineLimit(4...16)
            }
            Section {
                ForEach(Array(issues.enumerated()), id: \.offset) { _, issue in
                    Label(issue.message, systemImage: "exclamationmark.circle").foregroundStyle(.orange).font(.footnote).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(issue.message)
                }
                if !error.isEmpty {
                    Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red).font(.footnote).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(error)
                }
            } footer: {
                Text("Log monitors and permissions are edited on the desktop.")
            }
        }
        .navigationTitle(id == nil ? "New special agent" : "Edit")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button("Cancel") { dismiss() }
            }
            ToolbarItem(placement: .confirmationAction) {
                Button {
                    Task { await save() }
                } label: {
                    if saving { ProgressView() } else { Text("Save") }
                }
                .disabled(saving || !issues.isEmpty || changes.isEmpty || !loaded)
            }
        }
        .task { await load() }
    }

    private var providers: [String] {
        var list = store.options.providers
        if !draft.provider.isEmpty && !list.contains(draft.provider) { list.append(draft.provider) }
        return list
    }

    private func load() async {
        if loaded { return }
        await store.loadOptions()
        guard let id else {
            loaded = true
            return
        }
        do {
            let detail = try await store.detail(id)
            let base = TeammateDraft(detail: detail)
            original = base
            draft = base
            skillsText = base.skills.joined(separator: ", ")
            loaded = true
        } catch {
            self.error = TeamErrorText.describe(error)
        }
    }

    private func save() async {
        saving = true
        defer { saving = false }
        let payload = changes
        do {
            if let id {
                try await store.perform(.edit(id: id, changes: payload))
            } else {
                try await store.perform(.create(changes: payload))
            }
            dismiss()
        } catch {
            self.error = TeamErrorText.describe(error)
        }
    }
}
