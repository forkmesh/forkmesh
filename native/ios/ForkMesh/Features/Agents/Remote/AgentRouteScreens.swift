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
    @State private var looked = false

    private var row: UnifiedAgentRow? { center.rows.first { $0.id == id } }

    var body: some View {
        Group {
            if let row {
                AgentRunScreen(center: center, row: row)
            } else if looked {
                ContentUnavailableView(
                    "Agent not found", systemImage: "cpu",
                    description: Text("The desktop or the relay no longer lists this run."))
            } else {
                ProgressView()
            }
        }
        .task {
            if row == nil { await center.refresh() }
            looked = true
        }
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
