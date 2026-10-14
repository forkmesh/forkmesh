// The Actions tab (runs, a run's detail, status badges) and the Agents tab
// (sessions, a session's detail, status badges). Both tabs read when they open
// and when Refresh is pressed; nothing polls.

import FMApi
import FMBase
import FMCode
import FMControl
import SwiftUI

/// A tab header: title, a count, the "running" note and Refresh.
struct RepoActivityHeader: View {
    @Environment(\.fmPalette) private var fm

    let title: String
    let count: String?
    let running: Bool
    let refreshLabel: String
    let onRefresh: () -> Void

    private var trailing: some View {
        HStack(spacing: 0) {
            if let count = count {
                Text(count)
                    .fmText(.bodySmall, color: fm.textSecondary, weight: FmType.emphasis)
            }
            if running {
                Text("Running · refresh to update")
                    .fmText(.bodyMedium)
                    .padding(.leading, FmSpace.x2)
            }
            RepoRefreshButton(label: refreshLabel, action: onRefresh)
                .padding(.leading, FmSpace.x1)
        }
    }

    var body: some View {
        FmSectionHeader(title: title, trailing: AnyView(trailing))
            .padding(EdgeInsets.fmLTRB(FmSpace.x4, FmSpace.x2, FmSpace.x4, FmSpace.x2))
    }
}

struct RepoActionsTab: View {
    @State private var model: RepoActionsModel?
    @State private var dispatching: ActionsWorkflowSummary?

    init(repo: Repository) {
        _model = State(initialValue: CIServices.actionsModel(RepoRef(owner: repo.owner, name: repo.name)))
    }

    var body: some View {
        if let model = model {
            RepoActionsList(model: model, dispatching: $dispatching)
                .task { await model.load() }
                .fmRefreshable { await model.load() }
                .onDisappear { model.stop() }
                .sheet(item: $dispatching) { (workflow: ActionsWorkflowSummary) in
                    DispatchWorkflowSheet(model: model, workflow: workflow)
                }
        } else {
            FmEmptyState(icon: "play_circle_outline", title: "Actions are unavailable")
        }
    }
}

struct RepoAgentsTab: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoAgentsTabModel
    @State private var opened: AgentSession?

    init(repo: Repository) {
        _model = State(initialValue: RepoAgentsTabModel(
            client: RepoServices.client, repo: repo, signer: RepoServices.signer))
    }

    private func sessionRow(_ session: AgentSession) -> some View {
        RepoMobileCard(
            icon: "smart_toy_outlined",
            iconColor: fm.accent,
            title: session.displayTitle,
            subtitle: RepoAgentsTabModel.subtitle(session),
            chips: RepoAgentsTabModel.chips(session),
            trailing: AnyView(FmStatusBadge(label: session.statusLabel, color: fm.repoTone(RepoDisplay.agentStatusTone(session.statusLabel)))),
            onTap: { opened = session })
    }

    @ViewBuilder private var content: some View {
        if model.showsLoading {
            RepoLoadingCard(label: "Loading agent sessions…")
                .padding(FmSpace.x4)
        } else if let error = model.sessions.error {
            RepoEmptyCard(message: error)
                .padding(FmSpace.x4)
        } else if model.items.isEmpty {
            RepoEmptyCard(message: RepoAgentsTabModel.emptyMessage)
                .padding(FmSpace.x4)
        } else {
            RepoDividedRows(model.items) { (session: AgentSession) in
                sessionRow(session)
            }
        }
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                RepoActivityHeader(
                    title: "Agent sessions",
                    count: model.countLabel,
                    running: model.running,
                    refreshLabel: "Refresh agent sessions",
                    onRefresh: { Task { await model.refresh() } })
                content
            }
            .padding(.vertical, 10)
        }
        .task {
            await model.start()
        }
        .navigationDestination(item: $opened) { (session: AgentSession) in
            RepoAgentSessionScreen(repo: model.repo, session: session, ownerAccount: model.ownerAccount)
                .shellPageClearance()
        }
    }
}

struct RepoAgentSessionScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoAgentSessionModel

    init(repo: Repository, session: AgentSession, ownerAccount: String) {
        var desktop: (@MainActor () async throws -> [AgentAttachmentRef]?)?
        if let center: AgentControlCenter = HostsFeature.agents {
            desktop = { try await center.desktopAttachments(repoName: repo.name, sessionId: session.id) }
        }
        _model = State(initialValue: RepoAgentSessionModel(
            client: RepoServices.client, repo: repo, session: session, ownerAccount: ownerAccount,
            desktopAttachments: desktop))
    }

    private var sessionCard: some View {
        RepoInfoCard(title: model.session.displayTitle) {
            VStack(alignment: .leading, spacing: 12) {
                RepoChipWrap(model.chips())
                Text(RepoAgentSessionModel.explainer)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                if !model.failureReason.isEmpty {
                    Text(model.failureReason)
                        .fmText(.bodyMedium, color: fm.danger)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(model.failureReason)
                }
            }
        }
    }

    @ViewBuilder private var transcriptCard: some View {
        if model.transcript.loading {
            RepoLoadingCard(label: "Loading transcript…")
        } else if let error = model.transcript.error {
            RepoEmptyCard(message: error)
        } else {
            RepoInfoCard(title: "Transcript") {
                VStack(alignment: .leading, spacing: 12) {
                    RepoChipWrap([("info_outline", model.transcriptStatus), ("folder_outlined", model.repo.fullName)])
                    if !model.attachments.isEmpty {
                        AgentAttachmentStrip(refs: model.attachments, loader: AgentAttachmentsSupport.loader())
                    }
                    Text(model.transcriptText)
                        .fmMono(size: FmType.meta, color: fm.textPrimary, height: 1.45)
                        .textSelection(.enabled)
                }
            }
        }
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                sessionCard
                transcriptCard
            }
            .padding(20)
        }
        .background(fm.canvas)
        .navigationTitle(model.title)
        .navigationBarTitleDisplayMode(.inline)
        .task {
            await model.start()
        }
    }
}
