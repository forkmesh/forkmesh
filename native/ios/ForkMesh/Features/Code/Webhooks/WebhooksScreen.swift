import FMApi
import FMBase
import FMCode
import SwiftUI
import UIKit

struct RepoWebhooksScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoWebhooksModel?
    @State private var editing: WebhookEditTarget?

    init(repo: RepoRef) {
        if let client = CIServices.client {
            _model = State(initialValue: RepoWebhooksModel(client: client, repo: repo))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        Group {
            if let model = model {
                WebhooksList(model: model, editing: $editing)
                    .task { await model.load() }
                    .fmRefreshable { await model.load() }
                    .toolbar {
                        ToolbarItem(placement: .primaryAction) {
                            Button {
                                editing = WebhookEditTarget(hook: nil)
                            } label: {
                                Image(systemName: "plus")
                            }
                            .disabled(!model.canAdd)
                            .accessibilityLabel("Add webhook")
                        }
                    }
                    .sheet(item: $editing) { (target: WebhookEditTarget) in
                        WebhookEditSheet(model: model, hook: target.hook)
                    }
                    .alert(
                        "Webhook secret",
                        isPresented: Binding(get: { model.revealedSecret != nil }, set: { if !$0 { model.dismissSecret() } })
                    ) {
                        Button("Copy") {
                            UIPasteboard.general.string = model.revealedSecret?.secret
                            model.dismissSecret()
                        }
                        Button("Done", role: .cancel) { model.dismissSecret() }
                    } message: {
                        Text("Copy it now: it is shown only once.\n\n\(model.revealedSecret?.secret ?? "")")
                    }
            } else {
                FmEmptyState(icon: "webhook", title: "Webhooks are unavailable")
            }
        }
        .navigationTitle("Webhooks")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct WebhookEditTarget: Identifiable {
    let hook: RepoWebhook?
    var id: String { hook?.id ?? "new" }
}

struct WebhooksList: View {
    @Environment(\.fmPalette) private var fm
    let model: RepoWebhooksModel
    @Binding var editing: WebhookEditTarget?
    @State private var result: String = ""
    @State private var deleting: RepoWebhook?

    var body: some View {
        List {
            if let error = model.error {
                Text(error).foregroundStyle(fm.danger)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(error)
            }
            if !result.isEmpty {
                Text(result).foregroundStyle(fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(result)
            }
            if model.webhooks.isEmpty && !model.loading {
                Text("No webhooks. Add one to call a URL on a push, a merge or a release.").foregroundStyle(fm.textSecondary)
            }
            ForEach(model.webhooks) { (hook: RepoWebhook) in
                WebhookRow(hook: hook)
                    .contentShape(Rectangle())
                    .onTapGesture {
                        if !hook.managed {
                            editing = WebhookEditTarget(hook: hook)
                        }
                    }
                    .swipeActions(edge: .trailing) {
                        if !hook.managed {
                            Button("Delete", role: .destructive) { deleting = hook }
                        }
                        Button("Test") {
                            Task { result = await model.test(hook.id) }
                        }
                        .tint(fm.accent)
                    }
                    .contextMenu {
                        Button(hook.active ? "Pause" : "Resume") {
                            Task { await model.setActive(hook.id, !hook.active) }
                        }
                        Button("Rotate secret") {
                            Task { await model.rotate(hook.id) }
                        }
                        Button("Send test") {
                            Task { result = await model.test(hook.id) }
                        }
                    }
            }
        }
        .listStyle(.insetGrouped)
        .confirmationDialog(
            "Delete this webhook?",
            isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } }),
            titleVisibility: .visible
        ) {
            Button("Delete", role: .destructive) {
                if let hook = deleting {
                    Task { await model.delete(hook.id) }
                }
                deleting = nil
            }
        }
    }
}

struct WebhookRow: View {
    @Environment(\.fmPalette) private var fm
    let hook: RepoWebhook

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(hook.url).lineLimit(1).truncationMode(.middle)
                Spacer()
                if hook.managed {
                    FmStatusBadge(label: "Managed", color: fm.textSecondary)
                }
            }
            Text(hook.events.joined(separator: ", ")).font(.caption).foregroundStyle(fm.textSecondary)
            Text(hook.healthLabel)
                .font(.caption2)
                .foregroundStyle(hook.consecutiveFailures > 0 ? fm.danger : fm.textTertiary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fmMessageTools(hook.consecutiveFailures > 0 ? hook.healthLabel : nil)
        }
    }
}

struct WebhookEditSheet: View {
    @Environment(\.dismiss) private var dismiss
    let model: RepoWebhooksModel
    let hook: RepoWebhook?
    @State private var draft: RepoWebhookDraft

    init(model: RepoWebhooksModel, hook: RepoWebhook?) {
        self.model = model
        self.hook = hook
        if let hook = hook {
            _draft = State(initialValue: RepoWebhookDraft(url: hook.url, events: Set(hook.events), description: hook.description))
        } else {
            _draft = State(initialValue: RepoWebhookDraft())
        }
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("Payload URL") {
                    TextField("https://", text: $draft.url)
                        .keyboardType(.URL)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                }
                Section("Events") {
                    ForEach(model.eventTypes) { (type: RepoWebhookEventType) in
                        Toggle(isOn: Binding(
                            get: { draft.events.contains(type.type) },
                            set: { on in
                                if on {
                                    draft.events.insert(type.type)
                                } else {
                                    draft.events.remove(type.type)
                                }
                            }
                        )) {
                            VStack(alignment: .leading) {
                                Text(type.label)
                                if !type.hint.isEmpty {
                                    Text(type.hint).font(.caption).foregroundStyle(.secondary)
                                }
                            }
                        }
                    }
                }
                Section("Description") {
                    TextField("Optional", text: $draft.description)
                }
                if let problem = draft.validationError ?? model.error {
                    Section {
                        Text(problem).foregroundStyle(.secondary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .fmMessageTools(problem)
                    }
                }
            }
            .navigationTitle(hook == nil ? "New webhook" : "Edit webhook")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") { save() }
                        .disabled(model.busy || draft.validationError != nil)
                }
            }
        }
    }

    private func save() {
        Task {
            let ok: Bool
            if let hook = hook {
                ok = await model.update(hook.id, draft: draft)
            } else {
                ok = await model.create(draft)
            }
            if ok {
                dismiss()
            }
        }
    }
}
