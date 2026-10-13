import FMAgents
import SwiftUI

struct WorktreesDestination: View {
    let owner: String
    let name: String

    var body: some View {
        if let client = AgentOpsFeature.client {
            WorktreesScreen(client: client, owner: owner, name: name)
        } else {
            AgentOpsUnavailableView()
        }
    }
}

struct WorktreesScreen: View {
    private let client: AgentOpsClient
    @State private var model: WorktreesModel
    @State private var mergeSession: Int?

    init(client: AgentOpsClient, owner: String, name: String) {
        self.client = client
        _model = State(initialValue: WorktreesModel(client: client, owner: owner, name: name))
    }

    var body: some View {
        List {
            if !model.errorText.isEmpty {
                Label(model.errorText, systemImage: "exclamationmark.triangle")
                    .font(.footnote)
                    .foregroundStyle(.red)
                    .fmMessageTools(model.errorText)
            }
            if model.loaded && model.list.worktrees.isEmpty && model.errorText.isEmpty {
                Text("No worktrees.").foregroundStyle(.secondary)
            }
            ForEach(model.list.worktrees) { worktree in
                WorktreeRow(worktree: worktree, base: model.list.base)
                    .swipeActions(edge: .trailing) {
                        if let session = worktree.sessionId, !worktree.isMain {
                            Button {
                                mergeSession = session
                            } label: {
                                Label("Merge & clean up", systemImage: "arrow.triangle.merge")
                            }
                            .tint(.green)
                        }
                    }
            }
        }
        .navigationTitle("Worktrees")
        .overlay {
            if model.loading && !model.loaded { ProgressView() }
        }
        .fmRefreshable { await model.refresh() }
        .task { await model.refresh() }
        .sheet(item: Binding(get: { mergeSession.map { SessionRef(id: $0) } }, set: { mergeSession = $0?.id })) { ref in
            MergeCleanupSheet(client: client, sessionId: ref.id, onFixConflicts: MergeCleanupSheet.desktopFix(sessionId: ref.id)) { _ in
                Task { await model.refresh() }
            }
        }
    }
}

struct SessionRef: Identifiable, Hashable {
    let id: Int
}

private struct WorktreeRow: View {
    let worktree: AgentWorktree
    let base: String

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(worktree.branch.isEmpty ? worktree.name : worktree.branch)
                    .font(.system(.body, design: .monospaced))
                    .lineLimit(1)
                Spacer()
                if worktree.locked {
                    Image(systemName: "lock.fill")
                        .foregroundStyle(.secondary)
                        .accessibilityLabel("Locked")
                }
            }
            HStack(spacing: 6) {
                Text(worktree.syncLabel)
                if !base.isEmpty && !worktree.isMain {
                    Text("vs \(base)")
                }
                if let session = worktree.sessionId {
                    Text("· agent #\(session)")
                }
                if worktree.prunable {
                    Text("· missing on disk")
                }
            }
            .font(.caption)
            .foregroundStyle(.secondary)
        }
        .accessibilityElement(children: .combine)
    }
}
