import FMAgents
import FMApi
import FMBase
import SwiftUI

struct UsageScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var usage: AgentUsageModel?
    @State private var runnerAccounts: RunnerAccountsModel? = Services.get(RunnerAccountsModel.self)

    init(usage: AgentUsageModel? = nil) {
        _usage = State(initialValue: usage ?? AgentOpsFeature.client.map { AgentUsageModel(client: $0) })
    }

    var body: some View {
        content
            .navigationTitle("Usage")
            .navigationBarTitleDisplayMode(.inline)
    }

    @ViewBuilder private var content: some View {
        if let usage = usage {
            AgentAccountsPanel(usage: usage, runnerAccounts: runnerAccounts)
        } else {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    FmEmptyState(
                        icon: "computer",
                        title: "No desktop connected",
                        message: "Pair this phone with your desktop to see its Claude Code and Codex usage here."
                    )
                    if let runnerAccounts = runnerAccounts, runnerAccounts.visible {
                        RunnerAccountsPanel(
                            model: runnerAccounts, attention: Services.get(RunnerAttentionModel.self), titled: true)
                    }
                }
                .padding(FmSpace.x4)
            }
            .fmPreGlassBackground(fm.bgBase)
            .fmRefreshable { await runnerAccounts?.refresh() }
            .task { await runnerAccounts?.refresh() }
        }
    }
}
