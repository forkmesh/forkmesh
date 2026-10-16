import FMApi
import FMControl
import FMPackages
import FMTeammates
import SwiftUI

struct ShortcutsDestination: View {
    let repo: RepoRef?

    var body: some View {
        if let store = TeammatesFeature.shortcuts {
            ShortcutsScreen(store: store, opened: (repo ?? CodeCurrentRepo.memory.ref)?.slug ?? "")
        } else {
            TeamUnavailableView(title: "Shortcuts are not available")
        }
    }
}

struct ShortcutsScreen: View {
    @Bindable var store: ShortcutsStore
    let opened: String

    @State private var repo = ""

    var body: some View {
        List {
            Section {
                if store.catalog?.onPhone == true {
                    Label(
                        "From this iPhone's copy of the repository. Prompt shortcuts and skills run here; scripts need the desktop.",
                        systemImage: "iphone"
                    )
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                } else if store.unsupported {
                    TeamOutdatedRow()
                } else if !store.error.isEmpty {
                    Label(store.error, systemImage: "exclamationmark.triangle").foregroundStyle(.red).font(.footnote).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(store.error)
                }
                if let catalog = store.catalog, !catalog.repos.isEmpty {
                    Picker("Repository", selection: $repo) {
                        ForEach(catalog.repos, id: \.self) { Text($0).tag($0) }
                    }
                    if !opened.isEmpty, !catalog.repos.contains(where: { $0.caseInsensitiveCompare(opened) == .orderedSame }) {
                        Text(catalog.onPhone ? "This iPhone has no copy of \(opened)." : "The desktop has no working tree of \(opened).")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            if let run = store.shownRun {
                Section("Output") {
                    NavigationLink {
                        ShortcutOutputScreen(store: store)
                            .shellPageClearance()
                    } label: {
                        ShortcutRunSummary(run: run, tailLines: 12)
                    }
                }
            }
            if let catalog = store.catalog {
                if !catalog.shortcuts.isEmpty {
                    Section("Shortcuts") {
                        ForEach(catalog.shortcuts) { entry in row(entry) }
                    }
                }
                if !catalog.skills.isEmpty {
                    Section("Skills") {
                        ForEach(catalog.skills) { entry in row(entry) }
                    }
                }
                if catalog.entries.isEmpty && !store.loading {
                    Text("No shortcuts or skills here yet.").foregroundStyle(.secondary)
                }
            } else if store.loading {
                ProgressView()
            }
        }
        .navigationTitle("Shortcuts")
        .fmRefreshable { await store.refresh(repo: repo) }
        .task {
            await LocalRepoServices.store?.ensureLoaded()
            repo = await store.load(preferred: repo.isEmpty ? opened : repo)
        }
        .onChange(of: repo) { _, value in
            guard store.catalog?.repo != value else { return }
            Task { await store.refresh(repo: value) }
        }
    }

    private func row(_ entry: ShortcutEntry) -> some View {
        NavigationLink {
            ShortcutDetailScreen(store: store, repo: repo, entry: entry)
                .shellPageClearance()
        } label: {
            HStack(alignment: .top) {
                Image(systemName: entry.isSkill ? "sparkles" : (entry.isScript ? "terminal" : "text.bubble"))
                    .foregroundStyle(.secondary)
                VStack(alignment: .leading, spacing: 2) {
                    Text(entry.name).font(.subheadline)
                    if !entry.description.isEmpty {
                        Text(entry.description).font(.caption).foregroundStyle(.secondary).lineLimit(2)
                    }
                    let status = store.status(repo: repo, entry: entry)
                    ShortcutLastOutput(status: status)
                    ShortcutTiming(status: status)
                }
                Spacer()
                if entry.uses > 0 {
                    Text("\(entry.uses)").font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                }
            }
        }
    }
}

struct ShortcutLastOutput: View {
    let status: ShortcutRowStatus

    var body: some View {
        if status.ran {
            HStack(spacing: 4) {
                if status.running {
                    ProgressView().controlSize(.mini)
                } else {
                    Image(systemName: status.ok ? "checkmark.circle.fill" : "xmark.octagon.fill")
                        .foregroundStyle(status.ok ? .green : .red)
                        .font(.caption2)
                }
                Text(status.line.isEmpty ? (status.running ? "running" : "no output") : status.line)
                    .font(.system(.caption2, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.tail)
                FmMessageTools(status.line)
            }
        }
    }
}

struct ShortcutTiming: View {
    let status: ShortcutRowStatus

    var body: some View {
        if status.running, status.startedAtMs > 0 {
            TimelineView(.periodic(from: .now, by: 1)) { context in
                if let progress = status.progress(nowMs: Int64(context.date.timeIntervalSince1970 * 1000)) {
                    ShortcutProgressBar(progress: progress)
                }
            }
        } else if let took = status.tookText {
            Text(took).font(.caption2.monospacedDigit()).foregroundStyle(.secondary)
        }
    }
}

struct ShortcutProgressBar: View {
    let progress: ShortcutProgress

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            if let fraction = progress.fraction {
                ProgressView(value: fraction)
            }
            Text(progress.text)
                .font(.caption2.monospacedDigit())
                .foregroundStyle(.secondary)
                .lineLimit(1)
        }
    }
}

struct ShortcutDetailScreen: View {
    let store: ShortcutsStore
    let repo: String
    let entry: ShortcutEntry

    @State private var text = ""
    @State private var error = ""
    @State private var banner = ""
    @State private var pending: PendingTeamConfirmation?
    @State private var showTerminal = false
    @State private var terminalAfterSheet = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                if !entry.description.isEmpty { Text(entry.description).font(.subheadline) }
                HStack {
                    Text(entry.kind).font(.caption).padding(4).background(Color.secondary.opacity(0.12), in: Capsule())
                    if !entry.agent.isEmpty { Text(entry.agent).font(.caption) }
                    if !entry.model.isEmpty { Text(entry.model).font(.caption).foregroundStyle(.secondary) }
                    if entry.lastMs >= 0 {
                        Text("Last run took \(ShortcutProgress.duration(entry.lastMs))")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                if !entry.runnable {
                    Text(entry.kind == "script" ? "Script shortcuts run on the desktop itself." : "This one has nothing to send to an agent.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                } else if runsHere {
                    Button {
                        Task { await runHere() }
                    } label: {
                        Label("Run on this iPhone", systemImage: "iphone")
                    }
                    .buttonStyle(.bordered)
                    .disabled(repo.isEmpty)
                }
                if AppStoreStatusText.isStatusShortcut(file: entry.file), PackageGate.shows(Route.appStoreStatus) {
                    NavigationLink {
                        AppStoreStatusScreen()
                            .shellPageClearance()
                    } label: {
                        Label(AppStoreStatusText.openTitle, systemImage: "app.badge")
                    }
                    .buttonStyle(.bordered)
                    .accessibilityIdentifier("shortcut-open-app-store-status")
                }
                if !error.isEmpty {
                    Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red).font(.footnote).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(error)
                }
                if !banner.isEmpty {
                    Label(banner, systemImage: "checkmark.circle").foregroundStyle(.green).font(.footnote).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(banner)
                }
                if let run = store.liveRun(repo: repo, entry: entry) {
                    NavigationLink {
                        ShortcutOutputScreen(store: store, runId: run.id)
                            .shellPageClearance()
                    } label: {
                        ShortcutRunSummary(run: run, tailLines: 20)
                    }
                    .buttonStyle(.plain)
                    Divider()
                }
                Text(text)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
            }
            .padding()
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .navigationTitle(entry.name)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                if !entry.onPhone {
                    Button("Run") { Task { await prepare() } }.disabled(!entry.runnable || repo.isEmpty)
                }
            }
        }
        .sheet(item: $pending, onDismiss: {
            if terminalAfterSheet {
                terminalAfterSheet = false
                showTerminal = true
            }
        }) { item in
            NavigationStack {
                TeamConfirmSheet(pending: item) { confirmation in
                    try await store.run(confirmation)
                } onDone: { message in
                    banner = message
                    if entry.isScript { terminalAfterSheet = true }
                }
            }
        }
        .navigationDestination(isPresented: $showTerminal) {
            ShortcutOutputScreen(store: store)
                .shellPageClearance()
        }
        .task {
            do {
                text = try await store.text(repo: repo, entry: entry)
            } catch {
                self.error = TeamErrorText.describe(error)
            }
        }
    }

    private var runsHere: Bool { PhoneShortcuts.runsOnPhone(entry) }

    private func runHere() async {
        do {
            let prompt = try await store.phonePrompt(repo: repo, entry: entry)
            AppNavigator.shared?.router.requestCompose(
                ComposeTarget(repo: RepoRef(slug: repo), text: prompt, onDevice: true))
            error = ""
        } catch {
            self.error = TeamErrorText.describe(error)
        }
    }

    private func prepare() async {
        do {
            pending = PendingTeamConfirmation(
                title: "Run \(entry.name)", confirmation: try await store.prepare(repo: repo, entry: entry))
            error = ""
        } catch {
            self.error = TeamErrorText.describe(error)
        }
    }
}

struct ShortcutRunSummary: View {
    let run: ShortcutRun
    let tailLines: Int

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            ShortcutRunStatus(run: run)
            Text(tail)
                .font(.system(.caption2, design: .monospaced))
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var tail: String {
        run.output.split(separator: "\n", omittingEmptySubsequences: false)
            .suffix(tailLines + 1)
            .joined(separator: "\n")
            .trimmingCharacters(in: .newlines)
    }
}

struct ShortcutRunStatus: View {
    let run: ShortcutRun

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 6) {
                if run.running {
                    ProgressView().controlSize(.small)
                } else {
                    Image(systemName: run.ok ? "checkmark.circle.fill" : "xmark.octagon.fill")
                        .foregroundStyle(run.ok ? .green : .red)
                }
                Text(run.title).font(.subheadline)
                Spacer()
                Text(run.running ? "running" : run.message).font(.caption).foregroundStyle(.secondary)
            }
            if run.running, run.startedAtMs > 0 {
                TimelineView(.periodic(from: .now, by: 1)) { context in
                    if let progress = run.progress(nowMs: Int64(context.date.timeIntervalSince1970 * 1000)) {
                        ShortcutProgressBar(progress: progress)
                    }
                }
            }
        }
    }
}

struct ShortcutOutputScreen: View {
    let store: ShortcutsStore
    var runId: Int?

    var body: some View {
        VStack(spacing: 0) {
            if store.runs.count > 1 {
                Picker("Run", selection: Binding(get: { store.shownRun?.id ?? 0 }, set: { store.shownRunId = $0 })) {
                    ForEach(store.newestRuns, id: \.id) { run in
                        Text("\(run.title) — \(run.running ? "running" : run.message)").tag(run.id)
                    }
                }
                .pickerStyle(.menu)
                .padding(.horizontal)
            }
            if let run = store.shownRun {
                ShortcutRunStatus(run: run)
                    .padding(.horizontal)
                    .padding(.vertical, 8)
                ShortcutTerminal(output: run.output, running: run.running)
            } else {
                Text("Nothing has run yet.").foregroundStyle(.secondary).padding()
                Spacer()
            }
        }
        .navigationTitle(store.shownRun?.title ?? "Output")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear { store.shownRunId = runId }
        .task { await store.loadRun() }
    }
}

struct ShortcutTerminal: View {
    let output: String
    let running: Bool

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                VStack(alignment: .leading, spacing: 0) {
                    Text(output.isEmpty ? " " : output)
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.primary)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    if running {
                        Text("\u{2588}")
                            .font(.system(.caption, design: .monospaced))
                            .foregroundStyle(.green)
                    }
                    Color.clear.frame(height: 1).id("end")
                }
                .padding(10)
            }
            .defaultScrollAnchor(.bottom)
            .onAppear { proxy.scrollTo("end", anchor: .bottom) }
            .onChange(of: output) { _, _ in
                Task { @MainActor in proxy.scrollTo("end", anchor: .bottom) }
            }
        }
    }
}
