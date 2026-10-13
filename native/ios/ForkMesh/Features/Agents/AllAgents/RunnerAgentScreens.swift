import FMAgents
import FMApi
import FMBase
import FMCode
import FMControl
import SwiftUI

extension UnifiedAgentRow {
    var runnerRef: RunnerAgentRef {
        RunnerAgentRef(
            owner: repoOwner, repo: repoName, sessionId: cloudSessionId, orgScope: orgScope, branch: branchName,
            base: baseBranch, pull: prNumber)
    }
}

enum RunnerAgentFeature {
    @MainActor static var api: RunnerAgentsApi? { Services.get(ApiService.self) }
}

struct RunnerPullLabel: View {
    let text: String
    let running: Bool

    var body: some View {
        HStack(spacing: 3) {
            if running {
                ProgressView()
                    .controlSize(.small)
            } else {
                Image(systemName: "arrow.triangle.pull")
            }
            Text(text)
                .font(.caption.weight(.semibold))
                .monospacedDigit()
                .lineLimit(1)
        }
        .fixedSize()
    }
}

struct RunnerDiffScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var model: RunnerDiffModel
    @State private var merge: RunnerMergeModel
    @State private var fix = ConflictFixModel()
    @State private var mergeStarted = false
    private let api: RunnerAgentsApi
    private let ref: RunnerAgentRef
    private let canMerge: Bool
    private let mergeOnOpen: Bool
    private let onFixConflicts: (() async throws -> Void)?
    private let onMerged: (String) -> Void
    private let onLeave: (() -> Void)?

    init(
        api: RunnerAgentsApi, ref: RunnerAgentRef, canMerge: Bool = false, mergeOnOpen: Bool = false,
        onFixConflicts: (() async throws -> Void)? = nil, onMerged: @escaping (String) -> Void = { _ in },
        onLeave: (() -> Void)? = nil
    ) {
        _model = State(initialValue: RunnerDiffModel(api: api, ref: ref, store: Services.get(KeyValueStore.self)))
        _merge = State(initialValue: RunnerMergeModel(api: api, ref: ref))
        self.api = api
        self.ref = ref
        self.canMerge = canMerge
        self.mergeOnOpen = mergeOnOpen
        self.onFixConflicts = onFixConflicts
        self.onMerged = onMerged
        self.onLeave = onLeave
    }

    var body: some View {
        Group {
            if merge.done {
                MergeLandedScreen(
                    title: merge.successLine,
                    subtitle: merge.outcome?.stats.map { $0.base.isEmpty ? "" : "\(ref.branch) is part of \($0.base) now." } ?? "",
                    notice: merge.landedNotice,
                    stats: merge.outcome?.stats,
                    actionsRepo: merge.outcome == nil ? nil : merge.repo,
                    actionsCommit: merge.mergeOid
                ) {
                    if let onLeave { onLeave() } else { dismiss() }
                }
            } else {
                VStack(spacing: 0) {
                    mergeFailure
                    content
                }
            }
        }
        .navigationTitle(merge.done ? "Merged" : "")
        .navigationBarTitleDisplayMode(.inline)
        .modifier(MergeToolbar(
            merged: merge.done,
            showMerge: canMerge || merge.running,
            merging: merge.running,
            mergeIdentifier: "runner-diff-merge",
            onMerge: startMerge
        ) {
            DiffBranchTitle(
                base: model.diff?.base ?? ref.base, branch: model.diff?.branch ?? ref.branch,
                added: model.diff?.added, removed: model.diff?.removed, review: model.review)
        })
        .task {
            if mergeOnOpen && canMerge && !mergeStarted {
                mergeStarted = true
                startMerge()
            }
            await model.load()
        }
        .bareShellTop()
    }

    private func startMerge() {
        let merge: RunnerMergeModel = self.merge
        guard !merge.running, !merge.done else { return }
        Task {
            if await merge.merge() {
                FmHaptics.commit()
                onMerged(merge.successLine)
            } else if !merge.errorText.isEmpty {
                FmHaptics.failure()
            }
        }
    }

    private var fixConflicts: (() async -> Void)? {
        guard merge.conflicted, let send = onFixConflicts else { return nil }
        let fix: ConflictFixModel = self.fix
        return {
            if await fix.send(send) {
                FmHaptics.commit()
                onMerged(ConflictResolution.fixSent)
                dismiss()
            } else {
                FmHaptics.failure()
            }
        }
    }

    @ViewBuilder private var mergeFailure: some View {
        if !merge.running && (!merge.errorText.isEmpty || !merge.notice.isEmpty) {
            MergeFailureNote(errorText: merge.errorText, notice: merge.notice, fix: fix, onFix: fixConflicts)
                .padding(FmSpace.x3)
                .shellGlass(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
                .padding(.horizontal, FmSpace.x3)
                .padding(.vertical, FmSpace.x2)
        }
    }

    @ViewBuilder private var content: some View {
        if let review = model.review, let diff = model.diff {
            let model: RunnerDiffModel = self.model
            DiffReviewStream(
                model: review,
                extra: diff.committed.isEmpty ? [] : [
                    DiffMenuAction(label: "Copy patch", systemImage: "doc.on.doc") { diffCopy(diff.committed, "Patch") },
                ],
                emptyMessage: "No changes against the base branch.",
                showsHealth: false,
                refresh: { await model.load() }
            ) {
                if model.hasMore || !model.errorText.isEmpty || !(model.first?.conflicts.isEmpty ?? true) {
                    summary(diff)
                }
            }
        } else {
            List {
                if model.loading {
                    ProgressView("Reading the changes from ForkMesh…")
                        .frame(maxWidth: .infinity)
                }
                if !model.errorText.isEmpty {
                    Label(model.errorText, systemImage: "exclamationmark.triangle")
                        .font(.footnote)
                        .foregroundStyle(.red)
                        .fmMessageTools(model.errorText)
                }
            }
            .fmRefreshable { await model.load() }
        }
    }

    private func summary(_ diff: AgentDiff) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            if let first = model.first, !first.conflicts.isEmpty {
                Text("Conflicts with \(first.base): \(first.conflicts.prefix(5).joined(separator: ", "))")
                    .fmText(.bodySmall, color: fm.warning)
                    .fmMessageTools("Conflicts with \(first.base): \(first.conflicts.prefix(5).joined(separator: ", "))")
            }
            if model.hasMore {
                Button {
                    Task { await model.loadMore() }
                } label: {
                    HStack {
                        Text("Load more files")
                        if model.loading { ProgressView().controlSize(.mini) }
                    }
                }
                .disabled(model.loading)
                .accessibilityIdentifier("runner-diff-more")
            }
            if !model.errorText.isEmpty {
                Text(model.errorText)
                    .fmText(.bodySmall, color: fm.danger)
                    .fmMessageTools(model.errorText)
            }
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x2)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct RunnerMergeSheet: View {
    enum Mode { case merge, cleanup, update }

    @Environment(\.dismiss) private var dismiss
    @State private var model: RunnerMergeModel
    @State private var fix = ConflictFixModel()
    private let mode: Mode
    private let onDone: (String) -> Void
    private let onFixConflicts: (() async throws -> Void)?

    init(
        api: RunnerAgentsApi, ref: RunnerAgentRef, mode: Mode = .merge, onFixConflicts: (() async throws -> Void)? = nil,
        onDone: @escaping (String) -> Void = { _ in }
    ) {
        _model = State(initialValue: RunnerMergeModel(api: api, ref: ref))
        self.mode = mode
        self.onFixConflicts = onFixConflicts
        self.onDone = onDone
    }

    private var actionTitle: String {
        switch mode {
        case .merge: return "Merge & clean up"
        case .cleanup: return "Clean up"
        case .update: return model.updateTitle
        }
    }

    private var doneTitle: String {
        switch mode {
        case .merge: return "Merged"
        case .cleanup: return "Cleaned up"
        case .update: return "Updated"
        }
    }

    private var progressHeader: String {
        switch mode {
        case .merge: return "Merging"
        case .cleanup: return "Cleaning up"
        case .update: return "Updating"
        }
    }

    private var progressText: String {
        switch mode {
        case .merge: return model.confirmText
        case .cleanup:
            return "Deleting \(model.ref.branch.isEmpty ? "the merged branch" : model.ref.branch) on ForkMesh. Its last commit is kept as a recovery ref."
        case .update: return model.updateProgress
        }
    }

    private var finished: Bool { mode == .update ? model.updated : model.done }

    var body: some View {
        NavigationStack {
            Group {
                if mode == .update && model.updated {
                    updatedForm
                } else if model.done {
                    MergeLandedReport(
                        title: model.successLine,
                        subtitle: model.outcome?.stats.map { $0.base.isEmpty ? "" : "\(model.ref.branch) is part of \($0.base) now." } ?? "",
                        notice: model.landedNotice,
                        stats: model.outcome?.stats,
                        actionsRepo: model.outcome == nil ? nil : model.repo,
                        actionsCommit: model.mergeOid)
                } else {
                    form
                }
            }
            .navigationTitle(finished ? doneTitle : actionTitle)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(finished ? "Done" : "Close") { dismiss() }
                }
            }
            .task { await run() }
        }
    }

    private var form: some View {
        Form {
            if model.running || (model.errorText.isEmpty && model.notice.isEmpty) {
                Section {
                    HStack {
                        Text(progressText)
                            .fixedSize(horizontal: false, vertical: true)
                        Spacer()
                        ProgressView()
                    }
                } header: {
                    Text(progressHeader)
                }
            } else if !model.conflicted {
                Section {
                    Button("Try again") { Task { await run() } }
                        .accessibilityIdentifier("runner-merge-run")
                }
            }
            if !model.notice.isEmpty {
                Section {
                    Text(model.notice)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .fmMessageTools(model.notice)
                }
            }
            if !model.errorText.isEmpty {
                Section {
                    Label(model.errorText, systemImage: "exclamationmark.triangle")
                        .font(.footnote)
                        .foregroundStyle(.red)
                        .fmMessageTools(model.errorText)
                }
            }
            if model.conflicted, let onFixConflicts {
                ConflictFixSection(fix: fix) {
                    if await fix.send(onFixConflicts) {
                        FmHaptics.commit()
                        onDone(ConflictResolution.fixSent)
                        dismiss()
                    } else {
                        FmHaptics.failure()
                    }
                }
            }
        }
    }

    private var updatedForm: some View {
        Form {
            Section {
                Label(model.successLine, systemImage: "checkmark.circle")
                    .foregroundStyle(.green)
                    .fixedSize(horizontal: false, vertical: true)
            } footer: {
                Text("Merge the branch once its changes look right.")
            }
        }
        .accessibilityIdentifier("runner-update-done")
    }

    private func run() async {
        let ok: Bool
        switch mode {
        case .merge: ok = await model.merge()
        case .cleanup: ok = await model.cleanup()
        case .update: ok = await model.update()
        }
        if ok {
            FmHaptics.commit()
            onDone(model.successLine)
        } else if !model.errorText.isEmpty {
            FmHaptics.failure()
        }
    }
}
