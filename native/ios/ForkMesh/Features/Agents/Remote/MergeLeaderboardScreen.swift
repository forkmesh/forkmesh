import FMAgents
import SwiftUI

struct MergeLeaderboardDestination: View {
    var body: some View {
        if let client = AgentOpsFeature.client {
            MergeLeaderboardScreen(client: client)
        } else {
            AgentOpsUnavailableView()
        }
    }
}

struct MergeLeaderboardScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: MergeLeaderboardModel

    init(client: AgentOpsClient, sessionId: Int? = nil) {
        _model = State(initialValue: MergeLeaderboardModel(client: client, sessionId: sessionId))
    }

    var body: some View {
        Group {
            if model.loaded && !model.errorText.isEmpty {
                ScrollView {
                    Label(model.errorText, systemImage: "exclamationmark.triangle")
                        .font(.footnote)
                        .foregroundStyle(fm.danger)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(model.errorText)
                        .padding(FmSpace.x3)
                }
            } else if model.loaded {
                MergeLandedReport(
                    title: "Merge leaderboard", subtitle: model.board.subtitle, stats: model.board.stats)
            } else {
                ProgressView()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .background(fm.bgBase)
        .navigationTitle("Leaderboard")
        .navigationBarTitleDisplayMode(.inline)
        .fmRefreshable { await model.refresh() }
        .task { await model.refresh() }
        .accessibilityIdentifier("merge-leaderboard")
    }
}
