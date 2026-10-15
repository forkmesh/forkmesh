import FMApi
import FMBase
import FMCode
import FMControl
import LocalAuthentication
import SwiftUI

@MainActor
enum CIServices {
    static var client: CIClient? { Services.get(ApiService.self) }

    static func actionsModel(_ repo: RepoRef) -> RepoActionsModel? {
        guard let client else { return nil }
        let hub: DesktopControlHub? = HostsFeature.hub
        let desktop: DesktopActions? = hub.map { DesktopActions(channel: $0, desktopName: $0.desktop?.name ?? "") }
        return RepoActionsModel(client: client, repo: repo, desktop: desktop)
    }
}

@MainActor
enum CIAuthentication {
    static func confirm(_ reason: String) async -> Bool {
        let context: LAContext = LAContext()
        var failure: NSError?
        guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &failure) else {
            return true
        }
        do {
            return try await context.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: reason)
        } catch {
            return false
        }
    }
}

struct CIStatusBadge: View {
    @Environment(\.fmPalette) private var fm
    let status: ActionsRunStatus

    private var color: Color {
        switch status {
        case .success: return fm.success
        case .failed, .rejected: return fm.danger
        case .awaitingApproval: return fm.warning
        case .queued, .running: return fm.accent
        case .cancelled, .skipped, .unknown: return fm.textSecondary
        }
    }

    var body: some View {
        FmStatusBadge(label: status.label, color: color)
    }
}

struct RepoActionsScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoActionsModel?
    @State private var dispatching: ActionsWorkflowSummary?

    init(repo: RepoRef, commit: String = "") {
        let model: RepoActionsModel? = CIServices.actionsModel(repo)
        let wanted: String = commit.trimmingCharacters(in: .whitespaces).lowercased()
        if wanted.count >= 7 { model?.commitFilter = wanted }
        _model = State(initialValue: model)
    }

    var body: some View {
        Group {
            if let model = model {
                RepoActionsList(model: model, dispatching: $dispatching)
                    .task { await model.load() }
                    .fmRefreshable { await model.load() }
                    .onDisappear { model.stop() }
                    .sheet(item: $dispatching) { (workflow: ActionsWorkflowSummary) in
                        DispatchWorkflowSheet(model: model, workflow: workflow)
                    }
            } else {
                FmEmptyState(icon: "play_circle_outline", title: "Actions are unavailable")
            }
        }
        .background(fm.canvas)
        .navigationTitle("Actions")
        .navigationBarTitleDisplayMode(.inline)
    }
}

enum ActionsPane: String, CaseIterable, Identifiable {
    case runs, workflows

    var id: String { rawValue }
}

struct RepoActionsList: View {
    @Environment(\.fmPalette) private var fm
    let model: RepoActionsModel
    @Binding var dispatching: ActionsWorkflowSummary?
    @State private var pane: ActionsPane = .runs

    var body: some View {
        List {
            Section {
                Picker("Show", selection: $pane) {
                    Text(runsTitle).tag(ActionsPane.runs)
                    Text(workflowsTitle).tag(ActionsPane.workflows)
                }
                .pickerStyle(.segmented)
                .listRowBackground(Color.clear)
                .listRowInsets(EdgeInsets())
            } header: {
                sourceHeader
            }
            if let notice = model.notice {
                Section {
                    Label(notice.text, systemImage: notice.good ? "checkmark.circle" : "exclamationmark.triangle")
                        .foregroundStyle(notice.good ? fm.success : fm.danger)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(notice.text)
                }
            }
            if let error = model.error {
                Section {
                    Text(error).foregroundStyle(fm.danger)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(error)
                }
            }
            if model.summary?.actionsEnabled == false {
                Section {
                    Label("Actions are turned off for this repository.", systemImage: "pause.circle")
                        .foregroundStyle(fm.warning)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools("Actions are turned off for this repository.")
                }
            }
            switch pane {
            case .runs: runsSections
            case .workflows: workflowSections
            }
        }
        .listStyle(.insetGrouped)
        .overlay {
            if model.loading && model.summary == nil {
                ProgressView()
            }
        }
        .task(id: model.needsFollowUp) {
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: UInt64(model.nextLoadDelaySeconds * 1_000_000_000))
                if Task.isCancelled {
                    return
                }
                await model.load()
            }
        }
    }

    private var runsTitle: String {
        let active: Int = (model.summary?.runs ?? []).filter(\.status.isActive).count
        return active > 0 ? "Runs (\(active) active)" : "Runs"
    }

    private var workflowsTitle: String {
        model.workflows.isEmpty ? "Workflows" : "Workflows (\(model.workflows.count))"
    }

    @ViewBuilder
    private var sourceHeader: some View {
        if model.fromDesktop, let summary = model.summary {
            Label("Live from \(summary.node)", systemImage: "desktopcomputer")
                .font(.caption)
                .foregroundStyle(fm.textSecondary)
                .textCase(nil)
        } else if let node = model.summary?.node, !node.isEmpty {
            Label("From \(node)", systemImage: "server.rack")
                .font(.caption)
                .foregroundStyle(fm.textSecondary)
                .textCase(nil)
        }
    }

    private var filterBinding: Binding<String> {
        Binding(get: { model.workflowFilter ?? "" }, set: { model.workflowFilter = $0.isEmpty ? nil : $0 })
    }

    @ViewBuilder
    private var runsSections: some View {
        let active: [ActionsRunSummary] = model.runs.filter(\.status.isActive)
        let finished: [ActionsRunSummary] = model.runs.filter { !$0.status.isActive }
        if !model.workflows.isEmpty {
            Section {
                Picker("Workflow", selection: filterBinding) {
                    Text("All workflows").tag("")
                    ForEach(model.workflows) { (workflow: ActionsWorkflowSummary) in
                        Text(workflow.displayName).tag(workflow.path)
                    }
                }
                .pickerStyle(.menu)
            }
        }
        if let commit = model.commitFilter {
            Section {
                Label("Runs for commit \(String(commit.prefix(7)))", systemImage: "point.topleft.down.to.point.bottomright.curvepath")
                    .font(.footnote.monospaced())
                Button {
                    model.commitFilter = nil
                } label: {
                    Label("Show all runs", systemImage: "list.bullet")
                }
                .accessibilityIdentifier("actions-show-all-runs")
            }
        }
        if model.runs.isEmpty && !model.loading {
            Section {
                Text(runsEmptyMessage)
                    .foregroundStyle(fm.textSecondary)
            }
        }
        if !active.isEmpty {
            Section("In progress") {
                runRows(active)
            }
        }
        if !finished.isEmpty {
            Section("Recent") {
                runRows(finished)
            }
        }
    }

    private var runsEmptyMessage: String {
        if model.workflowFilter != nil { return "This workflow has no recent runs." }
        if model.commitFilter != nil && !model.isUnavailable { return model.commitEmptyMessage }
        return model.emptyMessage
    }

    private func runRows(_ runs: [ActionsRunSummary]) -> some View {
        ForEach(runs) { (run: ActionsRunSummary) in
            NavigationLink {
                ActionRunScreen(model: model, runId: run.id)
                    .shellPageClearance()
            } label: {
                ActionRunRow(run: run, pending: model.isPending(runId: run.id))
            }
        }
    }

    @ViewBuilder
    private var workflowSections: some View {
        if model.workflows.isEmpty && !model.loading {
            Section {
                Text("No workflows in .forkmesh/ yet.").foregroundStyle(fm.textSecondary)
            }
        }
        workflowGroup("Run by hand", model.manualWorkflows, footer: nil)
        workflowGroup("Automatic", model.automaticWorkflows, footer: nil)
        workflowGroup(
            "Turned off", model.disabledWorkflows,
            footer: "A turned-off workflow does not run on push, pull requests or releases, and cannot be run by hand.")
        if !model.workflows.isEmpty && !model.canSwitchWorkflows {
            Section {
                Text("This repository's node does not take remote changes, so workflows can only be run or turned off there.")
                    .font(.caption)
                    .foregroundStyle(fm.textSecondary)
            }
        }
    }

    @ViewBuilder
    private func workflowGroup(_ title: String, _ workflows: [ActionsWorkflowSummary], footer: String?) -> some View {
        if !workflows.isEmpty {
            Section {
                ForEach(workflows) { (workflow: ActionsWorkflowSummary) in
                    workflowRow(workflow)
                }
            } header: {
                Text(title)
            } footer: {
                if let footer {
                    Text(footer)
                }
            }
        }
    }

    private func workflowRow(_ workflow: ActionsWorkflowSummary) -> some View {
        NavigationLink {
            ActionWorkflowScreen(model: model, path: workflow.path, dispatching: $dispatching)
                .shellPageClearance()
        } label: {
            ActionWorkflowRow(
                workflow: workflow, latest: model.latestRun(of: workflow), switching: model.isSwitching(workflow),
                canRun: workflow.canDispatch && model.canControl, onRun: { dispatching = workflow })
        }
        .swipeActions(edge: .trailing, allowsFullSwipe: false) {
            if model.canSwitchWorkflows && !model.isSwitching(workflow) {
                Button(workflow.enabled ? "Turn off" : "Turn on") {
                    ActionWorkflowSwitch.set(model, workflow, !workflow.enabled)
                }
                .tint(workflow.enabled ? fm.warning : fm.success)
            }
        }
        .contextMenu {
            if workflow.canDispatch && model.canControl {
                Button {
                    dispatching = workflow
                } label: {
                    Label("Run workflow", systemImage: "play")
                }
            }
            Button {
                model.workflowFilter = workflow.path
                pane = .runs
            } label: {
                Label("Show runs", systemImage: "list.bullet")
            }
            if model.canSwitchWorkflows && !model.isSwitching(workflow) {
                Button(role: workflow.enabled ? .destructive : nil) {
                    ActionWorkflowSwitch.set(model, workflow, !workflow.enabled)
                } label: {
                    Label(workflow.enabled ? "Turn off" : "Turn on", systemImage: workflow.enabled ? "pause.circle" : "play.circle")
                }
            }
        }
    }
}

enum ActionWorkflowSwitch {
    static func set(_ model: RepoActionsModel, _ workflow: ActionsWorkflowSummary, _ enabled: Bool) {
        Task { @MainActor in
            if enabled {
                guard await CIAuthentication.confirm("Turn on \(workflow.displayName)") else {
                    return
                }
            }
            await model.setEnabled(workflow, enabled)
        }
    }
}

struct ActionWorkflowRow: View {
    @Environment(\.fmPalette) private var fm
    let workflow: ActionsWorkflowSummary
    let latest: ActionsRunSummary?
    let switching: Bool
    let canRun: Bool
    let onRun: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 4) {
                Text(workflow.displayName)
                    .font(.body)
                    .foregroundStyle(workflow.enabled ? fm.textPrimary : fm.textSecondary)
                    .lineLimit(2)
                Text(workflow.eventsLabel)
                    .font(.caption)
                    .foregroundStyle(fm.textSecondary)
                if !workflow.valid {
                    Label("Invalid workflow file", systemImage: "exclamationmark.triangle")
                        .font(.caption)
                        .foregroundStyle(fm.danger)
                } else if let latest {
                    HStack(spacing: 6) {
                        CIStatusBadge(status: latest.status)
                        Text("#\(latest.id)").font(.caption).foregroundStyle(fm.textSecondary)
                    }
                }
            }
            Spacer(minLength: 8)
            if switching {
                ProgressView().controlSize(.small)
            } else if canRun {
                Button("Run", action: onRun)
                    .buttonStyle(.bordered)
            }
        }
        .padding(.vertical, 2)
    }
}

struct ActionWorkflowScreen: View {
    @Environment(\.fmPalette) private var fm
    let model: RepoActionsModel
    let path: String
    @Binding var dispatching: ActionsWorkflowSummary?

    private var workflow: ActionsWorkflowSummary? { model.workflows.first { $0.path == path } }

    var body: some View {
        List {
            if let workflow {
                switchSection(workflow)
                detailsSection(workflow)
                runsSection(workflow)
            } else if model.loading {
                ProgressView()
            } else {
                Text("This workflow is no longer in the repository.").foregroundStyle(fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools("This workflow is no longer in the repository.")
            }
        }
        .listStyle(.insetGrouped)
        .navigationTitle(workflow?.displayName ?? "Workflow")
        .navigationBarTitleDisplayMode(.inline)
        .fmRefreshable { await model.load() }
        .toolbar {
            if let workflow, workflow.canDispatch && model.canControl {
                ToolbarItem(placement: .primaryAction) {
                    Button("Run") { dispatching = workflow }
                }
            }
        }
    }

    private func switchSection(_ workflow: ActionsWorkflowSummary) -> some View {
        Section {
            HStack {
                Toggle(
                    "On",
                    isOn: Binding(
                        get: { workflow.enabled },
                        set: { ActionWorkflowSwitch.set(model, workflow, $0) }))
                    .disabled(!model.canSwitchWorkflows || model.isSwitching(workflow))
                if model.isSwitching(workflow) {
                    ProgressView().controlSize(.small)
                }
            }
            if let notice = model.notice {
                Text(notice.text).font(.caption).foregroundStyle(notice.good ? fm.success : fm.danger)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(notice.text)
            }
        } footer: {
            if !model.canSwitchWorkflows {
                Text("Turn this workflow on or off on the node that runs this repository's actions.")
            } else if workflow.enabled {
                Text("Turned off, it does not run on push, pull requests or releases, and cannot be run by hand.")
            } else {
                Text("Turned off. It does not run until it is turned on again.")
            }
        }
    }

    private func detailsSection(_ workflow: ActionsWorkflowSummary) -> some View {
        Section("Details") {
            FmKeyValueRow(label: "File", value: workflow.path)
            FmKeyValueRow(label: "Triggers", value: workflow.eventsLabel)
            if !workflow.runsOn.isEmpty {
                FmKeyValueRow(label: "Runs on", value: workflow.runsOn.joined(separator: ", "))
            }
            if !workflow.valid {
                Label("The workflow file has an error, so it cannot run.", systemImage: "exclamationmark.triangle")
                    .foregroundStyle(fm.danger)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools("The workflow file \(workflow.path) has an error, so it cannot run.")
            }
        }
    }

    private func runsSection(_ workflow: ActionsWorkflowSummary) -> some View {
        Section("Recent runs") {
            let runs: [ActionsRunSummary] = model.runs(of: workflow)
            if runs.isEmpty {
                Text("No recent runs.").foregroundStyle(fm.textSecondary)
            }
            ForEach(runs) { (run: ActionsRunSummary) in
                NavigationLink {
                    ActionRunScreen(model: model, runId: run.id)
                        .shellPageClearance()
                } label: {
                    ActionRunRow(run: run, pending: model.isPending(runId: run.id))
                }
            }
        }
    }
}

struct ActionRunRow: View {
    @Environment(\.fmPalette) private var fm
    let run: ActionsRunSummary
    let pending: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(run.displayName).font(.body.weight(.medium))
                Spacer()
                if pending {
                    ProgressView().controlSize(.small)
                }
                CIStatusBadge(status: run.status)
            }
            HStack(spacing: 8) {
                Text("#\(run.id)")
                if !run.refLabel.isEmpty {
                    Label(run.refLabel, systemImage: "arrow.triangle.branch")
                }
                Text(run.shortCommit).monospaced()
                if run.pullNumber > 0 {
                    Text("PR #\(run.pullNumber)")
                }
            }
            .font(.caption)
            .foregroundStyle(fm.textSecondary)
            if run.stepProgress.total > 0 {
                ProgressView(value: Double(run.stepProgress.done), total: Double(run.stepProgress.total))
                    .tint(run.status == .failed ? fm.danger : fm.accent)
            }
        }
        .padding(.vertical, 2)
    }
}

struct ActionRunScreen: View {
    @Environment(\.fmPalette) private var fm
    let model: RepoActionsModel
    let runId: Int
    @State private var confirming: ActionsControlKind?
    @State private var desktopNotice: String?

    private var run: ActionsRunSummary? { model.run(runId) }

    private func offersDesktopRerun(_ run: ActionsRunSummary) -> Bool {
        let finished: [ActionsRunStatus] = [.success, .failed, .rejected, .cancelled, .skipped]
        return DesktopCommandAvailability.shared.available && !model.canControl && finished.contains(run.status)
    }

    var body: some View {
        List {
            if let run = run {
                summarySection(run)
                if !run.steps.isEmpty {
                    stepsSection(run)
                }
                logSection(run)
            } else if model.loading {
                ProgressView()
            } else {
                Text("This run is no longer in the recent list.").foregroundStyle(fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools("This run is no longer in the recent list.")
            }
        }
        .listStyle(.insetGrouped)
        .navigationTitle("Run #\(runId)")
        .navigationBarTitleDisplayMode(.inline)
        .fmRefreshable {
            await model.load()
            await model.loadLog(runId: runId)
        }
        .task(id: runId) {
            if model.summary == nil {
                await model.load()
            }
            await model.follow(runId: runId)
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: UInt64(model.nextLoadDelaySeconds * 1_000_000_000))
                if Task.isCancelled {
                    return
                }
                await model.load()
                await model.loadLog(runId: runId)
            }
        }
        .onDisappear { model.stop() }
        .task {
            if let api: ApiService = Services.get(ApiService.self) {
                await DesktopCommandAvailability.shared.refresh(owner: model.repo.owner, name: model.repo.name, probe: api)
            }
        }
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                actionsMenu
            }
        }
        .confirmationDialog(
            confirming?.label ?? "",
            isPresented: Binding(get: { confirming != nil }, set: { if !$0 { confirming = nil } }),
            titleVisibility: .visible
        ) {
            if let kind = confirming {
                Button(kind.label, role: kind.isDestructive ? .destructive : nil) {
                    perform(kind)
                }
            }
        }
    }

    private var actionsMenu: some View {
        Menu {
            if let run = run {
                ForEach(model.actions(for: run), id: \.self) { (kind: ActionsControlKind) in
                    Button(kind.label) { confirming = kind }
                }
                if offersDesktopRerun(run) {
                    Button(DesktopCommandRequests.rerunLabel) { requestDesktopRerun(run) }
                }
                if run.status == .failed {
                    Button("Send to agent") {
                        FmMessageHandoff.compose(CIPrompts.fixFailedRun(run, repo: model.repo))
                    }
                }
            }
        } label: {
            Image(systemName: "ellipsis.circle")
        }
        .accessibilityLabel("Run actions")
    }

    private func perform(_ kind: ActionsControlKind) {
        Task {
            if kind.needsAuthentication {
                guard await CIAuthentication.confirm("\(kind.label) run #\(runId)") else {
                    return
                }
            }
            await model.send(kind, runId: runId)
        }
    }

    private func requestDesktopRerun(_ run: ActionsRunSummary) {
        let repo: RepoRef = model.repo
        let id: Int = runId
        let workflowPath: String = run.workflowPath
        Task {
            guard await AccountFeature.requireAccount(action: DesktopCommand.guestAction) else {
                return
            }
            desktopNotice = await DesktopCommandRequests.rerun(
                client: RepoServices.client, signer: RepoServices.signer, repo: repo, runId: id,
                workflowPath: workflowPath)
        }
    }

    private func summarySection(_ run: ActionsRunSummary) -> some View {
        Section {
            HStack {
                Text(run.displayName).font(.headline)
                Spacer()
                CIStatusBadge(status: run.status)
            }
            if !run.refLabel.isEmpty {
                FmKeyValueRow(label: "Ref", value: run.refLabel)
            }
            if model.fromDesktop, let node = model.summary?.node {
                FmKeyValueRow(label: "Runs on", value: node)
            }
            FmKeyValueRow(label: "Commit", value: run.shortCommit)
            if run.pullNumber > 0 {
                FmKeyValueRow(label: "Pull request", value: "#\(run.pullNumber)")
            }
            let duration: String = run.durationLabel(nowMs: Int(Date().timeIntervalSince1970 * 1_000))
            if !duration.isEmpty {
                FmKeyValueRow(label: "Duration", value: duration)
            }
            if let notice = model.notice {
                Text(notice.text).font(.caption).foregroundStyle(notice.good ? fm.success : fm.danger)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(notice.text)
            }
            if let desktopNotice = desktopNotice {
                Text(desktopNotice).font(.caption).foregroundStyle(fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(desktopNotice)
            }
        }
    }

    private func stepsSection(_ run: ActionsRunSummary) -> some View {
        Section("Steps") {
            ForEach(Array(run.steps.enumerated()), id: \.offset) { (entry: (offset: Int, element: ActionsStep)) in
                HStack {
                    Text(entry.element.name)
                    Spacer()
                    CIStatusBadge(status: ActionsRunStatus(raw: entry.element.status == "pending" ? "queued" : entry.element.status))
                }
            }
        }
    }

    private func logSection(_ run: ActionsRunSummary) -> some View {
        Section(model.fromDesktop && run.status.isActive ? "Log (live)" : "Log (last lines)") {
            let lines: [String] = CILog.lines(run.logTail)
            if lines.isEmpty {
                Text(run.status.isActive ? "The log appears here as the run goes. Pull to refresh." : "No log output.")
                    .foregroundStyle(fm.textSecondary)
            } else {
                ScrollView(.horizontal) {
                    Text(lines.joined(separator: "\n"))
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                        .fixedSize(horizontal: true, vertical: false)
                }
                HStack {
                    ShareLink(item: lines.joined(separator: "\n")) {
                        Label("Share log", systemImage: "square.and.arrow.up")
                    }
                    if run.status == .failed || run.status == .rejected {
                        Spacer()
                        FmMessageTools(lines.joined(separator: "\n"))
                    }
                }
            }
        }
    }
}

struct DispatchWorkflowSheet: View {
    @Environment(\.dismiss) private var dismiss
    let model: RepoActionsModel
    let workflow: ActionsWorkflowSummary
    @State private var branch: String = "main"
    @State private var sending: Bool = false

    var body: some View {
        NavigationStack {
            Form {
                Section("Workflow") {
                    Text(workflow.displayName)
                    Text(workflow.path).font(.caption).monospaced()
                }
                Section("Branch") {
                    TextField("Branch", text: $branch)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                }
                if let notice = model.notice, !notice.good {
                    Section {
                        Text(notice.text).foregroundStyle(.red)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .fmMessageTools(notice.text)
                    }
                }
            }
            .navigationTitle("Run workflow")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Run") { run() }
                        .disabled(sending || branch.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
        }
    }

    private func run() {
        sending = true
        Task {
            guard await CIAuthentication.confirm("Run \(workflow.displayName) on \(branch)") else {
                sending = false
                return
            }
            let ok: Bool = await model.dispatch(workflow: workflow, branch: branch)
            sending = false
            if ok {
                dismiss()
            }
        }
    }
}

struct ActionRunDestination: View {
    let repo: RepoRef
    let runId: Int
    @State private var model: RepoActionsModel?

    init(repo: RepoRef, runId: Int) {
        self.repo = repo
        self.runId = runId
        _model = State(initialValue: CIServices.actionsModel(repo))
    }

    var body: some View {
        if let model = model {
            ActionRunScreen(model: model, runId: runId)
        } else {
            FmEmptyState(icon: "play_circle_outline", title: "Actions are unavailable")
        }
    }
}
