import FMBase
import FMWorkflows
import SwiftUI

struct WorkflowStepScreen: View {
    @Environment(\.fmPalette) private var fm
    let store: WorkflowsStore
    @Binding var draft: WorkflowDraft
    let nodeId: String
    let onDelete: () -> Void
    @State private var nameText: String = ""
    @State private var nameError: String = ""
    @State private var addingCredential: Bool = false

    var body: some View {
        content
            .navigationTitle(node?.name ?? "Step")
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
            .onAppear { nameText = node?.name ?? "" }
            .sheet(isPresented: $addingCredential) {
                WorkflowCredentialForm(store: store, integration: kind?.appKey ?? "", credential: nil) { (created: WorkflowCredential?) in
                    addingCredential = false
                    if let created { update { $0.credential = created.id } }
                }
                .fmSheetStyle(fitsContent: false)
            }
    }

    private var node: WorkflowNode? { draft.node(nodeId) }
    private var kind: WorkflowNodeKind? { node.flatMap { store.catalog.kind($0.type) } }

    @ViewBuilder private var content: some View {
        if let node = node {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    header(node)
                    FmField(label: "Name") {
                        FmTextField(text: $nameText, hint: "Step name", onSubmit: { rename() })
                    }
                    .onChange(of: nameText) { (_: String, _: String) in rename() }
                    FmNoticeLine(nameError.isEmpty ? nil : nameError, isError: true)
                    if node.isTrigger { triggerPicker(node) }
                    if let kind, kind.isApp { appSection(node, kind) }
                    fields(node)
                    if !node.isTrigger { options(node) }
                    if !node.isTrigger {
                        FmOutlinedButton("Delete step", icon: "delete_outline", foreground: fm.danger, fillWidth: true, onPressed: onDelete)
                            .padding(.top, FmSpace.x2)
                    }
                }
                .padding(FmSpace.x4)
                .padding(.bottom, FmSpace.x8)
            }
            .scrollDismissesKeyboard(.interactively)
        } else {
            FmEmptyState(icon: "error_outline", title: "This step was removed")
        }
    }

    private func header(_ node: WorkflowNode) -> some View {
        HStack(spacing: FmSpace.x3) {
            FmIconTile(icon: WorkflowLook.stepIcon(node.type), tint: WorkflowLook.color(kind?.color ?? "", fallback: fm.accent))
            VStack(alignment: .leading, spacing: 2) {
                Text(kind?.name ?? node.type).fmText(.titleMedium)
                if let description = kind?.description, !description.isEmpty {
                    Text(description)
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private func triggerPicker(_ node: WorkflowNode) -> some View {
        FmField(label: "Trigger") {
            Picker("Trigger", selection: Binding(get: { node.type }, set: { (type: String) in
                if let kind = store.catalog.kind(type) { draft.setTrigger(kind) }
            })) {
                ForEach(store.catalog.triggers) { (kind: WorkflowNodeKind) in
                    Text(kind.name).tag(kind.type)
                }
            }
            .pickerStyle(.segmented)
        }
    }

    @ViewBuilder private func appSection(_ node: WorkflowNode, _ kind: WorkflowNodeKind) -> some View {
        FmField(label: "Operation") {
            Picker("Operation", selection: Binding(get: { node.operation.isEmpty ? (kind.operations.first?.key ?? "") : node.operation },
                                                   set: { (operation: String) in update { $0.params["operation"] = .string(operation) } })) {
                ForEach(kind.operations) { (operation: WorkflowOperation) in
                    Text(operation.name).tag(operation.key)
                }
            }
            .pickerStyle(.menu)
            .tint(fm.accent)
        }
        if kind.needsCredential {
            let choices = store.credentials(for: kind.appKey)
            FmField(label: "Credential") {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    Picker("Credential", selection: Binding(get: { node.credential }, set: { (id: String) in update { $0.credential = id } })) {
                        Text("Choose a credential").tag("")
                        ForEach(choices) { (credential: WorkflowCredential) in
                            Text(credential.name.isEmpty ? kind.name : credential.name).tag(credential.id)
                        }
                    }
                    .pickerStyle(.menu)
                    .tint(fm.accent)
                    HStack(spacing: FmSpace.x2) {
                        if !node.credential.isEmpty {
                            FmStatusBadge(
                                label: store.phoneCredentialIds.contains(node.credential) ? "On this phone" : "Not on this phone",
                                color: store.phoneCredentialIds.contains(node.credential) ? fm.success : fm.warning)
                        }
                        Spacer(minLength: 0)
                        FmTextButton("New credential", icon: "add", onPressed: { addingCredential = true })
                    }
                }
            }
        }
    }

    @ViewBuilder private func fields(_ node: WorkflowNode) -> some View {
        let list = kind?.fields(operation: node.operation) ?? []
        ForEach(list) { (field: WorkflowField) in
            FmField(label: field.required ? field.label + " *" : field.label) {
                editor(field, node)
            }
        }
        if !list.isEmpty {
            Text("Use {{ $json.field }} for the item's values, {{ $node[\"Step\"].json.field }} for another step's and {{ $trigger.field }} for the trigger's.")
                .fmText(.bodySmall, color: fm.textSecondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    @ViewBuilder private func editor(_ field: WorkflowField, _ node: WorkflowNode) -> some View {
        let text = Binding<String>(
            get: { draft.node(nodeId).map { $0.params[field.key] == nil ? field.defaultValue : $0.text(field.key) } ?? "" },
            set: { (value: String) in update { $0.params[field.key] = .string(value) } })
        switch field.type {
        case "boolean":
            Toggle(field.label, isOn: Binding(
                get: { ["true", "1", "yes", "on"].contains(text.wrappedValue.lowercased()) },
                set: { text.wrappedValue = $0 ? "true" : "false" }))
                .tint(fm.accent)
        case "select":
            Picker(field.label, selection: text) {
                ForEach(field.options.isEmpty && field.key == "operator" ? store.catalog.operators : field.options, id: \.value) {
                    (option: WorkflowField.Option) in
                    Text(option.label).tag(option.value)
                }
            }
            .pickerStyle(.menu)
            .tint(fm.accent)
        default:
            FmTextField(text: text, hint: field.placeholder, multiline: field.multiline, mono: field.mono)
        }
    }

    private func options(_ node: WorkflowNode) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Toggle("Turned off (passes items through)", isOn: Binding(get: { node.disabled }, set: { (on: Bool) in update { $0.disabled = on } }))
                .tint(fm.accent)
            Toggle("Continue when this step fails", isOn: Binding(get: { node.continueOnFail },
                                                                 set: { (on: Bool) in update { $0.continueOnFail = on } }))
                .tint(fm.accent)
        }
    }

    private func update(_ change: (inout WorkflowNode) -> Void) {
        guard var step = draft.node(nodeId) else { return }
        change(&step)
        draft.update(step)
    }

    private func rename() {
        let name = nameText.trimmingCharacters(in: .whitespacesAndNewlines)
        if name.isEmpty {
            nameError = "Every step needs a name."
        } else if draft.nameTaken(name, except: nodeId) {
            nameError = "Another step is already called \(name)."
        } else {
            nameError = ""
            update { $0.name = String(name.prefix(60)) }
        }
    }
}

struct WorkflowStepPicker: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let catalog: WorkflowCatalog
    let outputs: [String]
    let onPick: (WorkflowNodeKind, String) -> Void
    @State private var query: String = ""
    @State private var output: String = ""

    var body: some View {
        NavigationStack {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    VStack(alignment: .leading, spacing: FmSpace.x2) {
                        FmTextField(text: $query, hint: "Search steps and integrations")
                        if outputs.count > 1 {
                            Picker("Branch", selection: $output) {
                                ForEach(outputs, id: \.self) { (name: String) in
                                    Text(name.capitalized).tag(name)
                                }
                            }
                            .pickerStyle(.segmented)
                        }
                    }
                    .padding(FmSpace.x4)
                    ForEach(groups) { (group: WorkflowPickerGroup) in
                        FmGroupHeader(label: group.title, count: group.kinds.count)
                        ForEach(group.kinds) { (kind: WorkflowNodeKind) in
                            FmListRow(
                                title: kind.name,
                                subtitle: kind.description,
                                leading: AnyView(FmIconTile(icon: WorkflowLook.stepIcon(kind.type),
                                                            tint: WorkflowLook.color(kind.color, fallback: fm.accent))),
                                onTap: { onPick(kind, output.isEmpty ? (outputs.first ?? "main") : output) })
                            FmRowDivider()
                        }
                    }
                }
                .padding(.bottom, FmSpace.x6)
            }
            .navigationTitle("Add a step")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    FmTextButton("Cancel", onPressed: { dismiss() })
                }
            }
            .onAppear { if output.isEmpty { output = outputs.first ?? "main" } }
        }
    }

    private var groups: [WorkflowPickerGroup] {
        let found = catalog.search(query)
        var out: [WorkflowPickerGroup] = []
        let core = found.filter { !$0.isApp }
        if !core.isEmpty { out.append(WorkflowPickerGroup(title: "Core", kinds: core)) }
        for category in catalog.categories {
            let apps = found.filter { $0.isApp && $0.group == category }
            if !apps.isEmpty { out.append(WorkflowPickerGroup(title: category, kinds: apps)) }
        }
        return out
    }
}

struct WorkflowPickerGroup: Identifiable {
    var title: String
    var kinds: [WorkflowNodeKind]

    var id: String { title }
}
