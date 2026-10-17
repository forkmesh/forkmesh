import FMApi
import FMBase
import FMWorkflows
import SwiftUI

enum WorkflowOpening: Identifiable, Hashable {
    case existing(String)
    case draft(Int)

    var id: String {
        switch self {
        case let .existing(id): return id
        case let .draft(serial): return "draft-\(serial)"
        }
    }
}

struct WorkflowsScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var store: WorkflowsStore?

    init() {
        if let api = Services.get(ApiService.self) {
            let secure: SecureStore = Services.get(SecureStore.self) ?? InMemorySecureStore()
            _store = State(initialValue: WorkflowsStore(api: api, secureStore: secure))
        } else {
            _store = State(initialValue: nil)
        }
    }

    var body: some View {
        content
            .navigationTitle(WorkflowsText.title)
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
    }

    @ViewBuilder private var content: some View {
        if let store = store {
            WorkflowsList(store: store)
                .task { await store.load() }
        } else {
            FmEmptyState(icon: "account_tree_outlined", title: WorkflowsText.unavailableTitle)
        }
    }
}

private struct WorkflowsList: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var store: WorkflowsStore
    @State private var opening: WorkflowOpening?
    @State private var drafts: [Int: WorkflowDraft] = [:]
    @State private var serial: Int = 0
    @State private var creating: Bool = false
    @State private var credentialsOpen: Bool = false

    var body: some View {
        main
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    HStack(spacing: 0) {
                        FmIconButton(icon: "lock_outline", label: "Credentials", onPressed: { credentialsOpen = true })
                        FmIconButton(icon: "refresh", label: "Refresh", onPressed: { Task { await store.load() } })
                    }
                }
            }
            .safeAreaInset(edge: .bottom, alignment: .trailing, spacing: 0) {
                FmFloatingActionButton(icon: "add", label: "New workflow", accessibilityLabel: "New workflow", onPressed: { creating = true })
                    .padding(FmSpace.x4)
                    .accessibilityIdentifier("workflows-new")
            }
            .sheet(isPresented: $creating) {
                WorkflowNewSheet(store: store) { (draft: WorkflowDraft) in
                    creating = false
                    serial += 1
                    drafts[serial] = draft
                    opening = .draft(serial)
                }
                .fmSheetStyle(fitsContent: false)
            }
            .navigationDestination(item: $opening) { (target: WorkflowOpening) in
                editor(target)
                    .shellPageClearance()
            }
            .navigationDestination(isPresented: $credentialsOpen) {
                WorkflowCredentialsScreen(store: store)
                    .shellPageClearance()
            }
    }

    @ViewBuilder private func editor(_ target: WorkflowOpening) -> some View {
        switch target {
        case let .existing(id):
            WorkflowEditorScreen(store: store, workflowId: id, draft: nil)
        case let .draft(serial):
            WorkflowEditorScreen(store: store, workflowId: nil, draft: drafts[serial] ?? .blank)
        }
    }

    @ViewBuilder private var main: some View {
        if store.showsSpinner {
            FmSkeletonList()
        } else if store.showsError {
            ScrollView {
                FmEmptyState(icon: "error_outline", title: WorkflowsText.unavailableTitle, message: store.error)
                    .containerRelativeFrame([.horizontal, .vertical])
            }
            .fmRefreshable { await store.load() }
        } else if store.workflows.isEmpty {
            ScrollView {
                FmEmptyState(icon: "account_tree_outlined", title: WorkflowsText.emptyTitle, message: WorkflowsText.emptyMessage)
                    .containerRelativeFrame([.horizontal, .vertical])
            }
            .fmRefreshable { await store.load() }
        } else {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    FmNoticeLine(store.error.isEmpty ? nil : store.error, isError: true)
                    ForEach(store.workflows) { (workflow: WorkflowSummary) in
                        FmListRow(
                            title: workflow.name,
                            subtitle: subtitle(workflow),
                            leading: AnyView(FmIconTile(icon: WorkflowLook.triggerIcon(workflow.trigger), tint: tint(workflow))),
                            trailing: workflow.active ? AnyView(FmStatusBadge(label: "Active", color: fm.accent)) : nil,
                            onTap: { opening = .existing(workflow.id) })
                            .accessibilityIdentifier("workflow-\(workflow.id)")
                        FmRowDivider()
                    }
                }
                .padding(.bottom, FmSpace.x8)
            }
            .fmRefreshable { await store.load() }
        }
    }

    private func subtitle(_ workflow: WorkflowSummary) -> String {
        var parts = [WorkflowsText.trigger(workflow.trigger)]
        if workflow.scheduleMinutes > 0 { parts.append(WorkflowsText.every(workflow.scheduleMinutes)) }
        parts.append(workflow.steps == 1 ? "1 step" : "\(workflow.steps) steps")
        if workflow.lastRunAt > 0 {
            parts.append("\(WorkflowsText.status(workflow.lastStatus)) \(Format.relativeTime(workflow.lastRunAt))")
        }
        return parts.joined(separator: " · ")
    }

    private func tint(_ workflow: WorkflowSummary) -> Color {
        switch workflow.lastStatus {
        case "success": return fm.success
        case "error": return fm.danger
        default: return fm.accent
        }
    }
}

enum WorkflowLook {
    static func triggerIcon(_ type: String) -> String {
        switch type {
        case "core.webhook": return "webhook"
        case "core.schedule": return "schedule"
        case "core.manual": return "touch_app"
        default: return "account_tree_outlined"
        }
    }

    static func stepIcon(_ type: String) -> String {
        switch type {
        case "core.manual", "core.webhook", "core.schedule": return triggerIcon(type)
        case "core.http": return "http"
        case "core.set": return "edit"
        case "core.if": return "call_split"
        case "core.filter": return "filter_alt"
        case "core.merge": return "merge_type"
        case "core.limit", "core.split": return "format_list_bulleted"
        case "core.noop": return "radio_button_unchecked"
        default: return "bolt"
        }
    }

    static func color(_ hex: String, fallback: Color) -> Color {
        let digits = hex.hasPrefix("#") ? String(hex.dropFirst()) : hex
        guard digits.count == 6, let value = UInt32(digits, radix: 16) else { return fallback }
        return Color(
            red: Double((value >> 16) & 0xff) / 255, green: Double((value >> 8) & 0xff) / 255, blue: Double(value & 0xff) / 255)
    }

    static func statusIcon(_ status: String) -> String {
        switch status {
        case "success": return "check_circle"
        case "error": return "error_outline"
        default: return "radio_button_unchecked"
        }
    }
}

private struct WorkflowNewSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let store: WorkflowsStore
    let onOpen: (WorkflowDraft) -> Void
    @State private var error: String = ""
    @State private var loadingKey: String = ""

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 0) {
                    FmNoticeLine(error.isEmpty ? nil : error, isError: true, onDismiss: { error = "" })
                    FmListRow(
                        title: "Blank workflow", subtitle: "A manual trigger and nothing else",
                        leading: AnyView(FmIconTile(icon: "add", tint: fm.accent)), onTap: { onOpen(.blank) })
                    FmSectionHeader(title: "Examples", count: store.templates.isEmpty ? nil : store.templates.count)
                        .padding(.horizontal, FmSpace.x4)
                        .padding(.top, FmSpace.x3)
                    ForEach(store.templates) { (template: WorkflowTemplate) in
                        FmListRow(
                            title: template.name,
                            subtitle: template.description,
                            leading: AnyView(FmIconTile(icon: "account_tree_outlined")),
                            trailing: loadingKey == template.key ? AnyView(FmCircularProgressIndicator(size: 20, strokeWidth: 2)) : nil,
                            onTap: { open(template) })
                        FmRowDivider()
                    }
                }
                .padding(.bottom, FmSpace.x6)
            }
            .navigationTitle("New workflow")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    FmTextButton("Cancel", onPressed: { dismiss() })
                }
            }
            .task {
                let failure = await store.loadTemplates()
                if !failure.isEmpty { error = failure }
            }
        }
    }

    private func open(_ template: WorkflowTemplate) {
        guard loadingKey.isEmpty else { return }
        loadingKey = template.key
        Task {
            defer { loadingKey = "" }
            do {
                onOpen(try await store.draft(fromTemplate: template.key))
            } catch {
                self.error = WorkflowsText.describe(error, "This example could not be opened.")
            }
        }
    }
}
