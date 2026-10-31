import FMBase
import FMControl
import FMWorkflows
import SwiftUI

@MainActor
enum WorkflowsDesktop {
    static var hub: DesktopControlHub? { HostsFeature.hub }

    static var store: DesktopWorkflowsStore? {
        if let existing = Services.get(DesktopWorkflowsStore.self) { return existing }
        guard let hub else { return nil }
        let created = DesktopWorkflowsStore(channel: hub)
        Services.put(created)
        return created
    }

    static func icon(_ row: DesktopWorkflowRow) -> String {
        row.specialAgent ? "smart_toy_outlined" : "desktop_windows_outlined"
    }

    static func when(_ millis: Int64) -> String {
        Date(timeIntervalSince1970: Double(millis) / 1000).formatted(date: .abbreviated, time: .shortened)
    }

    static func subtitle(_ row: DesktopWorkflowRow) -> String {
        var parts = [row.kindLabel, row.repo]
        if row.running {
            parts.append(row.runningStep.isEmpty ? "Running" : "Running \(row.runningStep)")
        } else if row.lastRunAt > 0 {
            parts.append("\(WorkflowsText.status(row.lastStatus)) \(Format.relativeTime(row.lastRunAt))")
        } else if !row.trigger.isEmpty {
            parts.append(row.trigger)
        }
        if !row.enabled { parts.append("Off") }
        return parts.filter { !$0.isEmpty }.joined(separator: " · ")
    }
}

struct DesktopWorkflowsSection: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var store: DesktopWorkflowsStore
    let onOpen: (DesktopWorkflowRow) -> Void
    let onRun: (DesktopWorkflowRow) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            FmSectionHeader(
                title: DesktopWorkflowsText.sectionTitle, icon: "desktop_windows_outlined",
                count: store.rows.isEmpty ? nil : store.rows.count)
                .padding(.horizontal, FmSpace.x4)
                .padding(.top, FmSpace.x4)
            if !store.error.isEmpty && store.rows.isEmpty {
                Text(store.unsupported ? store.error : "\(store.error) \(DesktopWorkflowsText.offline)")
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.vertical, FmSpace.x2)
            } else if !store.loaded {
                FmCircularProgressIndicator(size: 20, strokeWidth: 2)
                    .padding(FmSpace.x4)
            } else if store.rows.isEmpty {
                Text("The desktop has no workflows or special agents in its repositories yet.")
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.vertical, FmSpace.x2)
            }
            ForEach(store.rows) { (row: DesktopWorkflowRow) in
                FmListRow(
                    title: row.name,
                    subtitle: WorkflowsDesktop.subtitle(row),
                    leading: AnyView(FmIconTile(icon: WorkflowsDesktop.icon(row), tint: tint(row))),
                    trailing: AnyView(trailing(row)),
                    onTap: { onOpen(row) })
                    .accessibilityIdentifier("desktop-workflow-\(row.workflowId)")
                FmRowDivider()
            }
        }
    }

    @ViewBuilder private func trailing(_ row: DesktopWorkflowRow) -> some View {
        if row.running {
            FmCircularProgressIndicator(size: 20, strokeWidth: 2)
        } else if row.enabled && row.error.isEmpty {
            FmIconButton(icon: "play_arrow", label: "Run \(row.name) on the desktop", size: 22, onPressed: { onRun(row) })
        }
    }

    private func tint(_ row: DesktopWorkflowRow) -> Color {
        if row.running { return fm.accent }
        switch row.lastStatus {
        case "success": return fm.success
        case "error": return fm.danger
        default: return row.specialAgent ? fm.accent : fm.textSecondary
        }
    }
}

struct DesktopWorkflowScreen: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var store: DesktopWorkflowsStore
    let rowId: String
    @State private var detail: DesktopWorkflowDetail?
    @State private var loadError = ""
    @State private var notice = ""
    @State private var noticeIsError = false
    @State private var busy = false
    @State private var assignment = ""
    @State private var pending: DesktopWorkflowConfirmation?
    @State private var openedRun: DesktopWorkflowRun?

    private var row: DesktopWorkflowRow? { store.row(rowId) }

    var body: some View {
        content
            .navigationTitle(row?.name ?? detail?.name ?? "Workflow")
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
            .task { await load() }
            .onChange(of: store.revision) { _, _ in Task { await load() } }
            .sheet(item: $openedRun) { (run: DesktopWorkflowRun) in
                DesktopWorkflowRunSheet(run: run)
                    .fmSheetStyle(fitsContent: false)
            }
            .fmDialog(isPresented: Binding(get: { pending != nil }, set: { if !$0 { pending = nil } })) {
                FmConfirmDialog(
                    title: pending?.action == "stop" ? "Stop it?" : "Run on the desktop?",
                    message: pending?.summary,
                    confirmLabel: pending?.action == "stop" ? "Stop" : "Run",
                    destructive: pending?.action == "stop",
                    onCancel: { pending = nil },
                    onConfirm: {
                        let confirmation = pending
                        pending = nil
                        if let confirmation { Task { await perform(confirmation) } }
                    })
            }
    }

    @ViewBuilder private var content: some View {
        if let row {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    FmNoticeLine(notice.isEmpty ? nil : notice, isError: noticeIsError, onDismiss: { notice = "" })
                    header(row)
                    if !row.description.isEmpty {
                        Text(row.description)
                            .fmText(.bodyMedium, color: fm.textSecondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    if !row.error.isEmpty {
                        Text(row.error).fmText(.bodySmall, color: fm.danger).fixedSize(horizontal: false, vertical: true)
                    }
                    runSection(row)
                    stepsSection
                    runsSection
                    if !loadError.isEmpty {
                        Text(loadError).fmText(.bodySmall, color: fm.textSecondary)
                    }
                }
                .padding(FmSpace.x4)
                .padding(.bottom, FmSpace.x8)
            }
            .fmRefreshable {
                await store.refresh()
                await load()
            }
        } else {
            FmEmptyState(icon: "desktop_windows_outlined", title: "This workflow is no longer on the desktop")
        }
    }

    private func header(_ row: DesktopWorkflowRow) -> some View {
        HStack(spacing: FmSpace.x3) {
            FmIconTile(icon: WorkflowsDesktop.icon(row), tint: fm.accent)
            VStack(alignment: .leading, spacing: 2) {
                Text(row.name).fmText(.titleMedium)
                Text(facts(row)).fmText(.bodySmall, color: fm.textSecondary).fixedSize(horizontal: false, vertical: true)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            FmStatusBadge(
                label: row.running ? "Running" : row.enabled ? "On" : "Off",
                color: row.running ? fm.accent : row.enabled ? fm.success : fm.textSecondary)
        }
    }

    private func facts(_ row: DesktopWorkflowRow) -> String {
        var parts = [row.kindLabel, row.repo]
        if !row.trigger.isEmpty { parts.append(row.trigger) }
        parts.append(row.steps == 1 ? "1 step" : "\(row.steps) steps")
        if row.runs > 0 { parts.append(row.failedRuns > 0 ? "\(row.runs) runs, \(row.failedRuns) failed" : "\(row.runs) runs") }
        if row.nextRunAt > 0 && row.enabled { parts.append("Next \(WorkflowsDesktop.when(row.nextRunAt))") }
        return parts.filter { !$0.isEmpty }.joined(separator: " · ")
    }

    private func runSection(_ row: DesktopWorkflowRow) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(title: "Run")
            if row.running {
                Text(row.runningStep.isEmpty ? "Running on the desktop." : "Running \(row.runningStep) on the desktop.")
                    .fmText(.bodyMedium)
                FmOutlinedButton(
                    busy ? "Asking the desktop…" : "Stop", icon: "stop", fillWidth: true,
                    onPressed: busy ? nil : { Task { await prepare("stop", row) } })
                    .accessibilityIdentifier("desktop-workflow-stop")
            } else {
                FmTextField(text: $assignment, hint: row.specialAgent ? "Assignment (optional)" : "Input for the run (optional)")
                    .accessibilityIdentifier("desktop-workflow-assignment")
                FmFilledButton(
                    busy ? "Asking the desktop…" : "Run on the desktop", icon: "play_arrow", fillWidth: true, busy: busy,
                    onPressed: busy || !row.enabled || !row.error.isEmpty ? nil : { Task { await prepare("run", row) } })
                    .accessibilityIdentifier("desktop-workflow-run")
                if !row.enabled {
                    Text("It is turned off on the desktop.").fmText(.bodySmall, color: fm.textSecondary)
                }
            }
        }
    }

    @ViewBuilder private var stepsSection: some View {
        if let detail, !detail.nodes.isEmpty {
            let latest = detail.latest
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                FmSectionHeader(title: "Steps", count: detail.nodes.count)
                FmCard(padding: EdgeInsets(top: 0, leading: 0, bottom: 0, trailing: 0)) {
                    VStack(alignment: .leading, spacing: 0) {
                        ForEach(Array(detail.nodes.enumerated()), id: \.element.id) { (entry: (offset: Int, element: DesktopWorkflowDetail.Step)) in
                            if entry.offset > 0 { FmRowDivider() }
                            let status = latest?.steps.first { $0.nodeId == entry.element.id }?.status ?? ""
                            FmListRow(
                                title: entry.element.name,
                                subtitle: entry.element.type,
                                leading: AnyView(FmIconTile(icon: WorkflowLook.stepIcon(entry.element.type))),
                                trailing: status.isEmpty ? nil : AnyView(stepStatus(status)),
                                showChevron: false)
                        }
                    }
                }
            }
        }
    }

    @ViewBuilder private var runsSection: some View {
        if let detail {
            let runs = (detail.active.map { [$0] } ?? []) + detail.runs.filter { $0.id != detail.active?.id }
            if !runs.isEmpty {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    FmSectionHeader(title: "Runs on the desktop", count: runs.count)
                    FmCard(padding: EdgeInsets(top: 0, leading: 0, bottom: 0, trailing: 0)) {
                        VStack(alignment: .leading, spacing: 0) {
                            ForEach(Array(runs.enumerated()), id: \.element.id) { (entry: (offset: Int, element: DesktopWorkflowRun)) in
                                if entry.offset > 0 { FmRowDivider() }
                                let run = entry.element
                                FmListRow(
                                    title: WorkflowsText.status(run.status),
                                    subtitle: [run.startedAt > 0 ? Format.relativeTime(run.startedAt) : "", run.trigger, run.summary]
                                        .filter { !$0.isEmpty }.joined(separator: " · "),
                                    leading: AnyView(stepStatus(run.status)),
                                    onTap: { openedRun = run })
                            }
                        }
                    }
                }
            }
        }
    }

    @ViewBuilder private func stepStatus(_ status: String) -> some View {
        if status == "running" {
            FmCircularProgressIndicator(size: 18, strokeWidth: 2)
        } else {
            FmIcon(WorkflowLook.statusIcon(status), size: 20, color: DesktopWorkflowRunSheet.color(status, fm))
        }
    }

    private func load() async {
        guard let row else { return }
        do {
            detail = try await store.detail(row)
            loadError = ""
        } catch {
            loadError = DesktopWorkflowsText.describe(error)
        }
    }

    private func prepare(_ action: String, _ row: DesktopWorkflowRow) async {
        busy = true
        defer { busy = false }
        do {
            pending = try await store.prepare(action, row, assignment: action == "run" ? assignment : "")
        } catch {
            show(DesktopWorkflowsText.describe(error), error: true)
        }
    }

    private func perform(_ confirmation: DesktopWorkflowConfirmation) async {
        busy = true
        defer { busy = false }
        do {
            let message = try await store.perform(confirmation)
            if confirmation.action == "run" { assignment = "" }
            show(message.isEmpty ? "Sent to the desktop." : message)
            await load()
        } catch {
            show(DesktopWorkflowsText.describe(error), error: true)
        }
    }

    private func show(_ text: String, error: Bool = false) {
        notice = text
        noticeIsError = error
    }
}

struct DesktopWorkflowRunSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let run: DesktopWorkflowRun

    static func color(_ status: String, _ fm: FmPalette) -> Color {
        switch status {
        case "success": return fm.success
        case "error": return fm.danger
        case "running": return fm.accent
        default: return fm.textSecondary
        }
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    Text([run.startedAt > 0 ? WorkflowsDesktop.when(run.startedAt) : "", run.trigger].filter { !$0.isEmpty }
                        .joined(separator: " · "))
                        .fmText(.bodySmall, color: fm.textSecondary)
                    if !run.error.isEmpty {
                        Text(run.error).fmText(.bodyMedium, color: fm.danger).textSelection(.enabled)
                    }
                    if !run.output.isEmpty {
                        FmCard {
                            Text(run.output)
                                .fmText(.bodySmall)
                                .textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                    }
                    ForEach(run.steps) { (step: DesktopWorkflowStepRun) in
                        FmCard {
                            VStack(alignment: .leading, spacing: FmSpace.x2) {
                                HStack(spacing: FmSpace.x2) {
                                    FmIcon(WorkflowLook.statusIcon(step.status), size: 18, color: Self.color(step.status, fm))
                                    Text(step.name).fmText(.titleSmall).lineLimit(1)
                                    Spacer(minLength: 0)
                                    Text(step.ms > 0 ? "\(step.ms / 1000) s" : WorkflowsText.status(step.status))
                                        .fmText(.labelSmall, color: fm.textSecondary)
                                }
                                if !step.error.isEmpty {
                                    Text(step.error).fmText(.bodySmall, color: fm.danger).textSelection(.enabled)
                                }
                                if !step.output.isEmpty {
                                    Text(step.output).fmText(.bodySmall).lineLimit(12).textSelection(.enabled)
                                }
                                if !step.logs.isEmpty {
                                    Text(step.logs.joined(separator: "\n"))
                                        .font(.system(.caption, design: .monospaced))
                                        .foregroundStyle(fm.textSecondary)
                                        .textSelection(.enabled)
                                        .frame(maxWidth: .infinity, alignment: .leading)
                                }
                            }
                        }
                    }
                }
                .padding(FmSpace.x4)
                .padding(.bottom, FmSpace.x6)
            }
            .navigationTitle(WorkflowsText.status(run.status))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    FmTextButton("Done", onPressed: { dismiss() })
                }
            }
        }
    }
}
