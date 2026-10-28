// WS7: every agent the owner can steer, in one list: the desktop's own agents (on this network or
// through the relay) and runs on nodes (mirror nodes, runners, website jobs), each with a
// transport badge and the controls its transport supports.
//
// `AllAgentsDestination` is the entry point; the Agents tab (WS4's AgentsScreen) or the shell
// (WS3) places it. Nothing else in the Agents area is touched.

import FMAgents
import FMApi
import FMBase
import FMControl
import SwiftUI

struct AllAgentsDestination: View {
    var body: some View {
        if let center = HostsFeature.agents {
            AllAgentsScreen(center: center, hub: center.hub)
        } else {
            ContentUnavailableView("Agents are not available", systemImage: "cpu", description: Text("Sign in first."))
        }
    }
}

struct AllAgentsScreen: View {
    @Bindable var center: AgentControlCenter
    @Bindable var hub: DesktopControlHub
    @State private var filter: AgentTransportBadge?
    @State private var deleting: Set<String> = []
    @State private var confirmDelete: AgentLineDeleteRequest?
    @State private var notice: FmNotice?

    private var rows: [UnifiedAgentRow] {
        let pending: AgentPendingDeletes = .shared
        return center.rows.filter { row in
            (filter == nil || row.badge == filter)
                && !pending.isHidden(AgentLines.deletionKey(AgentLines.line(row)) ?? "")
        }
    }

    var body: some View {
        List {
            Section {
                DesktopReachLine(reach: hub.reach)
                if !center.desktopNote.isEmpty {
                    Label(center.desktopNote, systemImage: "exclamationmark.triangle")
                        .font(.footnote)
                        .foregroundStyle(.orange)
                        .fmMessageTools(center.desktopNote)
                }
                if !center.cloudNote.isEmpty {
                    Label(center.cloudNote, systemImage: "exclamationmark.icloud")
                        .font(.footnote)
                        .foregroundStyle(.orange)
                        .fmMessageTools(center.cloudNote)
                }
                Picker("Show", selection: $filter) {
                    Text("All").tag(AgentTransportBadge?.none)
                    ForEach(AgentTransportBadge.allCases, id: \.self) { badge in
                        Text(badge.label).tag(Optional(badge))
                    }
                }
                .pickerStyle(.menu)
            }
            Section {
                if AgentUndoDeleteLine.hasNotice {
                    AgentUndoDeleteLine()
                }
                if notice != nil {
                    FmNoticeLine($notice)
                }
                if rows.isEmpty && !center.loading {
                    Text("No agents yet.").foregroundStyle(.secondary)
                }
                ForEach(rows) { row in
                    NavigationLink {
                        AgentLazyPage {
                            AgentRunScreen(center: center, row: row)
                        }
                    } label: {
                        AgentRunRow(row: row)
                    }
                    .contextMenu {
                        if row.canStop {
                            Button(role: .destructive) {
                                Task { try? await center.stop(row) }
                            } label: {
                                Label("Stop", systemImage: "stop.fill")
                            }
                        }
                        if let request = AgentLineDeleteRequest(AgentLines.line(row)), !deleting.contains(request.id) {
                            Button(role: .destructive) {
                                ask(request)
                            } label: {
                                Label("Delete", systemImage: "trash")
                            }
                        }
                    }
                    .swipeActions(edge: .trailing) {
                        if let request = AgentLineDeleteRequest(AgentLines.line(row)), !deleting.contains(request.id) {
                            Button {
                                ask(request)
                            } label: {
                                Label("Delete", systemImage: "trash")
                            }
                            .tint(.red)
                        }
                        if row.canStop {
                            Button(role: .destructive) {
                                Task { try? await center.stop(row) }
                            } label: {
                                Label("Stop", systemImage: "stop.fill")
                            }
                        }
                    }
                }
            }
        }
        .navigationTitle("All agents")
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                NavigationLink {
                    MergeLeaderboardDestination()
                        .shellPageClearance()
                } label: {
                    Label("Leaderboard", systemImage: "trophy")
                }
                .accessibilityIdentifier("all-agents-leaderboard")
            }
            ToolbarItem(placement: .primaryAction) {
                DesktopQueueMenu(center: center)
            }
        }
        .overlay {
            if center.loading && center.rows.isEmpty { ProgressView() }
        }
        .fmRefreshable { await center.refresh() }
        .task { await center.refresh() }
        .onDisappear { AgentUndoDeleteLine.markSeen() }
        .agentOutsideDeleteConfirmation($confirmDelete) { delete($0) }
    }

    private func ask(_ request: AgentLineDeleteRequest) {
        if request.asksFirst {
            confirmDelete = request
        } else {
            delete(request)
        }
    }

    private func delete(_ request: AgentLineDeleteRequest) {
        guard deleting.insert(request.id).inserted else { return }
        Task {
            let outcome: AgentDeleteOutcome = await request.run()
            deleting.remove(request.id)
            notice = FmNotice(outcome.message, isError: outcome.failed)
            if !outcome.failed { await center.refresh() }
        }
    }
}

struct AgentRunRow: View {
    let row: UnifiedAgentRow

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(alignment: .firstTextBaseline) {
                Text(row.title)
                    .font(.body.weight(.semibold))
                    .lineLimit(2)
                Spacer(minLength: 8)
                StatusText(label: row.statusLabel, active: row.isActive)
            }
            HStack(spacing: 6) {
                TransportBadge(row: row)
                Text([row.repoLabel, row.isRunner ? "" : row.node].filter { !$0.isEmpty }.joined(separator: " · "))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            if !row.isDesktop, !row.branchName.isEmpty {
                HStack(spacing: 6) {
                    Label(row.branchName, systemImage: "arrow.triangle.branch")
                        .lineLimit(1)
                        .truncationMode(.middle)
                    if row.filesChanged >= 0 {
                        Text(row.filesChanged == 1 ? "1 file" : "\(row.filesChanged) files")
                    }
                    if row.linesAdded > 0 || row.linesRemoved > 0 {
                        Text("+\(max(row.linesAdded, 0))").foregroundStyle(.green)
                        Text("\u{2212}\(max(row.linesRemoved, 0))").foregroundStyle(.red)
                    }
                    if row.merged {
                        Label("Merged", systemImage: "arrow.triangle.merge").foregroundStyle(.purple)
                    }
                }
                .font(.caption2.monospacedDigit())
                .foregroundStyle(.secondary)
            }
        }
        .accessibilityElement(children: .combine)
    }
}

private struct StatusText: View {
    let label: String
    let active: Bool

    var body: some View {
        HStack(spacing: 4) {
            if active { ProgressView().controlSize(.mini) }
            Text(label)
                .font(.caption.weight(.medium))
                .foregroundStyle(active ? Color.accentColor : Color.secondary)
        }
    }
}
