import FMApi
import FMBase
import FMWorkflows
import SwiftUI
import UIKit

struct WorkflowAddTarget: Identifiable, Equatable {
    var after: String?
    var outputs: [String]

    var id: String { (after ?? "end") + ":" + outputs.joined(separator: ",") }
}

struct WorkflowEditorScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL
    @Environment(\.dismiss) private var dismiss
    let store: WorkflowsStore
    let workflowId: String?
    @State private var draft: WorkflowDraft?
    @State private var saved: WorkflowDraft?
    @State private var loadError: String = ""
    @State private var notice: String = ""
    @State private var noticeIsError: Bool = false
    @State private var busy: String = ""
    @State private var runs: [WorkflowRun] = []
    @State private var openedRun: WorkflowRun?
    @State private var editingStep: String?
    @State private var adding: WorkflowAddTarget?
    @State private var confirmDelete: Bool = false
    @State private var credentialsOpen: Bool = false

    init(store: WorkflowsStore, workflowId: String?, draft: WorkflowDraft?) {
        self.store = store
        self.workflowId = workflowId
        _draft = State(initialValue: draft)
    }

    var body: some View {
        content
            .navigationTitle(draft?.name ?? WorkflowsText.title)
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
            .toolbar { toolbarItems }
            .task { await load() }
            .navigationDestination(item: $editingStep) { (id: String) in
                WorkflowStepScreen(store: store, draft: draftBinding, nodeId: id, onDelete: {
                    editingStep = nil
                    draft?.remove(id)
                })
                .shellPageClearance()
            }
            .navigationDestination(isPresented: $credentialsOpen) {
                WorkflowCredentialsScreen(store: store)
                    .shellPageClearance()
            }
            .sheet(item: $adding) { (target: WorkflowAddTarget) in
                WorkflowStepPicker(catalog: store.catalog, outputs: target.outputs) { (kind: WorkflowNodeKind, output: String) in
                    adding = nil
                    guard draft != nil else { return }
                    let step = draft!.addStep(kind, after: target.after, output: output)
                    editingStep = step.id
                }
                .fmSheetStyle(fitsContent: false)
            }
            .sheet(item: $openedRun) { (run: WorkflowRun) in
                WorkflowRunSheet(store: store, run: run, draft: draft ?? .blank)
                    .fmSheetStyle(fitsContent: false)
            }
            .fmDialog(isPresented: $confirmDelete) {
                FmConfirmDialog(
                    title: "Delete this workflow?",
                    message: "Its run history goes with it. Credentials stay on your account.",
                    confirmLabel: "Delete", destructive: true,
                    onCancel: { confirmDelete = false },
                    onConfirm: {
                        confirmDelete = false
                        delete()
                    })
            }
    }

    private var draftBinding: Binding<WorkflowDraft> {
        Binding(get: { draft ?? .blank }, set: { draft = $0 })
    }

    private var dirty: Bool { draft != nil && draft != saved }

    @ToolbarContentBuilder private var toolbarItems: some ToolbarContent {
        ToolbarItem(placement: .primaryAction) {
            HStack(spacing: 0) {
                if busy == "save" {
                    FmCircularProgressIndicator(size: 20, strokeWidth: 2)
                } else if dirty {
                    FmTextButton("Save", onPressed: { Task { await save() } })
                        .accessibilityIdentifier("workflow-save")
                }
                Menu {
                    if let path = draft?.id, !path.isEmpty {
                        Button("Open the canvas on the web") { openOnWeb() }
                        Button("Delete workflow", role: .destructive) { confirmDelete = true }
                    }
                    Button("Credentials") { credentialsOpen = true }
                } label: {
                    FmIcon("more_vert", size: 24, tone: .primary)
                        .frame(width: 44, height: 44)
                }
                .accessibilityLabel("More")
            }
        }
    }

    @ViewBuilder private var content: some View {
        if let draft = draft {
            editor(draft)
        } else if !loadError.isEmpty {
            FmEmptyState(icon: "error_outline", title: WorkflowsText.unavailableTitle, message: loadError)
        } else {
            FmSkeletonList()
        }
    }

    private func editor(_ draft: WorkflowDraft) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                FmNoticeLine(notice.isEmpty ? nil : notice, isError: noticeIsError, onDismiss: { notice = "" })
                FmTextField(text: draftBinding.name, hint: "Workflow name")
                    .accessibilityIdentifier("workflow-name")
                Toggle(isOn: draftBinding.active) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Active").fmText(.titleSmall)
                        Text("Webhooks and schedules start it on ForkMesh while it is active.")
                            .fmText(.bodySmall, color: fm.textSecondary)
                    }
                }
                .tint(fm.accent)
                triggerSection(draft)
                stepsSection(draft)
                runSection(draft)
                historySection(draft)
            }
            .padding(FmSpace.x4)
            .padding(.bottom, FmSpace.x8)
        }
        .scrollDismissesKeyboard(.interactively)
    }

    private func triggerSection(_ draft: WorkflowDraft) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(title: "Trigger")
            FmCard(padding: EdgeInsets(top: 0, leading: 0, bottom: 0, trailing: 0)) {
                if let trigger = draft.trigger {
                    FmListRow(
                        title: trigger.name,
                        subtitle: triggerLine(trigger, draft),
                        leading: AnyView(FmIconTile(icon: WorkflowLook.triggerIcon(trigger.type), tint: kindColor(trigger.type))),
                        onTap: { editingStep = trigger.id })
                } else {
                    FmListRow(
                        title: "Add a trigger", subtitle: "Manual, webhook or schedule",
                        leading: AnyView(FmIconTile(icon: "add", tint: fm.accent)),
                        onTap: {
                            if let manual = store.catalog.kind("core.manual") { self.draft?.setTrigger(manual) }
                        })
                }
            }
            if draft.trigger?.type == "core.webhook", let hook = hookURL(draft) {
                HStack(spacing: FmSpace.x2) {
                    Text(hook)
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .lineLimit(2)
                        .truncationMode(.middle)
                        .textSelection(.enabled)
                    Spacer(minLength: 0)
                    FmIconButton(icon: "link", label: "Copy webhook URL", size: 20, onPressed: {
                        UIPasteboard.general.string = hook
                        show("Webhook URL copied")
                    })
                }
            }
        }
    }

    private func stepsSection(_ draft: WorkflowDraft) -> some View {
        let steps = draft.ordered.filter { !$0.isTrigger }
        let last = latestRun
        return VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(title: "Steps", count: steps.isEmpty ? nil : steps.count)
            if !steps.isEmpty {
                FmCard(padding: EdgeInsets(top: 0, leading: 0, bottom: 0, trailing: 0)) {
                    VStack(alignment: .leading, spacing: 0) {
                        ForEach(Array(steps.enumerated()), id: \.element.id) { (entry: (offset: Int, element: WorkflowNode)) in
                            if entry.offset > 0 { FmRowDivider() }
                            stepRow(entry.element, draft: draft, run: last)
                        }
                    }
                }
            }
            FmOutlinedButton("Add step", icon: "add", fillWidth: true, onPressed: {
                let anchor = steps.last ?? draft.trigger
                adding = WorkflowAddTarget(after: anchor?.id, outputs: anchor.map { store.catalog.outputs($0.type) } ?? ["main"])
            })
            .accessibilityIdentifier("workflow-add-step")
        }
    }

    private func stepRow(_ node: WorkflowNode, draft: WorkflowDraft, run: WorkflowRun?) -> some View {
        let inputs = draft.inputs(of: node.id).map { store.catalog.branch($0, in: draft) }
        let summary = store.catalog.summary(node)
        let subtitle = ([store.catalog.title(node.type), summary] + (inputs.isEmpty ? ["Not connected"] : inputs))
            .filter { !$0.isEmpty }.joined(separator: " · ")
        let status = run.flatMap { $0.nodes[node.id]?.objectValue?.str("status") } ?? ""
        return FmListRow(
            title: node.name,
            subtitle: subtitle,
            leading: AnyView(FmIconTile(icon: WorkflowLook.stepIcon(node.type), tint: kindColor(node.type))),
            trailing: status.isEmpty ? nil : AnyView(FmIcon(WorkflowLook.statusIcon(status), size: 18, color: statusColor(status))),
            onTap: { editingStep = node.id })
            .contextMenu {
                Button("Add step after") {
                    adding = WorkflowAddTarget(after: node.id, outputs: store.catalog.outputs(node.type))
                }
                Button("Delete step", role: .destructive) { self.draft?.remove(node.id) }
            }
            .accessibilityIdentifier("workflow-step-\(node.id)")
    }

    private func runSection(_ draft: WorkflowDraft) -> some View {
        let missing = store.missingPhoneCredentials(draft)
        return VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(title: "Run")
            FmFilledButton(
                busy == "phone" ? "Running on this phone…" : "Run on this phone", icon: "smartphone", fillWidth: true,
                onPressed: busy.isEmpty && store.canRunOnPhone ? { Task { await runOnPhone() } } : nil)
                .accessibilityIdentifier("workflow-run-phone")
            FmOutlinedButton(
                busy == "server" ? "Running on ForkMesh…" : "Run on ForkMesh", icon: "cloud_outlined", fillWidth: true,
                onPressed: busy.isEmpty ? { Task { await runOnServer() } } : nil)
                .accessibilityIdentifier("workflow-run-server")
            Text(store.canRunOnPhone ? WorkflowsText.phoneRunNote : WorkflowsText.phoneEngineMissing)
                .fmText(.bodySmall, color: fm.textSecondary)
                .fixedSize(horizontal: false, vertical: true)
            if !missing.isEmpty && store.canRunOnPhone {
                FmCard(onTap: { credentialsOpen = true }) {
                    HStack(alignment: .top, spacing: FmSpace.x2) {
                        FmIcon("lock_outline", size: 18, color: fm.warning)
                        Text("Save a credential on this phone for " + missing.map(\.name).joined(separator: ", ")
                             + " to run those steps here.")
                            .fmText(.bodySmall)
                            .fixedSize(horizontal: false, vertical: true)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
            }
        }
    }

    @ViewBuilder private func historySection(_ draft: WorkflowDraft) -> some View {
        if !runs.isEmpty {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                FmSectionHeader(title: "Runs", count: runs.count)
                FmCard(padding: EdgeInsets(top: 0, leading: 0, bottom: 0, trailing: 0)) {
                    VStack(alignment: .leading, spacing: 0) {
                        ForEach(Array(runs.prefix(30).enumerated()), id: \.element.id) { (entry: (offset: Int, element: WorkflowRun)) in
                            if entry.offset > 0 { FmRowDivider() }
                            runRow(entry.element)
                        }
                    }
                }
            }
        }
    }

    private func runRow(_ run: WorkflowRun) -> some View {
        let place = run.place == .phone ? "On this phone" : "On ForkMesh"
        let when = run.startedAt > 0 ? Format.relativeTime(run.startedAt) : ""
        let subtitle = [place, run.trigger.capitalized, when, run.error].filter { !$0.isEmpty }.joined(separator: " · ")
        return FmListRow(
            title: WorkflowsText.status(run.status),
            subtitle: subtitle,
            leading: AnyView(FmIcon(WorkflowLook.statusIcon(run.status), size: 22, color: statusColor(run.status))),
            onTap: { open(run) })
    }

    private var latestRun: WorkflowRun? { runs.first(where: \.hasDetail) }

    private func triggerLine(_ trigger: WorkflowNode, _ draft: WorkflowDraft) -> String {
        switch trigger.type {
        case "core.webhook": return draft.id.isEmpty ? "Save to get the secret URL" : "Called by its secret URL"
        case "core.schedule": return WorkflowsText.every(Int(trigger.text("every")) ?? 60)
        default: return "Runs when you press Run"
        }
    }

    private func hookURL(_ draft: WorkflowDraft) -> String? {
        guard !draft.id.isEmpty, !draft.hookPath.isEmpty else { return nil }
        return Services.get(ApiService.self)?.url(draft.hookPath).absoluteString
    }

    private func kindColor(_ type: String) -> Color {
        WorkflowLook.color(store.catalog.kind(type)?.color ?? "", fallback: fm.accent)
    }

    private func statusColor(_ status: String) -> Color {
        switch status {
        case "success": return fm.success
        case "error": return fm.danger
        default: return fm.textSecondary
        }
    }

    private func show(_ text: String, error: Bool = false) {
        notice = text
        noticeIsError = error
    }

    private func load() async {
        if store.catalog.core.isEmpty { await store.loadCatalog() }
        _ = await store.loadCredentials()
        if draft == nil, let workflowId {
            do {
                let opened = try await store.open(workflowId)
                draft = opened
                saved = opened
            } catch {
                loadError = WorkflowsText.describe(error, "This workflow could not be opened.")
                return
            }
        }
        await refreshRuns()
    }

    private func refreshRuns() async {
        guard let id = draft?.id, !id.isEmpty else {
            runs = store.phoneRuns["draft"] ?? []
            return
        }
        if let loaded = try? await store.runs(id) { runs = loaded }
    }

    @discardableResult
    private func save() async -> Bool {
        guard let current = draft, busy.isEmpty else { return false }
        let problem = store.check(current)
        if !problem.isEmpty {
            show(problem, error: true)
            return false
        }
        busy = "save"
        defer { busy = "" }
        do {
            let stored = try await store.save(current)
            draft = stored
            saved = stored
            show("Saved")
            return true
        } catch {
            show(WorkflowsText.describe(error, "This workflow could not be saved."), error: true)
            return false
        }
    }

    private func runOnPhone() async {
        guard let current = draft, busy.isEmpty else { return }
        busy = "phone"
        let run = await store.runOnPhone(current)
        busy = ""
        await refreshRuns()
        if !runs.contains(where: { $0.id == run.id }) { runs.insert(run, at: 0) }
        openedRun = run
    }

    private func runOnServer() async {
        if dirty || draft?.id.isEmpty != false {
            guard await save() else { return }
        }
        guard let current = draft, busy.isEmpty else { return }
        busy = "server"
        defer { busy = "" }
        do {
            let run = try await store.runOnServer(current)
            await refreshRuns()
            openedRun = run
        } catch {
            show(WorkflowsText.describe(error, "ForkMesh could not run this workflow."), error: true)
        }
    }

    private func open(_ run: WorkflowRun) {
        if run.hasDetail || run.place == .phone {
            openedRun = run
            return
        }
        guard let id = draft?.id else { return }
        Task {
            do {
                openedRun = try await store.run(id, run.id)
            } catch {
                show(WorkflowsText.describe(error, "This run could not be opened."), error: true)
            }
        }
    }

    private func openOnWeb() {
        guard let url = Services.get(ApiService.self)?.url(HubItem.workflowsPath) else { return }
        openURL(url)
    }

    private func delete() {
        guard let id = draft?.id, !id.isEmpty else { return }
        busy = "delete"
        Task {
            defer { busy = "" }
            do {
                try await store.delete(id)
                dismiss()
            } catch {
                show(WorkflowsText.describe(error, "This workflow could not be deleted."), error: true)
            }
        }
    }
}
