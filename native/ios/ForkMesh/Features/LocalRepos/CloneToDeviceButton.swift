// The "Clone to this device" action and its local name prompt. The flow is
// FMCode's CloneToDeviceModel; on success the new local repository opens
// straight away.

import FMApi
import FMCode
import SwiftUI

/// Toolbar button that clones `repo` into the on-device store. Hidden when the
/// local store is not wired in.
struct CloneToDeviceButton: View {
    let repo: Repository

    var body: some View {
        if let store = LocalRepoServices.store, let publisher = LocalRepoServices.publisher {
            CloneToDeviceControl(repo: repo, store: store, publisher: publisher)
        } else {
            EmptyView()
        }
    }
}

private enum CloneCover: Equatable {
    case working(String)
    case opened(LocalRepoRecord)
    case result(title: String, message: String, record: LocalRepoRecord?)
}

private struct CloneToDeviceControl: View {
    @Environment(\.fmPalette) private var fm
    let repo: Repository
    let store: LocalRepoStore
    @State private var model: CloneToDeviceModel
    @State private var asking: Bool = false
    @State private var localName: String = ""
    @State private var problem: String?
    @State private var cover: CloneCover?

    init(repo: Repository, store: LocalRepoStore, publisher: PublishService) {
        self.repo = repo
        self.store = store
        _model = State(initialValue: CloneToDeviceModel(store: store, publisher: publisher, session: LocalRepoServices.sessionProvider))
    }

    private var existing: LocalRepoRecord? { store.record(for: repo) }

    var body: some View {
        button
            .sheet(isPresented: $asking) { nameSheet }
            // The button sits in a toolbar, where neither an overlay nor a
            // navigation destination reaches the page: one full-screen cover
            // shows the cancellable busy card while the clone runs, then the
            // cloned repository in its own stack.
            .fullScreenCover(isPresented: coverShown) { coverContent }
            .task { await store.ensureLoaded() }
    }

    private var coverShown: Binding<Bool> {
        Binding<Bool>(
            get: { cover != nil },
            set: { (open: Bool) in if !open { cover = nil } })
    }

    @ViewBuilder private var coverContent: some View {
        switch cover {
        case .some(.opened(let record)):
            NavigationStack {
                LocalRepoScreen(record: record)
                    .toolbar {
                        ToolbarItem(placement: .topBarLeading) {
                            Button("Done") { cover = nil }
                        }
                    }
            }
        case .some(.result(let title, let message, let record)):
            CloneResultCard(
                title: title,
                message: message,
                onOpen: record.map { (found: LocalRepoRecord) in { cover = .opened(found) } },
                onDone: { cover = nil })
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(fm.bgBase.opacity(0.55))
                .presentationBackground(.clear)
        case .some(.working(let fallback)):
            LocalGitBusyCard(label: model.busyLabel ?? fallback, onCancel: { model.cancel() })
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(fm.bgBase.opacity(0.55))
                .presentationBackground(.clear)
        case .none:
            EmptyView()
        }
    }

    @ViewBuilder private var button: some View {
        if model.isBusy {
            ProgressView()
                .frame(width: 18, height: 18)
        } else if let record = existing {
            Menu {
                Button("Open on this device") { cover = .opened(record) }
                Button("Sync from ForkMesh") { startSync(record) }
            } label: {
                FmIcon("cloud_done_outlined", size: 24, color: fm.accent, label: "On this device")
                    .frame(width: 48, height: 48)
            }
            .accessibilityLabel(Text("On this device"))
        } else {
            FmIconButton(icon: "download_for_offline_outlined", label: "Clone to this device", onPressed: { start() })
        }
    }

    /// A guest is asked to sign in first: a working copy on the device is for
    /// changing code, and the repository can already be read online.
    private func start() {
        Task { @MainActor in
            guard await AccountFeature.requireAccount(action: "clone to this device") else { return }
            await store.ensureLoaded()
            if let record = existing {
                cover = .opened(record)
                return
            }
            localName = await model.suggestedName(repo.name)
            problem = nil
            asking = true
        }
    }

    private func startSync(_ record: LocalRepoRecord) {
        Task { @MainActor in
            guard await AccountFeature.requireAccount(action: "sync") else { return }
            cover = .working("Syncing \(record.publishedAs ?? record.name)")
            let result: LocalPullResult? = await model.sync(record)
            if model.wasCancelled {
                cover = nil
                return
            }
            cover = .result(
                title: result?.title ?? "Sync failed",
                message: model.notice?.message ?? "",
                record: record)
        }
    }

    private var nameSheet: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Folder name on this device", text: $localName)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .onChange(of: localName) { (_: String, _: String) in problem = nil }
                    if let problem = problem {
                        Text(problem).fmText(.bodySmall, color: fm.danger)
                            .fmMessageTools(problem)
                    }
                } footer: {
                    Text(CloneToDeviceModel.sizeNote)
                }
            }
            .navigationTitle("Clone to this device")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { asking = false }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Clone") { confirm() }
                }
            }
        }
        .presentationDetents([.medium])
    }

    private func confirm() {
        let trimmed: String = localName.trimmingCharacters(in: .whitespacesAndNewlines)
        if let found = LocalRepoStore.validateName(trimmed) {
            problem = found
            return
        }
        asking = false
        cover = .working("Cloning \(repo.fullName)")
        Task { @MainActor in
            let record: LocalRepoRecord? = await model.clone(repo: repo, localName: trimmed)
            if let record = record {
                cover = .opened(record)
            } else if model.wasCancelled {
                cover = nil
            } else {
                cover = .result(title: "Clone failed", message: model.notice?.message ?? "", record: existing)
            }
        }
    }
}

private struct CloneResultCard: View {
    @Environment(\.fmPalette) private var fm
    let title: String
    let message: String
    let onOpen: (() -> Void)?
    let onDone: () -> Void

    var body: some View {
        FmCard(radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text(title)
                    .fmText(.titleSmall, color: fm.textPrimary)
                Text(message)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .fmMessageTools(message)
                HStack(spacing: FmSpace.x2) {
                    Spacer(minLength: 0)
                    if let onOpen = onOpen {
                        FmToneButton(label: "Open", icon: "open_in_new", onPressed: onOpen)
                    }
                    FmToneButton(label: "Done", tone: .accent, filled: true, onPressed: onDone)
                }
            }
            .frame(maxWidth: 360, alignment: .leading)
        }
        .padding(FmSpace.x5)
    }
}
