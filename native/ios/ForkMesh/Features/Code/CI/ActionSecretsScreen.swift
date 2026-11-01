import FMApi
import FMBase
import FMCode
import SwiftUI

struct RepoActionSecretsScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoActionSecretsModel?
    @State private var editing: ActionSecretEditTarget?

    init(repo: RepoRef) {
        if let client = CIServices.client {
            _model = State(initialValue: RepoActionSecretsModel(client: client, repo: repo))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        Group {
            if let model = model {
                ActionSecretsList(model: model, editing: $editing)
                    .task { await model.load() }
                    .fmRefreshable { await model.load() }
                    .toolbar {
                        ToolbarItem(placement: .primaryAction) {
                            Button {
                                editing = ActionSecretEditTarget(name: nil)
                            } label: {
                                Image(systemName: "plus")
                            }
                            .disabled(!model.canAdd || model.busy)
                            .accessibilityLabel("Add secret")
                        }
                    }
                    .sheet(item: $editing) { (target: ActionSecretEditTarget) in
                        ActionSecretEditSheet(model: model, name: target.name)
                    }
            } else {
                FmEmptyState(icon: "key", title: "Secrets are unavailable")
            }
        }
        .background(fm.canvas)
        .navigationTitle("Secrets")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct ActionSecretEditTarget: Identifiable {
    let name: String?
    var id: String { name ?? "new" }
}

struct ActionSecretsList: View {
    @Environment(\.fmPalette) private var fm
    let model: RepoActionSecretsModel
    @Binding var editing: ActionSecretEditTarget?
    @State private var deleting: RepoActionSecret?

    var body: some View {
        List {
            if let error = model.error {
                Section {
                    Text(error).foregroundStyle(fm.danger)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(error)
                }
            }
            if let notice = model.notice {
                Section {
                    Label(notice.text, systemImage: notice.good ? "checkmark.circle" : "exclamationmark.triangle")
                        .foregroundStyle(notice.good ? fm.success : fm.danger)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            Section {
                if model.secrets.isEmpty && model.loaded && !model.loading {
                    Text("No secrets yet. Workflows that this instance's runner runs can read the secrets you add here.")
                        .foregroundStyle(fm.textSecondary)
                }
                ForEach(model.secrets) { (secret: RepoActionSecret) in
                    ActionSecretRow(secret: secret)
                        .contentShape(Rectangle())
                        .onTapGesture { editing = ActionSecretEditTarget(name: secret.name) }
                        .swipeActions(edge: .trailing) {
                            Button("Delete", role: .destructive) { deleting = secret }
                            Button("Replace") { editing = ActionSecretEditTarget(name: secret.name) }
                                .tint(fm.accent)
                        }
                        .contextMenu {
                            Button {
                                editing = ActionSecretEditTarget(name: secret.name)
                            } label: {
                                Label("Replace value", systemImage: "pencil")
                            }
                            Button(role: .destructive) {
                                deleting = secret
                            } label: {
                                Label("Delete", systemImage: "trash")
                            }
                        }
                }
            } footer: {
                if model.loaded {
                    Text("Values are write-only: once saved, nobody can read them here. Save a new value to replace one.")
                }
            }
        }
        .listStyle(.insetGrouped)
        .fmLoading(model.loading && !model.loaded)
        .confirmationDialog(
            "Delete \(deleting?.name ?? "this secret")?",
            isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } }),
            titleVisibility: .visible
        ) {
            Button("Delete", role: .destructive) {
                if let secret = deleting {
                    Task { await model.delete(secret.name) }
                }
                deleting = nil
            }
        } message: {
            Text("Workflows that use it no longer get a value.")
        }
    }
}

struct ActionSecretRow: View {
    @Environment(\.fmPalette) private var fm
    let secret: RepoActionSecret

    private var detail: String {
        var parts: [String] = []
        if secret.updatedAtMs > 0 { parts.append("Updated \(Format.relativeTime(Int64(secret.updatedAtMs)))") }
        if !secret.updatedBy.isEmpty { parts.append("by @\(secret.updatedBy)") }
        return parts.joined(separator: " ")
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Label(secret.name, systemImage: "key")
                .font(.body.monospaced())
                .lineLimit(1)
                .truncationMode(.middle)
            if !detail.isEmpty {
                Text(detail).font(.caption).foregroundStyle(fm.textSecondary)
            }
        }
        .padding(.vertical, 2)
    }
}

struct ActionSecretEditSheet: View {
    @Environment(\.dismiss) private var dismiss
    let model: RepoActionSecretsModel
    let existing: String?
    @State private var name: String
    @State private var value: String = ""
    @State private var saving: Bool = false

    init(model: RepoActionSecretsModel, name: String?) {
        self.model = model
        existing = name
        _name = State(initialValue: name ?? "")
    }

    private var problem: String? {
        RepoActionSecret.nameProblem(RepoActionSecret.normalizedName(name)) ?? RepoActionSecret.valueProblem(value)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("NAME", text: $name)
                        .font(.body.monospaced())
                        .textInputAutocapitalization(.characters)
                        .autocorrectionDisabled()
                        .disabled(existing != nil)
                } header: {
                    Text("Name")
                } footer: {
                    Text("A-Z, 0-9 and _, not starting with a digit, up to 64 characters.")
                }
                Section {
                    SecureField(existing == nil ? "Value" : "New value", text: $value)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                } header: {
                    Text("Value")
                } footer: {
                    Text("Up to 48 KiB. It is never shown again after you save it.")
                }
                if let error = model.error {
                    Section {
                        Text(error).foregroundStyle(.secondary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .fmMessageTools(error)
                    }
                }
            }
            .navigationTitle(existing == nil ? "New secret" : "Replace \(existing ?? "")")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") { save() }
                        .disabled(saving || model.busy || problem != nil)
                }
            }
        }
    }

    private func save() {
        saving = true
        Task {
            let ok: Bool = await model.save(name: name, value: value)
            saving = false
            if ok {
                value = ""
                dismiss()
            }
        }
    }
}
