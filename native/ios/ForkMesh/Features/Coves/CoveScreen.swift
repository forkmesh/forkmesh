import FMApi
import FMBase
import FMCoves
import SwiftUI
import UIKit

struct CoveScreen: View {
    @Bindable var model: CovesModel
    let coveId: String
    @Environment(\.dismiss) private var dismiss
    @State private var creating = false
    @State private var sharing = false
    @State private var showingLog = false
    @State private var renaming: CoveDocument?
    @State private var deleting: CoveDocument?
    @State private var error: String?

    var body: some View {
        Group {
            if let cove = model.cove(coveId), cove.unlocked {
                content(cove)
            } else {
                ContentUnavailableView("Cove locked", systemImage: "lock.fill", description: Text("Open it again from the list."))
            }
        }
        .navigationTitle(model.cove(coveId)?.displayName ?? "Cove")
        .navigationBarTitleDisplayMode(.inline)
        .task(id: model.cove(coveId)?.unlocked == true) { _ = model.noteOpened(coveId) }
    }

    private func content(_ cove: Cove) -> some View {
        List {
            if model.entry(coveId)?.pendingReview == true {
                Section {
                    Label("Your changes are proposed and wait for the repository owner to merge them.", systemImage: "clock.arrow.circlepath")
                        .font(.footnote)
                }
            }
            if let error {
                Section { Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(error) }
            }
            Section {
                ForEach(cove.documents.sorted { $0.name.lowercased() < $1.name.lowercased() }) { document in
                    NavigationLink {
                        CoveDocumentScreen(model: model, coveId: coveId, documentName: document.name)
                            .shellPageClearance()
                    } label: {
                        Label {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(document.name)
                                if document.updatedAtMs > 0 {
                                    Text(Date(timeIntervalSince1970: Double(document.updatedAtMs) / 1000), style: .relative)
                                        .font(.caption).foregroundStyle(.secondary)
                                }
                            }
                        } icon: {
                            Image(systemName: symbol(for: document))
                        }
                    }
                    .swipeActions {
                        Button("Delete", role: .destructive) { deleting = document }
                        Button("Rename") { renaming = document }
                    }
                    .contextMenu {
                        Button("Rename", systemImage: "pencil") { renaming = document }
                        Button("Delete", systemImage: "trash", role: .destructive) { deleting = document }
                    }
                }
            } header: {
                Text("Documents")
            } footer: {
                Text("Encrypted with AES-256 before it leaves this phone. The repository only ever holds ciphertext.")
            }
        }
        .overlay {
            if cove.documents.isEmpty {
                ContentUnavailableView {
                    Label("Empty cove", systemImage: "doc.badge.plus")
                } description: {
                    Text("Add a note, a spreadsheet, a lead pipeline or another starter document.")
                } actions: {
                    Button("Add document") { creating = true }.buttonStyle(.borderedProminent)
                }
            }
        }
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                Button { creating = true } label: { Label("Add document", systemImage: "doc.badge.plus") }
                    .disabled(model.busy)
                Menu {
                    if cove.accountScoped {
                        Button("Share", systemImage: "person.badge.plus") { sharing = true }
                    }
                    Button("Copy link", systemImage: "link") {
                        UIPasteboard.general.string = model.shareLink(coveId)
                    }
                    Button("Access log", systemImage: "list.bullet.rectangle") { showingLog = true }
                    Button("Lock", systemImage: "lock") {
                        model.lock(coveId)
                        dismiss()
                    }
                } label: {
                    Label("More", systemImage: "ellipsis.circle")
                }
            }
        }
        .sheet(isPresented: $creating) {
            CoveTemplatePicker(model: model, coveId: coveId) { creating = false }
        }
        .sheet(isPresented: $sharing) {
            CoveShareSheet(model: model, coveId: coveId) { sharing = false }
        }
        .sheet(isPresented: $showingLog) {
            CoveAccessLogSheet(cove: cove) { showingLog = false }
        }
        .sheet(item: $renaming) { document in
            CoveRenameSheet(original: document.name) { newName in
                renaming = nil
                guard let newName else { return }
                Task { await run { try await model.renameDocument(coveId: coveId, from: document.name, to: newName) } }
            }
        }
        .confirmationDialog(
            "Delete \(deleting?.name ?? "")?", isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } }),
            titleVisibility: .visible
        ) {
            Button("Delete", role: .destructive) {
                if let document = deleting {
                    Task { await run { try await model.deleteDocument(coveId: coveId, name: document.name) } }
                }
                deleting = nil
            }
        } message: {
            Text("The deletion is proposed like any other cove change.")
        }
    }

    private func symbol(for document: CoveDocument) -> String {
        switch CoveFiles.editorKind(path: document.name, mime: document.mime) {
        case .spreadsheet: return "tablecells"
        case .pipeline: return "chart.bar.doc.horizontal"
        case .text: return document.name.lowercased().hasSuffix(".json") ? "curlybraces" : "doc.text"
        }
    }

    private func run(_ work: () async throws -> Void) async {
        do {
            try await work()
            error = nil
        } catch {
            self.error = "\(error)"
            FmHaptics.failure()
        }
    }
}

private struct CoveRenameSheet: View {
    let original: String
    let onDone: (String?) -> Void
    @State private var name: String

    init(original: String, onDone: @escaping (String?) -> Void) {
        self.original = original
        self.onDone = onDone
        _name = State(initialValue: original)
    }

    var body: some View {
        NavigationStack {
            Form { TextField("Name", text: $name).textInputAutocapitalization(.never).autocorrectionDisabled() }
                .navigationTitle("Rename")
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) { Button("Cancel") { onDone(nil) } }
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Rename") { onDone(name) }
                            .disabled(name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || name == original)
                    }
                }
        }
        .presentationDetents([.medium])
    }
}

private struct CoveTemplatePicker: View {
    let model: CovesModel
    let coveId: String
    let onDone: () -> Void
    @State private var chosen: CoveFileTemplate?
    @State private var name = ""
    @State private var working = false
    @State private var error: String?

    var body: some View {
        NavigationStack {
            List {
                if let error {
                    Section { Text(error).foregroundStyle(.red).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(error) }
                }
                Section {
                    ForEach(CoveFiles.templates()) { template in
                        Button {
                            chosen = template
                            name = template.defaultPath
                        } label: {
                            HStack(spacing: 12) {
                                Image(systemName: template.symbol).frame(width: 28)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(template.title).foregroundStyle(.primary)
                                    Text(template.description).font(.caption).foregroundStyle(.secondary)
                                }
                                Spacer()
                                if chosen?.id == template.id {
                                    Image(systemName: "checkmark").foregroundStyle(Color.accentColor)
                                }
                            }
                        }
                        .accessibilityAddTraits(chosen?.id == template.id ? .isSelected : [])
                    }
                }
                if chosen != nil {
                    Section("File name") {
                        TextField("File name", text: $name).textInputAutocapitalization(.never).autocorrectionDisabled()
                    }
                }
            }
            .navigationTitle("Quick create")
            .navigationBarTitleDisplayMode(.inline)
            .interactiveDismissDisabled(working)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel", action: onDone).disabled(working) }
                ToolbarItem(placement: .confirmationAction) {
                    if working {
                        ProgressView()
                    } else {
                        Button("Create") { Task { await create() } }.disabled(chosen == nil)
                    }
                }
            }
        }
    }

    private func create() async {
        guard let chosen else { return }
        working = true
        defer { working = false }
        do {
            _ = try await model.addDocument(coveId: coveId, template: chosen, name: name)
            FmHaptics.commit()
            onDone()
        } catch {
            self.error = "\(error)"
        }
    }
}

private struct CoveShareSheet: View {
    @Bindable var model: CovesModel
    let coveId: String
    let onDone: () -> Void
    @State private var invitee = ""
    @State private var working = false
    @State private var status: String?
    @State private var failed = false

    var body: some View {
        NavigationStack {
            Form {
                if let cove = model.cove(coveId) {
                    Section {
                        ForEach(cove.members, id: \.self) { person in
                            HStack {
                                Text(verbatim: "@\(person)")
                                Spacer()
                                Text(person == CoveStore.normalizedAccount(cove.creatorAccount) ? "Owner \u{00B7} can edit" : "Can edit")
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                        }
                    } header: {
                        Text("People with access")
                    } footer: {
                        Text("Only these accounts receive an encryption grant. The link carries no key and no contents.")
                    }
                    if cove.createdBy(account: model.account) {
                        Section("Add a person") {
                            TextField("ForkMesh account", text: $invitee)
                                .textInputAutocapitalization(.never)
                                .autocorrectionDisabled()
                                .submitLabel(.send)
                                .onSubmit { Task { await invite() } }
                            Button(working ? "Adding\u{2026}" : "Add person") { Task { await invite() } }
                                .disabled(working || invitee.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                        }
                    } else {
                        Section { Text("Only the cove creator can change sharing.").foregroundStyle(.secondary) }
                    }
                    Section {
                        ShareLink(item: model.shareLink(coveId)) { Label("Send link", systemImage: "square.and.arrow.up") }
                        Button("Copy link", systemImage: "doc.on.doc") {
                            UIPasteboard.general.string = model.shareLink(coveId)
                            status = "Link copied. It opens only for an account listed above."
                            failed = false
                        }
                    }
                    if let status {
                        Section { Text(status).foregroundStyle(failed ? Color.red : Color.secondary).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(status) }
                    }
                }
            }
            .navigationTitle("Share cove")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done", action: onDone) } }
        }
    }

    private func invite() async {
        let name = invitee
        working = true
        defer { working = false }
        do {
            let delivery: CoveInviteDelivery = try await model.invite(coveId: coveId, account: name)
            let person: String = CoveStore.normalizedAccount(name)
            invitee = ""
            failed = false
            switch delivery {
            case .relayed:
                status = "Added @\(person) and sent them a Cove link notification. It opens once the owner merges the change."
            case .shareLink(let link):
                UIPasteboard.general.string = link
                status = "Added @\(person). The relay is offline, so the link was copied. Send it to them once the owner merges the change."
            }
            FmHaptics.commit()
        } catch {
            failed = true
            status = "\(error)"
        }
    }
}

private struct CoveAccessLogSheet: View {
    let cove: Cove
    let onDone: () -> Void

    var body: some View {
        NavigationStack {
            List {
                ForEach(Array(cove.accessLog.reversed().enumerated()), id: \.offset) { _, entry in
                    HStack {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(entry.name.isEmpty ? "Someone" : entry.name)
                            Text(entry.action == "edit" ? "Edited" : "Opened").font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        if entry.ts > 0 {
                            Text(Date(timeIntervalSince1970: Double(entry.ts) / 1000), style: .date)
                                .font(.caption).foregroundStyle(.secondary)
                        }
                    }
                    .accessibilityElement(children: .combine)
                }
            }
            .overlay {
                if cove.accessLog.isEmpty {
                    ContentUnavailableView("No activity yet", systemImage: "clock", description: Text("Edits are recorded inside the encrypted cove."))
                }
            }
            .navigationTitle("Access log")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done", action: onDone) } }
        }
    }
}
