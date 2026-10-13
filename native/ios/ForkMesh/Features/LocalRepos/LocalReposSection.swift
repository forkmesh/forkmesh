// The "On this device" block at the top of the Repos tab — repositories
// authored locally with the owned Git engine, plus create / import / mirror
// entry points. Renders nothing when no LocalRepoStore is registered
// (previews), so the network catalog below is unaffected. State and actions:
// FMCode's LocalReposSectionModel.

import FMBase
import FMCode
import SwiftUI
import UniformTypeIdentifiers

struct LocalReposSection: View {
    var body: some View {
        if let store = LocalRepoServices.store {
            LocalReposSectionContent(store: store)
        } else {
            EmptyView()
        }
    }
}

/// What the name prompt is for.
private enum LocalImportKind: Equatable {
    case folder(URL)
    case zip(URL)
}

private struct LocalReposSectionContent: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: LocalReposSectionModel
    @State private var opened: LocalRepoRecord?
    @State private var creating: Bool = false
    @State private var newName: String = ""
    @State private var newDescription: String = ""
    @State private var nameProblem: String?
    @State private var choosingImport: Bool = false
    @State private var pickingFolder: Bool = false
    @State private var pickingZip: Bool = false
    @State private var importing: LocalImportKind?
    @State private var importName: String = ""
    @State private var importProblem: String?
    @State private var mirroring: Bool = false
    private let store: LocalRepoStore

    init(store: LocalRepoStore) {
        self.store = store
        _model = State(initialValue: LocalReposSectionModel(
            store: store, session: LocalRepoServices.sessionProvider, prefs: Services.get(KeyValueStore.self),
            publisher: LocalRepoServices.publisher))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            FmSectionHeader(title: "On this device", icon: "phone_iphone", count: model.records.count)
                .padding(EdgeInsets(top: FmSpace.x2, leading: FmSpace.x4, bottom: FmSpace.x2, trailing: FmSpace.x4))
            listBody
            actionRow
            if let folder = LocalRepoServices.folder {
                LocalRepoFolderRow(folder: folder, store: store, isBusy: model.isBusy) { (notice: LocalNotice) in
                    model.notice = notice
                }
            }
            if let notice = model.notice {
                FmNoticeLine(notice.message, isError: notice.isError, onDismiss: { model.notice = nil })
                    .padding(EdgeInsets(top: 0, leading: FmSpace.x4, bottom: FmSpace.x2, trailing: FmSpace.x4))
            }
            if let applier = LocalRepoServices.applier {
                LocalNodeSyncSummary(applier: applier)
            }
            FmHairline()
            FmSectionHeader(title: "Network", icon: "public")
                .padding(EdgeInsets(top: FmSpace.x3, leading: FmSpace.x4, bottom: FmSpace.x2, trailing: FmSpace.x4))
        }
        .task { await model.load() }
        .navigationDestination(item: $opened) { (record: LocalRepoRecord) in
            LocalRepoScreen(record: record)
                .shellPageClearance()
        }
        .sheet(isPresented: $creating) { createSheet }
        .sheet(item: importBinding) { (kind: LocalImportPrompt) in importSheet(kind.kind) }
        .sheet(isPresented: $mirroring) {
            MirrorImportSheet(onDone: { (notice: String?) in
                mirroring = false
                if let notice = notice { model.notice = LocalNotice(notice) }
            })
        }
        .confirmationDialog("Import from Files", isPresented: $choosingImport, titleVisibility: .hidden) {
            Button("Folder — copy a folder from Files into a new repository") { pickingFolder = true }
            Button("Zip archive — extract a .zip into a new repository") { pickingZip = true }
            Button("Cancel", role: .cancel) {}
        }
        .fileImporter(isPresented: $pickingFolder, allowedContentTypes: [UTType.folder]) { (result: Result<URL, Error>) in
            if case .success(let url) = result {
                importName = LocalReposSectionModel.slugify(url.lastPathComponent)
                importing = .folder(url)
            }
        }
        .fileImporter(isPresented: $pickingZip, allowedContentTypes: [UTType.zip]) { (result: Result<URL, Error>) in
            if case .success(let url) = result {
                importName = LocalReposSectionModel.slugify(LocalReposSectionModel.zipBaseName(url.lastPathComponent))
                importing = .zip(url)
            }
        }
        .overlay {
            if let label = model.busyLabel {
                LocalGitBusyOverlay(label: label, onCancel: { model.cancel() })
            }
        }
    }

    // MARK: List

    @ViewBuilder private var listBody: some View {
        if let error = model.loadError {
            Text(error)
                .fmText(.bodySmall, color: fm.danger)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fmMessageTools(error)
                .padding(.horizontal, FmSpace.x4)
        } else if !model.isLoaded {
            ProgressView()
                .frame(maxWidth: .infinity)
                .padding(FmSpace.x3)
        } else if model.records.isEmpty {
            Text(LocalReposSectionModel.emptyMessage)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .padding(EdgeInsets(top: 0, leading: FmSpace.x4, bottom: FmSpace.x2, trailing: FmSpace.x4))
        } else {
            VStack(spacing: 0) {
                ForEach(Array(model.records.enumerated()), id: \.element.name) { (index: Int, record: LocalRepoRecord) in
                    if index > 0 {
                        FmRowDivider()
                    }
                    LocalRepoTile(record: record) { opened = record }
                        .contextMenu {
                            Button("Open") { opened = record }
                            if record.isPublished && model.canSync {
                                Button("Sync from ForkMesh") { startSync(record) }
                            }
                        }
                }
            }
            .background(fm.listSurface)
        }
    }

    private var actionRow: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: FmSpace.x2) {
                if model.canSync {
                    FmToneButton(
                        label: model.cloneCount == model.syncableRecords.count ? "Sync clones" : "Sync all",
                        icon: "sync",
                        tone: .accent,
                        onPressed: model.isBusy ? nil : { startSync(nil) })
                }
                FmToneButton(label: "New repository", icon: "add", tone: .accent, onPressed: model.isBusy ? nil : { startCreate() })
                FmToneButton(label: "Import from Files", icon: "drive_folder_upload_outlined", onPressed: model.isBusy ? nil : { startImport() })
                FmToneButton(label: "Mirror from GitHub/GitLab/Codeberg", icon: "cloud_sync_outlined", onPressed: model.isBusy ? nil : { startMirror() })
            }
            .padding(EdgeInsets(top: FmSpace.x2, leading: FmSpace.x4, bottom: FmSpace.x3, trailing: FmSpace.x4))
        }
    }

    private func startSync(_ record: LocalRepoRecord?) {
        Task { @MainActor in
            guard await AccountFeature.requireAccount(action: "sync") else { return }
            if let record = record {
                await model.sync(record)
            } else {
                await model.syncAll()
            }
        }
    }

    // MARK: Create

    private func startCreate() {
        Task { @MainActor in
            guard await AccountFeature.requireAccount(action: "create a repository") else { return }
            newName = ""
            newDescription = ""
            nameProblem = nil
            creating = true
        }
    }

    private var createSheet: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("my-project", text: $newName)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    if let problem = nameProblem {
                        Text(problem).fmText(.bodySmall, color: fm.danger)
                            .fmMessageTools(problem)
                    }
                } header: {
                    Text("Name")
                }
                Section {
                    TextField("Description (optional)", text: $newDescription, axis: .vertical)
                        .lineLimit(1...3)
                } footer: {
                    Text(model.createHintText)
                }
            }
            .navigationTitle("New repository")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { creating = false }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Create") { confirmCreate() }
                }
            }
        }
        .presentationDetents([.medium])
    }

    private func confirmCreate() {
        let trimmed: String = newName.trimmingCharacters(in: .whitespacesAndNewlines)
        if let problem = LocalRepoStore.validateName(trimmed) {
            nameProblem = problem
            return
        }
        creating = false
        let description: String = newDescription
        Task { @MainActor in
            if let record = await model.create(name: trimmed, description: description) {
                opened = record
            }
        }
    }

    // MARK: Import

    private func startImport() {
        Task { @MainActor in
            guard await AccountFeature.requireAccount(action: "import a repository") else { return }
            choosingImport = true
        }
    }

    private func startMirror() {
        Task { @MainActor in
            guard await AccountFeature.requireAccount(action: "import a repository") else { return }
            mirroring = true
        }
    }

    private var importBinding: Binding<LocalImportPrompt?> {
        Binding<LocalImportPrompt?>(
            get: { importing.map { LocalImportPrompt(kind: $0) } },
            set: { (value: LocalImportPrompt?) in importing = value?.kind })
    }

    private func importSheet(_ kind: LocalImportKind) -> some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Repository name", text: $importName)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    if let problem = importProblem {
                        Text(problem).fmText(.bodySmall, color: fm.danger)
                            .fmMessageTools(problem)
                    }
                }
            }
            .navigationTitle("Import from Files")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { importing = nil }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Import") { confirmImport(kind) }
                }
            }
        }
        .presentationDetents([.medium])
    }

    private func confirmImport(_ kind: LocalImportKind) {
        let trimmed: String = importName.trimmingCharacters(in: .whitespacesAndNewlines)
        if let problem = LocalRepoStore.validateName(trimmed) {
            importProblem = problem
            return
        }
        importProblem = nil
        importing = nil
        Task { @MainActor in
            let record: LocalRepoRecord?
            switch kind {
            case .folder(let url):
                let access: Bool = url.startAccessingSecurityScopedResource()
                record = await model.importFolder(name: trimmed, source: url)
                if access { url.stopAccessingSecurityScopedResource() }
            case .zip(let url):
                let access: Bool = url.startAccessingSecurityScopedResource()
                record = await model.importZip(name: trimmed, zipPath: url.path)
                if access { url.stopAccessingSecurityScopedResource() }
            }
            if let record = record { opened = record }
        }
    }
}

/// Identifiable wrapper so the import prompt can drive `.sheet(item:)`.
private struct LocalImportPrompt: Identifiable {
    let kind: LocalImportKind
    var id: String {
        switch kind {
        case .folder(let url): return "folder:\(url.path)"
        case .zip(let url): return "zip:\(url.path)"
        }
    }
}

/// Name, mesh identity (or branch), last commit and the
/// sync-state badge.
struct LocalRepoTile: View {
    @Environment(\.fmPalette) private var fm
    let record: LocalRepoRecord
    let onTap: () -> Void

    /// Green once the mesh has it, accent for a repository that came from the
    /// mesh, muted while it is phone-only.
    private var tint: Color {
        switch record.syncLabel {
        case "Published": return fm.success
        case "Cloned": return fm.accent
        default: return fm.textSecondary
        }
    }

    var body: some View {
        FmListRow(
            title: record.name,
            subtitle: LocalRepoText.recordMeta(record),
            leading: AnyView(FmIconTile(icon: record.isCloned ? "cloud_done_outlined" : "phone_iphone", tint: fm.accent, size: 40)),
            trailing: AnyView(FmStatusBadge(label: record.syncLabel, color: tint)),
            onTap: onTap)
    }
}
