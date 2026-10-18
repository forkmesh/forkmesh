// LocalRepoScreen: browse and edit files, stage and commit, manage branches,
// read history, publish, push and fetch. State and actions: FMCode's
// LocalRepoScreenModel; this file lays it out.

import FMApi
import FMBase
import FMCode
import SwiftUI
import UIKit

struct LocalRepoScreen: View {
    let record: LocalRepoRecord
    let agentPrompt: String?
    let agentAutoStart: Bool

    init(record: LocalRepoRecord, agentPrompt: String? = nil, agentAutoStart: Bool = false) {
        self.record = record
        self.agentPrompt = agentPrompt
        self.agentAutoStart = agentAutoStart
    }

    @State private var composeToken: UUID?

    var body: some View {
        content
            .onAppear {
                if composeToken == nil {
                    composeToken = Services.get(ShellComposer.self)?.enter(repo: record.name)
                }
            }
            .onDisappear {
                if let token = composeToken {
                    Services.get(ShellComposer.self)?.leave(token)
                    composeToken = nil
                }
            }
    }

    @ViewBuilder private var content: some View {
        if let store = LocalRepoServices.store {
            LocalRepoScreenContent(record: record, store: store, agentPrompt: agentPrompt, agentAutoStart: agentAutoStart)
        } else {
            FmEmptyState(icon: "error_outline", title: "Could not open repository", message: "Local repositories are not available in this build.")
        }
    }
}

/// The agent run the screen hands over to AgentScreen.
private struct LocalAgentLaunch: Hashable {
    let prompt: String
    let autoStart: Bool
}

private struct LocalRepoScreenContent: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var model: LocalRepoScreenModel
    @State private var openedFile: LocalFileEntry?
    @State private var agentLaunch: LocalAgentLaunch?
    @State private var started: Bool = false

    // Dialogs.
    @State private var newFilePath: String = ""
    @State private var askingNewFile: Bool = false
    @State private var discarding: String?
    @State private var creatingBranch: Bool = false
    @State private var publishing: Bool = false
    @State private var notANode: String?
    @State private var mirrored: PublishOutcome?
    @State private var shownCommit: LocalCommit?
    @State private var confirmingDelete: Bool = false
    @State private var insightRoute: Route?

    private let agentPrompt: String?
    private let agentAutoStart: Bool

    init(record: LocalRepoRecord, store: LocalRepoStore, agentPrompt: String?, agentAutoStart: Bool) {
        _model = State(initialValue: LocalRepoScreenModel(
            record: record, store: store, publisher: LocalRepoServices.publisher,
            session: LocalRepoServices.sessionProvider, prefs: Services.get(KeyValueStore.self)))
        self.agentPrompt = agentPrompt
        self.agentAutoStart = agentAutoStart
    }

    var body: some View {
        page
            .navigationTitle(model.record.name)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { toolbarItems }
            .navigationDestination(item: $openedFile) { (entry: LocalFileEntry) in
                LocalFileEditorScreen(workspace: model.workspace, path: entry.path, onClose: { (changed: Bool) in
                    if changed { Task { await model.refreshAll() } }
                })
                .shellPageClearance()
            }
            .navigationDestination(item: $insightRoute) { (route: Route) in
                AppServices.current?.routes.destination(for: route) ?? AnyView(EmptyView())
            }
            .navigationDestination(item: $agentLaunch) { (launch: LocalAgentLaunch) in
                AgentScreen(
                    repoName: model.record.name, workspace: model.workspace, record: model.record,
                    initialPrompt: launch.prompt, autoStart: launch.autoStart)
            }
            .task { await start() }
            .modifier(LocalRepoDialogs(
                model: model, newFilePath: $newFilePath, askingNewFile: $askingNewFile, discarding: $discarding,
                notANode: $notANode, mirrored: $mirrored, shownCommit: $shownCommit,
                confirmingDelete: $confirmingDelete, onNewFile: createFile, onRename: { publishing = true },
                onDelete: deleteRepository))
            .sheet(isPresented: $creatingBranch) {
                LocalNewBranchSheet(from: model.branchLabel, canSwitch: model.canSwitchToNewBranch) { (name: String, checkout: Bool) in
                    creatingBranch = false
                    Task { await model.createBranch(name, checkout: checkout) }
                } onCancel: {
                    creatingBranch = false
                }
            }
            .sheet(isPresented: $publishing) {
                PublishRepositorySheet(initialName: model.record.name, initialDescription: model.record.description) { (request: PublishRequest?) in
                    publishing = false
                    if let request = request { runPublish(request) }
                }
            }
    }

    private func start() async {
        if started { return }
        started = true
        await model.refreshAll()
        // A ping (or a prompt typed in the Agents tab) opens the agent at once.
        if let prompt = agentPrompt, !prompt.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            agentLaunch = LocalAgentLaunch(prompt: prompt, autoStart: agentAutoStart)
        }
    }

    // MARK: Page

    private var page: some View {
        VStack(spacing: 0) {
            FmSegmentTabBar(tabs: tabs, index: model.tab.rawValue) { (index: Int) in
                model.tab = LocalRepoTab(rawValue: index) ?? .files
            }
            if model.isPublished {
                meshBanner
            }
            Group {
                if let error = model.loadError {
                    errorView(error)
                } else {
                    tabBody
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            if let notice = model.notice {
                FmNoticeLine(notice.message, isError: notice.isError, onDismiss: { model.notice = nil })
                    .padding(.horizontal, FmSpace.x4)
            }
            FmBottomBar(actions: meshActions) {
                FmBranchLabel(name: model.branchLabel, trailingIcon: "unfold_more") {
                    model.tab = .branches
                }
            }
        }
        .background(fm.canvas)
        .overlay {
            if let label = model.busyLabel {
                LocalGitBusyOverlay(label: label, onCancel: { model.cancel() })
            }
        }
    }

    private var tabs: [FmSegmentTab] {
        LocalRepoTab.allCases.map { (tab: LocalRepoTab) -> FmSegmentTab in
            FmSegmentTab(label: tab.label, count: model.tabCount(tab))
        }
    }

    @ViewBuilder private var tabBody: some View {
        switch model.tab {
        case .files:
            LocalFilesTab(model: model, onOpen: openEntry, onNewFile: startNewFile)
        case .changes:
            LocalChangesTab(model: model, onDiscard: { (path: String) in startDiscard(path) }, onGate: localGate)
        case .branches:
            LocalBranchesTab(model: model, onNewBranch: startNewBranch)
        case .history:
            LocalHistoryTab(model: model, onShow: { (commit: LocalCommit) in shownCommit = commit })
        }
    }

    /// Which mesh repository this worktree is bound to, and how it got here.
    private var meshBanner: some View {
        HStack(spacing: FmSpace.x2) {
            FmIcon("public", size: 14, color: fm.textTertiary)
            Text(model.meshBanner)
                .fmText(.bodySmall, color: fm.textSecondary, weight: .semibold)
                .lineLimit(1)
            Spacer(minLength: 0)
            if let sync = model.sync {
                Text(sync.label)
                    .fmText(.bodySmall, color: fm.textTertiary, weight: .semibold)
                    .lineLimit(1)
                    .layoutPriority(1)
            }
        }
        .padding(EdgeInsets(top: FmSpace.x2, leading: FmSpace.x4, bottom: FmSpace.x2, trailing: FmSpace.x4))
        .shellGlass(Capsule())
        .padding(.horizontal, FmSpace.x2)
        .padding(.top, FmSpace.x1)
    }

    private func errorView(_ message: String) -> some View {
        FmCard(radius: FmRadius.lg) {
            VStack(spacing: FmSpace.x3) {
                FmEmptyState(icon: "error_outline", title: "Could not open repository", message: message)
                FmOutlinedButton("Retry", onPressed: { Task { await model.refreshAll() } })
            }
        }
        .padding(FmSpace.x4)
    }

    /// Publish an unpublished repository, or sync/push a published one.
    /// Neither asks for user presence (contract §6).
    private var meshActions: [AnyView] {
        let idle: Bool = !model.isBusy
        if !model.isPublished {
            return [
                AnyView(FmStatusBadge(label: "Local only", color: fm.textSecondary)),
                AnyView(FmToneButton(
                    label: "Publish to ForkMesh", icon: "cloud_upload_outlined", tone: .accent, filled: true,
                    onPressed: idle ? { startPublish() } : nil)),
            ]
        }
        return [
            AnyView(FmToneButton(label: "Sync", icon: "sync", onPressed: idle ? { runPull() } : nil).fixedSize()),
            AnyView(FmToneButton(
                label: "Push", icon: "upload_outlined", tone: .accent, filled: true,
                onPressed: idle ? { runPush() } : nil).fixedSize()),
        ]
    }

    @ToolbarContentBuilder private var toolbarItems: some ToolbarContent {
        ToolbarItemGroup(placement: .topBarTrailing) {
            FmIconButton(icon: "auto_awesome_outlined", label: "Coding agent", onPressed: model.isBusy ? nil : {
                agentLaunch = LocalAgentLaunch(prompt: "", autoStart: false)
            })
            FmIconButton(icon: "refresh", label: "Refresh repository", onPressed: model.isBusy ? nil : {
                Task { await model.refreshAll() }
            })
            Menu {
                if model.isPublished {
                    Button("Fetch from ForkMesh") { runFetch() }
                        .disabled(model.isBusy)
                }
                ForEach(CodeInsightPlugin.allCases.filter(\.isShown)) { (plugin: CodeInsightPlugin) in
                    Button {
                        CodeInsightServices.prepareLocal(model.record)
                        insightRoute = plugin.route(nil)
                    } label: {
                        Label(plugin.title, systemImage: plugin.systemImage)
                    }
                }
                Button(role: .destructive) {
                    startDelete()
                } label: {
                    Text("Delete repository")
                }
            } label: {
                FmIcon("more_vert", size: 22, label: "More")
            }
        }
    }

    // MARK: Actions

    /// Guests browse a local repository but sign in before changing it.
    private func gate(_ action: String, _ body: @escaping @MainActor () async -> Void) {
        Task { @MainActor in
            guard await AccountFeature.requireAccount(action: action) else { return }
            await body()
        }
    }

    private func localGate(_ action: String, _ body: @escaping @MainActor () async -> Void) {
        Task { @MainActor in
            guard await AccountFeature.requireLocalAccess(action: action) else { return }
            await body()
        }
    }

    private func openEntry(_ entry: LocalFileEntry) {
        if entry.isDirectory {
            Task { await model.openDirectory(entry.path) }
        } else {
            openedFile = entry
        }
    }

    private func startNewFile() {
        localGate("create files") {
            newFilePath = model.newFilePrefill
            askingNewFile = true
        }
    }

    private func createFile() {
        let path: String = newFilePath
        Task { @MainActor in
            if let entry = await model.newFile(path) { openedFile = entry }
        }
    }

    private func startDiscard(_ path: String) {
        localGate("discard changes") { discarding = path }
    }

    private func startNewBranch() {
        localGate("create branches") { creatingBranch = true }
    }

    private func startPublish() {
        gate("publish to ForkMesh") {
            guard model.canPublishHere else { return }
            switch model.beginPublish() {
            case .notANode(let refusal): notANode = refusal
            case .noCommits: break
            case .ready: publishing = true
            }
        }
    }

    private func runPublish(_ request: PublishRequest) {
        Task { @MainActor in
            let outcome: PublishOutcome? = await model.publish(
                name: request.name, description: request.description, isPublic: request.isPublic)
            // Contract §7: another node owns the name; offer another one.
            if let outcome = outcome, outcome.status == .mirrored { mirrored = outcome }
        }
    }

    private func runPush() {
        gate("push") {
            if let refusal = model.publishRefusal {
                notANode = refusal
                return
            }
            await model.push()
        }
    }

    private func runFetch() {
        Task { await model.fetch() }
    }

    private func runPull() {
        gate("sync") {
            await model.pull()
        }
    }

    private func startDelete() {
        localGate("delete repositories") { confirmingDelete = true }
    }

    private func deleteRepository() {
        Task { @MainActor in
            let reason: String = "Delete \"\(model.record.name)\" from this device. Commits that were never published are lost."
            guard await LocalUserPresence.confirm(reason: reason) else { return }
            if await model.deleteRepository() { dismiss() }
        }
    }
}

// MARK: - Dialogs

/// The screen's alerts, kept out of the main body so it type-checks quickly.
private struct LocalRepoDialogs: ViewModifier {
    let model: LocalRepoScreenModel
    @Binding var newFilePath: String
    @Binding var askingNewFile: Bool
    @Binding var discarding: String?
    @Binding var notANode: String?
    @Binding var mirrored: PublishOutcome?
    @Binding var shownCommit: LocalCommit?
    @Binding var confirmingDelete: Bool
    let onNewFile: () -> Void
    let onRename: () -> Void
    let onDelete: () -> Void

    private func shown<T>(_ value: Binding<T?>) -> Binding<Bool> {
        Binding<Bool>(get: { value.wrappedValue != nil }, set: { (open: Bool) in if !open { value.wrappedValue = nil } })
    }

    func body(content: Content) -> some View {
        content
            .alert("New file", isPresented: $askingNewFile) {
                TextField("docs/notes.md", text: $newFilePath)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button("Cancel", role: .cancel) {}
                Button("Create") { onNewFile() }
            }
            .alert("Discard changes?", isPresented: shown($discarding)) {
                Button("Cancel", role: .cancel) {}
                Button("Discard", role: .destructive) {
                    if let path = discarding { Task { await model.discard(path) } }
                }
            } message: {
                Text("Changes to \(discarding ?? "") are dropped and cannot be recovered.")
            }
            .alert("This device is not a node yet", isPresented: shown($notANode)) {
                Button("Copy") { UIPasteboard.general.string = notANode ?? "" }
                Button("Send to agent") { FmMessageHandoff.send(notANode ?? "") }
                Button("OK", role: .cancel) {}
            } message: {
                Text(notANode ?? "")
            }
            .alert("That name is taken", isPresented: shown($mirrored)) {
                Button("Cancel", role: .cancel) {}
                Button("Choose another name") { onRename() }
                Button("Copy") { UIPasteboard.general.string = mirrored?.message ?? "" }
                Button("Send to agent") { FmMessageHandoff.send(mirrored?.message ?? "") }
            } message: {
                Text(mirrored?.message ?? "")
            }
            .alert(shownCommit?.subject ?? "", isPresented: shown($shownCommit)) {
                Button("Close", role: .cancel) {}
            } message: {
                Text(shownCommit.map { LocalRepoText.commitDetail($0) } ?? "")
            }
            .alert("Delete repository?", isPresented: $confirmingDelete) {
                Button("Cancel", role: .cancel) {}
                Button("Delete", role: .destructive) { onDelete() }
            } message: {
                Text("Delete \"\(model.record.name)\" from this device. Commits that were never published are lost.")
            }
    }
}

/// "New branch": name, created from the current branch, optionally switched
/// to (only with a clean worktree).
private struct LocalNewBranchSheet: View {
    @Environment(\.fmPalette) private var fm
    let from: String
    let canSwitch: Bool
    let onCreate: (String, Bool) -> Void
    let onCancel: () -> Void
    @State private var name: String = ""
    @State private var switchTo: Bool

    init(from: String, canSwitch: Bool, onCreate: @escaping (String, Bool) -> Void, onCancel: @escaping () -> Void) {
        self.from = from
        self.canSwitch = canSwitch
        self.onCreate = onCreate
        self.onCancel = onCancel
        _switchTo = State(initialValue: canSwitch)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("feature/my-change", text: $name)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                } footer: {
                    Text("Created from \(from)")
                }
                Section {
                    Toggle("Switch to it", isOn: $switchTo)
                        .disabled(!canSwitch)
                } footer: {
                    if !canSwitch {
                        Text("Commit your changes first to switch.")
                    }
                }
            }
            .navigationTitle("New branch")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { onCancel() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Create") { onCreate(name.trimmingCharacters(in: .whitespacesAndNewlines), switchTo) }
                }
            }
        }
        .presentationDetents([.medium])
    }
}

// MARK: - Tabs

private struct LocalFilesTab: View {
    @Environment(\.fmPalette) private var fm
    let model: LocalRepoScreenModel
    let onOpen: (LocalFileEntry) -> Void
    let onNewFile: () -> Void

    var body: some View {
        VStack(spacing: 0) {
            LocalBreadcrumb(path: model.directory, onPath: { (path: String) in
                Task { await model.openDirectory(path) }
            }, trailing: AnyView(FmToneButton(
                label: "New file", icon: "note_add_outlined", tone: .accent, onPressed: model.isBusy ? nil : onNewFile)))
            content
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    @ViewBuilder private var content: some View {
        if let files = model.files {
            if files.isEmpty {
                FmEmptyState(icon: "folder_open_outlined", title: "Empty folder", message: "Create a file to start editing.")
            } else {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        ForEach(Array(files.enumerated()), id: \.element.path) { (index: Int, entry: LocalFileEntry) in
                            if index > 0 {
                                FmRowDivider()
                            }
                            LocalFileRow(entry: entry, onTap: { onOpen(entry) })
                        }
                    }
                    .padding(.bottom, FmSpace.x5)
                }
                .background(fm.listSurface)
            }
        } else {
            FmSkeletonList(rows: 6, label: "Loading files")
        }
    }
}

/// Root › folder › folder, newest end visible.
private struct LocalBreadcrumb: View {
    @Environment(\.fmPalette) private var fm
    let path: String
    let onPath: (String) -> Void
    let trailing: AnyView

    var body: some View {
        let crumbs: [(label: String, path: String)] = LocalRepoText.breadcrumbs(path)
        HStack(spacing: FmSpace.x2) {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 0) {
                    ForEach(Array(crumbs.indices), id: \.self) { (index: Int) in
                        if index > 0 {
                            FmIcon("chevron_right", size: 16, color: fm.textDisabled)
                        }
                        crumb(crumbs[index].label, current: index == crumbs.count - 1, target: crumbs[index].path)
                    }
                }
                .padding(.horizontal, FmSpace.x2)
            }
            .defaultScrollAnchor(.trailing)
            .shellGlass(Capsule())
            trailing
        }
        .padding(.horizontal, FmSpace.x3)
        .frame(height: 48)
    }

    private func crumb(_ label: String, current: Bool, target: String) -> some View {
        Button(action: { onPath(target) }) {
            Text(label)
                .fmText(.bodySmall, color: current ? fm.textPrimary : fm.textSecondary, weight: current ? .semibold : .regular)
                .padding(.horizontal, FmSpace.x1)
                .padding(.vertical, FmSpace.x2)
        }
        .buttonStyle(.plain)
    }
}

private struct LocalFileRow: View {
    @Environment(\.fmPalette) private var fm
    let entry: LocalFileEntry
    let onTap: () -> Void

    private var icon: String {
        switch entry.kind {
        case .directory: return "folder_outlined"
        case .symlink: return "link"
        case .file: return "description_outlined"
        }
    }

    var body: some View {
        FmListRow(
            title: entry.name,
            subtitle: LocalRepoText.fileMeta(entry),
            leading: AnyView(FmIconTile(icon: icon, tint: entry.isDirectory ? fm.accent : nil)),
            trailing: entry.tracked ? nil : AnyView(FmStatusBadge(label: "New", color: fm.success)),
            onTap: entry.kind == .symlink ? nil : onTap,
            showChevron: entry.isDirectory)
    }
}

private struct LocalChangesTab: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: LocalRepoScreenModel
    let onDiscard: (String) -> Void
    let onGate: (String, @escaping @MainActor () async -> Void) -> Void

    var body: some View {
        VStack(spacing: 0) {
            changes
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            composer
        }
    }

    @ViewBuilder private var changes: some View {
        if model.status.isClean {
            FmEmptyState(icon: "check_circle_outline", title: "Working tree clean", message: "Edit files to see changes here.")
        } else {
            ScrollView {
                LazyVStack(spacing: 0) {
                    stagedGroup
                    unstagedGroup
                    untrackedGroup
                }
                .padding(.bottom, FmSpace.x4)
            }
            .background(fm.listSurface)
        }
    }

    private func stage(_ paths: [String]) {
        onGate("stage changes") { await model.stage(paths) }
    }

    private func unstage(_ paths: [String]) {
        onGate("stage changes") { await model.unstage(paths) }
    }

    @ViewBuilder private var stagedGroup: some View {
        if !model.status.staged.isEmpty {
            FmGroupHeader(label: "Staged", count: model.status.staged.count, color: fm.success, actions: [
                AnyView(FmToneButton(label: "Unstage all", height: 28, onPressed: { onGate("stage changes") { await model.unstageAll() } })),
            ])
            ForEach(model.status.staged) { (change: LocalChange) in
                LocalChangeRow(change: change, action: AnyView(FmToneButton(
                    label: "Unstage", tooltip: "Unstage \(change.path)", height: 28, onPressed: { unstage([change.path]) })),
                    onLongPress: nil)
            }
        }
    }

    @ViewBuilder private var unstagedGroup: some View {
        if !model.status.unstaged.isEmpty {
            FmGroupHeader(label: "Changes", count: model.status.unstaged.count, color: fm.warning, actions: [
                AnyView(FmToneButton(label: "Stage all", height: 28, onPressed: { onGate("stage changes") { await model.stageAll() } })),
            ])
            ForEach(model.status.unstaged) { (change: LocalChange) in
                LocalChangeRow(change: change, action: AnyView(FmToneButton(
                    label: "Stage", tone: .success, tooltip: "Stage \(change.path)", height: 28,
                    onPressed: { stage([change.path]) })),
                    onLongPress: { onDiscard(change.path) })
            }
        }
    }

    @ViewBuilder private var untrackedGroup: some View {
        if !model.status.untracked.isEmpty {
            FmGroupHeader(label: "Untracked", count: model.status.untracked.count, actions: untrackedActions)
            ForEach(model.status.untracked) { (change: LocalChange) in
                LocalChangeRow(change: change, action: AnyView(FmToneButton(
                    label: "Stage", tone: .success, tooltip: "Stage \(change.path)", height: 28,
                    onPressed: { stage([change.path]) })),
                    onLongPress: { onDiscard(change.path) })
            }
        }
    }

    private var untrackedActions: [AnyView] {
        if !model.status.unstaged.isEmpty { return [] }
        return [AnyView(FmToneButton(label: "Stage all", height: 28, onPressed: { onGate("stage changes") { await model.stageAll() } }))]
    }

    private var composer: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            TextField("Commit message", text: $model.commitMessage, axis: .vertical)
                .lineLimit(1...4)
                .padding(EdgeInsets(top: FmSpace.x2, leading: FmSpace.x3, bottom: FmSpace.x2, trailing: FmSpace.x3))
                .shellGlass(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
            HStack(spacing: FmSpace.x2) {
                Text("Committing as \(model.author.name) <\(model.author.email)>")
                    .fmText(.bodySmall, color: fm.textTertiary, weight: .semibold)
                    .lineLimit(1)
                Spacer(minLength: 0)
                FmToneButton(
                    label: model.status.staged.isEmpty ? "Commit" : "Commit \(model.status.staged.count)",
                    icon: "check", tone: .accent, filled: true,
                    onPressed: model.canCommit ? { onGate("commit") { await model.commit() } } : nil)
            }
        }
        .padding(EdgeInsets(top: FmSpace.x3, leading: FmSpace.x4, bottom: FmSpace.x3, trailing: FmSpace.x4))
    }
}

private struct LocalChangeRow: View {
    let change: LocalChange
    let action: AnyView
    let onLongPress: (() -> Void)?

    private var kind: FmChangeKind {
        switch change.kind {
        case .added, .untracked: return .added
        case .modified: return .modified
        case .deleted: return .deleted
        }
    }

    var body: some View {
        FmListRow(
            title: change.path,
            subtitle: LocalRepoText.changeLabel(change),
            leading: AnyView(FmChangeBadge(kind: kind, size: 28)),
            trailing: action,
            onLongPress: onLongPress,
            showChevron: false,
            minHeight: 52)
    }
}

private struct LocalBranchesTab: View {
    @Environment(\.fmPalette) private var fm
    let model: LocalRepoScreenModel
    let onNewBranch: () -> Void

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: FmSpace.x3) {
                FmSectionHeader(title: "Branches", icon: "account_tree_outlined", count: model.branches.count)
                FmToneButton(label: "New branch", icon: "add", tone: .accent, onPressed: model.isBusy ? nil : onNewBranch)
            }
            .padding(EdgeInsets(top: FmSpace.x3, leading: FmSpace.x4, bottom: FmSpace.x2, trailing: FmSpace.x4))
            list
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    @ViewBuilder private var list: some View {
        if model.branches.isEmpty {
            FmEmptyState(icon: "account_tree_outlined", title: "No branches yet", message: "Make a first commit to create branches.")
        } else {
            ScrollView {
                LazyVStack(spacing: 0) {
                    ForEach(Array(model.branches.enumerated()), id: \.element.name) { (index: Int, branch: LocalBranch) in
                        if index > 0 {
                            FmRowDivider()
                        }
                        row(branch)
                    }
                }
            }
            .background(fm.listSurface)
        }
    }

    private func row(_ branch: LocalBranch) -> some View {
        FmListRow(
            title: branch.name,
            titleView: AnyView(FmBranchLabel(name: branch.name, color: branch.isCurrent ? fm.success : fm.textPrimary)),
            subtitle: branch.tipId.map { String($0.prefix(7)) } ?? "No commits yet",
            leading: AnyView(FmIconTile(icon: branch.isCurrent ? "check_circle" : "radio_button_unchecked", tint: branch.isCurrent ? fm.success : nil)),
            trailing: branch.isCurrent ? AnyView(FmStatusBadge(label: "Current", color: fm.success)) : nil,
            onTap: { Task { await model.checkout(branch) } },
            showChevron: !branch.isCurrent)
    }
}

private struct LocalHistoryTab: View {
    @Environment(\.fmPalette) private var fm
    let model: LocalRepoScreenModel
    let onShow: (LocalCommit) -> Void

    var body: some View {
        if model.log.isEmpty {
            FmEmptyState(icon: "history", title: "No commits yet", message: "Stage changes and commit to build history.")
        } else {
            ScrollView {
                LazyVStack(spacing: 0) {
                    ForEach(Array(model.log.enumerated()), id: \.element.id) { (index: Int, commit: LocalCommit) in
                        if index > 0 {
                            FmRowDivider()
                        }
                        FmListRow(
                            title: commit.subject.isEmpty ? "(no message)" : commit.subject,
                            subtitle: LocalRepoText.commitMeta(commit),
                            leading: AnyView(FmIconTile(icon: "commit", tint: index == 0 ? fm.accent : nil)),
                            onTap: { onShow(commit) },
                            showChevron: false)
                    }
                }
                .padding(.bottom, FmSpace.x5)
            }
            .background(fm.listSurface)
        }
    }
}
