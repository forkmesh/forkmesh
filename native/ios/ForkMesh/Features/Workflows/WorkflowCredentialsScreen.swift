import FMBase
import FMWorkflows
import SwiftUI

struct WorkflowCredentialsScreen: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var store: WorkflowsStore
    @State private var error: String = ""
    @State private var loading: Bool = true
    @State private var creating: Bool = false
    @State private var editing: WorkflowCredential?
    @State private var deleting: WorkflowCredential?

    var body: some View {
        main
            .navigationTitle("Credentials")
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
            .task { await reload() }
            .safeAreaInset(edge: .bottom, alignment: .trailing, spacing: 0) {
                FmFloatingActionButton(icon: "add", label: "Credential", accessibilityLabel: "New credential", onPressed: { creating = true })
                    .padding(FmSpace.x4)
            }
            .sheet(isPresented: $creating) {
                WorkflowCredentialForm(store: store, integration: "", credential: nil) { (_: WorkflowCredential?) in creating = false }
                    .fmSheetStyle(fitsContent: false)
            }
            .sheet(item: $editing) { (credential: WorkflowCredential) in
                WorkflowCredentialForm(store: store, integration: credential.integration, credential: credential) { (_: WorkflowCredential?) in
                    editing = nil
                }
                .fmSheetStyle(fitsContent: false)
            }
            .fmDialog(isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } })) {
                FmConfirmDialog(
                    title: "Delete this credential?",
                    message: "Steps that use it stop working on ForkMesh and on this phone.",
                    confirmLabel: "Delete", destructive: true,
                    onCancel: { deleting = nil },
                    onConfirm: {
                        let target = deleting
                        deleting = nil
                        if let target { delete(target) }
                    })
            }
    }

    @ViewBuilder private var main: some View {
        if loading && store.credentials.isEmpty {
            FmSkeletonList()
        } else if store.credentials.isEmpty {
            ScrollView {
                FmEmptyState(
                    icon: "lock_outline", title: error.isEmpty ? "No credentials yet" : "Credentials are unavailable",
                    message: error.isEmpty ? "A credential holds the API key or token an integration step signs in with." : error)
                    .containerRelativeFrame([.horizontal, .vertical])
            }
            .fmRefreshable { await reload() }
        } else {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    FmNoticeLine(error.isEmpty ? nil : error, isError: true, onDismiss: { error = "" })
                    Text("Your account keeps credentials encrypted and never sends their values back. To run a step on this phone, save its values here too: they stay in this phone's Keychain.")
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .fixedSize(horizontal: false, vertical: true)
                        .padding(FmSpace.x4)
                    ForEach(store.credentials) { (credential: WorkflowCredential) in
                        row(credential)
                        FmRowDivider()
                    }
                }
                .padding(.bottom, FmSpace.x8)
            }
            .fmRefreshable { await reload() }
        }
    }

    private func row(_ credential: WorkflowCredential) -> some View {
        let onPhone = store.phoneCredentialIds.contains(credential.id)
        let app = store.catalog.kind("app." + credential.integration)
        return FmListRow(
            title: credential.name.isEmpty ? (app?.name ?? credential.integration) : credential.name,
            subtitle: [app?.name ?? credential.integration, credential.hintText].filter { !$0.isEmpty }.joined(separator: " · "),
            leading: AnyView(FmIconTile(icon: "lock_outline", tint: WorkflowLook.color(app?.color ?? "", fallback: fm.accent))),
            trailing: AnyView(FmStatusBadge(label: onPhone ? "On this phone" : "Account only", color: onPhone ? fm.success : fm.textSecondary)),
            onTap: { editing = credential })
            .contextMenu {
                if onPhone {
                    Button("Remove from this phone") { store.removePhoneCredential(credential.id) }
                } else {
                    Button("Save on this phone") { editing = credential }
                }
                Button("Delete credential", role: .destructive) { deleting = credential }
            }
    }

    private func reload() async {
        loading = true
        if store.catalog.core.isEmpty { await store.loadCatalog() }
        error = await store.loadCredentials()
        loading = false
    }

    private func delete(_ credential: WorkflowCredential) {
        Task {
            do {
                try await store.deleteCredential(credential.id)
            } catch {
                self.error = WorkflowsText.describe(error, "This credential could not be deleted.")
            }
        }
    }
}

struct WorkflowCredentialForm: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let store: WorkflowsStore
    let credential: WorkflowCredential?
    let onDone: (WorkflowCredential?) -> Void
    @State private var integration: String
    @State private var name: String = ""
    @State private var values: [String: String] = [:]
    @State private var keepOnPhone: Bool = true
    @State private var error: String = ""
    @State private var saving: Bool = false

    init(store: WorkflowsStore, integration: String, credential: WorkflowCredential?, onDone: @escaping (WorkflowCredential?) -> Void) {
        self.store = store
        self.credential = credential
        self.onDone = onDone
        _integration = State(initialValue: integration)
    }

    private var apps: [WorkflowNodeKind] { store.catalog.integrations.filter(\.needsCredential) }
    private var kind: WorkflowNodeKind? { store.catalog.kind("app." + integration) }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    FmNoticeLine(error.isEmpty ? nil : error, isError: true, onDismiss: { error = "" })
                    if credential == nil {
                        FmField(label: "Integration") {
                            Picker("Integration", selection: $integration) {
                                Text("Choose an integration").tag("")
                                ForEach(apps) { (app: WorkflowNodeKind) in
                                    Text(app.name).tag(app.appKey)
                                }
                            }
                            .pickerStyle(.menu)
                            .tint(fm.accent)
                        }
                        FmField(label: "Name") {
                            FmTextField(text: $name, hint: (kind?.name ?? "Integration") + " account")
                        }
                    } else {
                        Text("Enter the values again to keep a copy in this phone's Keychain. ForkMesh never sends saved values back.")
                            .fmText(.bodySmall, color: fm.textSecondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    ForEach(kind?.credentialFields ?? []) { (field: WorkflowField) in
                        FmField(label: field.label) {
                            FmTextField(
                                text: Binding(get: { values[field.key] ?? "" }, set: { values[field.key] = $0 }),
                                hint: field.placeholder, secure: field.secret)
                        }
                    }
                    if credential == nil {
                        Toggle("Keep a copy on this phone for phone runs", isOn: $keepOnPhone)
                            .tint(fm.accent)
                    }
                    FmFilledButton(
                        saving ? "Saving…" : (credential == nil ? "Save credential" : "Save on this phone"),
                        fillWidth: true, busy: saving, onPressed: saving || kind == nil ? nil : { save() })
                    if let credential, store.phoneCredentialIds.contains(credential.id) {
                        FmOutlinedButton("Remove from this phone", foreground: fm.danger, fillWidth: true, onPressed: {
                            store.removePhoneCredential(credential.id)
                            onDone(nil)
                        })
                    }
                }
                .padding(FmSpace.x4)
            }
            .navigationTitle(credential == nil ? "New credential" : (credential?.name ?? "Credential"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    FmTextButton("Cancel", onPressed: {
                        dismiss()
                        onDone(nil)
                    })
                }
            }
            .task { if store.catalog.core.isEmpty { await store.loadCatalog() } }
        }
    }

    private func save() {
        guard let kind else { return }
        let filled = values.mapValues { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
        if let credential {
            store.savePhoneCredential(credential, values: filled)
            onDone(credential)
            return
        }
        saving = true
        Task {
            defer { saving = false }
            do {
                let created = try await store.createCredential(
                    integration: kind.appKey, name: name.trimmingCharacters(in: .whitespacesAndNewlines), values: filled,
                    keepOnPhone: keepOnPhone)
                onDone(created)
            } catch {
                self.error = WorkflowsText.describe(error, "This credential could not be saved.")
            }
        }
    }
}
