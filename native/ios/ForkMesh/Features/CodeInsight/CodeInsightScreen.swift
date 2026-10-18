import FMApi
import FMBase
import FMCode
import FMCodeInsight
import SwiftUI

struct CodeInsightLocalFile: Identifiable, Hashable {
    let id = UUID()
    let path: String
    let workspace: GitWorkspace

    static func == (left: CodeInsightLocalFile, right: CodeInsightLocalFile) -> Bool { left.id == right.id }

    func hash(into hasher: inout Hasher) { hasher.combine(id) }
}

struct CodeInsightScreen<Content: View>: View {
    let plugin: CodeInsightPlugin
    let repo: RepoRef?
    private let content: (CodeInsightContext) -> Content

    @Environment(\.fmPalette) private var fm
    @State private var session: CodeInsightSession = CodeInsightServices.session
    @State private var target: CodeInsightTarget?
    @State private var resolved = false
    @State private var choosing = false
    @State private var attached: CodeInsightTarget?
    @State private var summaries: [String: String] = [:]
    @State private var localFile: CodeInsightLocalFile?

    init(plugin: CodeInsightPlugin, repo: RepoRef?, @ViewBuilder content: @escaping (CodeInsightContext) -> Content) {
        self.plugin = plugin
        self.repo = repo
        self.content = content
    }

    var body: some View {
        VStack(spacing: 0) {
            if let target {
                header(target)
                main(target)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if resolved {
                choosePrompt
            } else {
                ProgressView()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .background(fm.canvas)
        .navigationTitle(plugin.title)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar { toolbarItems }
        .task { await resolveTarget() }
        .onAppear {
            if let target, attached == nil { attach(target) }
        }
        .onDisappear { detach() }
        .onChange(of: target) { _, next in
            detach()
            if let next { attach(next) }
        }
        .onChange(of: localFile) { previous, _ in
            previous?.workspace.dispose()
        }
        .sheet(isPresented: $choosing) {
            CodeInsightRepoPicker(current: target) { (picked: CodeInsightTarget) in
                choosing = false
                target = picked
            }
        }
        .navigationDestination(item: $localFile) { (file: CodeInsightLocalFile) in
            LocalFileEditorScreen(workspace: file.workspace, path: file.path, onClose: { (_: Bool) in })
        }
    }

    @ToolbarContentBuilder private var toolbarItems: some ToolbarContent {
        ToolbarItemGroup(placement: .topBarTrailing) {
            if let target {
                if session.isRunning(target) {
                    FmIconButton(icon: "close", label: "Stop analysis", onPressed: { session.cancel(target) })
                } else {
                    FmIconButton(icon: "refresh", label: "Re-analyze", onPressed: { session.reanalyze(target) })
                }
            }
            historyMenu
        }
    }

    private var historyMenu: some View {
        Menu {
            Picker("History", selection: Binding(get: { session.history }, set: { (value: CodeInsightHistory) in changeHistory(value) })) {
                ForEach(CodeInsightHistory.allCases) { (option: CodeInsightHistory) in
                    Text(option.label).tag(option)
                }
            }
            Button {
                choosing = true
            } label: {
                Label("Choose repository", systemImage: "folder")
            }
        } label: {
            FmIcon("history", size: 22, label: "History")
        }
        .accessibilityLabel("History and repository")
    }

    private func header(_ target: CodeInsightTarget) -> some View {
        let result = session.result(for: target)
        let progress = session.progress(for: target)
        return VStack(alignment: .leading, spacing: FmSpace.x1) {
            Button {
                choosing = true
            } label: {
                HStack(spacing: FmSpace.x1) {
                    Text(target.title)
                        .fmText(.titleMedium)
                        .lineLimit(1)
                    FmIcon("unfold_more", size: 18, color: fm.textSecondary)
                    Spacer(minLength: 0)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Repository \(target.title). Choose another repository")
            Text(statusLine(target, result))
                .fmText(.bodySmall, color: fm.textSecondary)
                .lineLimit(2)
                .accessibilityIdentifier("code-insight-status")
            if let progress, result != nil {
                ProgressView(value: progress.fraction) {
                    Text(progress.text).fmText(.bodySmall, color: fm.textTertiary)
                }
                .tint(fm.accent)
            }
            if result != nil, let error = session.error(for: target) {
                noticeRow(error)
            }
            if let warning = result?.prepared.warning, !warning.isEmpty {
                noticeRow(warning)
            }
            if let warning = result?.snapshot.warning, !warning.isEmpty {
                noticeRow(warning)
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x2)
    }

    private func noticeRow(_ text: String) -> some View {
        HStack(alignment: .top, spacing: FmSpace.x1) {
            FmIcon("info_outline", size: 14, color: fm.textTertiary)
            Text(text)
                .fmText(.bodySmall, color: fm.textTertiary)
                .frame(maxWidth: .infinity, alignment: .leading)
            FmMessageTools(text)
        }
    }

    private func statusLine(_ target: CodeInsightTarget, _ result: CodeInsightResult?) -> String {
        guard let result else {
            return target.isLocal ? CodeInsightLabels.onThisPhone : CodeInsightLabels.sourceLabel(target, session.history)
        }
        return CodeInsightLabels.statusLine(
            source: result.sourceLabel, files: result.snapshot.files.count, summary: summaries[target.key] ?? "")
    }

    @ViewBuilder private func main(_ target: CodeInsightTarget) -> some View {
        if let result = session.result(for: target) {
            if result.snapshot.error.isEmpty {
                content(context(target, result))
            } else {
                failure(title: "Nothing to analyze", message: result.snapshot.error, icon: "search", target: target)
            }
        } else if let error = session.error(for: target) {
            failure(title: "Could not analyze \(target.title)", message: error, icon: "error_outline", target: target)
        } else {
            running(session.progress(for: target))
        }
    }

    private func running(_ progress: CodeInsightProgress?) -> some View {
        VStack(spacing: FmSpace.x3) {
            ProgressView(value: progress?.fraction ?? 0)
                .tint(fm.accent)
                .frame(maxWidth: 280)
            Text(progress?.text ?? "Preparing…")
                .fmText(.bodySmall, color: fm.textSecondary)
                .multilineTextAlignment(.center)
        }
        .padding(FmSpace.x4)
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("code-insight-progress")
    }

    private func failure(title: String, message: String, icon: String, target: CodeInsightTarget) -> some View {
        VStack(spacing: FmSpace.x3) {
            FmEmptyState(icon: icon, title: title, message: message)
            FmMessageTools(message)
            HStack(spacing: FmSpace.x2) {
                FmTextButton("Try again", icon: "refresh", onPressed: { session.reanalyze(target) })
                FmTextButton("Choose repository", onPressed: { choosing = true })
            }
        }
        .padding(FmSpace.x4)
    }

    private var choosePrompt: some View {
        VStack(spacing: FmSpace.x3) {
            FmEmptyState(icon: "search", title: "Choose a repository", message: "\(plugin.title) analyzes one repository at a time: a copy on this phone or one of your account's repositories.")
            FmTextButton("Choose repository", icon: "search", onPressed: { choosing = true })
        }
        .padding(FmSpace.x4)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func context(_ target: CodeInsightTarget, _ result: CodeInsightResult) -> CodeInsightContext {
        let session = self.session
        let key = target.key
        return CodeInsightContext(
            snapshot: result.snapshot,
            repoTitle: target.title,
            branch: result.branch,
            sourceLabel: result.sourceLabel,
            openFile: { (path: String, line: Int) in openFile(target, result, path: path, line: line) },
            loadTrend: { (path: String) async -> [InsightTrendPoint] in await session.trend(target, path: path) },
            reanalyze: { session.reanalyze(target) },
            setSummary: { (text: String) in
                Task { @MainActor in
                    if summaries[key] != text { summaries[key] = text }
                }
            })
    }

    private func openFile(_ target: CodeInsightTarget, _ result: CodeInsightResult, path: String, line: Int) {
        FmHaptics.tap()
        switch target {
        case .local:
            localFile = CodeInsightLocalFile(path: path, workspace: GitWorkspace(worktree: result.prepared.worktree))
        case let .account(owner, name):
            Services.get(AppRouter.self)?.open(
                .file(RepoRef(owner: owner, name: name), ref: result.fileRef, path: path), placement: .currentTab)
        }
    }

    private func changeHistory(_ value: CodeInsightHistory) {
        if value == session.history { return }
        session.setHistory(value)
        if let target { session.reanalyze(target) }
    }

    private func attach(_ next: CodeInsightTarget) {
        if attached == next { return }
        session.attach(next)
        attached = next
    }

    private func detach() {
        guard let attached else { return }
        session.detach(attached)
        self.attached = nil
    }

    private func resolveTarget() async {
        if resolved { return }
        let store = LocalRepoServices.store
        await store?.ensureLoaded()
        let records = store?.records ?? []
        target = CodeInsightTarget.initial(
            route: repo, preferred: session.takePreferredTarget(), last: session.lastTarget, local: records)
        resolved = true
        if target == nil { choosing = true }
    }
}

struct CodeInsightRepoPicker: View {
    let current: CodeInsightTarget?
    let onPick: (CodeInsightTarget) -> Void

    @Environment(\.dismiss) private var dismiss
    @Environment(\.fmPalette) private var fm
    @State private var query = ""
    @State private var local: [LocalRepoRecord] = []
    @State private var account: [Repository] = []
    @State private var loading = true
    @State private var error: String?

    private var needle: String { query.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() }

    private func matches(_ target: CodeInsightTarget) -> Bool {
        needle.isEmpty || target.title.lowercased().contains(needle) || target.subtitle.lowercased().contains(needle)
    }

    private var localTargets: [CodeInsightTarget] {
        CodeInsightTarget.choices(local: local, account: []).filter(matches)
    }

    private var accountTargets: [CodeInsightTarget] {
        CodeInsightTarget.choices(local: [], account: account).filter(matches)
    }

    var body: some View {
        NavigationStack {
            List {
                if !localTargets.isEmpty {
                    Section("On this phone") {
                        ForEach(localTargets) { (target: CodeInsightTarget) in row(target, icon: "phone_iphone") }
                    }
                }
                Section("Your repositories") {
                    if loading {
                        HStack(spacing: FmSpace.x2) {
                            ProgressView()
                            Text("Loading repositories…").fmText(.bodySmall, color: fm.textSecondary)
                        }
                    } else if let error {
                        HStack(alignment: .top, spacing: FmSpace.x2) {
                            Text(error)
                                .fmText(.bodySmall, color: fm.textSecondary)
                                .frame(maxWidth: .infinity, alignment: .leading)
                            FmMessageTools(error)
                        }
                    } else if accountTargets.isEmpty {
                        Text(needle.isEmpty ? "No repositories in this account yet." : "No repository matches.")
                            .fmText(.bodySmall, color: fm.textSecondary)
                    } else {
                        ForEach(accountTargets) { (target: CodeInsightTarget) in row(target, icon: "cloud_outlined") }
                    }
                }
            }
            .searchable(text: $query, prompt: "Find a repository")
            .navigationTitle("Choose a repository")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
            }
            .task { await load() }
        }
        .presentationDetents([.medium, .large])
    }

    private func row(_ target: CodeInsightTarget, icon: String) -> some View {
        Button {
            FmHaptics.tap()
            onPick(target)
        } label: {
            HStack(spacing: FmSpace.x3) {
                FmIcon(icon, size: 20, color: fm.textSecondary)
                VStack(alignment: .leading, spacing: 2) {
                    Text(target.title).fmText(.bodyMedium)
                    Text(target.subtitle).fmText(.bodySmall, color: fm.textTertiary)
                }
                Spacer(minLength: 0)
                if target.key == current?.key {
                    FmIcon("check", size: 18, color: fm.accent)
                }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(target.key == current?.key ? .isSelected : [])
    }

    private func load() async {
        if let store = LocalRepoServices.store {
            await store.ensureLoaded()
            local = store.records
        }
        do {
            account = try await CodeInsightServices.accountRepositories()
            error = nil
        } catch {
            self.error = dartErrorText(error)
        }
        loading = false
    }
}
