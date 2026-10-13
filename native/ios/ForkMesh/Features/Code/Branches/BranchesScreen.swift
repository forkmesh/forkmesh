import FMApi
import FMBase
import FMCode
import SwiftUI

struct BranchesScreen: View {
    let repo: RepoRef
    @State private var branches: [RepoBranch] = []
    @State private var error: String?
    @State private var loading = true
    @State private var query = ""
    @State private var compareBase = ""
    @State private var compareHead = ""
    @State private var conflictsBranch: String?
    @State private var commitsBranch: String?
    @State private var running: BranchOpRequest?
    @State private var progress = ""
    @State private var outcome: BranchOpOutcome?
    @State private var confirmDelete: RepoBranch?
    @State private var showActions = false

    private var defaultBranch: String { branches.first(where: \.isDefault)?.name ?? "main" }

    private var defaultRow: RepoBranch? { branches.first(where: \.isDefault) }

    private var shown: [RepoBranch] {
        let needle = query.dartTrimmed.lowercased()
        let sorted = branches.sorted { a, b in
            a.isDefault != b.isDefault ? a.isDefault : a.name.localizedStandardCompare(b.name) == .orderedAscending
        }
        return needle.isEmpty ? sorted : sorted.filter { $0.name.lowercased().contains(needle) }
    }

    private var listTitle: String {
        if loading { return "Loading…" }
        return "\(shown.count) branch\(shown.count == 1 ? "" : "es")"
    }

    var body: some View {
        List {
            if let error {
                Section { Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(error) }
            }
            if running != nil || outcome != nil {
                opSection
            }
            if branches.count > 1 {
                Section("Compare") {
                    Picker("Base", selection: $compareBase) {
                        ForEach(branches.map(\.name), id: \.self) { Text($0).tag($0) }
                    }
                    Picker("Compare", selection: $compareHead) {
                        ForEach(branches.map(\.name), id: \.self) { Text($0).tag($0) }
                    }
                    NavigationLink(value: Route.compare(repo, base: compareBase, head: compareHead)) {
                        Label("Show changes", systemImage: "arrow.left.arrow.right")
                    }
                    .disabled(compareBase.isEmpty || compareHead.isEmpty || compareBase == compareHead)
                }
            }
            Section(listTitle) {
                ForEach(shown) { (branch: RepoBranch) in
                    NavigationLink(value: branch.isDefault
                                   ? Route.commits(repo, ref: branch.name)
                                   : Route.compare(repo, base: defaultBranch, head: branch.name)) {
                        HStack {
                            Image(systemName: "arrow.triangle.branch")
                                .foregroundStyle(branch.isDefault ? Color.green : Color.secondary)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(branch.name).font(.body.monospaced()).lineLimit(1)
                                if !branch.sha.isEmpty {
                                    Text(String(branch.sha.prefix(7))).font(.caption.monospaced()).foregroundStyle(.secondary)
                                }
                            }
                            Spacer()
                            if branch.isDefault {
                                Text("Default").font(.caption2.weight(.semibold)).foregroundStyle(.green)
                            }
                        }
                    }
                    .swipeActions(edge: .trailing) {
                        if !branch.isDefault, defaultRow != nil {
                            Button(role: .destructive) {
                                confirmDelete = branch
                            } label: {
                                Label(BranchOpText.deleteTitle, systemImage: "trash")
                            }
                            .disabled(running != nil)
                            Button {
                                merge(branch, deleting: false)
                            } label: {
                                Label(BranchOpText.mergeTitle(defaultBranch), systemImage: "arrow.triangle.merge")
                            }
                            .tint(.green)
                            .disabled(running != nil)
                        }
                    }
                    .contextMenu {
                        if !branch.isDefault {
                            if defaultRow != nil {
                                Button {
                                    merge(branch, deleting: false)
                                } label: {
                                    Label(BranchOpText.mergeTitle(defaultBranch), systemImage: "arrow.triangle.merge")
                                }
                                .disabled(running != nil)
                                Button {
                                    merge(branch, deleting: true)
                                } label: {
                                    Label(BranchOpText.mergeDeleteTitle, systemImage: "arrow.triangle.merge")
                                }
                                .disabled(running != nil)
                                Button {
                                    start(.update(branch, from: defaultBranch, defaultBranch: defaultBranch))
                                } label: {
                                    Label(BranchOpText.updateTitle(defaultBranch), systemImage: "arrow.triangle.2.circlepath")
                                }
                                .disabled(running != nil)
                            }
                            Button {
                                conflictsBranch = branch.name
                            } label: {
                                Label("Check conflicts", systemImage: "exclamationmark.triangle")
                            }
                        }
                        Button {
                            commitsBranch = branch.name
                        } label: {
                            Label("Commits", systemImage: "clock")
                        }
                        if !branch.isDefault, defaultRow != nil {
                            Button(role: .destructive) {
                                confirmDelete = branch
                            } label: {
                                Label(BranchOpText.deleteTitle, systemImage: "trash")
                            }
                            .disabled(running != nil)
                        }
                    }
                }
            }
        }
        .searchable(text: $query, prompt: "Filter branches")
        .confirmationDialog(
            "Delete \(confirmDelete?.name ?? "")?",
            isPresented: Binding(get: { confirmDelete != nil }, set: { if !$0 { confirmDelete = nil } }),
            titleVisibility: .visible,
            presenting: confirmDelete
        ) { (branch: RepoBranch) in
            Button(BranchOpText.deleteTitle, role: .destructive) {
                start(.delete(branch, defaultBranch: defaultBranch))
            }
            Button("Cancel", role: .cancel) {}
        } message: { (branch: RepoBranch) in
            Text(BranchOpText.deleteConfirm(branch.name))
        }
        .navigationDestination(isPresented: $showActions) {
            RepoActionsScreen(repo: repo, commit: outcome?.mergeOid ?? "")
                .shellPageClearance()
        }
        .navigationDestination(isPresented: Binding(get: { conflictsBranch != nil }, set: { if !$0 { conflictsBranch = nil } })) {
            ConflictsScreen(repo: repo, branch: conflictsBranch ?? "")
                .shellPageClearance()
        }
        .navigationDestination(isPresented: Binding(get: { commitsBranch != nil }, set: { if !$0 { commitsBranch = nil } })) {
            RouteDestinationView(route: Route.commits(repo, ref: commitsBranch ?? defaultBranch))
                .shellPageClearance()
        }
        .navigationTitle("Branches")
        .navigationBarTitleDisplayMode(.inline)
        .fmRefreshable { await load() }
        .task { if branches.isEmpty { await load() } }
    }

    @ViewBuilder private var opSection: some View {
        Section("Branch change") {
            if running != nil {
                HStack {
                    Text(progress)
                        .fixedSize(horizontal: false, vertical: true)
                    Spacer()
                    ProgressView()
                }
            } else if let outcome {
                Label(outcome.message, systemImage: outcomeIcon(outcome))
                    .foregroundStyle(outcomeColor(outcome))
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(outcome.message)
                if outcome.kind == .pending {
                    Button {
                        start(outcome.request)
                    } label: {
                        Label("Try again", systemImage: "arrow.clockwise")
                    }
                }
                if outcome.isConflict {
                    ForEach(Array(outcome.conflicts.prefix(50)), id: \.self) { (path: String) in
                        NavigationLink(value: Route.file(repo, ref: outcome.request.branch, path: path)) {
                            Text(path).font(.footnote.monospaced())
                        }
                    }
                    Button {
                        conflictsBranch = outcome.request.branch
                    } label: {
                        Label("Check conflicts", systemImage: "exclamationmark.triangle")
                    }
                }
                if outcome.landedOnDefault {
                    Button {
                        showActions = true
                    } label: {
                        Label("Actions", systemImage: "play.circle")
                    }
                    .accessibilityIdentifier("branches-merge-actions")
                }
                Button("Dismiss") { self.outcome = nil }
                    .font(.footnote)
            }
        }
    }

    private func outcomeIcon(_ outcome: BranchOpOutcome) -> String {
        switch outcome.kind {
        case .done: return "checkmark.circle"
        case .queued: return "clock"
        case .pending: return "hourglass"
        case .failed: return "exclamationmark.triangle"
        }
    }

    private func outcomeColor(_ outcome: BranchOpOutcome) -> Color {
        switch outcome.kind {
        case .done: return .green
        case .queued, .pending: return .secondary
        case .failed: return .red
        }
    }

    private func merge(_ branch: RepoBranch, deleting: Bool) {
        guard let base = defaultRow else { return }
        start(.merge(branch, into: base, deleteBranch: deleting, defaultBranch: base.name))
    }

    private func start(_ request: BranchOpRequest) {
        guard running == nil else { return }
        running = request
        outcome = nil
        progress = BranchOpText.progress(request)
        Task {
            let result: BranchOpOutcome
            do {
                result = try await ReviewEnv.api.branchOp(repo.owner, repo.name, request) { progress = $0 }
            } catch {
                result = BranchOpOutcome(kind: .failed, request: request, answer: nil, message: dartErrorText(error))
            }
            running = nil
            outcome = result
            if result.kind == .failed {
                FmHaptics.failure()
            } else {
                FmHaptics.commit()
            }
            await load()
        }
    }

    private func load() async {
        loading = true
        defer { loading = false }
        do {
            branches = try await ReviewEnv.api.branches(repo.owner, repo.name)
            error = nil
            if compareBase.isEmpty { compareBase = defaultBranch }
            if compareHead.isEmpty { compareHead = branches.first { !$0.isDefault }?.name ?? defaultBranch }
        } catch {
            self.error = dartErrorText(error)
        }
    }
}

struct ConflictsScreen: View {
    let repo: RepoRef
    let branch: String
    @State private var base = ""
    @State private var clean: Bool?
    @State private var conflicts: [String] = []
    @State private var error: String?
    @State private var outcome: String?
    @State private var launching = false

    var body: some View {
        List {
            Section {
                LabeledContent("Branch", value: branch)
                LabeledContent("Into", value: base.isEmpty ? "…" : base)
                if let clean {
                    if clean {
                        Label("Merges cleanly", systemImage: "checkmark.circle").foregroundStyle(.green)
                    } else {
                        Label("\(conflicts.count) conflicting file\(conflicts.count == 1 ? "" : "s")", systemImage: "exclamationmark.triangle")
                            .foregroundStyle(.red)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .fmMessageTools((["\(conflicts.count) conflicting file\(conflicts.count == 1 ? "" : "s") merging \(branch) into \(base.isEmpty ? "the base branch" : base):"] + conflicts).joined(separator: "\n"))
                    }
                } else if error == nil {
                    ProgressView()
                }
                if let error { Text(error).foregroundStyle(.red).font(.footnote).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(error) }
            }
            if !conflicts.isEmpty {
                Section("Files") {
                    ForEach(conflicts, id: \.self) { (path: String) in
                        NavigationLink(value: Route.file(repo, ref: branch, path: path)) {
                            Text(path).font(.footnote.monospaced())
                        }
                    }
                }
                Section {
                    Button {
                        Task { await handToAgent() }
                    } label: {
                        Label(launching ? "Starting…" : "Resolve with an agent", systemImage: "sparkles")
                    }
                    .disabled(launching)
                    if let outcome { Text(outcome).font(.footnote).foregroundStyle(.secondary).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(outcome) }
                } footer: {
                    Text("The agent merges \(base.isEmpty ? "the base branch" : base) into \(branch), resolves each conflict and pushes the branch. You can also pick ours or theirs per hunk on the desktop.")
                }
            }
        }
        .navigationTitle("Conflicts")
        .navigationBarTitleDisplayMode(.inline)
        .fmRefreshable { await load() }
        .task { await load() }
    }

    private func load() async {
        do {
            if base.isEmpty {
                let branches = try await ReviewEnv.api.branches(repo.owner, repo.name)
                base = branches.first(where: \.isDefault)?.name ?? "main"
            }
            let reply = try await CollabHTTP.json(
                ReviewEnv.api, "GET", "/api/repo/\(repo.owner)/\(repo.name)/merge-preview",
                query: [("base", base), ("head", branch), ("ref", "")])
            clean = reply["clean"] == .bool(true)
            conflicts = Array(reply.strings("conflicts").filter { !$0.isEmpty }.prefix(200))
            error = nil
        } catch {
            self.error = dartErrorText(error)
        }
    }

    private func handToAgent() async {
        launching = true
        defer { launching = false }
        do {
            let result = try await ReviewEnv.api.launchAgentSession(
                repo.owner, repo.name, prompt: ConflictResolution.agentPrompt(branch: branch, base: base, files: conflicts),
                title: "Resolve conflicts on \(branch)", createPr: false)
            outcome = result.message.isEmpty ? "Agent queued." : result.message
            FmHaptics.commit()
        } catch {
            outcome = dartErrorText(error)
            FmHaptics.failure()
        }
    }
}
