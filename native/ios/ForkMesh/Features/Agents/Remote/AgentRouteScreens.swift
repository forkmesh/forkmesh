import FMAgents
import FMApi
import FMControl
import SwiftUI

struct AgentsRouteDestination: View {
    let filter: AgentFilter

    var body: some View {
        if filter == .needsYou {
            NeedsYouScreen()
        } else {
            AgentsScreen()
        }
    }
}

struct NeedsYouScreen: View {
    var body: some View {
        if let queue = AgentOpsFeature.approvals {
            NeedsYouList(queue: queue)
        } else {
            AgentOpsUnavailableView()
        }
    }
}

private struct NeedsYouList: View {
    @Bindable var queue: ApprovalsQueue

    var body: some View {
        List {
            if queue.unsupported {
                Text("Update ForkMesh on the desktop to answer its agents from here.")
                    .foregroundStyle(.secondary)
            } else if queue.isEmpty && !queue.loading {
                Text("No agent is waiting on you.")
                    .foregroundStyle(.secondary)
            }
            if !queue.errorText.isEmpty {
                Label(queue.errorText, systemImage: "exclamationmark.triangle")
                    .font(.footnote)
                    .foregroundStyle(.red)
                    .fmMessageTools(queue.errorText)
            }
            ForEach(queue.approvals) { approval in
                NavigationLink {
                    ApprovalScreen(queue: queue, approvalId: approval.id)
                        .shellPageClearance()
                } label: {
                    ApprovalRow(approval: approval)
                }
                .swipeActions(edge: .trailing) {
                    Button(role: .destructive) {
                        Task { await queue.deny(approval.id) }
                    } label: {
                        Label("Deny", systemImage: "xmark")
                    }
                }
            }
        }
        .navigationTitle("Needs you")
        .fmRefreshable { await queue.refresh() }
        .task {
            queue.start()
            await queue.refresh()
        }
    }
}

struct AgentRouteDestination: View {
    let id: String

    var body: some View {
        if let center = HostsFeature.agents {
            AgentRouteLookup(center: center, id: AgentRouteDestination.rowId(id))
        } else {
            AgentOpsUnavailableView()
        }
    }

    static func rowId(_ id: String) -> String {
        if id.hasPrefix("desktop:") || id.hasPrefix("node:") { return id }
        if Int(id) != nil { return "desktop:\(id)" }
        return "node:\(id)"
    }
}

private struct AgentRouteLookup: View {
    @Bindable var center: AgentControlCenter
    let id: String
    @State private var pinned: UnifiedAgentRow?
    @State private var looked = false

    private var found: UnifiedAgentRow? { center.row(forRoute: id) }

    var body: some View {
        Group {
            if let row = pinned ?? found {
                AgentRunScreen(center: center, row: row)
                    .id(row.id)
            } else if looked && !center.loading {
                AgentRouteMissing(note: center.desktopNote) {
                    looked = false
                    await center.refresh()
                    looked = true
                }
            } else {
                AgentRouteLoading()
            }
        }
        .onChange(of: found?.id, initial: true) { (_: String?, _: String?) in
            guard let found else { return }
            if pinned == nil || center.loading { pinned = found }
        }
        .task {
            if pinned == nil && found == nil { await center.refresh() }
            looked = true
        }
    }
}

private struct AgentRouteLoading: View {
    private static let lineWidths: [CGFloat] = [0.92, 0.78, 0.86, 0.6, 0.9, 0.7]

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            FmSkeleton(width: 220, height: 18)
            HStack(spacing: FmSpace.x2) {
                FmSkeleton(width: 54, height: 16, radius: FmRadius.md)
                FmSkeleton(width: 120, height: 10)
                Spacer(minLength: 0)
                FmSkeleton(width: 56, height: 10)
            }
            GeometryReader { (proxy: GeometryProxy) in
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    ForEach(Array(AgentRouteLoading.lineWidths.enumerated()), id: \.offset) { entry in
                        FmSkeleton(width: proxy.size.width * entry.element, height: 11)
                    }
                }
            }
            .padding(.top, FmSpace.x3)
        }
        .padding(.horizontal, 12)
        .padding(.top, 4)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text("Opening agent"))
        .accessibilityIdentifier("agent-route-loading")
    }
}

private struct AgentRouteMissing: View {
    let note: String
    let retry: () async -> Void
    @State private var retrying = false

    var body: some View {
        FmEmptyState(
            icon: "smart_toy_outlined", title: "Agent not found",
            message: note.isEmpty
                ? "The desktop or the relay no longer lists this run."
                : "Your desktop did not answer: \(note)")
            .overlay(alignment: .bottom) {
                FmTextButton("Try again", icon: "refresh", onPressed: {
                    guard !retrying else { return }
                    retrying = true
                    Task {
                        await retry()
                        retrying = false
                    }
                })
                .disabled(retrying)
                .padding(.bottom, FmSpace.x6)
            }
            .accessibilityIdentifier("agent-route-missing")
    }
}

struct AgentOpsHubScreen: View {
    var body: some View {
        if let client = AgentOpsFeature.client, let queue = AgentOpsFeature.approvals {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    AgentApprovalsSection(queue: queue)
                    AgentUsageCard(client: client)
                    NavigationLink {
                        CapabilityGrantsScreen(client: client)
                            .shellPageClearance()
                    } label: {
                        agentOutlinedLabel(icon: "account_tree_outlined", text: "Saved permissions")
                    }
                    .buttonStyle(.plain)
                    NavigationLink {
                        RecycleBinScreen(client: client)
                            .shellPageClearance()
                    } label: {
                        agentOutlinedLabel(icon: "restore_from_trash_outlined", text: "Recycle bin")
                    }
                    .buttonStyle(.plain)
                }
                .padding(FmSpace.x4)
            }
            .navigationTitle("Desktop agents")
        } else {
            AgentOpsUnavailableView()
        }
    }
}
