import FMAgents
import FMBase
import FMCode
import SwiftUI

struct AgentDiffScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var model: AgentDiffModel
    @State private var merge: MergeCleanupModel
    @State private var fix = ConflictFixModel()
    @State private var mergeStarted = false
    private let client: AgentOpsClient
    private let sessionId: Int
    private let canMerge: Bool
    private let mergeOnOpen: Bool
    private let onMerged: (String) -> Void
    private let onLeave: (() -> Void)?

    init(
        client: AgentOpsClient, sessionId: Int, canMerge: Bool = false, mergeOnOpen: Bool = false,
        onMerged: @escaping (String) -> Void = { _ in }, onLeave: (() -> Void)? = nil
    ) {
        _model = State(initialValue: AgentDiffModel(client: client, sessionId: sessionId, store: Services.get(KeyValueStore.self)))
        _merge = State(initialValue: MergeCleanupModel(client: client, sessionId: sessionId))
        self.client = client
        self.sessionId = sessionId
        self.canMerge = canMerge
        self.mergeOnOpen = mergeOnOpen
        self.onMerged = onMerged
        self.onLeave = onLeave
    }

    var body: some View {
        Group {
            if let branch = merge.mergedBranch {
                MergeLandedScreen(
                    title: "Merged \(branch) and cleaned up.",
                    subtitle: merge.mergeStats.map { $0.base.isEmpty ? "" : "\(branch) is part of \($0.base) now." } ?? "",
                    notice: merge.setAsideNote,
                    stats: merge.mergeStats
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
        .navigationTitle(merge.mergedBranch == nil ? "" : "Merged")
        .navigationBarTitleDisplayMode(.inline)
        .modifier(MergeToolbar(
            merged: merge.mergedBranch != nil,
            showMerge: canMerge || merge.busy,
            merging: merge.busy,
            mergeIdentifier: "agent-diff-merge",
            onMerge: startMerge
        ) {
            if let diff = model.diff {
                DiffBranchTitle(base: diff.base, branch: diff.branch, added: diff.added, removed: diff.removed, review: model.review)
            }
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
        startMerge(setAside: false)
    }

    private func startMerge(setAside: Bool) {
        let merge: MergeCleanupModel = self.merge
        guard !merge.busy, merge.mergedBranch == nil else { return }
        Task {
            if await merge.mergeNow(setAside: setAside) {
                FmHaptics.commit()
                onMerged(merge.outcome.message())
            } else if !merge.errorText.isEmpty {
                FmHaptics.failure()
            }
        }
    }

    private var fixConflicts: (() async -> Void)? {
        guard merge.conflicted, let send = MergeCleanupSheet.desktopFix(sessionId: sessionId) else { return nil }
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
        if !merge.busy && !merge.errorText.isEmpty {
            MergeFailureNote(
                errorText: merge.errorText, fix: fix, onFix: fixConflicts,
                onSetAside: merge.canSetAside ? { startMerge(setAside: true) } : nil)
                .padding(FmSpace.x3)
                .shellGlass(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
                .padding(.horizontal, FmSpace.x3)
                .padding(.vertical, FmSpace.x2)
        }
    }

    @ViewBuilder private var content: some View {
        if let review = model.review, let diff = model.diff {
            let model: AgentDiffModel = self.model
            DiffReviewStream(
                model: review,
                extra: extra(diff),
                emptyMessage: "No changes against the base branch.",
                showsHealth: false,
                refresh: { await model.load() }
            ) {
                if diff.truncated || !model.errorText.isEmpty || !diff.untracked.isEmpty {
                    summary(diff)
                }
            }
        } else {
            List {
                if model.loading {
                    ProgressView("Reading the changes from the desktop…")
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

    private func extra(_ diff: AgentDiff) -> [DiffMenuAction] {
        var actions: [DiffMenuAction] = []
        if !diff.committed.isEmpty {
            actions.append(DiffMenuAction(label: "Copy committed patch", systemImage: "doc.on.doc") { diffCopy(diff.committed, "Patch") })
        }
        if !diff.uncommitted.isEmpty {
            actions.append(DiffMenuAction(label: "Copy uncommitted patch", systemImage: "doc.on.doc") { diffCopy(diff.uncommitted, "Patch") })
        }
        return actions
    }

    private func summary(_ diff: AgentDiff) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            if diff.truncated {
                Text("Truncated: the full diff is on the desktop.")
                    .fmText(.bodySmall, color: fm.warning)
                    .fmMessageTools("Truncated: the full diff is on the desktop.")
            }
            if !model.errorText.isEmpty {
                Text(model.errorText)
                    .fmText(.bodySmall, color: fm.danger)
                    .fmMessageTools(model.errorText)
            }
            if !diff.untracked.isEmpty {
                Text("New files not yet added")
                    .fmText(.labelMedium, color: fm.textSecondary, weight: FmType.emphasis)
                ForEach(diff.untracked, id: \.self) { (path: String) in
                    Text(path)
                        .fmMono(size: 12, color: fm.textSecondary)
                }
            }
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x2)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}
