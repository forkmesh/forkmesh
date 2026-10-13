import FMBase
import FMChat
import FMControl
import FMCoves
import SwiftUI

@MainActor
final class SecretsHubChannel: SecretsControlCalling {
    private let hub: DesktopControlHub

    init(hub: DesktopControlHub) {
        self.hub = hub
    }

    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        do {
            return try await hub.request(method, params, timeout: timeout)
        } catch let error as LocalControlException {
            throw ActionsSecretError.desktop(error.code)
        }
    }
}

@MainActor
enum SecretsFeature {
    static var model: ActionsSecretsModel? {
        if let existing = Services.get(ActionsSecretsModel.self) { return existing }
        guard let hub = HostsFeature.hub else { return nil }
        let model = ActionsSecretsModel(client: ActionsSecretsClient(channel: SecretsHubChannel(hub: hub)))
        Services.put(model)
        return model
    }
}

struct ActionsSecretsScreen: View {
    var body: some View {
        if let model = SecretsFeature.model, let hub = HostsFeature.hub {
            ActionsSecretsList(model: model, hub: hub)
        } else {
            ContentUnavailableView {
                Label("Desktop not connected", systemImage: "desktopcomputer")
            } description: {
                Text("Actions variables and secrets live on your desktop. Pair this phone with it to manage them.")
            } actions: {
                FmMessageTools("Desktop not connected. Actions variables and secrets live on your desktop. Pair this phone with it to manage them.")
            }
                .navigationTitle("Actions secrets")
        }
    }
}

private struct ActionsSecretsList: View {
    @Bindable var model: ActionsSecretsModel
    @Bindable var hub: DesktopControlHub
    @State private var editing: SecretDraft?
    @State private var deleting: ActionsSecretRow?

    struct SecretDraft: Identifiable {
        let id = UUID()
        var name: String
        let existing: Bool
    }

    var body: some View {
        List {
            Section {
                DesktopReachLine(reach: hub.reach)
            }
            if let error = model.error {
                Section {
                    Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(error)
                    Button("Retry") { Task { await model.load() } }
                }
            }
            Section {
                ForEach(model.rows) { row in
                    HStack {
                        Image(systemName: "key.horizontal").foregroundStyle(.secondary)
                        Text(row.name).font(.system(.body, design: .monospaced))
                        Spacer()
                        Text("\u{2022}\u{2022}\u{2022}\u{2022}\u{2022}\u{2022}").foregroundStyle(.secondary).accessibilityLabel("Value hidden")
                    }
                    .contentShape(Rectangle())
                    .onTapGesture { editing = SecretDraft(name: row.name, existing: true) }
                    .swipeActions {
                        Button("Delete", role: .destructive) { deleting = row }
                        Button("Replace") { editing = SecretDraft(name: row.name, existing: true) }
                    }
                    .accessibilityAction(named: "Replace value") { editing = SecretDraft(name: row.name, existing: true) }
                    .accessibilityAction(named: "Delete") { deleting = row }
                }
            } header: {
                Text("Variables and secrets")
            } footer: {
                Text("Values are write-only from the phone: you can replace or delete one, never read it. Every change asks the desktop for a one-time confirmation.")
            }
        }
        .overlay {
            if model.loading && model.rows.isEmpty {
                ProgressView()
            } else if !model.loading && model.rows.isEmpty && model.error == nil {
                ContentUnavailableView {
                    Label("No variables yet", systemImage: "key")
                } description: {
                    Text("Add one to make it available to every action run on your desktop.")
                } actions: {
                    Button("Add variable") { editing = SecretDraft(name: "", existing: false) }.buttonStyle(.borderedProminent)
                }
            }
        }
        .navigationTitle("Actions secrets")
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button { editing = SecretDraft(name: "", existing: false) } label: { Label("Add variable", systemImage: "plus") }
                    .disabled(model.busy)
            }
        }
        .task { await model.load() }
        .fmRefreshable { await model.load() }
        .sheet(item: $editing) { draft in
            SecretEditor(model: model, draft: draft) { editing = nil }
        }
        .confirmationDialog(
            "Delete \(deleting?.name ?? "")?", isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } }),
            titleVisibility: .visible
        ) {
            Button("Delete", role: .destructive) {
                if let row = deleting {
                    Task {
                        if await CovesFeature.authenticate(reason: "Delete \(row.name) from your desktop") {
                            _ = await model.delete(name: row.name)
                        }
                    }
                }
                deleting = nil
            }
        } message: {
            Text("Actions that use it will no longer receive it.")
        }
    }
}

private struct SecretEditor: View {
    let model: ActionsSecretsModel
    let draft: ActionsSecretsList.SecretDraft
    let onDone: () -> Void
    @State private var name: String
    @State private var value = ""
    @State private var working = false
    @State private var problem: String?

    init(model: ActionsSecretsModel, draft: ActionsSecretsList.SecretDraft, onDone: @escaping () -> Void) {
        self.model = model
        self.draft = draft
        self.onDone = onDone
        _name = State(initialValue: draft.name)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Name, e.g. CLOUDFLARE_API_TOKEN", text: $name)
                        .font(.system(.body, design: .monospaced))
                        .textInputAutocapitalization(.characters)
                        .autocorrectionDisabled()
                        .disabled(draft.existing)
                    SecureField(draft.existing ? "New value" : "Value", text: $value)
                } footer: {
                    Text(problem ?? "Letters, digits and underscores. The value goes to your desktop encrypted end to end and is not kept on this phone.")
                        .foregroundStyle(problem == nil ? Color.secondary : Color.red)
                        .fmMessageTools(problem)
                }
            }
            .navigationTitle(draft.existing ? "Replace value" : "Add variable")
            .navigationBarTitleDisplayMode(.inline)
            .interactiveDismissDisabled(working)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel", action: onDone).disabled(working) }
                ToolbarItem(placement: .confirmationAction) {
                    if working {
                        ProgressView()
                    } else {
                        Button("Save") { Task { await save() } }
                            .disabled(!ActionsSecrets.validName(name.trimmingCharacters(in: .whitespacesAndNewlines)))
                    }
                }
            }
        }
        .onDisappear { value = "" }
    }

    private func save() async {
        working = true
        defer { working = false }
        let ok = await model.save(name: name, value: value)
        if ok {
            value = ""
            FmHaptics.commit()
            onDone()
        } else {
            problem = model.error
        }
    }
}
