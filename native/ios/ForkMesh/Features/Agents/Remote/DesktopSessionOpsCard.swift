import FMAgents
import FMApi
import FMControl
import SwiftUI

struct DesktopSessionOpsCard: View {
    let session: AgentSession
    let repo: Repository

    var body: some View {
        if let client = AgentOpsFeature.client, let queue = AgentOpsFeature.approvals {
            DesktopSessionOpsContent(client: client, queue: queue, session: session, repo: repo)
        }
    }
}

private struct DesktopSessionOpsContent: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let client: AgentOpsClient
    @Bindable var queue: ApprovalsQueue
    let session: AgentSession
    let repo: Repository

    @State private var controls: BulkActionsModel
    @State private var showMerge = false
    @State private var confirming = false
    @State private var note = ""

    init(client: AgentOpsClient, queue: ApprovalsQueue, session: AgentSession, repo: Repository) {
        self.client = client
        self.queue = queue
        self.session = session
        self.repo = repo
        _controls = State(initialValue: BulkActionsModel(client: client))
    }

    private var live: UnifiedAgentRow? {
        HostsFeature.agents.flatMap { liveDesktopRow($0.rows, repoName: repo.name, sessionId: session.id) }
    }

    private var isActive: Bool { live?.isActive ?? session.isActive }

    private var merged: Bool { live?.merged ?? session.merged }

    private var candidate: BulkCandidate {
        BulkCandidate(id: session.id, isActive: isActive, stoppable: live?.canStop ?? isActive, merged: merged)
    }

    private var canMerge: Bool {
        !session.branchName.isEmpty && !merged && !isActive
    }

    var body: some View {
        FmCard(radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text("On the desktop")
                    .fmText(.titleSmall)
                if session.status == "queued", let center = HostsFeature.agents {
                    DesktopQueuePositionLine(center: center, sessionId: session.id)
                }
                ForEach(queue.approvals(forSession: session.id)) { approval in
                    NavigationLink {
                        ApprovalScreen(queue: queue, approvalId: approval.id)
                            .shellPageClearance()
                    } label: {
                        ApprovalRow(approval: approval)
                    }
                    .buttonStyle(.plain)
                }
                AgentChipRow {
                    if candidate.allows(.stop) {
                        FmToneButton(label: "Stop", icon: "stop_circle_outlined", tone: .danger) {
                            begin(.stop)
                        }
                    }
                    if candidate.allows(.continueRun) {
                        FmToneButton(label: "Continue", icon: "play_arrow", tone: .accent) {
                            begin(.continueRun)
                        }
                    }
                    if canMerge {
                        FmToneButton(label: "Merge & clean up", icon: "call_merge_outlined", tone: .success) {
                            showMerge = true
                        }
                    }
                    FmToneButton(label: "Delete", icon: "delete_outline", tone: .danger) {
                        AgentLineDeleteRequest(id: "desktop:\(session.id)", deletion: .desktop(sessionId: session.id))
                            .runAfterUndoWindow()
                        dismiss()
                    }
                    .accessibilityIdentifier("agent-session-delete")
                    NavigationLink {
                        WorktreesScreen(client: client, owner: repo.owner, name: repo.name)
                            .shellPageClearance()
                    } label: {
                        agentOutlinedLabel(icon: "account_tree_outlined", text: "Worktrees")
                    }
                    .buttonStyle(.plain)
                }
                if !note.isEmpty {
                    Text(note)
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .fmMessageTools(note)
                }
                if !controls.errorText.isEmpty {
                    Text(controls.errorText)
                        .fmText(.bodySmall, color: fm.danger)
                        .fmMessageTools(controls.errorText)
                }
            }
        }
        .sheet(isPresented: $showMerge) {
            MergeCleanupSheet(
                client: client, sessionId: session.id,
                onFixConflicts: MergeCleanupSheet.desktopFix(sessionId: session.id, base: session.diffBase)
            ) { message in
                note = message
            }
        }
        .confirmationDialog(controls.confirmation?.summary ?? "", isPresented: $confirming, titleVisibility: .visible) {
            Button(controls.pendingAction?.title ?? "Confirm", role: controls.pendingAction == .stop ? .destructive : nil) {
                Task {
                    if let outcome = await controls.run() {
                        note = outcome.message
                        FmHaptics.commit()
                    }
                }
            }
            Button("Cancel", role: .cancel) { controls.cancel() }
        }
    }

    private func begin(_ action: BulkAgentAction) {
        Task {
            await controls.prepare(action, in: [candidate])
            confirming = controls.confirmation != nil
        }
    }
}
