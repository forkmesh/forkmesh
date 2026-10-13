import FMApi
import FMBase
import FMCode
import SwiftUI

struct ProviderMirrorSections: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL
    let repo: RepoRef
    let canEdit: Bool
    @State private var model: ProviderMirrorModel
    @State private var draft = ProviderMirrorDraft()
    @State private var editing = false
    @State private var confirmDisconnect = false
    @State private var picking = false

    init(repo: RepoRef, canEdit: Bool) {
        self.repo = repo
        self.canEdit = canEdit
        _model = State(initialValue: ProviderMirrorModel(client: ReviewEnv.api, repo: repo))
    }

    private var now: Int { Int(Date().timeIntervalSince1970 * 1000) }

    private var providerLabel: String { model.provider(draft.provider)?.label ?? "GitHub" }

    var body: some View {
        Group {
            if !canEdit {
                Section {
                    Text("Only the repository owner can mirror it to GitHub or GitLab.").foregroundStyle(fm.textSecondary)
                }
            } else if !model.loaded {
                Section {
                    if let error = model.error {
                        Text(error).foregroundStyle(fm.danger)
                    } else {
                        ProgressView()
                    }
                }
            } else {
                messages
                if let mirror = model.mirror, !editing {
                    overview(mirror)
                } else {
                    form
                }
            }
        }
        .task {
            if canEdit && !model.loaded {
                await model.load()
                resetDraft()
            }
        }
        .sheet(isPresented: $picking, onDismiss: { model.closeCandidates() }) {
            ProviderMirrorPicker(model: model)
        }
        .confirmationDialog(
            "Disconnect from \(model.mirror?.providerLabel ?? "the provider")?",
            isPresented: $confirmDisconnect, titleVisibility: .visible
        ) {
            Button("Disconnect", role: .destructive) {
                Task {
                    await model.disconnect()
                    resetDraft()
                }
            }
        } message: {
            Text("The stored token is deleted and nothing more is mirrored. Nothing already copied is removed on either side.")
        }
    }

    @ViewBuilder private var messages: some View {
        if model.error != nil || model.notice != nil {
            Section {
                if let error = model.error {
                    Text(error).foregroundStyle(fm.danger).font(.footnote)
                }
                if let notice = model.notice {
                    Text(notice).foregroundStyle(fm.textSecondary).font(.footnote)
                }
            }
        }
    }

    private func resetDraft() {
        if let mirror = model.mirror {
            draft = ProviderMirrorDraft(state: mirror)
        } else {
            draft = ProviderMirrorDraft(provider: draft.provider)
            if draft.remote.isEmpty { draft.remote = "" }
        }
    }

    @ViewBuilder private func overview(_ mirror: ProviderMirrorState) -> some View {
        Section {
            VStack(alignment: .leading, spacing: 4) {
                Label(mirror.remote, systemImage: "arrow.left.arrow.right.circle.fill")
                    .font(.headline)
                Text(mirror.connectionError.isEmpty
                     ? "\(mirror.providerLabel) as @\(mirror.login.isEmpty ? "unknown" : mirror.login)\(mirror.remotePrivate ? " · private" : "")\(mirror.canPush ? "" : " · token cannot push")"
                     : mirror.connectionError)
                    .font(.footnote)
                    .foregroundStyle(mirror.connectionError.isEmpty ? fm.textSecondary : fm.danger)
            }
            if let url = URL(string: mirror.remoteUrl), !mirror.remoteUrl.isEmpty {
                Button {
                    openURL(url)
                } label: {
                    Label("Open on \(mirror.providerLabel)", systemImage: "safari")
                }
            }
            Toggle("Mirroring", isOn: Binding(get: { mirror.active }, set: { value in Task { await model.setActive(value) } }))
                .disabled(model.busy)
        } header: {
            Text("Connected")
        }
        Section("What is mirrored") {
            ForEach(model.resources.filter { $0.supported(by: mirror.provider) }) { (resource: ProviderMirrorResource) in
                let setting = mirror.setting(resource.key)
                HStack {
                    Text(resource.title(for: mirror.provider))
                    Spacer()
                    Text(ProviderMirrorText.direction(setting.direction, provider: mirror.providerLabel)
                         + (setting.manual ? " · picked" : ""))
                        .font(.footnote)
                        .foregroundStyle(setting.direction == "off" ? fm.textTertiary : fm.textSecondary)
                }
            }
        }
        Section("Status") {
            statusRow("Code", ProviderMirrorText.codeLine(mirror, hosted: model.codeHosted, now: now))
            statusRow("Import", ProviderMirrorText.syncLine(mirror, now: now))
            if model.pending > 0 {
                Text("\(model.pending) item\(model.pending == 1 ? "" : "s") waiting to be sent to \(mirror.providerLabel).")
                    .font(.footnote).foregroundStyle(fm.textSecondary)
            }
        }
        Section {
            Button {
                Task { await model.sync() }
            } label: {
                Label(model.busy ? "Working…" : "Sync now", systemImage: "arrow.triangle.2.circlepath")
            }
            .disabled(model.busy)
            ForEach(model.importKinds) { (resource: ProviderMirrorResource) in
                Button {
                    picking = true
                    Task { await model.openCandidates(resource.key) }
                } label: {
                    Label("Pick \(resource.title(for: mirror.provider).lowercased()) to import", systemImage: "checklist")
                }
            }
            Button {
                draft = ProviderMirrorDraft(state: mirror)
                editing = true
            } label: {
                Label("Change settings", systemImage: "slider.horizontal.3")
            }
            Button {
                Task { await model.verify() }
            } label: {
                Label("Check connection", systemImage: "checkmark.shield")
            }
            .disabled(model.busy)
            Button(role: .destructive) {
                confirmDisconnect = true
            } label: {
                Label("Disconnect", systemImage: "xmark.circle")
            }
        }
        if !mirror.history.isEmpty {
            Section("Recent activity") {
                ForEach(Array(mirror.history.prefix(8).indices), id: \.self) { (index: Int) in
                    let entry = mirror.history[index]
                    HStack(alignment: .firstTextBaseline) {
                        Image(systemName: entry.ok ? "checkmark.circle.fill" : "exclamationmark.circle.fill")
                            .foregroundStyle(entry.ok ? fm.success : fm.danger)
                        Text(entry.message.isEmpty ? entry.kind : entry.message).font(.footnote)
                        Spacer()
                        Text(ProviderMirrorText.ago(entry.atMs, now: now)).font(.caption2).foregroundStyle(fm.textTertiary)
                    }
                }
            }
        }
    }

    private func statusRow(_ title: String, _ line: (ok: Bool?, text: String)) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Circle()
                .fill(line.ok == true ? fm.success : line.ok == false ? fm.danger : fm.textTertiary)
                .frame(width: 8, height: 8)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.subheadline.weight(.semibold))
                Text(line.text).font(.footnote).foregroundStyle(fm.textSecondary)
            }
        }
    }

    @ViewBuilder private var form: some View {
        Section {
            Picker("Mirror to", selection: $draft.provider) {
                ForEach(model.providers) { (provider: ProviderMirrorProvider) in
                    Text(provider.label).tag(provider.key)
                }
            }
            .pickerStyle(.segmented)
        } header: {
            Text("Mirror to")
        } footer: {
            Text("Keep a copy of this repository on GitHub or GitLab with your own access token, and choose for each part whether it is sent from ForkMesh, brought in, both, or neither.")
        }
        Section {
            HStack(spacing: 2) {
                Text("\(model.provider(draft.provider)?.host ?? "github.com")/")
                    .foregroundStyle(fm.textSecondary)
                    .font(.body.monospaced())
                TextField("owner/name", text: $draft.remote)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .font(.body.monospaced())
            }
            SecureField(
                model.mirror?.provider == draft.provider && !(model.mirror?.tokenHint.isEmpty ?? true)
                    ? "Saved (\(model.mirror?.tokenHint ?? "")); leave blank to keep"
                    : "Personal access token",
                text: $draft.token)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            if let provider = model.provider(draft.provider), let url = URL(string: provider.tokenUrl) {
                Button {
                    openURL(url)
                } label: {
                    Label("Create a token on \(provider.label)", systemImage: "key")
                }
            }
            Toggle("Create it on \(providerLabel) if it does not exist\(model.isPrivate ? " (private)" : "")", isOn: $draft.createRemote)
        } header: {
            Text("\(providerLabel) repository")
        } footer: {
            Text("\(model.provider(draft.provider)?.tokenHelp ?? "") The token is stored encrypted and never shown again.")
        }
        Section {
            ForEach(model.resources) { (resource: ProviderMirrorResource) in
                resourceRow(resource)
            }
        } header: {
            Text("What to mirror")
        } footer: {
            Text("Items brought in from \(providerLabel) keep a link to the original and are never sent back. Imported pull requests arrive as issues labelled pull-request.")
        }
        Section {
            Button {
                Task {
                    if await model.save(draft, description: "") {
                        editing = false
                        resetDraft()
                        FmHaptics.commit()
                    }
                }
            } label: {
                Text(model.busy ? "Checking the token…" : model.mirror == nil ? "Connect \(providerLabel)" : "Save mirror")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(model.busy)
            if model.mirror != nil {
                Button("Cancel") {
                    editing = false
                    resetDraft()
                }
            }
        }
    }

    @ViewBuilder private func resourceRow(_ resource: ProviderMirrorResource) -> some View {
        let supported = resource.supported(by: draft.provider)
        let setting = draft.setting(resource)
        VStack(alignment: .leading, spacing: 6) {
            Text(resource.title(for: draft.provider)).font(.subheadline.weight(.semibold))
            Text(supported ? resource.hint : "\(providerLabel) has no \(resource.title(for: draft.provider).lowercased()).")
                .font(.footnote)
                .foregroundStyle(fm.textSecondary)
            if supported {
                Picker("Direction", selection: binding(resource, \.direction)) {
                    ForEach(resource.directions, id: \.self) { (direction: String) in
                        Text(ProviderMirrorText.direction(direction, provider: providerLabel)).tag(direction)
                    }
                }
                if resource.canImport && setting.imports {
                    Picker("Import", selection: binding(resource, \.mode)) {
                        Text("Automatically").tag("auto")
                        Text("I pick what to import").tag("manual")
                    }
                }
                if resource.canImport && setting.direction != "off" {
                    Toggle("Comments", isOn: binding(resource, \.comments))
                }
                if resource.key == "code" && setting.direction == "push" {
                    Toggle("Delete branches there that no longer exist here", isOn: $draft.prune)
                    if !model.codeHosted {
                        Text("Code is pushed by the ForkMesh server that holds this repository; this one is served from its owner's node.")
                            .font(.caption)
                            .foregroundStyle(fm.textTertiary)
                    }
                }
            }
        }
        .padding(.vertical, 2)
        .opacity(supported ? 1 : 0.5)
    }

    private func binding<Value>(_ resource: ProviderMirrorResource, _ path: WritableKeyPath<ProviderMirrorSetting, Value>) -> Binding<Value> {
        Binding(
            get: { draft.setting(resource)[keyPath: path] },
            set: { value in
                var setting = draft.setting(resource)
                setting[keyPath: path] = value
                draft.resources[resource.key] = setting
            })
    }
}

struct ProviderMirrorPicker: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL
    let model: ProviderMirrorModel

    private var title: String {
        let resource = model.resources.first { $0.key == model.candidateKind }
        return "Pick \(resource?.title(for: model.mirror?.provider ?? "github").lowercased() ?? "items")"
    }

    var body: some View {
        NavigationStack {
            List {
                if model.candidatesLoading {
                    ProgressView()
                } else if model.candidates.isEmpty {
                    Text("Nothing here yet.").foregroundStyle(fm.textSecondary)
                }
                ForEach(model.candidates) { (item: ProviderMirrorCandidate) in
                    Button {
                        model.toggle(item)
                    } label: {
                        HStack(alignment: .top, spacing: 10) {
                            Image(systemName: model.selected.contains(item.key) ? "checkmark.circle.fill" : "circle")
                                .foregroundStyle(item.selectable ? fm.accent : fm.textTertiary)
                            VStack(alignment: .leading, spacing: 3) {
                                Text(item.title).foregroundStyle(item.selectable ? fm.textPrimary : fm.textTertiary)
                                Text("#\(item.number) · @\(item.author) · \(item.stateLabel)\(item.imported ? " · already imported" : item.mirrored ? " · came from ForkMesh" : "")")
                                    .font(.caption)
                                    .foregroundStyle(fm.textSecondary)
                                if !item.excerpt.isEmpty {
                                    Text(item.excerpt).font(.caption).foregroundStyle(fm.textTertiary).lineLimit(2)
                                }
                            }
                        }
                    }
                    .disabled(!item.selectable)
                    .swipeActions {
                        if let url = URL(string: item.url) {
                            Button("Open") { openURL(url) }.tint(fm.accent)
                        }
                    }
                }
                if model.candidatePage > 1 || model.candidatesMore {
                    HStack {
                        if model.candidatePage > 1 {
                            Button("Newer") {
                                Task { await model.openCandidates(model.candidateKind ?? "", page: model.candidatePage - 1) }
                            }
                        }
                        Spacer()
                        Text("Page \(model.candidatePage)").font(.caption).foregroundStyle(fm.textSecondary)
                        Spacer()
                        if model.candidatesMore {
                            Button("Older") {
                                Task { await model.openCandidates(model.candidateKind ?? "", page: model.candidatePage + 1) }
                            }
                        }
                    }
                    .buttonStyle(.borderless)
                }
                if let error = model.error {
                    Text(error).foregroundStyle(fm.danger).font(.footnote)
                }
            }
            .listStyle(.insetGrouped)
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button(model.selected.isEmpty ? "Import" : "Import \(model.selected.count)") {
                        Task {
                            if await model.importSelected() > 0 { FmHaptics.commit() }
                        }
                    }
                    .disabled(model.selected.isEmpty || model.busy)
                }
            }
        }
        .presentationDetents([.medium, .large])
    }
}
